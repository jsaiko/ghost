// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// CaTrust end to end (gdp-spec.md §2.3): a server certificate is always
// accepted by the handshake, and Connection::peer_certificate_ca_verified()
// says whether it also chains to the client's CAs and is valid for the
// name dialed. Certificates come from the openssl CLI, as in
// transport_test.cpp.
#include "gdp/transport.hpp"

// Plain assert()s: a Release build (-DNDEBUG) must not compile them away.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace {

const std::string kDir = "/tmp/gdp_ca_trust_test";
// LibreSSL (macOS's openssl CLI) otherwise writes explicit curve
// parameters, and signs with SHA-1 without -sha256: OpenSSL 3 refuses
// both.
const std::string kNamedCurve = " -pkeyopt ec_param_enc:named_curve";

void run(const std::string &cmd) {
	std::string full = cmd + " >>" + kDir + "/openssl.log 2>&1";
	if (system(full.c_str()) != 0) {
		fprintf(stderr, "ca_trust_test: `%s` failed, see %s/openssl.log\n", cmd.c_str(), kDir.c_str());
		exit(1);
	}
}

// A private CA, and leaves it signed: one valid for localhost and
// 127.0.0.1, one for another name only. Plus a self-signed leaf.
struct Certs {
	Certs() {
		if (system(("rm -rf " + kDir + " && mkdir -p " + kDir).c_str()) != 0) {
			fprintf(stderr, "ca_trust_test: can't create %s\n", kDir.c_str());
			exit(1);
		}
		run("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256" + kNamedCurve +
			" -nodes -days 1 -subj /CN=gdp-test-ca"
			" -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign"
			" -keyout " +
			kDir + "/ca.key -out " + kDir + "/ca.pem");
		leaf("good", "DNS:localhost,IP:127.0.0.1");
		leaf("other", "DNS:other.test");
		run("openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256" + kNamedCurve +
			" -nodes -days 1 -subj /CN=localhost"
			" -addext subjectAltName=DNS:localhost,IP:127.0.0.1"
			" -keyout " +
			kDir + "/self.key -out " + kDir + "/self.pem");
	}
	~Certs() { (void)!system(("rm -rf " + kDir).c_str()); }

	static void leaf(const std::string &name, const std::string &san) {
		std::string base = kDir + "/" + name;
		run("openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256" + kNamedCurve +
			" -nodes -subj /CN=" + name + " -keyout " + base + ".key -out " + base + ".csr");
		FILE *ext = fopen((base + ".ext").c_str(), "w");
		assert(ext);
		fprintf(ext, "subjectAltName=%s\n", san.c_str());
		fclose(ext);
		run("openssl x509 -req -in " + base + ".csr -CA " + kDir + "/ca.pem -CAkey " + kDir +
			"/ca.key -CAcreateserial -sha256 -days 1 -extfile " + base + ".ext -out " + base + ".pem");
	}
};

bool pump_until(gdp::Transport &a, gdp::Transport &b, int timeout_ms, const std::function<bool()> &done) {
	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (done()) {
			return true;
		}
		struct pollfd fds[2] = {{a.notify_fd(), POLLIN, 0}, {b.notify_fd(), POLLIN, 0}};
		poll(fds, 2, 20);
		a.dispatch();
		b.dispatch();
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1'000'000 > timeout_ms) {
			return false;
		}
	}
}

// Connects to a fresh server presenting `cert` and returns whether the
// client saw it as CA-verified. The handshake must complete either way.
bool verified(uint16_t port, const std::string &cert, const std::string &dial, const gdp::CaTrust &ca) {
	const char *alpn = "gdp-test/1";
	gdp::Transport server;
	gdp::Transport client;
	std::unique_ptr<gdp::Connection> accepted;
	bool listening = server.listen(port, alpn, kDir + "/" + cert + ".pem", kDir + "/" + cert + ".key",
		[&](std::unique_ptr<gdp::Connection> conn) { accepted = std::move(conn); });
	assert(listening);
	auto conn = client.connect(dial, port, alpn, ca);
	bool connected = false;
	conn->on_state_changed = [&](gdp::ConnectionState state) {
		connected = connected || state == gdp::ConnectionState::kConnected;
	};
	bool ok = pump_until(client, server, 5000, [&] { return connected; });
	assert(ok && "the handshake must complete whatever the certificate");
	assert(!conn->peer_certificate_sha256().empty());
	return conn->peer_certificate_ca_verified();
}

} // namespace

int main() {
	Certs certs;
	gdp::CaTrust ours;
	ours.extra_file = kDir + "/ca.pem";
	gdp::CaTrust system_only;
	system_only.system = true;
	gdp::CaTrust unreadable;
	unreadable.extra_file = kDir + "/missing.pem";

	uint16_t port = 44360;
	// Issued by a trusted CA for the name dialed: an address or a DNS name.
	assert(verified(port++, "good", "127.0.0.1", ours));
	assert(verified(port++, "good", "localhost", ours));
	// No CaTrust: never verified, however good the certificate.
	assert(!verified(port++, "good", "127.0.0.1", {}));
	// The CA isn't one the platform trusts.
	assert(!verified(port++, "good", "127.0.0.1", system_only));
	// A missing CA file is skipped, not fatal.
	assert(!verified(port++, "good", "127.0.0.1", unreadable));
	// Trusted issuer, wrong name.
	assert(!verified(port++, "other", "127.0.0.1", ours));
	assert(!verified(port++, "other", "localhost", ours));
	// Self-signed for the right name: what pinning is for, not a CA.
	assert(!verified(port++, "self", "127.0.0.1", ours));

	printf("ca_trust_test: OK\n");
	return 0;
}
