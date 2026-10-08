// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Cross-language wire-compatibility check for the lobby phase
// (gdp-spec.md §4): drives gdp::LobbyClient -- the same class
// spectre-qt's login form uses -- over libgdp's real QUIC transport
// (ngtcp2) against a *running* ghostd process (quinn + prost, Rust) and
// checks the full LobbyHello -> AuthChallenge -> AuthResponse -> LobbyError
// sequence round-trips correctly.
//
// Uses a nonexistent username so this needs no local account or PAM
// fixture to run anywhere: ghostd's real PAM authentication
// (the pamconv crate, host/pamconv) still completes the wire handshake (one
// challenge/response round for the password) and correctly fails it, so
// this exercises the exact same framing/sequencing a real login would
// without depending on what accounts happen to exist on the machine
// running the test. Getting past auth to exercise SessionList/SessionOpen/
// Redirect needs a real local account, a matching /etc/pam.d/ghostd
// (packaging/system/pam.d/ghostd), and a wraith.service for that account -- real
// environment setup this test intentionally avoids needing.
//
// Unlike transport_test.cpp this can't spin up its own server -- ghostd is
// a separate process (built by the top-level `ghostd` custom target via
// cargo, but not something this test can launch portably). Not registered
// as a ctest for that reason; run manually:
//
//   ./host/target/debug/ghostd --port 44310 &
//   ./build/bin/gdp_lobby_client_test 127.0.0.1 44310
//
// With --broker, it instead logs in through Veil for real
// (gdp-spec.md §5) -- a real account on Veil and on
// the joined host, so it does start or resume that user's session:
//
//   ./build/bin/gdp_lobby_client_test <veil> <port> --broker <user> <password> \
//       [--device <name>] [--expect redirect|<error code number>]
//
// It prints the DeviceList, picks --device (or the first online one),
// answers any prompt after the first with $LOBBY_TEST_OTP, opens the
// default session type, and checks the outcome against --expect
// (default: a Redirect).
#include "gdp/cert_fingerprint.hpp"
#include "gdp/error_codes.hpp"
#include "gdp/lobby_client.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <poll.h>
#include <string>

namespace {

bool pump_until(gdp::LobbyClient &client, int timeout_ms, const std::function<bool()> &done) {
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (done()) {
			return true;
		}
		struct pollfd pfd{client.notify_fd(), POLLIN, 0};
		poll(&pfd, 1, 20);
		client.dispatch();

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		int64_t elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1'000'000;
		if (elapsed_ms > timeout_ms) {
			return false;
		}
	}
}

// The --broker mode: one real login through Veil. Returns the exit code.
int broker_login(const std::string &host, uint16_t port, int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "--broker needs <user> <password>\n");
		return 1;
	}
	std::string user = argv[0];
	std::string password = argv[1];
	std::string device_name;
	std::string expect = "redirect";
	for (int i = 2; i + 1 < argc; i += 2) {
		std::string flag = argv[i];
		if (flag == "--device") {
			device_name = argv[i + 1];
		} else if (flag == "--expect") {
			expect = argv[i + 1];
		}
	}
	const char *otp = getenv("LOBBY_TEST_OTP");

	gdp::LobbyClient client;
	bool first_prompt = true;
	bool done = false;
	bool redirected = false;
	int device_lists = 0;
	client.on_certificate = [](const std::string &cert_sha256) { return gdp::is_sha256_hex(cert_sha256); };
	client.on_auth_prompt = [&](const std::string &prompt, bool echo) {
		printf("broker: AuthChallenge %s(prompt=\"%s\" echo=%d)\n", first_prompt ? "" : "after the first ",
			prompt.c_str(), echo);
		client.respond(first_prompt ? password : (otp ? otp : ""));
		first_prompt = false;
	};
	client.on_device_list = [&](const std::vector<gdp::DeviceInfo> &devices) {
		device_lists++;
		const gdp::DeviceInfo *pick = nullptr;
		for (const auto &d : devices) {
			printf("broker: device %s \"%s\" online=%d has_session=%d type=%s\n", d.id.c_str(),
				d.name.c_str(), d.online, d.has_session, d.session_type.c_str());
			if (!pick && (device_name.empty() ? d.online : d.name == device_name)) {
				pick = &d;
			}
		}
		// A --device that isn't listed goes out as-is, an id Veil must refuse.
		std::string id = pick ? pick->id : !device_name.empty() ? device_name : "no-such-device";
		printf("broker: selecting %s\n", id.c_str());
		client.select_device(id);
	};
	client.on_session_list = [&](const gdp::SessionListInfo &list) {
		printf("broker: SessionList: %zu types (default %s), %zu running\n", list.types.size(),
			list.default_type.c_str(), list.running.size());
		client.open_session("");
	};
	client.on_redirect = [&](const std::string &rhost, uint16_t rport, const std::string &token, int64_t,
							 const std::string &cert) {
		printf("broker: Redirect host=%s port=%u token=%zu bytes cert=%s\n", rhost.c_str(), rport,
			token.size(), cert.c_str());
		redirected = true;
		done = true;
	};
	client.on_error = [&](const std::string &message) {
		printf("broker: error %llu (%s): %s\n", (unsigned long long)client.error_code(),
			gdp::describe_error_code(client.error_code()).c_str(), message.c_str());
		done = true;
	};
	assert(client.connect(host, port, user));
	bool finished = pump_until(client, 60000, [&] { return done; });
	assert(finished && "the brokered login never finished");
	bool ok = expect == "redirect"
		? redirected
		: (!redirected && client.error_code() == strtoull(expect.c_str(), nullptr, 10));
	printf("broker: %s (expected %s, saw %d DeviceList)\n", ok ? "PASS" : "FAIL", expect.c_str(),
		device_lists);
	return ok ? 0 : 1;
}

} // namespace

int main(int argc, char *argv[]) {
	if (argc >= 4 && std::string(argv[3]) == "--broker") {
		return broker_login(argv[1], (uint16_t)strtoul(argv[2], nullptr, 10), argc - 4, argv + 4);
	}
	if (argc != 3) {
		fprintf(stderr, "usage: %s <ghostd-host> <ghostd-port>\n", argv[0]);
		return 1;
	}
	std::string host = argv[1];
	uint16_t port = (uint16_t)strtoul(argv[2], nullptr, 10);

	// An unpinned host: on_certificate refusing must end the login before
	// LobbyHello (and so the username) is sent -- ghostd never prompts.
	{
		gdp::LobbyClient refused;
		bool refused_errored = false;
		int refused_prompts = 0;
		refused.on_certificate = [](const std::string &) { return false; };
		refused.on_auth_prompt = [&](const std::string &, bool) { refused_prompts++; };
		refused.on_error = [&](const std::string &) { refused_errored = true; };
		assert(refused.connect(host, port, "lobby-client-test-no-such-user"));
		bool done = pump_until(refused, 10000, [&] { return refused_errored; });
		assert(done && "a refused certificate never ended the login");
		assert(refused.certificate_rejected());
		assert(refused_prompts == 0 && "LobbyHello went out to an untrusted host");
		printf("lobby_client_test: refusing the certificate ended the login before LobbyHello\n");
	}

	gdp::LobbyClient client;

	int prompts = 0;
	bool redirected = false;
	bool errored = false;
	std::string error_message;

	client.on_auth_prompt = [&](const std::string &prompt, bool echo) {
		prompts++;
		printf("lobby_client_test: got AuthChallenge (prompt=%s echo=%d)\n", prompt.c_str(), echo);
		// Wrong on purpose -- see the file header: this test only needs PAM
		// to *complete the handshake and fail it*, not succeed.
		client.respond("wrong-password");
	};
	client.on_redirect = [&](const std::string &, uint16_t, const std::string &, int64_t,
							 const std::string &) { redirected = true; };
	client.on_error = [&](const std::string &message) {
		errored = true;
		error_message = message;
	};

	// Pinning isn't what's under test: trust whatever certificate ghostd
	// presents, but insist there is one to see.
	client.on_certificate = [](const std::string &cert_sha256) { return gdp::is_sha256_hex(cert_sha256); };

	// Nonexistent on any real machine, so no local-account fixture needed.
	bool started = client.connect(host, port, "lobby-client-test-no-such-user");
	assert(started && "LobbyClient::connect() failed locally");

	bool ok = pump_until(client, 10000, [&] { return redirected || errored; });
	assert(ok && "lobby exchange never finished -- is ghostd running at that host:port?");

	assert(prompts >= 1 && "ghostd never sent an AuthChallenge");
	assert(!redirected && "a nonexistent user must not be redirected");
	assert(errored);
	// LobbyClient surfaces a LobbyError as its message only; the transport-
	// level failures it also folds into on_error use fixed local strings
	// (see LobbyClient::fail's callers), so a message that isn't one of
	// those means a real LobbyError frame came back from ghostd.
	assert(error_message != "malformed lobby frame" && error_message != "failed to open lobby stream" &&
		error_message != "the host's certificate was not accepted" &&
		error_message.rfind("connection closed before completing login", 0) != 0);
	printf(
		"lobby_client_test: got expected LobbyError (PAM auth correctly failed for a nonexistent user): %s\n",
		error_message.c_str());

	printf("lobby_client_test: all checks passed -- ghostd (Rust/quinn/prost, real PAM) and libgdp's\n"
		   "gdp::LobbyClient (C++/ngtcp2/protobuf) agree on the wire format end to end.\n");
	return 0;
}
