// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "server_demux.hpp"

#include <openssl/rand.h>

#include <algorithm>

namespace gdp {

namespace {

// More handshakes than this in progress at once and a new client has to
// prove its address with a Retry round trip first.
constexpr size_t kRetryThreshold = 64;
// How long a Retry token stays good.
constexpr uint64_t kRetryTokenLifetime = 10 * NGTCP2_SECONDS;
// Received packets handled per wakeup before the loop services timers and
// writes again.
constexpr size_t kReceiveBudget = 256;

std::string cid_key(const uint8_t *data, size_t len) {
	return std::string(reinterpret_cast<const char *>(data), len);
}

std::string cid_key(const ngtcp2_cid &cid) {
	return cid_key(cid.data, cid.datalen);
}

} // namespace

ServerDemux::ServerDemux(std::shared_ptr<TransportCore> core, std::shared_ptr<AcceptTarget> target,
	CongestionControl cc)
	: core_(std::move(core)), target_(std::move(target)), cc_(cc) {
	RAND_bytes(static_secret_, sizeof(static_secret_));
}

ServerDemux::~ServerDemux() {
	// A host-held Connection can outlive this; its Impl must not reach
	// back into a socket or registry that's gone.
	for (auto &conn : conns_) {
		conn->net.socket = nullptr;
		conn->net.cids = nullptr;
	}
}

bool ServerDemux::listen(uint16_t port, const std::string &alpn, const std::string &cert_file,
	const std::string &key_file, bool *address_in_use) {
	if (!tls_.init_server(alpn, cert_file, key_file)) {
		if (address_in_use) {
			*address_in_use = false;
		}
		return false;
	}
	return socket_.bind_any(port, address_in_use);
}

bool ServerDemux::wants_write() const {
	return std::any_of(conns_.begin(), conns_.end(), [](const auto &conn) { return conn->wants_write(); });
}

void ServerDemux::on_readable(uint64_t now) {
	socket_.receive(kReceiveBudget, [&](const UdpSocket::Packet &pkt) { handle_packet(pkt, now); });
}

void ServerDemux::service(uint64_t now) {
	// A copy: service() can end a connection, and remove() edits conns_.
	std::vector<std::shared_ptr<Connection::Impl>> conns = conns_;
	for (auto &conn : conns) {
		conn->service(now);
		if (conn->finished(now)) {
			remove(conn.get());
		}
	}
}

uint64_t ServerDemux::next_deadline() const {
	uint64_t deadline = UINT64_MAX;
	for (auto &conn : conns_) {
		deadline = std::min(deadline, conn->next_deadline());
	}
	return deadline;
}

void ServerDemux::add_cid(const ngtcp2_cid &cid, Connection::Impl *conn) {
	by_cid_[cid_key(cid)] = conn;
}

void ServerDemux::remove_cid(const ngtcp2_cid &cid) {
	by_cid_.erase(cid_key(cid));
}

void ServerDemux::remove(Connection::Impl *conn) {
	for (auto it = by_cid_.begin(); it != by_cid_.end();) {
		if (it->second == conn) {
			it = by_cid_.erase(it);
		} else {
			++it;
		}
	}
	conn->net.socket = nullptr;
	conn->net.cids = nullptr;
	auto it = std::find_if(conns_.begin(), conns_.end(), [conn](const auto &c) { return c.get() == conn; });
	if (it != conns_.end()) {
		conns_.erase(it);
	}
}

size_t ServerDemux::pending_handshakes() const {
	return std::count_if(conns_.begin(), conns_.end(),
		[](const auto &conn) { return !conn->net.handshake_completed && !conn->net.linger_until; });
}

void ServerDemux::handle_packet(const UdpSocket::Packet &pkt, uint64_t now) {
	// An empty datagram is no QUIC packet -- and ngtcp2 asserts on one (its
	// distro builds keep asserts on), so one stray empty UDP packet would
	// otherwise abort the process.
	if (pkt.len == 0) {
		return;
	}
	ngtcp2_version_cid vc;
	int rv = ngtcp2_pkt_decode_version_cid(&vc, pkt.data, pkt.len, kCidLen);
	if (rv == NGTCP2_ERR_VERSION_NEGOTIATION) {
		send_version_negotiation(vc, pkt);
		return;
	}
	if (rv != 0) {
		return;
	}
	auto it = by_cid_.find(cid_key(vc.dcid, vc.dcidlen));
	if (it != by_cid_.end()) {
		it->second->read_packet(pkt, now);
		return;
	}
	if (!(pkt.data[0] & 0x80)) {
		// A short-header packet for a connection we don't have (any more):
		// tell the peer, so it needn't wait out its idle timeout.
		send_stateless_reset(vc, pkt);
		return;
	}
	accept(pkt, now);
}

void ServerDemux::accept(const UdpSocket::Packet &pkt, uint64_t now) {
	ngtcp2_pkt_hd hd;
	if (ngtcp2_accept(&hd, pkt.data, pkt.len) != 0) {
		return;
	}

	ngtcp2_cid odcid;
	const ngtcp2_cid *podcid = nullptr;
	ngtcp2_token_type token_type = NGTCP2_TOKEN_TYPE_UNKNOWN;
	if (hd.tokenlen) {
		if (hd.token[0] == NGTCP2_CRYPTO_TOKEN_MAGIC_RETRY2) {
			if (ngtcp2_crypto_verify_retry_token2(&odcid, hd.token, hd.tokenlen, static_secret_,
					sizeof(static_secret_), hd.version, pkt.remote.get(), pkt.remote.len, &hd.dcid,
					kRetryTokenLifetime, now) != 0) {
				send_invalid_token_close(hd, pkt);
				return;
			}
			podcid = &odcid;
			token_type = NGTCP2_TOKEN_TYPE_RETRY;
		} else {
			// A NEW_TOKEN token: we never issue those.
			hd.token = nullptr;
			hd.tokenlen = 0;
		}
	}
	if (!podcid && pending_handshakes() >= kRetryThreshold) {
		send_retry(hd, pkt, now);
		return;
	}

	auto conn = std::make_shared<Connection::Impl>(core_, true);
	conn->weak_self = conn;
	Connection::Impl *raw = conn.get();
	conn->net.on_handshake = [raw, target = target_]() {
		raw->net.handed_over = true;
		// The closure owns the connection until the host takes it: a
		// Connection made from it then, or none if the handler is gone.
		raw->core->events.post([target, conn = raw->weak_self.lock()]() {
			auto wrapper = std::make_unique<Connection>(conn);
			if (target->on_new_connection) {
				target->on_new_connection(std::move(wrapper));
			}
		});
	};
	if (!conn->start_server(&socket_, pkt, hd, podcid, token_type, tls_, this, static_secret_,
			sizeof(static_secret_), cc_, now)) {
		return;
	}
	// The client keeps addressing its Initials to the DCID it made up until
	// it hears ours.
	add_cid(hd.dcid, raw);
	conns_.push_back(std::move(conn));
	raw->read_packet(pkt, now);
}

void ServerDemux::send_retry(const ngtcp2_pkt_hd &hd, const UdpSocket::Packet &pkt, uint64_t now) {
	ngtcp2_cid scid;
	generate_server_cid(&scid);
	uint8_t token[NGTCP2_CRYPTO_MAX_RETRY_TOKENLEN2];
	ngtcp2_ssize tokenlen = ngtcp2_crypto_generate_retry_token2(token, static_secret_, sizeof(static_secret_),
		hd.version, pkt.remote.get(), pkt.remote.len, &scid, &hd.dcid, now);
	if (tokenlen < 0) {
		return;
	}
	uint8_t buf[NGTCP2_MAX_UDP_PAYLOAD_SIZE];
	ngtcp2_ssize n = ngtcp2_crypto_write_retry(buf, sizeof(buf), hd.version, &hd.scid, &scid, &hd.dcid, token,
		static_cast<size_t>(tokenlen));
	if (n > 0) {
		socket_.send(buf, static_cast<size_t>(n), &pkt.local, &pkt.remote);
	}
}

void ServerDemux::send_version_negotiation(const ngtcp2_version_cid &vc, const UdpSocket::Packet &pkt) {
	// Only in answer to something Initial-sized, so this can't be used to
	// amplify.
	if (pkt.len < NGTCP2_MAX_UDP_PAYLOAD_SIZE) {
		return;
	}
	uint8_t unused;
	RAND_bytes(&unused, 1);
	const uint32_t versions[] = {NGTCP2_PROTO_VER_V1};
	uint8_t buf[256];
	ngtcp2_ssize n = ngtcp2_pkt_write_version_negotiation(buf, sizeof(buf), unused, vc.scid, vc.scidlen,
		vc.dcid, vc.dcidlen, versions, 1);
	if (n > 0) {
		socket_.send(buf, static_cast<size_t>(n), &pkt.local, &pkt.remote);
	}
}

void ServerDemux::send_stateless_reset(const ngtcp2_version_cid &vc, const UdpSocket::Packet &pkt) {
	// Smaller than what triggered it, so two endpoints can't loop on each
	// other's resets.
	if (pkt.len <= 1 + NGTCP2_STATELESS_RESET_TOKENLEN + NGTCP2_MIN_STATELESS_RESET_RANDLEN + 1) {
		return;
	}
	size_t randlen =
		std::min<size_t>(pkt.len - 1, NGTCP2_MAX_UDP_PAYLOAD_SIZE) - 1 - NGTCP2_STATELESS_RESET_TOKENLEN;
	ngtcp2_cid cid;
	ngtcp2_cid_init(&cid, vc.dcid, vc.dcidlen);
	uint8_t token[NGTCP2_STATELESS_RESET_TOKENLEN];
	if (ngtcp2_crypto_generate_stateless_reset_token(token, static_secret_, sizeof(static_secret_), &cid) !=
		0) {
		return;
	}
	uint8_t rand[NGTCP2_MAX_UDP_PAYLOAD_SIZE];
	RAND_bytes(rand, static_cast<int>(randlen));
	uint8_t buf[NGTCP2_MAX_UDP_PAYLOAD_SIZE];
	ngtcp2_ssize n = ngtcp2_pkt_write_stateless_reset(buf, sizeof(buf), token, rand, randlen);
	if (n > 0) {
		socket_.send(buf, static_cast<size_t>(n), &pkt.local, &pkt.remote);
	}
}

void ServerDemux::send_invalid_token_close(const ngtcp2_pkt_hd &hd, const UdpSocket::Packet &pkt) {
	uint8_t buf[NGTCP2_MAX_UDP_PAYLOAD_SIZE];
	ngtcp2_ssize n = ngtcp2_crypto_write_connection_close(buf, sizeof(buf), hd.version, &hd.scid, &hd.dcid,
		NGTCP2_INVALID_TOKEN, nullptr, 0);
	if (n > 0) {
		socket_.send(buf, static_cast<size_t>(n), &pkt.local, &pkt.remote);
	}
}

} // namespace gdp
