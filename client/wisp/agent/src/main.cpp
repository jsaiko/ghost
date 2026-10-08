// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// wisp-agent: the Wisp thin client's link to Veil (docs/design/wisp.md).
// Runs as root from wisp-agent.service for as long as the client is up:
//   - connects to the Veil named on the kernel command line, over `wisp/1`
//     on its lobby port, pinned to veil_cert= as the greeter's logins are,
//     and presents wisp_key= with a report on this machine (WispHello);
//   - writes the spectre profile Veil sends (on connecting, and whenever an
//     administrator changes it) to /run/wisp/profile.json, which the greeter
//     reads at each spectre launch;
//   - watches /run/wisp/session.json, which the greeter writes while spectre
//     runs, and tells Veil when a session starts or ends;
//   - carries out an administrator's log out (SIGUSR1 to spectre: its own
//     End Session), restart or shut down;
//   - reconnects with backoff whenever Veil is down or restarts.
// Nothing here is kept between boots: the image is RAM-only, and the
// profile comes from Veil again every time.
#include "boot_args.h"
#include "system_info.h"
#include "wisp_client.h"

#include "gdp/clock.hpp"
#include "gdp/error_codes.hpp"

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>

namespace {

using namespace wisp_agent;

// Exit status for a configuration that can't work (no veil= and so on):
// the unit's RestartPreventExitStatus, so systemd doesn't retry it forever.
constexpr int kExitBadConfig = 2;

constexpr uint64_t kBackoffMinMs = 1000;
constexpr uint64_t kBackoffMaxMs = 60000;
// A wrong key stays wrong until the client reboots with a new one.
constexpr uint64_t kBackoffWrongKeyMs = 300000;
// How long an administrator's log out waits for the host to end the
// session before spectre is simply stopped (a disconnect, not a logout).
constexpr uint64_t kLogoutGraceMs = 30000;

void log(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fputs("wisp-agent: ", stderr);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
	va_end(args);
	fflush(stderr);
}

uint64_t now_ms() {
	return gdp::monotonic_us() / 1000;
}

// `ms`, give or take a quarter: after a Veil restart, every client would
// otherwise retry in the same instant and run into its [auth]
// max_startups together, again and again.
uint64_t jittered(uint64_t ms) {
	static std::minstd_rand rng(std::random_device{}());
	return ms * 3 / 4 + std::uniform_int_distribution<uint64_t>(0, ms / 2)(rng);
}

std::string json_string(const std::string &s) {
	std::string out = "\"";
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += (char)c;
			}
		}
	}
	return out + "\"";
}

// Every field, defaults included, in SpectreSettings' names
// (client/spectre-settings/spectre_settings.h) plus the client's own: the
// greeter fills in its own defaults for whatever is missing, so nothing may
// be left out.
std::string profile_json(const gdp::wisp::ClientProfile &p) {
	auto b = [](bool v) { return v ? "true" : "false"; };
	std::ostringstream out;
	out << "{\n"
		<< "  \"resolution\": " << json_string(p.resolution()) << ",\n"
		<< "  \"view\": " << json_string(p.view()) << ",\n"
		<< "  \"preferred_decoder\": " << json_string(p.preferred_decoder()) << ",\n"
		<< "  \"preferred_codec\": " << json_string(p.preferred_codec()) << ",\n"
		<< "  \"lossless_refinement\": " << b(p.lossless_refinement()) << ",\n"
		<< "  \"allow_pyrowave\": " << b(p.allow_pyrowave()) << ",\n"
		<< "  \"network_profile\": " << json_string(p.network_profile()) << ",\n"
		<< "  \"forward_gamepads\": " << b(p.forward_gamepads()) << ",\n"
		<< "  \"microphone\": " << b(p.microphone()) << ",\n"
		<< "  \"debug_logging\": " << b(p.debug_logging()) << ",\n"
		<< "  \"display_sleep_minutes\": " << p.display_sleep_minutes() << "\n"
		<< "}\n";
	return out.str();
}

// Replaces `path` in one rename, so the greeter never reads half a file.
bool write_atomically(const std::string &path, const std::string &content) {
	std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "w");
	if (!f) {
		return false;
	}
	bool ok = fwrite(content.data(), 1, content.size(), f) == content.size();
	ok = fclose(f) == 0 && ok;
	ok = ok && chmod(tmp.c_str(), 0644) == 0 && rename(tmp.c_str(), path.c_str()) == 0;
	if (!ok) {
		unlink(tmp.c_str());
	}
	return ok;
}

struct SessionFile {
	gdp::wisp::WispSession session;
	pid_t spectre_pid = 0; // 0: not given
};

// The greeter's session.json ({"user", "host_name", "pid"}, pid being
// spectre's); an empty session when there is none.
SessionFile read_session(const std::string &path) {
	SessionFile result;
	std::ifstream file(path);
	if (!file) {
		return result;
	}
	std::stringstream buf;
	buf << file.rdbuf();
	if (buf.str().find_first_not_of(" \t\r\n") == std::string::npos) {
		return result; // just created by a writer that hasn't written yet
	}
	google::protobuf::util::JsonParseOptions options;
	options.ignore_unknown_fields = true;
	google::protobuf::Struct fields;
	if (!google::protobuf::util::JsonStringToMessage(buf.str(), &result.session, options).ok() ||
		!google::protobuf::util::JsonStringToMessage(buf.str(), &fields).ok()) {
		log("%s doesn't parse; reporting no session", path.c_str());
		return SessionFile();
	}
	auto pid = fields.fields().find("pid");
	if (pid != fields.fields().end() && pid->second.has_number_value() && pid->second.number_value() > 1) {
		result.spectre_pid = (pid_t)pid->second.number_value();
	}
	return result;
}

// True if `pid` is still a spectre: the greeter's session.json can outlive
// it by a moment, and the pid be reused.
bool is_spectre(pid_t pid) {
	std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
	std::string name;
	return comm && std::getline(comm, name) && name == "spectre";
}

// Runs `systemctl --no-block <verb>`: queued, so this returns at once and
// the agent is stopped with everything else.
void systemctl(const char *verb) {
	const char *argv[] = {"systemctl", "--no-block", verb, nullptr};
	pid_t pid;
	int err = posix_spawnp(&pid, "systemctl", nullptr, nullptr, const_cast<char **>(argv), environ);
	if (err != 0) {
		log("can't run systemctl %s: %s", verb, strerror(err));
		return;
	}
	int status = 0;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		log("systemctl %s failed", verb);
	}
}

std::string usage(const char *argv0) {
	return std::string("Usage: ") + argv0 +
		" [--veil host[:port]] [--veil-cert sha256] [--wisp-key key] [--run-dir dir] [--spectre path]\n"
		"       [--mac aa:bb:cc:dd:ee:ff]\n"
		"Each flag overrides the kernel command line's veil=, veil_cert= and wisp_key=, for running\n"
		"the agent on a desktop. --run-dir is where profile.json and session.json live (/run/wisp).\n"
		"--mac reports as that client instead of the default route's interface (testing).\n";
}

class Agent {
public:
	Agent(BootArgs boot, std::string run_dir, std::string spectre, std::string mac_override)
		: boot_(std::move(boot)), run_dir_(std::move(run_dir)), spectre_(std::move(spectre)),
		  mac_override_(std::move(mac_override)), session_path_(run_dir_ + "/session.json"),
		  profile_path_(run_dir_ + "/profile.json") {}

	int run();

private:
	void try_connect();
	void on_closed(const std::string &reason, uint64_t code);
	void session_file_changed();
	void write_profile(const gdp::wisp::ClientProfile &profile);
	void run_command(gdp::wisp::WispAction action);
	void check_logout_deadline();
	int poll_timeout_ms() const;

	BootArgs boot_;
	std::string run_dir_;
	std::string spectre_;
	std::string mac_override_;
	std::string session_path_;
	std::string profile_path_;

	std::unique_ptr<WispClient> client_;
	// Set from the client's own callback; it is destroyed after dispatch().
	bool client_closed_ = false;
	uint64_t next_attempt_ms_ = 0;
	uint64_t backoff_ms_ = kBackoffMinMs;
	std::string mac_;
	std::string iface_;
	gdp::wisp::SystemInfo system_;
	bool have_system_ = false;
	gdp::wisp::WispSession session_;
	pid_t spectre_pid_ = 0;
	// While an administrator's log out waits for the host: when to stop
	// spectre instead. 0: none pending.
	uint64_t logout_deadline_ms_ = 0;
};

int Agent::run() {
	mkdir(run_dir_.c_str(), 0775); // tmpfiles.d makes it on the image
	int inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	// IN_CREATE as well as the rename events: Qt's QSaveFile (the greeter)
	// links an unnamed temp file straight into place when the target doesn't
	// exist yet, which is only a create, not a rename or a close.
	if (inotify < 0 ||
		inotify_add_watch(inotify, run_dir_.c_str(),
			IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE) < 0) {
		log("can't watch %s: %s", run_dir_.c_str(), strerror(errno));
		return 1;
	}
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGTERM);
	sigaddset(&signals, SIGINT);
	sigprocmask(SIG_BLOCK, &signals, nullptr);
	int sigfd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);

	SessionFile file = read_session(session_path_);
	session_ = file.session;
	spectre_pid_ = file.spectre_pid;
	log("Veil %s:%u", boot_.veil_host.c_str(), boot_.veil_port);
	for (;;) {
		if (!client_ && now_ms() >= next_attempt_ms_) {
			try_connect();
		}
		pollfd fds[3] = {{inotify, POLLIN, 0}, {sigfd, POLLIN, 0},
			{client_ ? client_->notify_fd() : -1, POLLIN, 0}};
		int n = poll(fds, 3, poll_timeout_ms());
		if (n < 0 && errno != EINTR) {
			log("poll: %s", strerror(errno));
			return 1;
		}
		if (fds[1].revents & POLLIN) {
			log("stopping");
			client_.reset(); // closes the connection, so Veil sees us go at once
			return 0;
		}
		if (fds[0].revents & POLLIN) {
			alignas(inotify_event) char buf[4096];
			bool ours = false;
			ssize_t len;
			while ((len = read(inotify, buf, sizeof(buf))) > 0) {
				for (char *p = buf; p < buf + len;) {
					auto *event = reinterpret_cast<inotify_event *>(p);
					ours = ours || (event->len && strcmp(event->name, "session.json") == 0);
					p += sizeof(inotify_event) + event->len;
				}
			}
			if (ours) {
				session_file_changed();
			}
		}
		if (client_) {
			client_->dispatch();
			if (client_closed_) {
				client_.reset();
				client_closed_ = false;
			}
		}
		check_logout_deadline();
	}
}

int Agent::poll_timeout_ms() const {
	uint64_t now = now_ms();
	auto until = [now](uint64_t at) { return at > now ? (int)std::min<uint64_t>(at - now, INT_MAX) : 0; };
	// Connected: the connection's own events, including its end.
	int timeout = client_ ? -1 : until(next_attempt_ms_);
	if (logout_deadline_ms_) {
		int logout = until(logout_deadline_ms_);
		timeout = timeout < 0 ? logout : std::min(timeout, logout);
	}
	return timeout;
}

void Agent::try_connect() {
	// The identity is the boot NIC's MAC, so wait for the default route.
	std::string iface = default_route_interface();
	std::string mac = iface.empty() ? std::string() : interface_mac(iface);
	if (!mac.empty() && !mac_override_.empty()) {
		mac = mac_override_;
	}
	if (mac.empty()) {
		next_attempt_ms_ = now_ms() + kBackoffMinMs;
		return;
	}
	if (mac != mac_ || !have_system_ || system_.hw_decode_size() == 0) {
		// Once per boot NIC; again while spectre found no decoder at all,
		// in case the GPU driver was still loading.
		system_ = read_system_info(iface, spectre_);
		have_system_ = true;
		if (mac != mac_) {
			log("this client is %s (%s)", mac.c_str(), iface.c_str());
		}
	}
	mac_ = mac;
	iface_ = iface;

	gdp::wisp::WispHello hello = make_hello(mac_, system_);
	hello.set_key(boot_.wisp_key);
	if (!session_.user().empty() || !session_.host_name().empty()) {
		*hello.mutable_session() = session_;
	}
	client_ = std::make_unique<WispClient>();
	client_closed_ = false;
	client_->on_certificate = [this](
								  const std::string &cert_sha256) { return cert_sha256 == boot_.veil_cert; };
	client_->on_profile = [this, announced = false](const gdp::wisp::ClientProfile &profile) mutable {
		if (!announced) {
			announced = true;
			log("connected to Veil");
		}
		backoff_ms_ = kBackoffMinMs;
		write_profile(profile);
	};
	client_->on_command = [this](gdp::wisp::WispAction action) { run_command(action); };
	client_->on_closed = [this](const std::string &reason, uint64_t code) { on_closed(reason, code); };
	if (!client_->connect(boot_.veil_host, boot_.veil_port, std::move(hello))) {
		log("can't start a connection to Veil");
		client_.reset();
		next_attempt_ms_ = now_ms() + jittered(backoff_ms_);
	}
}

void Agent::on_closed(const std::string &reason, uint64_t code) {
	client_closed_ = true;
	bool was_welcomed = client_->welcomed();
	if (code == (uint64_t)gdp::ErrorCode::kAuthFailed) {
		log("Veil refused this client's wisp_key= (rotated? the boot server's WISP_KEY must match "
			"Veil's /etc/ghost/veil-wisp.key, and clients pick up a new one at their next boot)");
		backoff_ms_ = kBackoffWrongKeyMs;
	} else if (was_welcomed) {
		log("lost Veil%s%s", reason.empty() ? "" : ": ", reason.c_str());
		backoff_ms_ = kBackoffMinMs;
	} else {
		log("can't reach Veil%s%s; retrying in about %llu s", reason.empty() ? "" : ": ", reason.c_str(),
			(unsigned long long)(backoff_ms_ / 1000));
	}
	next_attempt_ms_ = now_ms() + jittered(backoff_ms_);
	if (!was_welcomed && code != (uint64_t)gdp::ErrorCode::kAuthFailed) {
		backoff_ms_ = std::min(backoff_ms_ * 2, kBackoffMaxMs);
	}
}

void Agent::session_file_changed() {
	SessionFile file = read_session(session_path_);
	spectre_pid_ = file.spectre_pid;
	if (file.session.SerializeAsString() == session_.SerializeAsString()) {
		return;
	}
	session_ = file.session;
	if (session_.user().empty()) {
		logout_deadline_ms_ = 0;
		log("session ended");
	} else {
		log("session started: %s on %s", session_.user().c_str(), session_.host_name().c_str());
	}
	if (client_) {
		client_->send_session(session_);
	}
}

void Agent::run_command(gdp::wisp::WispAction action) {
	switch (action) {
	case gdp::wisp::WISP_ACTION_LOG_OUT:
		if (session_.user().empty() || spectre_pid_ <= 0 || !is_spectre(spectre_pid_)) {
			log("Veil asked to log out, but no session is running");
			return;
		}
		// spectre's End Session: the host ends the desktop session, and
		// spectre exits when it has, which sends the greeter back to its
		// sign-in screen.
		log("logging %s out (Veil's request)", session_.user().c_str());
		if (kill(spectre_pid_, SIGUSR1) == 0) {
			logout_deadline_ms_ = now_ms() + kLogoutGraceMs;
		} else {
			log("can't signal spectre (%d): %s", (int)spectre_pid_, strerror(errno));
		}
		return;
	case gdp::wisp::WISP_ACTION_REBOOT:
		log("restarting (Veil's request)");
		systemctl("reboot");
		return;
	case gdp::wisp::WISP_ACTION_POWER_OFF:
		log("shutting down (Veil's request)");
		systemctl("poweroff");
		return;
	default: log("ignoring an unknown command from Veil (%d)", (int)action); return;
	}
}

void Agent::check_logout_deadline() {
	if (!logout_deadline_ms_ || now_ms() < logout_deadline_ms_) {
		return;
	}
	logout_deadline_ms_ = 0;
	if (!session_.user().empty() && spectre_pid_ > 0 && is_spectre(spectre_pid_)) {
		log("the host didn't end the session in %llu s; stopping spectre",
			(unsigned long long)(kLogoutGraceMs / 1000));
		kill(spectre_pid_, SIGTERM);
	}
}

void Agent::write_profile(const gdp::wisp::ClientProfile &profile) {
	if (!write_atomically(profile_path_, profile_json(profile))) {
		log("can't write %s: %s", profile_path_.c_str(), strerror(errno));
		return;
	}
	log("profile updated; spectre uses it from its next launch, display sleep at once");
}

std::string default_spectre_path(const char *argv0) {
	char self[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	std::string dir = n > 0 ? std::string(self, n) : std::string(argv0);
	size_t slash = dir.rfind('/');
	return slash == std::string::npos ? "spectre" : dir.substr(0, slash) + "/spectre";
}

} // namespace

int main(int argc, char *argv[]) {
	std::string veil, cert, key, mac, run_dir = "/run/wisp", spectre = default_spectre_path(argv[0]);
	for (int i = 1; i < argc; ++i) {
		std::string arg = argv[i];
		std::string *target = arg == "--veil" ? &veil
			: arg == "--veil-cert"            ? &cert
			: arg == "--wisp-key"             ? &key
			: arg == "--run-dir"              ? &run_dir
			: arg == "--spectre"              ? &spectre
			: arg == "--mac"                  ? &mac
											  : nullptr;
		if (!target || i + 1 >= argc) {
			fputs(usage(argv[0]).c_str(), stderr);
			return arg == "--help" || arg == "-h" ? 0 : 1;
		}
		*target = argv[++i];
	}
	BootArgs boot = BootArgs::load(veil, cert, key);
	if (!boot.error.empty()) {
		log("%s; not reporting to Veil", boot.error.c_str());
		return kExitBadConfig;
	}
	return Agent(std::move(boot), run_dir, spectre, mac).run();
}
