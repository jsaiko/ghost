// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/report_cli.hpp"

#include "gdp/framing.hpp"
#include "session/report_server.hpp"
#include "util/config.hpp"
#include "wraith.pb.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace wraith {

namespace {

// A helper that hangs (vulkaninfo on a wedged GPU) costs this much, not the
// whole report.
constexpr int kCommandTimeoutMs = 15000;

// Runs `argv` with stderr folded into stdout and returns what it printed,
// plus a line saying how it ended if it didn't exit 0 -- a missing tool and
// a failing one both end up in the report as the reason for the gap.
std::string capture(const std::vector<std::string> &argv) {
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) < 0) {
		return "(pipe: " + std::string(strerror(errno)) + ")\n";
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return "(fork: " + std::string(strerror(errno)) + ")\n";
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		int devnull = open("/dev/null", O_RDONLY);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
		}
		std::vector<char *> args;
		for (const std::string &a : argv) {
			args.push_back(const_cast<char *>(a.c_str()));
		}
		args.push_back(nullptr);
		execvp(args[0], args.data());
		_exit(127);
	}
	close(pipefd[1]);

	std::string out;
	bool timed_out = false;
	char buf[8192];
	for (;;) {
		pollfd p{pipefd[0], POLLIN, 0};
		int ready = poll(&p, 1, kCommandTimeoutMs);
		if (ready == 0) {
			timed_out = true;
			kill(pid, SIGKILL);
			break;
		}
		if (ready < 0 && errno == EINTR) {
			continue;
		}
		ssize_t n = ready > 0 ? read(pipefd[0], buf, sizeof(buf)) : 0;
		if (n <= 0) {
			break;
		}
		out.append(buf, (size_t)n);
	}
	close(pipefd[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	if (timed_out) {
		out += "(" + argv[0] + " did not finish in 15 s and was killed)\n";
	} else if (WIFEXITED(status) && WEXITSTATUS(status) == 127 && out.empty()) {
		out = "(" + argv[0] + " is not installed)\n";
	} else if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
		out += "(" + argv[0] + " exited with status " +
			std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")\n";
	}
	return out;
}

std::string read_file(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return "(" + path + " could not be read: " + strerror(errno) + ")\n";
	}
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

// One response frame, or false if the connection ends or the frame is bad.
bool read_response(int fd, ghost::wraith::WraithResponse *response) {
	gdp::FrameReader reader;
	for (;;) {
		gdp::FrameReader::Result result = reader.drain(response);
		if (result == gdp::FrameReader::Result::kOk) {
			return true;
		}
		if (result != gdp::FrameReader::Result::kIncomplete) {
			return false;
		}
		uint8_t buf[65536];
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		reader.feed(buf, (size_t)n);
	}
}

// What the running wraith knows, and what the client says. Both come back
// as text; when this isn't run inside a session, or the session is gone,
// the text says that instead.
void ask_running_session(std::string *status, std::string *client) {
	std::string path = control_socket_path();
	if (path.empty()) {
		*status = "XDG_RUNTIME_DIR is not set: this is not run from inside a desktop session\n";
		*client = "not asked\n";
		return;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (fd < 0 || path.size() >= sizeof(addr.sun_path)) {
		*status = "could not open a socket\n";
		*client = "not asked\n";
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	memcpy(addr.sun_path, path.c_str(), path.size() + 1);
	if (connect(fd, (const sockaddr *)&addr, sizeof(addr)) < 0) {
		*status = "no running wraith answered at " + path + " (" + strerror(errno) +
			"): is this run inside the remote desktop?\n";
		*client = "not asked\n";
		close(fd);
		return;
	}
	ghost::wraith::WraithRequest request;
	request.mutable_report();
	std::vector<uint8_t> frame;
	if (!gdp::encode_frame(request, &frame) ||
		send(fd, frame.data(), frame.size(), MSG_NOSIGNAL) != (ssize_t)frame.size()) {
		*status = "could not send the request to the session\n";
		*client = "not asked\n";
		close(fd);
		return;
	}
	// The server waits up to 5 s for the client; this adds a margin.
	timeval timeout{10, 0};
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	ghost::wraith::WraithResponse response;
	if (!read_response(fd, &response)) {
		*status = "the session's answer was cut short\n";
		*client = "not asked\n";
	} else if (response.has_report()) {
		*status = response.report().host_status();
		*client = response.report().client_text();
	} else {
		*status = "the session said: " +
			(response.has_error() ? response.error().message() : std::string("(nothing)")) + "\n";
		*client = "not asked\n";
	}
	close(fd);
}

std::string home_directory() {
	const char *home = getenv("HOME");
	if (home && *home) {
		return home;
	}
	const struct passwd *pw = getpwuid(getuid());
	return pw ? pw->pw_dir : ".";
}

std::string environment_summary() {
	std::string text;
	for (const char *name : {"GHOST_SESSION_TYPE", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP",
			 "WAYLAND_DISPLAY", "DISPLAY", "XDG_RUNTIME_DIR", "XDG_GHOST_KWIN_ARGS", "SPECTRE_LOG"}) {
		const char *value = getenv(name);
		text += std::string(name) + "=" + (value ? value : "(unset)") + "\n";
	}
	return text;
}

} // namespace

int run_report_mode(const char *directory) {
	fs::path out_dir = directory && *directory ? fs::path(directory) : fs::path(home_directory());
	char stamp[32];
	time_t now = time(nullptr);
	struct tm local;
	localtime_r(&now, &local);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
	std::string name = std::string("ghost-report-") + stamp;
	fs::path staging = out_dir / name;

	std::error_code ec;
	fs::create_directories(staging, ec);
	if (ec) {
		fprintf(stderr, "wraith --report: %s: %s\n", staging.c_str(), ec.message().c_str());
		return 1;
	}
	auto add = [&](const char *file, const std::string &text) {
		std::ofstream(staging / file, std::ios::binary) << text;
	};

	fprintf(stderr, "Asking the running session (and its client)...\n");
	std::string status, client;
	ask_running_session(&status, &client);
	add("wraith-status.txt", status);
	add("client.txt", client);

	fprintf(stderr, "Collecting logs and system information...\n");
	add("wraith-journal.txt",
		capture(
			{"journalctl", "--user", "-u", "wraith.service", "--no-pager", "-o", "short-iso", "-n", "5000"}));
	// Only the units that bear on a session -- the compositors, PipeWire and
	// the portals -- not the whole user journal: other applications log file
	// names and addresses there, and none of it helps with a streaming problem.
	add("desktop-warnings.txt",
		capture({"journalctl", "--user", "--no-pager", "-o", "short-iso", "-p", "warning", "-n", "500", "-u",
			"plasma-kwin*", "-u", "org.gnome.Shell*", "-u", "pipewire*", "-u", "wireplumber*", "-u",
			"xdg-desktop-portal*"}));
	add("wraith.toml", read_file(kDefaultConfigPath));
	add("system.txt",
		capture({"uname", "-a"}) + "\n" + read_file("/etc/os-release") + "\n" + environment_summary());
	add("gpu.txt",
		"$ ls -l /dev/dri\n" + capture({"ls", "-l", "/dev/dri"}) + "\n$ vainfo\n" + capture({"vainfo"}) +
			"\n$ vulkaninfo --summary\n" + capture({"vulkaninfo", "--summary"}));
	add("README.txt",
		"Made by `wraith --report`, on the host, in this user's account. Nothing was sent anywhere.\n"
		"desktop-warnings.txt holds warnings from the compositor, PipeWire and the portals only.\n"
		"Not included: ghostd's and Veil's logs (root's; an administrator has them), other\n"
		"applications' logs, and anything from the client beyond client.txt.\n");

	fs::path archive = out_dir / (name + ".tar.gz");
	std::string tar_output = capture({"tar", "-czf", archive.string(), "-C", out_dir.string(), name});
	if (fs::exists(archive)) {
		fs::remove_all(staging, ec);
		printf("%s\n", archive.c_str());
	} else {
		// No tar here: the directory is the report.
		fprintf(stderr, "wraith --report: could not make the archive (%s)", tar_output.c_str());
		printf("%s\n", staging.c_str());
	}
	return 0;
}

} // namespace wraith
