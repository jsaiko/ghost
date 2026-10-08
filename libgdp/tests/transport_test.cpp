// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Loopback client/server test: the one thing worth genuinely validating
// end-to-end, since transport.hpp's contract (streams, datagrams, the
// dispatch() bridge) can't be meaningfully unit-tested piecewise the way
// datagram_test.cpp/framing_test.cpp are.
#include "gdp/cert_fingerprint.hpp"
#include "gdp/clock.hpp"
#include "gdp/error_codes.hpp"
#include "gdp/transport.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

// Generates an ephemeral self-signed cert+key for this test run only, via
// the openssl CLI -- the simplest option for a test-only need.
struct TempCert {
	std::string cert_path = "/tmp/gdp_test_cert.pem";
	std::string key_path = "/tmp/gdp_test_key.pem";

	TempCert() {
		std::string cmd = "openssl req -x509 -newkey rsa:2048 -keyout " + key_path + " -out " + cert_path +
			" -days 1 -nodes -subj /CN=localhost >/tmp/gdp_test_openssl.log 2>&1";
		int rc = system(cmd.c_str());
		if (rc != 0) {
			fprintf(stderr,
				"transport_test: openssl cert generation failed (rc=%d), see /tmp/gdp_test_openssl.log\n",
				rc);
			exit(1);
		}
	}
	~TempCert() {
		unlink(cert_path.c_str());
		unlink(key_path.c_str());
	}
};

// Pumps both transports' dispatch() until `done()` returns true or
// `timeout_ms` elapses. Returns true if `done()` became true.
bool pump_until(gdp::Transport &a, gdp::Transport &b, int timeout_ms, const std::function<bool()> &done) {
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (done()) {
			return true;
		}
		struct pollfd fds[2] = {
			{a.notify_fd(), POLLIN, 0},
			{b.notify_fd(), POLLIN, 0},
		};
		poll(fds, 2,
			20); // short poll interval: the network thread's timers can produce events with no fd activity
		a.dispatch();
		b.dispatch();

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		int64_t elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1'000'000;
		if (elapsed_ms > timeout_ms) {
			return false;
		}
	}
}

} // namespace

int main() {
	TempCert cert;

	gdp::Transport server_transport;
	gdp::Transport client_transport;

	const uint16_t port = 44339; // fixed test port; nothing else on this host should be using it
	const char *alpn = "gdp-test/1";

	std::unique_ptr<gdp::Connection> server_conn;
	bool server_got_control = false;
	bool server_got_input = false;
	std::string server_received_control_msg;
	std::vector<uint8_t> server_received_datagram;

	bool listen_ok = server_transport.listen(port, alpn, cert.cert_path, cert.key_path,
		[&](std::unique_ptr<gdp::Connection> conn) {
			server_conn = std::move(conn);
			server_conn->on_control_stream = [&](gdp::Stream *s) {
				server_got_control = true;
				// Capture `s` by value: it's a Stream* parameter of this
				// lambda invocation, gone once it returns, but on_data
				// fires later from a separate dispatch() call -- a `[&]`
				// capture here would dangle.
				s->on_data = [&, s](const uint8_t *data, size_t len) {
					server_received_control_msg.assign(reinterpret_cast<const char *>(data), len);
					s->send(data, len); // echo back
				};
			};
			server_conn->on_input_stream = [&](gdp::Stream *) { server_got_input = true; };
			server_conn->on_datagram = [&](const uint8_t *data, size_t len, uint64_t) {
				server_received_datagram.assign(data, data + len);
			};
		});
	assert(listen_ok);

	auto client_conn = client_transport.connect("127.0.0.1", port, alpn);
	assert(client_conn);

	bool client_connected = false;
	client_conn->on_state_changed = [&](gdp::ConnectionState state) {
		if (state == gdp::ConnectionState::kConnected) {
			client_connected = true;
		}
	};

	bool ok = pump_until(client_transport, server_transport, 5000, [&] { return client_connected; });
	assert(ok && "client never reached kConnected");

	// The pinning input (gdp-spec.md §2.3): the
	// server's DER certificate hash, as `openssl x509 -fingerprint`
	// computes it independently, must already be there at kConnected.
	{
		std::string cmd = "openssl x509 -in " + cert.cert_path + " -noout -fingerprint -sha256";
		FILE *p = popen(cmd.c_str(), "r");
		assert(p);
		char line[256] = {};
		assert(fgets(line, sizeof(line), p));
		pclose(p);
		std::string expected;
		for (const char *c = strchr(line, '=') + 1; *c && *c != '\n'; c++) {
			if (*c != ':') {
				expected += (char)tolower((unsigned char)*c);
			}
		}
		assert(gdp::is_sha256_hex(expected));
		assert(client_conn->peer_certificate_sha256() == expected && "peer certificate fingerprint mismatch");
	}

	// Input first would take stream 0, which the host reads as control.
	assert(!client_conn->open_input_stream());
	gdp::Stream *control = client_conn->open_control_stream();
	assert(control);
	gdp::Stream *input = client_conn->open_input_stream();
	assert(input);

	ok = pump_until(client_transport, server_transport, 5000,
		[&] { return server_got_control && server_got_input; });
	assert(ok && "server never saw both streams (or misidentified control vs input)");

	// Control stream: send a message, expect the echo back.
	const char *msg = "hello over control stream";
	std::string client_received_echo;
	control->on_data = [&](const uint8_t *data, size_t len) {
		client_received_echo.assign(reinterpret_cast<const char *>(data), len);
	};
	control->send(reinterpret_cast<const uint8_t *>(msg), strlen(msg));

	ok = pump_until(client_transport, server_transport, 5000, [&] { return !client_received_echo.empty(); });
	assert(ok && "never got the echo back");
	assert(server_received_control_msg == msg);
	assert(client_received_echo == msg);

	// Datagram.
	uint8_t dgram[] = {0x01, 0xde, 0xad, 0xbe, 0xef};
	bool sent = client_conn->send_datagram(dgram, sizeof(dgram));
	assert(sent);

	ok = pump_until(client_transport, server_transport, 5000,
		[&] { return !server_received_datagram.empty(); });
	assert(ok && "server never got the datagram");
	assert(server_received_datagram.size() == sizeof(dgram));
	assert(memcmp(server_received_datagram.data(), dgram, sizeof(dgram)) == 0);

	// Path MTU discovery: on loopback the transport probes its way up to
	// its 1500-byte ceiling, and max_datagram_size() follows. The server
	// side is the one that matters (wraith sends the video), but both are
	// checked. Datagrams from the minimum MTU fit in ~1150 bytes; 1400 is
	// only reachable once discovery has raised it.
	ok = pump_until(client_transport, server_transport, 5000,
		[&] { return client_conn->max_datagram_size() >= 1400 && server_conn->max_datagram_size() >= 1400; });
	assert(ok && "max_datagram_size() never grew past the minimum MTU");
	assert(server_conn->path_mtu() > server_conn->max_datagram_size());

	// Server -> client datagrams at exactly the limit go through; one byte
	// more is refused up front, without being queued.
	std::vector<uint8_t> client_received_datagram;
	int client_datagrams = 0;
	client_conn->on_datagram = [&](const uint8_t *data, size_t len, uint64_t) {
		client_received_datagram.assign(data, data + len);
		client_datagrams++;
	};
	uint16_t limit = server_conn->max_datagram_size();
	std::vector<uint8_t> big(limit + 1, 0xab); // one byte more than the limit
	gdp::Connection::DatagramStats before = server_conn->datagram_stats();
	assert(!server_conn->send_datagram(big.data(), big.size()));
	assert(server_conn->datagram_stats().queued_datagrams == before.queued_datagrams);
	constexpr int kBurst = 20;
	for (int i = 0; i < kBurst; i++) {
		assert(server_conn->send_datagram(big.data(), limit, /*priority=*/i == kBurst - 1));
	}
	ok = pump_until(client_transport, server_transport, 5000, [&] { return client_datagrams == kBurst; });
	assert(ok && "client never got the full-size datagrams");
	assert(client_received_datagram.size() == limit);

	// datagram_stats(): everything sent is eventually acked, nothing is left
	// queued, and the byte counts add up.
	ok = pump_until(client_transport, server_transport, 5000, [&] {
		gdp::Connection::DatagramStats st = server_conn->datagram_stats();
		return st.acked_datagrams - before.acked_datagrams == (uint64_t)kBurst;
	});
	assert(ok && "server never saw its datagrams acked");
	gdp::Connection::DatagramStats after = server_conn->datagram_stats();
	assert(after.queued_datagrams == 0 && after.queued_bytes == 0 && after.oldest_queued_age_us == 0);
	assert(after.sent_datagrams - before.sent_datagrams == (uint64_t)kBurst);
	assert(after.sent_bytes - before.sent_bytes == (uint64_t)kBurst * limit);
	assert(after.acked_bytes - before.acked_bytes == (uint64_t)kBurst * limit);
	assert(after.lost_datagrams == before.lost_datagrams);

	// Pacing: past the first burst's worth, datagrams wait their turn at
	// the set rate -- counted as queued meanwhile -- and still arrive, all
	// of them, in send order.
	constexpr uint64_t kPaceBytesPerS = 10'000'000;
	constexpr int kPaced = 200;
	std::vector<uint32_t> paced_order;
	client_conn->on_datagram = [&](const uint8_t *data, size_t len, uint64_t) {
		uint32_t index;
		assert(len >= sizeof(index));
		memcpy(&index, data, sizeof(index));
		paced_order.push_back(index);
	};
	server_conn->set_pacing_rate(kPaceBytesPerS);
	uint64_t paced_start = gdp::monotonic_us();
	for (uint32_t i = 0; i < (uint32_t)kPaced; i++) {
		memcpy(big.data(), &i, sizeof(i));
		assert(server_conn->send_datagram(big.data(), limit));
	}
	assert(server_conn->datagram_stats().queued_datagrams > 0);
	ok = pump_until(client_transport, server_transport, 5000,
		[&] { return server_conn->datagram_stats().queued_datagrams == 0; });
	assert(ok && "paced datagrams never all left");
	uint64_t paced_us = gdp::monotonic_us() - paced_start;
	uint64_t expected_us = ((uint64_t)kPaced * limit - gdp::Connection::pacing_burst_bytes(kPaceBytesPerS)) *
		1'000'000 / kPaceBytesPerS;
	assert(paced_us >= expected_us * 9 / 10 && "pacing let datagrams out faster than its rate");
	ok = pump_until(client_transport, server_transport, 5000,
		[&] { return paced_order.size() == (size_t)kPaced; });
	assert(ok && "client never got every paced datagram");
	for (uint32_t i = 0; i < (uint32_t)kPaced; i++) {
		assert(paced_order[i] == i && "paced datagrams arrived out of order");
	}
	// Some still held when the connection goes, below: dropped with it.
	for (int i = 0; i < kPaced; i++) {
		server_conn->send_datagram(big.data(), limit);
	}

	// Close with a gdp-spec.md §12 application error code: the peer must see
	// the code itself and a description of it in shutdown_reason, both set
	// by the time kShutdown fires.
	bool client_shutdown = false;
	uint64_t client_saw_code = 0;
	std::string client_saw_reason;
	client_conn->on_state_changed = [&](gdp::ConnectionState state) {
		if (state == gdp::ConnectionState::kShutdown) {
			client_shutdown = true;
			client_saw_code = client_conn->shutdown_error_code();
			client_saw_reason = client_conn->shutdown_reason();
		}
	};
	// Destroyed straight after close(), as every real caller does: the
	// destructor's own shutdown must not overtake close()'s and put 0 on
	// the wire instead of the chosen code.
	server_conn->close(static_cast<uint64_t>(gdp::ErrorCode::kAuthFailed));
	server_conn.reset();

	ok = pump_until(client_transport, server_transport, 5000, [&] { return client_shutdown; });
	assert(ok && "client never saw the server's close");
	assert(client_saw_code == static_cast<uint64_t>(gdp::ErrorCode::kAuthFailed));
	assert(client_saw_reason == "closed by peer: authentication failed");

	printf("transport_test: all checks passed (control stream round-trip + datagram delivered,\n"
		   "control/input streams correctly identified by QUIC stream ID, MTU discovery raised the\n"
		   "datagram limit, full-size datagrams counted through to acked, pacing held its rate in\n"
		   "order, close code surfaced to peer)\n");
	return 0;
}
