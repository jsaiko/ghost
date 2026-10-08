// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/gnome_remote_session.hpp"
#include "screencast/ext_remote_session.hpp"
#include "screencast/kwin_remote_session.hpp"
#include "screencast/screencast_host.hpp"
#include "session/seat_client.hpp"
#include "session/report_cli.hpp"
#include "session/report_server.hpp"
#include "session/session_cert.hpp"
#include "session/session_profile.hpp"
#include "session/token.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include <getopt.h>
#include <openssl/crypto.h>
#include <signal.h>
#include <unistd.h>

// Reports this uid's session as over and exits -- the whole job of
// `--session-ended` (docs/design/login-and-sessions.md#teardown). Run as
// wraith.service's `ExecStopPost=-`, after the session has already torn
// down, so this never touches the compositor; it only speaks the control
// protocol wraith already links.
static int run_session_ended_mode(const char *socket_path) {
	wraith::Log::init(wraith::LogLevel::Error);

	wraith::SeatClient client;
	std::string error;
	if (!client.connect_only(socket_path, &error)) {
		WLOG_ERROR("--session-ended: %s", error.c_str());
		return 1;
	}

	// systemd's own verdict on why wraith.service stopped
	// (systemd.exec(5)'s ExecStopPost= environment); empty when run
	// outside that context (e.g. by hand while testing).
	const char *result = getenv("SERVICE_RESULT");
	const char *exit_code = getenv("EXIT_CODE");
	const char *exit_status = getenv("EXIT_STATUS");
	if (!client.send_session_ended(result ? result : "", exit_code ? exit_code : "",
			exit_status ? exit_status : "", &error)) {
		WLOG_ERROR("--session-ended: %s", error.c_str());
		return 1;
	}
	return 0;
}

// `--check-config [path]`: validates wraith.toml the way startup reads it
// and prints the settings a session would run with. Exits 1 on a file
// wraith would reject.
static int run_check_config_mode(const char *path) {
	wraith::ConfigLoad load = wraith::load_config(path);
	for (const std::string &warning : load.warnings) {
		fprintf(stderr, "%s: warning: %s\n", path, warning.c_str());
	}
	if (!load.ok) {
		fprintf(stderr, "%s: %s\n", path, load.error.c_str());
		return 1;
	}
	if (load.missing) {
		fprintf(stderr, "%s: not found, every setting at its default\n", path);
	}
	fputs(wraith::describe_config(wraith::config()).c_str(), stdout);
	return 0;
}

static void usage(const char *argv0) {
	printf("Usage: %s -p session-type [-d sessions-datadir] [-o WIDTHxHEIGHT]\n"
		   "          [-G ghostd-control-socket | -l gdp-port -t token -c cert.pem -k key.pem]\n"
		   "          [-e output.h264] [-b bitrate-bps] [-F] [-C wraith.toml]\n"
		   "       %s --check-config [wraith.toml]\n"
		   "       %s --session-ended ghostd-control-socket\n"
		   "       %s --report [directory]\n"
		   "\n"
		   "wraith is GDP's host-side session agent: it starts a session type's\n"
		   "compositor headless, captures and encodes its output, streams it to\n"
		   "the client and injects the client's input back.\n"
		   "\n"
		   "  -p   session type id (docs/reference/session-profiles.md): resolves a\n"
		   "       sessions.d/<id>.conf hosting profile and launches its Exec as\n"
		   "       the session leader. With -G, overrides the type ghostd sent in\n"
		   "       SessionInit -- for testing a type without a lobby.\n"
		   "  -d   directory to search for sessions.d/*.conf before the compiled-in\n"
		   "       datadir (default: /etc/ghost/sessions.d, then GHOST_DATADIR).\n"
		   "  -o   output size until a client asks for its own (default 1920x1080).\n"
		   "  -G   ghostd-mediated mode (the deployed path): connect to the session's\n"
		   "       control socket (host/proto/control.proto), get a session secret, a\n"
		   "       port range, and the session type to launch, bind the first free\n"
		   "       port in it, and verify spectre's token against that secret\n"
		   "       (gdp-spec.md §4.8). Presents a throwaway certificate generated\n"
		   "       at startup, whose fingerprint ghostd vouches for to spectre\n"
		   "       (gdp-spec.md §2.3). Mutually exclusive with\n"
		   "       -l/-t/-c/-k.\n"
		   "  -l   direct-connect mode: listen on a fixed port, authenticate\n"
		   "       spectre with a static shared token (-t), present -c/-k. No\n"
		   "       ghostd involved -- for local testing only.\n"
		   "  -e   also append the encoded video to this file (Annex B).\n"
		   "  -b   bitrate ceiling in bits per second, overriding wraith.toml.\n"
		   "  -F   force the software (x264) encoder fallback even when a\n"
		   "       hardware encoder (VA-API or NVENC) works -- for testing the\n"
		   "       fallback path itself; normally chosen automatically when\n"
		   "       no hardware encoder is usable (e.g. no DRM render node).\n"
		   "       Same as [encode] force_software = true in the settings file.\n"
		   "  -C   settings file to read instead of %s\n"
		   "       (packaging/config/wraith.toml has every key at its default).\n"
		   "  --check-config  validate a settings file (default: the one above)\n"
		   "       and print the settings a session would run with; exits 1 if\n"
		   "       wraith would reject it.\n"
		   "  --session-ended  report this uid's session as over and exit --\n"
		   "       run as wraith.service's ExecStopPost=, never by hand.\n"
		   "  --report  run inside the remote desktop: collect the logs and settings\n"
		   "       a bug report needs, the client's side included, into\n"
		   "       ghost-report-<date>-<time>.tar.gz in the directory (default: the\n"
		   "       home directory). Nothing is sent anywhere.\n",
		argv0, argv0, argv0, argv0, wraith::kDefaultConfigPath);
}

int main(int argc, char *argv[]) {
	if (argc >= 3 && std::strcmp(argv[1], "--session-ended") == 0) {
		return run_session_ended_mode(argv[2]);
	}
	if (argc >= 2 && std::strcmp(argv[1], "--report") == 0) {
		return wraith::run_report_mode(argc >= 3 ? argv[2] : nullptr);
	}
	if (argc >= 2 && std::strcmp(argv[1], "--check-config") == 0) {
		return run_check_config_mode(argc >= 3 ? argv[2] : wraith::kDefaultConfigPath);
	}

	// SIGUSR1 is ghostseat ending the session for a console login
	// (SessionServices::start_gdp_session reads it through a signalfd).
	// Blocked here, before any thread exists, so every thread inherits
	// the mask: an encoder or PipeWire thread with it unblocked would take
	// the default action and kill wraith outright.
	sigset_t usr1;
	sigemptyset(&usr1);
	sigaddset(&usr1, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &usr1, nullptr);

	wraith::Log::init(wraith::LogLevel::Debug);

	const char *session_type_arg = nullptr;
	const char *datadir_override = "";
	const char *encode_path = nullptr;
	unsigned width = 1920, height = 1080;
	uint32_t bitrate_bps = 0; // 0: wraith.toml's encode.max_bitrate_mbps

	uint16_t gdp_port = 0;
	const char *gdp_token = nullptr;
	const char *gdp_cert = nullptr;
	const char *gdp_key = nullptr;
	const char *control_socket = nullptr;
	bool force_software_encoder = false;
	const char *config_path = wraith::kDefaultConfigPath;

	int c;
	while ((c = getopt(argc, argv, "o:e:b:l:t:c:k:G:d:p:C:Fh")) != -1) {
		switch (c) {
		case 'p': session_type_arg = optarg; break;
		case 'd': datadir_override = optarg; break;
		case 'o': {
			unsigned w, h;
			if (sscanf(optarg, "%ux%u", &w, &h) != 2) {
				WLOG_ERROR("invalid -o value %s, expected WIDTHxHEIGHT", optarg);
				return 1;
			}
			width = w;
			height = h;
			break;
		}
		case 'e': encode_path = optarg; break;
		case 'b': bitrate_bps = (uint32_t)strtoul(optarg, nullptr, 10); break;
		case 'l': gdp_port = (uint16_t)strtoul(optarg, nullptr, 10); break;
		case 't': gdp_token = optarg; break;
		case 'c': gdp_cert = optarg; break;
		case 'k': gdp_key = optarg; break;
		case 'G': control_socket = optarg; break;
		case 'F': force_software_encoder = true; break;
		case 'C': config_path = optarg; break;
		default: usage(argv[0]); return c == 'h' ? 0 : 1;
		}
	}

	// Before anything it configures is built. A file wraith can't use
	// doesn't stop the session -- a typo there shouldn't lock a user out
	// of their desktop -- but it is ignored whole and said so loudly;
	// `wraith --check-config` finds these before a login does.
	{
		wraith::ConfigLoad load = wraith::load_config(config_path);
		wraith::Log::init(wraith::config().log.level);
		for (const std::string &warning : load.warnings) {
			WLOG_ERROR("config %s: %s", config_path, warning.c_str());
		}
		if (!load.ok) {
			WLOG_ERROR("config %s: %s -- ignoring the file, every setting at its default", config_path,
				load.error.c_str());
		} else if (load.missing) {
			WLOG_INFO("config %s: not found, every setting at its default", config_path);
		} else {
			WLOG_INFO("config %s: loaded", config_path);
		}
	}

	if (gdp_port != 0 && (!gdp_token || !gdp_cert || !gdp_key)) {
		WLOG_ERROR("-l requires -t, -c, and -k");
		return 1;
	}
	if (control_socket && gdp_port != 0) {
		WLOG_ERROR("-G and -l are mutually exclusive (direct-connect vs. ghostd-mediated)");
		return 1;
	}
	if (control_socket && (gdp_cert || gdp_key)) {
		// The host's identity key is ghostd's alone; a session's
		// certificate is always its own throwaway one (SessionCert).
		WLOG_ERROR("-c/-k are for -l only -- -G sessions generate their own certificate");
		return 1;
	}

	// ghostd-mediated mode needs SessionInit (for its session_secret and
	// port range) before the session type is known, unless -p overrides
	// it -- so this happens before the host is initialised.
	wraith::SeatClient seat_client;
	wraith::SeatSessionInit seat_init;
	std::string session_type;
	if (control_socket) {
		std::string error;
		if (!seat_client.connect_and_read_init(control_socket, &seat_init, &error)) {
			WLOG_ERROR("ghostd: %s", error.c_str());
			return 1;
		}
		session_type = seat_init.session_type;
	}
	if (session_type_arg) {
		session_type = session_type_arg;
	}

	if (session_type.empty()) {
		WLOG_ERROR("no session type: pass -p, or -G for ghostd to name one");
		return 1;
	}

	wraith::SessionProfile profile;
	{
		std::vector<std::string> dirs =
			wraith::default_session_dirs(datadir_override, seat_init.sessions_dir);
		std::string error;
		// Strict only under ghostd: the directory list came over the
		// control socket, and the profile's Exec runs as this user. A
		// developer's -p/-d run is their own.
		if (!wraith::resolve_session_profile(session_type, dirs, &profile, &error,
				control_socket != nullptr)) {
			WLOG_ERROR("session type %s: %s", session_type.c_str(), error.c_str());
			if (control_socket) {
				seat_client.send_error("unknown session type " + session_type);
			}
			return 1;
		}
	}

	// Backend=screencast-<compositor> (docs/design/capture-backends.md):
	// a real, top-level compositor runs on its own virtual output and
	// wraith is purely a client of that session (ScreencastHost), not a
	// host for it. Which compositor decides the RemoteSession; the host
	// is the same for all of them. Torn down by its destructor on every
	// exit path below, including the early returns.
	std::unique_ptr<wraith::RemoteSession> remote_session;
	std::string which = profile.screencast_compositor();
	if (which == "gnome") {
		remote_session = std::make_unique<wraith::GnomeRemoteSession>();
	} else if (which == "kwin") {
		remote_session = std::make_unique<wraith::KwinRemoteSession>();
	} else if (which == "ext") {
		remote_session = std::make_unique<wraith::ExtRemoteSession>();
	} else {
		WLOG_ERROR("unknown Backend=%s (expected screencast-gnome, screencast-kwin or screencast-ext)",
			profile.backend.c_str());
		if (control_socket) {
			seat_client.send_error("unknown backend for session type " + session_type);
		}
		return 1;
	}

	auto screencast_host = std::make_unique<wraith::ScreencastHost>();
	{
		wraith::ScreencastHost::Options options;
		options.width = width;
		options.height = height;
		options.remote_session = std::move(remote_session);
		if (!screencast_host->init(std::move(options))) {
			WLOG_ERROR("failed to initialize wraith");
			return 1;
		}
	}
	wraith::SessionHost *host = screencast_host.get();

	wraith::SessionServices &session = host->session();
	if (bitrate_bps == 0) {
		bitrate_bps = wraith::config().encode.max_bitrate_mbps * 1'000'000;
	}
	session.set_encode_bitrate_bps(bitrate_bps);
	session.set_encode_gop_size(wraith::config().encode.gop);
	session.set_force_software_encoder(force_software_encoder || wraith::config().encode.force_software);

	if (encode_path) {
		if (!session.start_encoder(encode_path)) {
			WLOG_ERROR("failed to start encoder");
			return 1;
		}
	}

	if (gdp_port != 0) {
		std::string static_token = gdp_token;
		// Constant-time compare (gdp-spec §6.11).
		auto validator = [static_token](const std::string &token) {
			return token.size() == static_token.size() &&
				CRYPTO_memcmp(token.data(), static_token.data(), token.size()) == 0;
		};
		if (!session.start_gdp_session(gdp_port, validator, gdp_cert, gdp_key)) {
			WLOG_ERROR("failed to start gdp session");
			return 1;
		}
	}

	uint16_t bound_port = 0;
	wraith::SessionCert session_cert;
	if (control_socket) {
		std::string cert_error;
		if (!session_cert.generate(&cert_error)) {
			WLOG_ERROR("session certificate: %s", cert_error.c_str());
			seat_client.send_error("failed to generate the session certificate");
			return 1;
		}
		gdp_cert = session_cert.cert_path().c_str();
		gdp_key = session_cert.key_path().c_str();

		// Shared, so the spent tokens outlive the validator copies the
		// port loop below makes.
		auto token_gate = std::make_shared<wraith::RedirectTokenGate>(seat_init.session_secret,
			static_cast<uint32_t>(getuid()));
		auto validator = [token_gate](
							 const std::string &token) { return token_gate->accept(token, time(nullptr)); };

		// Gamepads are offered only if ghostd says virtual input devices
		// work here.
		if (!seat_init.uinput_available) {
			WLOG_INFO("gamepad: ghostd reports no uinput support on this host -- standard gamepads disabled");
		}
		if (!seat_init.uhid_available) {
			WLOG_INFO("hid: ghostd reports no uhid support on this host -- raw controllers disabled");
		}
		// First free port in ghostd's range.
		// Only "already in use" moves on to the next one: anything else
		// (the encoder, the certificate) would fail on every port alike.
		for (uint32_t port = seat_init.port_range_start; port <= seat_init.port_range_end; ++port) {
			bool address_in_use = false;
			if (session.start_gdp_session((uint16_t)port, validator, gdp_cert, gdp_key, control_socket,
					seat_init.uinput_available, seat_init.uhid_available, &address_in_use)) {
				bound_port = (uint16_t)port;
				break;
			}
			if (!address_in_use) {
				WLOG_ERROR("failed to start gdp session");
				seat_client.send_error("failed to start the GDP session");
				return 1;
			}
		}
		if (bound_port == 0) {
			WLOG_ERROR("ghostd: every GDP port in [%u, %u] is in use", seat_init.port_range_start,
				seat_init.port_range_end);
			seat_client.send_error("every GDP port in the given range is in use", gdp::ErrorCode::kHostFull);
			return 1;
		}
	}

	wraith::LaunchSpec spec;
	spec.command = profile.exec;
	spec.logout_command = profile.logout_exec;
	spec.env = profile.environment;
	spec.env.emplace_back("GHOST_SESSION_TYPE", session_type);

	// Everything that can still fail -- forking the session leader, the
	// capture and input bring-up -- happens in start(), so ghostd hears
	// "ready" only once all of it has succeeded.
	if (!host->start(&spec)) {
		WLOG_ERROR("failed to start wraith");
		if (control_socket) {
			seat_client.send_error("failed to start the session host");
		}
		return 1;
	}

	if (control_socket) {
		std::string error;
		if (!seat_client.send_ready(bound_port, session_cert.fingerprint(), &error)) {
			WLOG_ERROR("ghostd: failed to report readiness: %s", error.c_str());
			return 1;
		}
		WLOG_INFO("ghostd: session ready on port %u (certificate %s)", bound_port,
			session_cert.fingerprint().c_str());
	}

	// `wraith --report` from inside the desktop reaches this session here.
	// Declared after the host, so it goes first on every exit.
	wraith::ReportServer report_server;
	report_server.start(host->event_loop(), session.gdp_session());

	host->run();
	return 0;
}
