// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Stream integrity under loss: a stream's send buffer is ours to keep until
// the peer acks it (src/transport/quic_conn.cpp), and freeing any of it
// early would corrupt the control stream silently, only under loss. So a
// client and a server talk through a UDP relay, in-process, that drops,
// reorders and duplicates packets in both directions, and each side pushes
// 8 MiB of seeded pseudo-random bytes at the other in random-sized sends,
// with datagrams interleaved. Every received byte is checked against the
// same sequence.
#include "gdp/transport.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <arpa/inet.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TempCert {
	std::string cert_path = "/tmp/gdp_stream_loss_cert.pem";
	std::string key_path = "/tmp/gdp_stream_loss_key.pem";

	TempCert() {
		std::string cmd = "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -keyout " +
			key_path + " -out " + cert_path + " -days 1 -nodes -subj /CN=localhost >/dev/null 2>&1";
		if (system(cmd.c_str()) != 0) {
			fprintf(stderr, "stream_loss_test: openssl cert generation failed\n");
			exit(1);
		}
	}
	~TempCert() {
		unlink(cert_path.c_str());
		unlink(key_path.c_str());
	}
};

// xorshift64*: the byte sequence both ends generate, and the relay's dice.
struct Rng {
	uint64_t s;
	explicit Rng(uint64_t seed) : s(seed) {}
	uint64_t next() {
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return s * 2685821657736338717ull;
	}
	uint8_t byte() { return static_cast<uint8_t>(next() >> 56); }
};

// Checks received bytes against the sequence seeded with `seed`.
struct Verifier {
	Rng rng;
	uint64_t received = 0;
	bool corrupt = false;
	explicit Verifier(uint64_t seed) : rng(seed) {}
	void feed(const uint8_t *data, size_t len) {
		for (size_t i = 0; i < len; i++) {
			if (data[i] != rng.byte()) {
				if (!corrupt) {
					fprintf(stderr, "stream_loss_test: byte %llu is wrong\n",
						(unsigned long long)(received + i));
				}
				corrupt = true;
			}
		}
		received += len;
	}
};

// Sends `total` bytes of the `seed` sequence in random-sized pieces.
void send_sequence(gdp::Stream *stream, uint64_t seed, size_t total, Rng &sizes) {
	Rng rng(seed);
	std::vector<uint8_t> chunk;
	size_t sent = 0;
	while (sent < total) {
		size_t len = std::min<size_t>(total - sent, 1 + sizes.next() % (64 * 1024));
		chunk.resize(len);
		for (auto &b : chunk) {
			b = rng.byte();
		}
		stream->send(chunk.data(), len);
		sent += len;
	}
}

sockaddr_in loopback(uint16_t port) {
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return sa;
}

// A lossy UDP relay: the client talks to `port`, the relay to the server.
// Each packet, either way: 5% dropped, 5% held back and sent after the
// next one (reordered), 1% sent twice.
class Relay {
public:
	Relay(uint16_t port, uint16_t server_port) {
		front_ = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in sa = loopback(port);
		assert(bind(front_, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) == 0);
		back_ = socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in server = loopback(server_port);
		assert(connect(back_, reinterpret_cast<sockaddr *>(&server), sizeof(server)) == 0);
		thread_ = std::thread([this] { run(); });
	}
	~Relay() {
		stop_ = true;
		thread_.join();
		close(front_);
		close(back_);
	}

	std::atomic<uint64_t> dropped{0};
	std::atomic<uint64_t> reordered{0};

private:
	struct Direction {
		std::vector<uint8_t> held;
		bool holding = false;
	};

	void forward(bool to_server, const uint8_t *data, size_t len) {
		auto out = [&](const uint8_t *d, size_t n) {
			if (to_server) {
				send(back_, d, n, 0);
			} else if (have_client_) {
				sendto(front_, d, n, 0, reinterpret_cast<sockaddr *>(&client_), sizeof(client_));
			}
		};
		Direction &dir = to_server ? up_ : down_;
		uint64_t roll = rng_.next() % 100;
		if (roll < 5) {
			dropped++;
			return;
		}
		if (roll < 10 && !dir.holding) {
			dir.held.assign(data, data + len);
			dir.holding = true;
			reordered++;
			return;
		}
		out(data, len);
		if (roll < 11) {
			out(data, len);
		}
		if (dir.holding) {
			out(dir.held.data(), dir.held.size());
			dir.holding = false;
		}
	}

	void run() {
		uint8_t buf[2048];
		while (!stop_) {
			pollfd fds[2] = {{front_, POLLIN, 0}, {back_, POLLIN, 0}};
			if (poll(fds, 2, 20) <= 0) {
				continue;
			}
			if (fds[0].revents & POLLIN) {
				socklen_t len = sizeof(client_);
				ssize_t n =
					recvfrom(front_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&client_), &len);
				if (n > 0) {
					have_client_ = true;
					forward(true, buf, (size_t)n);
				}
			}
			if (fds[1].revents & POLLIN) {
				ssize_t n = recv(back_, buf, sizeof(buf), 0);
				if (n > 0) {
					forward(false, buf, (size_t)n);
				}
			}
		}
	}

	int front_ = -1;
	int back_ = -1;
	sockaddr_in client_{};
	bool have_client_ = false;
	Direction up_;
	Direction down_;
	Rng rng_{0x5eed};
	std::atomic<bool> stop_{false};
	std::thread thread_;
};

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
		poll(fds, 2, 20);
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
	constexpr uint16_t kServerPort = 44341;
	constexpr uint16_t kRelayPort = 44342;
	constexpr size_t kBytes = 8 << 20;
	constexpr uint64_t kClientSeed = 0xc11e47;
	constexpr uint64_t kServerSeed = 0x5e7e7;
	const char *alpn = "gdp-test/1";

	gdp::Transport server_transport;
	gdp::Transport client_transport;
	Relay relay(kRelayPort, kServerPort);

	std::unique_ptr<gdp::Connection> server_conn;
	Verifier server_got(kClientSeed);
	Verifier client_got(kServerSeed);
	Rng sizes(42);
	int server_datagrams = 0;

	bool listening = server_transport.listen(kServerPort, alpn, cert.cert_path, cert.key_path,
		[&](std::unique_ptr<gdp::Connection> conn) {
			server_conn = std::move(conn);
			server_conn->on_control_stream = [&](gdp::Stream *s) {
				s->on_data = [&](const uint8_t *data, size_t len) { server_got.feed(data, len); };
				send_sequence(s, kServerSeed, kBytes, sizes);
			};
			server_conn->on_datagram = [&](const uint8_t *, size_t, uint64_t) { server_datagrams++; };
		});
	assert(listening);

	auto client_conn = client_transport.connect("127.0.0.1", kRelayPort, alpn);
	assert(client_conn);
	bool connected = false;
	client_conn->on_state_changed = [&](gdp::ConnectionState state) {
		connected |= state == gdp::ConnectionState::kConnected;
	};
	assert(pump_until(client_transport, server_transport, 10000, [&] { return connected; }) &&
		"never connected through the relay");

	gdp::Stream *control = client_conn->open_control_stream();
	assert(control);
	control->on_data = [&](const uint8_t *data, size_t len) { client_got.feed(data, len); };
	send_sequence(control, kClientSeed, kBytes, sizes);

	// Datagrams alongside: lost ones stay lost, but they share packets and
	// congestion control with the stream.
	uint8_t dgram[1000];
	memset(dgram, 0xab, sizeof(dgram));
	int sent_datagrams = 0;
	bool ok = pump_until(client_transport, server_transport, 60000, [&] {
		if (sent_datagrams < 2000 && client_conn->send_datagram(dgram, sizeof(dgram))) {
			sent_datagrams++;
		}
		return (server_got.received >= kBytes && client_got.received >= kBytes) || server_got.corrupt ||
			client_got.corrupt;
	});
	fprintf(stderr,
		"stream_loss_test: relay dropped %llu, reordered %llu; server got %llu, client %llu bytes; %d/%d datagrams\n",
		(unsigned long long)relay.dropped, (unsigned long long)relay.reordered,
		(unsigned long long)server_got.received, (unsigned long long)client_got.received, server_datagrams,
		sent_datagrams);
	assert(!server_got.corrupt && !client_got.corrupt && "stream data corrupted under loss");
	assert(ok && "streams never delivered everything through the lossy relay");
	assert(server_got.received == kBytes && client_got.received == kBytes);
	assert(relay.dropped > 100 && "the relay should have dropped plenty");
	assert(server_datagrams > 0);

	printf("stream_loss_test: 8 MiB each way arrived intact through 5%% loss, reordering and duplication\n");
	return 0;
}
