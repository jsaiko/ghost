// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// One QUIC connection over ngtcp2: the state behind a Connection and its
// Streams. Split three ways by who may touch what -- the host's thread
// (the public wrappers and the closures dispatch() runs), any thread (the
// datagram queue and the figures the getters read), and the network
// thread (everything ngtcp2) -- and each field is grouped under the one
// that owns it.
#pragma once

#include "gdp/transport.hpp"

#include "event_queue.hpp"
#include "net_loop.hpp"
#include "tls.hpp"
#include "udp_socket.hpp"

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gdp {

// The largest UDP payload any GDP endpoint sends or will take: a 1500-byte
// IPv4 MTU, the ceiling for path MTU discovery. What each endpoint
// advertises as max_udp_payload_size, so nothing larger ever arrives.
constexpr size_t kMaxUdpPayload = 1500 - 20 - 8;
// Our connection IDs, both roles.
constexpr size_t kCidLen = 16;

// What a Transport and everything it creates share. Connections and
// Streams hold it too, so a wrapper that outlives its Transport still has
// something valid to post into (the posts are dropped: the loop has
// stopped).
struct TransportCore {
	EventQueue events;
	NetLoop loop;
	CongestionControl congestion_control = CongestionControl::kCubic; // host thread
};

// Server-side hook the listener implements: every connection ID a server
// connection starts or stops answering to, so packets for it can be found.
class CidRegistry {
public:
	virtual ~CidRegistry() = default;
	virtual void add_cid(const ngtcp2_cid &cid, Connection::Impl *conn) = 0;
	virtual void remove_cid(const ngtcp2_cid &cid) = 0;
};

// Every connection ID a server hands out comes from here: random bytes.
void generate_server_cid(ngtcp2_cid *cid);

struct Stream::Impl {
	std::shared_ptr<TransportCore> core;
	std::weak_ptr<Connection::Impl> conn;
	int64_t id = -1;
	// Host thread only; see the lifetime model in transport.cpp.
	Stream *self = nullptr;
	std::weak_ptr<Impl> weak_self;
};

struct Connection::Impl {
	Impl(std::shared_ptr<TransportCore> core, bool is_server);
	~Impl();

	const std::shared_ptr<TransportCore> core;
	const bool is_server;
	std::weak_ptr<Impl> weak_self;

	// --- Host thread only ---

	Connection *self = nullptr;
	ConnectionState state = ConnectionState::kConnecting;
	std::unique_ptr<Stream> control_stream;
	std::unique_ptr<Stream> input_stream;
	// Set by closures posted before the kShutdown one, so always in place
	// by the time on_state_changed(kShutdown) runs.
	std::string shutdown_reason;
	uint64_t shutdown_error_code = 0;
	// Client: posted before kConnected.
	std::string peer_cert_sha256;
	bool peer_cert_ca_verified = false;
	// The application error code of the last close(): ~Connection() closes
	// with it, so a close(code) followed straight away by the destructor
	// still puts `code` on the wire.
	uint64_t close_code = 0;

	// --- Any thread ---

	// Set by the network thread once the connection can send nothing more.
	std::atomic<bool> terminated{false};
	std::atomic<uint32_t> rtt_us{0};
	std::atomic<uint16_t> path_mtu{0};
	std::atomic<uint16_t> max_datagram_size{0};
	// The peer's transport parameters said no to datagrams.
	std::atomic<bool> datagrams_refused{false};

	struct QueuedDatagram {
		std::vector<uint8_t> data;
		uint64_t enqueued_us;
	};
	// Everything below under datagram_mutex. `priority_queue` goes out
	// ahead of `queue`; `queue` stays in send order, because the video
	// reassembler drops a frame whose slices another frame's overtake.
	// Both are unbounded.
	mutable std::mutex datagram_mutex;
	Connection::DatagramStats datagram_stats;
	std::deque<QueuedDatagram> priority_queue;
	std::deque<QueuedDatagram> queue;
	// set_pacing_rate()'s token bucket, which only `queue` answers to.
	uint64_t pacing_bytes_per_s = 0;
	double pacing_tokens = 0;
	uint64_t pacing_refilled_us = 0;

	// --- Network thread only ---

	struct NetStream {
		std::weak_ptr<Stream::Impl> stream; // where received data goes
		bool opened = false;                // exists in ngtcp2 (a client stream can wait for credit)
		bool any_sent = false;              // announced to the peer, even if empty
		bool fin_requested = false;
		bool fin_sent = false;
		bool blocked = false; // flow control; cleared when credit arrives
		// Sent-but-unacked and unsent bytes, oldest first. ngtcp2 keeps
		// pointers into these until the peer acks them, so a chunk is only
		// dropped once acked_stream_data_offset covers all of it.
		std::deque<std::vector<uint8_t>> chunks;
		uint64_t chunks_offset = 0; // stream offset of chunks.front()[0]
		uint64_t written = 0;       // stream offset of the first byte not yet handed to ngtcp2
		uint64_t end = 0;           // stream offset one past the last byte queued
	};

	struct Net {
		ngtcp2_conn *conn = nullptr;
		ngtcp2_crypto_conn_ref conn_ref{};
		TlsSession tls;
		UdpSocket *socket = nullptr;            // client: its own; server: the listener's
		CidRegistry *cids = nullptr;            // server only
		const uint8_t *static_secret = nullptr; // server: for stateless reset tokens
		size_t static_secret_len = 0;
		SockAddr local;
		SockAddr remote;
		bool handshake_completed = false;
		// When the connection stops (see terminate()), the deadline for
		// letting go of it: the end of the closing or draining period.
		uint64_t linger_until = 0;
		// Closing period: the CONNECTION_CLOSE to repeat to packets that
		// still arrive, and how many have, for the rate limit.
		std::vector<uint8_t> close_packet;
		uint64_t packets_while_closing = 0;
		bool stateless_reset = false;

		std::map<int64_t, NetStream> streams;
		// Client streams opened by the host, waiting for the peer's stream
		// credit before ngtcp2 can open them, in order.
		std::deque<int64_t> pending_open;

		uint64_t next_dgram_id = 1;
		struct InFlight {
			uint32_t len;
			bool lost;
			uint64_t sent_ns;
		};
		std::unordered_map<uint64_t, InFlight> dgrams_in_flight;
		uint64_t next_prune_ns = 0;

		// A packet the socket had no room for (EAGAIN), sent first next time.
		std::vector<uint8_t> blocked_packet;
		SockAddr blocked_local;
		SockAddr blocked_remote;
		// Packed-but-unwritten datagrams: ngtcp2 reads them while it builds
		// the packet, so they stay alive until the packet is done.
		std::vector<QueuedDatagram> in_packet;
		// Server: the listener's, called once the handshake completes, to
		// hand the connection to the host.
		std::function<void()> on_handshake;
		bool handed_over = false;
		// When the pacer next lets a datagram go; UINT64_MAX when it isn't
		// holding one back.
		uint64_t pacer_wakeup = UINT64_MAX;
		// A send found an ICMP error waiting (client); acted on after the
		// write loop.
		int pending_socket_error = 0;
	} net;

	// Network thread. Client: starts the handshake towards `remote`, over
	// `socket`, which must outlive the connection.
	// `net.remote` must already be set.
	bool start_client(UdpSocket *socket, const std::string &server_name, const TlsContext &tls_ctx,
		CongestionControl cc);
	// Network thread. Server: `hd` is the client's first Initial, `odcid`
	// its original DCID when that Initial came back with a Retry token.
	bool start_server(UdpSocket *socket, const UdpSocket::Packet &first, const ngtcp2_pkt_hd &hd,
		const ngtcp2_cid *odcid, ngtcp2_token_type token_type, const TlsContext &tls_ctx, CidRegistry *cids,
		const uint8_t *static_secret, size_t static_secret_len, CongestionControl cc, uint64_t now);

	// Network thread: one received packet. Returns false once the
	// connection has stopped (see finished()).
	bool read_packet(const UdpSocket::Packet &pkt, uint64_t now);
	// Network thread: timers, then everything that can be written.
	void service(uint64_t now);
	uint64_t next_deadline() const;
	// Network thread: the connection has stopped and its closing or
	// draining period is over; it can be dropped.
	bool finished(uint64_t now) const;
	bool wants_write() const { return !net.blocked_packet.empty(); }

	// Network thread: an application close (close(), ~Connection()).
	void close_locally(uint64_t error_code, uint64_t now);
	// Network thread: a socket error (client) -- ECONNREFUSED and friends.
	void socket_error(int err, uint64_t now);

	// Network thread: host-requested stream work.
	void open_stream(int64_t id, std::weak_ptr<Stream::Impl> stream);
	void stream_send(int64_t id, std::vector<uint8_t> data, bool fin);
	void stream_abort(int64_t id);

	// Any thread.
	void refill_pacing_tokens(uint64_t now_us); // caller holds datagram_mutex

	// The rest are the network thread's internals (and ngtcp2's callbacks').
	void flush(uint64_t now);
	// Writes one packet into `buf`; 0 when there's nothing (more) to send.
	ngtcp2_ssize write_packet(ngtcp2_path *path, ngtcp2_pkt_info *pi, uint8_t *buf, size_t buflen,
		uint64_t now);
	bool send_packet(const uint8_t *data, size_t len, const ngtcp2_path &path);
	bool send_blocked_packet();
	void try_open_pending_streams();
	void update_path_figures();
	// Stops the connection: nothing more is sent, the host sees `reason`
	// (and `peer_code`, if the peer closed it) and then kShutdown, and the
	// connection lingers `linger_ns` (its closing or draining period).
	void terminate(const std::string &reason, uint64_t peer_code, uint64_t linger_ns, uint64_t now);
	// Sends a CONNECTION_CLOSE for `err`, then terminate()s.
	void fail(const ngtcp2_ccerr &err, const std::string &reason, uint64_t now);
	void drop_queued_datagrams();
	void post_state(ConnectionState state);
};

} // namespace gdp
