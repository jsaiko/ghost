// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// SessionCert end to end: the throwaway certificate a -G wraith generates
// must load in libgdp's listener from its /proc/self/fd paths, and a client must see
// exactly the fingerprint wraith reports to ghostd in SessionReady --
// that's the value spectre gets as -P (gdp-spec.md §2.3), so any disagreement would make every session
// unverifiable.
#include "session/session_cert.hpp"

#include "gdp/cert_fingerprint.hpp"
#include "gdp/transport.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <ctime>
#include <poll.h>

int main() {
	wraith::SessionCert cert;
	std::string error;
	bool generated = cert.generate(&error);
	if (!generated) {
		fprintf(stderr, "session_cert_test: %s\n", error.c_str());
	}
	assert(generated);
	assert(gdp::is_sha256_hex(cert.fingerprint()));

	// A second one must be a different certificate: each session's own.
	wraith::SessionCert other;
	assert(other.generate(&error));
	assert(other.fingerprint() != cert.fingerprint());

	gdp::Transport server;
	gdp::Transport client;
	const uint16_t port = 44345; // fixed test port; libgdp's tests hold 44339, 44341-44342 and 44360
	std::unique_ptr<gdp::Connection> accepted;
	bool listening = server.listen(port, "gdp-test/1", cert.cert_path(), cert.key_path(),
		[&](std::unique_ptr<gdp::Connection> conn) { accepted = std::move(conn); });
	assert(listening && "the listener couldn't load the memfd-backed certificate");

	auto conn = client.connect("127.0.0.1", port, "gdp-test/1");
	assert(conn);
	bool connected = false;
	conn->on_state_changed = [&](gdp::ConnectionState state) {
		connected = connected || state == gdp::ConnectionState::kConnected;
	};
	time_t deadline = time(nullptr) + 5;
	while (!connected && time(nullptr) < deadline) {
		pollfd fds[2] = {{server.notify_fd(), POLLIN, 0}, {client.notify_fd(), POLLIN, 0}};
		poll(fds, 2, 20);
		server.dispatch();
		client.dispatch();
	}
	assert(connected && "client never connected");
	assert(conn->peer_certificate_sha256() == cert.fingerprint());

	printf("session_cert_test: OK (%s)\n", cert.fingerprint().c_str());
	return 0;
}
