// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// The server side of a Transport: one UDP socket for one listen() port,
// shared by every connection accepted on it. Each packet is routed by its
// destination connection ID; an Initial that matches nothing starts a new
// connection, which is handed to the host once its handshake completes.
#pragma once

#include "quic_conn.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gdp {

// Where accepted connections go: the listen() handler, which only ever
// runs on the host's thread. Owned by the Transport; the listener holds it
// only to pass it along in the closures it posts.
struct AcceptTarget {
	Transport::NewConnectionHandler on_new_connection;
};

class ServerDemux : public NetLoop::Endpoint, public CidRegistry {
public:
	ServerDemux(std::shared_ptr<TransportCore> core, std::shared_ptr<AcceptTarget> target,
		CongestionControl cc);
	~ServerDemux() override;

	// Host thread, before the demux is handed to the network thread.
	bool listen(uint16_t port, const std::string &alpn, const std::string &cert_file,
		const std::string &key_file, bool *address_in_use);

	// NetLoop::Endpoint
	socket_t fd() const override { return socket_.fd(); }
	bool wants_write() const override;
	void on_readable(uint64_t now) override;
	void service(uint64_t now) override;
	uint64_t next_deadline() const override;

	// CidRegistry
	void add_cid(const ngtcp2_cid &cid, Connection::Impl *conn) override;
	void remove_cid(const ngtcp2_cid &cid) override;

	// One received packet. on_readable() calls it per packet; the fuzz
	// target (fuzz/server_demux_fuzzer.cpp) calls it directly.
	void handle_packet(const UdpSocket::Packet &pkt, uint64_t now);
	size_t connection_count() const { return conns_.size(); }

private:
	void accept(const UdpSocket::Packet &pkt, uint64_t now);
	size_t pending_handshakes() const;
	void send_retry(const ngtcp2_pkt_hd &hd, const UdpSocket::Packet &pkt, uint64_t now);
	void send_version_negotiation(const ngtcp2_version_cid &vc, const UdpSocket::Packet &pkt);
	void send_stateless_reset(const ngtcp2_version_cid &vc, const UdpSocket::Packet &pkt);
	void send_invalid_token_close(const ngtcp2_pkt_hd &hd, const UdpSocket::Packet &pkt);
	void remove(Connection::Impl *conn);

	std::shared_ptr<TransportCore> core_;
	std::shared_ptr<AcceptTarget> target_;
	CongestionControl cc_;
	UdpSocket socket_;
	TlsContext tls_;
	// Keys Retry tokens and stateless reset tokens. Per listener, so both
	// stop being valid when the process goes.
	uint8_t static_secret_[32];

	std::vector<std::shared_ptr<Connection::Impl>> conns_;
	std::unordered_map<std::string, Connection::Impl *> by_cid_;
};

} // namespace gdp
