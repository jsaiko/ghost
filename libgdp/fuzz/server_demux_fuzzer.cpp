// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// libFuzzer target for the server side of the QUIC transport
// (src/transport/server_demux.cpp): everything a stranger's packet reaches
// before any TLS -- header decoding, routing by connection ID, accepting
// an Initial, Retry token checks, version negotiation, stateless reset,
// setting up a connection, and ngtcp2's first read of it. Build with
// GDP_ENABLE_FUZZING=ON (needs clang).
//
// Input: a mode byte, then datagrams, each a 2-byte big-endian length and
// its bytes. The mode byte's bits pick:
//   0x01  replay a real client Initial first, so a half-open connection
//         exists for the rest to reach;
//   0x02  splice that Initial's destination connection ID into every
//         datagram (where a long or a short header carries it), so they
//         route to that connection rather than looking like new ones;
//   0x0c  which of four source addresses the datagrams come from.
// Time is synthetic and advances between datagrams; at the end it jumps
// past every timeout, so each input starts from an empty demux.
#include "quic_conn.hpp"
#include "server_demux.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace gdp;

std::string write_temp(const std::string &pem) {
	char path[] = "/tmp/gdp_demux_fuzz_XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0 || write(fd, pem.data(), pem.size()) != (ssize_t)pem.size()) {
		abort();
	}
	close(fd);
	return path;
}

// A throwaway self-signed ECDSA certificate, as PEM files.
void make_cert(std::string *cert_path, std::string *key_path) {
	EVP_PKEY *key = EVP_EC_gen("P-256");
	X509 *cert = X509_new();
	ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
	X509_gmtime_adj(X509_getm_notBefore(cert), 0);
	X509_gmtime_adj(X509_getm_notAfter(cert), 86400);
	X509_set_pubkey(cert, key);
	X509_NAME *name = X509_get_subject_name(cert);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"fuzz", -1, -1, 0);
	X509_set_issuer_name(cert, name);
	X509_sign(cert, key, EVP_sha256());

	BIO *bio = BIO_new(BIO_s_mem());
	PEM_write_bio_X509(bio, cert);
	char *data;
	long len = BIO_get_mem_data(bio, &data);
	*cert_path = write_temp(std::string(data, len));
	BIO_free(bio);
	bio = BIO_new(BIO_s_mem());
	PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
	len = BIO_get_mem_data(bio, &data);
	*key_path = write_temp(std::string(data, len));
	BIO_free(bio);
	X509_free(cert);
	EVP_PKEY_free(key);
}

SockAddr ipv4(const char *addr, uint16_t port) {
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	inet_pton(AF_INET, addr, &sa.sin_addr);
	SockAddr out;
	out.set(reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
	return out;
}

struct Fixture {
	std::shared_ptr<TransportCore> core = std::make_shared<TransportCore>();
	std::unique_ptr<ServerDemux> demux;
	SockAddr local;
	SockAddr remotes[4];
	// A real client's first flight, and the DCID it chose.
	std::vector<std::vector<uint8_t>> initial;
	std::vector<uint8_t> initial_dcid;
	uint64_t now = 1'000'000'000;

	Fixture() {
		std::string cert_path, key_path;
		make_cert(&cert_path, &key_path);
		demux =
			std::make_unique<ServerDemux>(core, std::make_shared<AcceptTarget>(), CongestionControl::kCubic);
		if (!demux->listen(0, "gdp/1", cert_path, key_path, nullptr)) {
			fprintf(stderr, "server_demux_fuzzer: listen failed\n");
			abort();
		}
		unlink(cert_path.c_str());
		unlink(key_path.c_str());
		// Replies (Retry, resets, CONNECTION_CLOSE) really are sent, to
		// ports nothing listens on.
		local = ipv4("127.0.0.1", 4433);
		remotes[0] = ipv4("127.0.0.1", 50001);
		remotes[1] = ipv4("127.0.0.1", 50002);
		remotes[2] = ipv4("10.9.8.7", 50003);
		sockaddr_in6 sa6{};
		sa6.sin6_family = AF_INET6;
		sa6.sin6_port = htons(50004);
		sa6.sin6_addr = in6addr_loopback;
		remotes[3].set(reinterpret_cast<sockaddr *>(&sa6), sizeof(sa6));
		capture_initial();
	}

	// Runs a real ngtcp2 client just far enough to send its first flight,
	// into a socket of our own, and keeps what it sent.
	void capture_initial() {
		UdpSocket sink;
		if (!sink.bind_any(0, nullptr)) {
			abort();
		}
		auto client = std::make_shared<Connection::Impl>(core, false);
		client->weak_self = client;
		client->net.remote = ipv4("127.0.0.1", sink.local().port());
		UdpSocket socket;
		TlsContext tls;
		if (!socket.connect_to(client->net.remote) || !tls.init_client("gdp/1", false) ||
			!client->start_client(&socket, "fuzz", tls, CongestionControl::kCubic)) {
			abort();
		}
		client->service(now_ns());
		usleep(10000);
		sink.receive(16,
			[&](const UdpSocket::Packet &pkt) { initial.emplace_back(pkt.data, pkt.data + pkt.len); });
		if (initial.empty()) {
			fprintf(stderr, "server_demux_fuzzer: captured no Initial\n");
			abort();
		}
		ngtcp2_version_cid vc;
		if (ngtcp2_pkt_decode_version_cid(&vc, initial[0].data(), initial[0].size(), kCidLen) != 0) {
			abort();
		}
		initial_dcid.assign(vc.dcid, vc.dcid + vc.dcidlen);
		client->net.socket = nullptr;
		core->events.clear();
	}

	void feed(std::vector<uint8_t> &pkt, const SockAddr &remote) {
		UdpSocket::Packet p;
		p.data = pkt.data();
		p.len = pkt.size();
		p.remote = remote;
		p.local = local;
		demux->handle_packet(p, now);
		now += 1'000'000; // 1 ms
		demux->service(now);
	}

	void splice_dcid(std::vector<uint8_t> &pkt) {
		if (pkt.empty()) {
			return;
		}
		size_t len = initial_dcid.size();
		if (pkt[0] & 0x80) {
			// Long header: flags, 4-byte version, DCID length, DCID.
			if (pkt.size() >= 6 + len) {
				pkt[5] = static_cast<uint8_t>(len);
				memcpy(&pkt[6], initial_dcid.data(), len);
			}
		} else if (pkt.size() >= 1 + len) {
			memcpy(&pkt[1], initial_dcid.data(), len);
		}
	}

	void reset() {
		// Past every handshake and idle timeout: each connection ends and
		// is dropped, so no input's state leaks into the next.
		now += 60ull * 1'000'000'000;
		demux->service(now);
		now += 1'000'000'000;
		demux->service(now);
		core->events.clear();
		if (demux->connection_count() != 0) {
			fprintf(stderr, "server_demux_fuzzer: %zu connections outlived every timeout\n",
				demux->connection_count());
			abort();
		}
	}
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	static Fixture fixture;
	if (size == 0) {
		return 0;
	}
	uint8_t mode = data[0];
	const SockAddr &remote = fixture.remotes[(mode >> 2) & 3];
	if (mode & 0x01) {
		for (auto &pkt : fixture.initial) {
			std::vector<uint8_t> copy = pkt;
			fixture.feed(copy, remote);
		}
	}
	size_t pos = 1;
	while (pos + 2 <= size) {
		size_t len = (size_t(data[pos]) << 8) | data[pos + 1];
		pos += 2;
		len = std::min(len, size - pos);
		std::vector<uint8_t> pkt(data + pos, data + pos + len);
		pos += len;
		if (mode & 0x02) {
			fixture.splice_dcid(pkt);
		}
		fixture.feed(pkt, remote);
	}
	fixture.reset();
	return 0;
}
