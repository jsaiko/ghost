// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// QUIC over ngtcp2: Connection, reliable streams,
// and datagrams. No I/O policy of its own -- each Transport runs one
// network thread for sockets and timers (unavoidable for any QUIC stack),
// but every callback the *application* sees runs from dispatch(), called
// on whatever thread the host chooses whenever it pumps its own event
// loop. See src/transport/transport.cpp's top comment for how that bridge
// works.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace google::protobuf {
class MessageLite;
}

namespace gdp {

enum class ConnectionState {
	kConnecting,
	kConnected,
	kShutdown, // terminal; the Connection/its Streams are no longer usable
};

class Connection;

// The certificate authorities a client checks a server's certificate
// against, besides pinning it (gdp-spec.md §2.3): a lobby whose
// certificate a trusted CA issued for the name the user typed needs no
// trust-on-first-use prompt. Empty (the default): no CA check at all,
// which is right wherever a pin is the only trust root -- session
// connections, Veil's own channels.
struct CaTrust {
	// The platform's trust store: OpenSSL's default paths on Linux, the
	// system bundle on macOS, the ROOT certificate store on Windows.
	bool system = false;
	// A PEM file of further CAs (an organization's own), or empty. A file
	// that can't be read is logged and skipped, never fatal.
	std::string extra_file;

	bool any() const { return system || !extra_file.empty(); }
};

class Stream {
public:
	Stream(const Stream &) = delete;
	Stream &operator=(const Stream &) = delete;
	~Stream();

	// Public, but not meaningfully callable from outside src/transport/:
	// Impl is only ever fully defined there, so nothing outside it can
	// construct a valid shared_ptr<Impl> to pass in. Pimpl-as-access-control
	// rather than a friend-function list that would tie this header to
	// the transport's internal helper names.
	//
	// shared_ptr rather than unique_ptr because the events the network
	// thread posts onto the dispatch queue capture a weak_ptr to it: an event that
	// is still queued when the host destroys this Stream (including from
	// inside one of its own callbacks) then simply finds nothing to deliver
	// to, instead of dereferencing a freed object.
	struct Impl;
	explicit Stream(std::shared_ptr<Impl> impl);

	// Queues `[data, data+len)` for send. Safe to call from any thread (the
	// data is copied and handed to the network thread).
	// `fin` marks this as the last data the local side will ever send on
	// this stream.
	void send(const uint8_t *data, size_t len, bool fin = false);

	// Length-prefix-frames `msg` (framing.hpp's encode_frame, docs/gdp-
	// spec.md §3.1) and queues it with send(). Returns false, sending
	// nothing, if `msg` exceeds kMaxFrameSize or fails to serialize --
	// neither should happen for the protocol's own messages, so callers
	// treat it as an internal error worth logging, not a network fault.
	bool send_message(const google::protobuf::MessageLite &msg);

	void close();

	// Fires only from dispatch(), on the calling thread. Destroying the
	// Stream (or the Connection that owns it) from inside it is safe. A
	// stream's end is observed through its Connection's state, not here.
	std::function<void(const uint8_t *data, size_t len)> on_data;

private:
	std::shared_ptr<Impl> impl_;
};

class Connection {
public:
	Connection(const Connection &) = delete;
	Connection &operator=(const Connection &) = delete;
	~Connection();

	// Public, but not meaningfully callable from outside src/transport/ --
	// see the identical note on Stream::Impl above.
	struct Impl;
	explicit Connection(std::shared_ptr<Impl> impl);

	ConnectionState state() const;

	// ngtcp2's smoothed RTT estimate for this connection, microseconds.
	// 0 if unavailable (not yet connected, or the query failed). Exists so
	// StatsReport (gdp-spec.md §7.5) can echo a real transport-level
	// RTT rather than spectre needing its own ping/pong round trip.
	uint32_t rtt_us() const;

	// Human-readable reason the connection shut down (e.g. "connection
	// refused", "timed out", "TLS handshake failed", "closed by peer:
	// authentication failed"), set from the CONNECTION_CLOSE, timeout or
	// socket error that ended it, before on_state_changed fires with
	// kShutdown.
	// Empty until then, and for a graceful app-level close (peer sent
	// CONNECTION_CLOSE with application error code 0).
	const std::string &shutdown_reason() const;

	// The peer's address, a v4-mapped IPv6 address in its IPv4 form, and
	// its port: "192.0.2.1" and 51000. What a server groups
	// unauthenticated connections by. Set from the moment the connection
	// is handed out.
	std::string remote_host() const;
	uint16_t remote_port() const;

	// The QUIC CONNECTION_CLOSE application error code the peer closed
	// with (gdp-spec.md §12, error_codes.hpp) -- 0 for a normal close,
	// a transport-level failure, or before the connection has shut down.
	uint64_t shutdown_error_code() const;

	// Client role: lowercase hex SHA-256 of the server's DER certificate
	// (gdp/cert_fingerprint.hpp), captured during the handshake. Always set by the time
	// on_state_changed(kConnected) fires; empty before that, and server-side. What host pinning
	// (gdp-spec.md §2.3) compares against -- see ClientConnection.
	const std::string &peer_certificate_sha256() const;

	// Client role: whether that certificate also chains to a CA the
	// connect() call's CaTrust names and is valid for the name dialed.
	// Never fails a handshake by itself -- it is one more fact for the
	// caller's pinning to weigh (gdp-spec.md §2.3). False until the
	// handshake completes, server-side, and with no CaTrust.
	bool peer_certificate_ca_verified() const;

	// Client-only: opens the two streams gdp-spec.md §2.2 fixes the
	// order of (control first, then input). Returns null if called
	// server-side, a second time, or (input) before the control stream:
	// the host takes stream 0 as control, whatever the client meant by it.
	Stream *open_control_stream();
	Stream *open_input_stream();

	// Queues one datagram. Fails (returning false, sending nothing) if the peer never enabled
	// datagrams or `len` exceeds max_datagram_size() -- refused outright rather than dropped unseen
	// when its turn comes. `priority` puts it ahead of every non-priority datagram still waiting to
	// go out (audio, so it never queues behind a keyframe). Safe to call from any thread.
	bool send_datagram(const uint8_t *data, size_t len, bool priority = false);

	// Paces non-priority datagrams to `bytes_per_s` on their way into QUIC.
	// QUIC's own pacing follows cwnd over RTT, which on a LAN (well under
	// 1 ms of RTT) is close to line rate: a keyframe otherwise leaves in one
	// burst, and wherever the path steps down (a 10G host to a 2.5G
	// client's switch port, a wired host to Wi-Fi) the burst overflows
	// that hop's buffer and is dropped. A short
	// burst (pacing_burst_bytes()) still goes at once, so ordinary frames
	// see no delay. Datagrams held back count as queued in
	// datagram_stats(). 0, the default, turns pacing off. Safe to call
	// from any thread.
	void set_pacing_rate(uint64_t bytes_per_s);
	// What may leave at once at `bytes_per_s`: a millisecond of it, at
	// least two full datagrams and at most 24 KiB. A fixed burst would be
	// a free 17 packets into a slow link's queue -- over half of a cheap
	// router's -- ahead of whatever the pacing itself adds.
	static size_t pacing_burst_bytes(uint64_t bytes_per_s);

	// The largest datagram send_datagram() will currently accept, in bytes:
	// the path MTU minus IP/UDP/QUIC overhead, capped by the peer's
	// max_datagram_frame_size. A connection starts at QUIC's minimum MTU
	// (~1150 bytes of datagram here) and this rises as path MTU discovery
	// confirms bigger packets, so read it per use rather than once. It can
	// also drop, on a path change. 0 until the handshake completes, and
	// when the peer takes no datagrams. Safe to call from any thread.
	uint16_t max_datagram_size() const;

	// The current path MTU, IP header included, as path MTU discovery has
	// confirmed it. 0 until the handshake completes. For logs and overlays;
	// slicing uses max_datagram_size().
	uint16_t path_mtu() const;

	// What has become of the datagrams this side sent. `queued_*` is what
	// is still waiting to go out -- congestion control gates datagrams like
	// any other data, and the queue for them is unbounded, so this is where
	// send-side latency builds up unseen. The rest are running totals
	// since the connection opened: diff two snapshots for a rate.
	struct DatagramStats {
		uint32_t queued_datagrams = 0;
		uint64_t queued_bytes = 0;
		// How long the oldest still-unsent datagram has waited, µs; 0 when
		// nothing is queued.
		uint64_t oldest_queued_age_us = 0;
		uint64_t sent_datagrams = 0;
		uint64_t sent_bytes = 0;
		uint64_t acked_datagrams = 0; // includes late acks for ones already counted lost
		uint64_t acked_bytes = 0;
		uint64_t lost_datagrams = 0; // declared lost by QUIC's loss detection
		uint64_t lost_bytes = 0;
		uint64_t canceled_datagrams = 0; // dropped before ever being sent (connection closed, path shrank)
	};
	// Snapshot; safe to call from any thread.
	DatagramStats datagram_stats() const;

	// Has QUIC send a PING whenever the connection has been quiet for
	// `interval_ms`, so a connection that may sit idle for hours (Wisp's
	// link to Veil) outlives both sides' idle timeouts. 0 turns it off,
	// the default. Safe to call from any thread.
	void set_keep_alive(uint32_t interval_ms);

	// Sends CONNECTION_CLOSE with `error_code` as the application error
	// code (gdp-spec.md §12; gdp::ErrorCode values from error_codes.hpp).
	// The peer sees it via shutdown_error_code()/shutdown_reason(). 0 is a
	// normal close.
	void close(uint64_t error_code = 0);

	// All fire only from dispatch(), on the calling thread. Destroying the
	// Connection from inside any of them is safe: events still queued for
	// it (or for its Streams) are dropped rather than delivered.
	std::function<void(ConnectionState)> on_state_changed;
	// Server-only: fires once each, identified by QUIC stream ID (0 =
	// control, 4 = input per gdp-spec.md §2.2) rather than arrival order,
	// which QUIC doesn't actually guarantee matches send order once
	// packets can be reordered in flight.
	std::function<void(Stream *)> on_control_stream;
	std::function<void(Stream *)> on_input_stream;
	// `arrival_us` is monotonic_us() when the network thread read the
	// datagram off the socket -- dispatch() runs later, and on some hosts much
	// later (a timer-polled loop), so this is the time to measure arrival
	// spacing by.
	std::function<void(const uint8_t *data, size_t len, uint64_t arrival_us)> on_datagram;

private:
	std::shared_ptr<Impl> impl_;
};

// QUIC's own congestion controller, which gates datagrams like any other
// data (ngtcp2_settings.cc_algo).
enum class CongestionControl {
	kCubic, // the default: loss-based, fills the path's buffers before backing off
	kBbr,   // paces to measured bandwidth and RTT, keeping queues short
};

// One per process (or one per role, if a process needs both -- tests do).
// Owns the network thread, and with it every socket.
class Transport {
public:
	Transport();
	~Transport();
	Transport(const Transport &) = delete;
	Transport &operator=(const Transport &) = delete;

	// Applies to every connection this transport listens for or starts
	// after the call; existing ones keep what they started with.
	void set_congestion_control(CongestionControl cc);

	// Becomes readable whenever dispatch() has queued work. Add it to the
	// host's own event loop (wl_event_loop_add_fd, a QSocketNotifier, a
	// raw poll() in spectre's SDL loop, ...) and call dispatch() when it
	// fires. Safe to call dispatch() speculatively even when not readable.
	int notify_fd() const;

	// Drains queued events and invokes the on_* callbacks they produced,
	// on the calling thread. Returns the number of events processed.
	size_t dispatch();

	// Client role. Never returns null today: name resolution and every
	// failure after it are asynchronous, and come through the returned
	// Connection's on_state_changed like success does.
	// The server's certificate is never what fails the handshake: GDP pins
	// it by fingerprint (Connection::peer_certificate_sha256(),
	// ClientConnection), and `ca` only adds whether it also checks out
	// against a CA (Connection::peer_certificate_ca_verified()).
	std::unique_ptr<Connection> connect(const std::string &host, uint16_t port, const std::string &alpn,
		const CaTrust &ca = {});

	// Server role. `cert_file`/`key_file` are PEM paths. `on_new_connection`
	// fires (via dispatch()) once per inbound connection that completes
	// its handshake; ownership passes to the handler. On failure,
	// `*address_in_use` (if given) says whether it was only because
	// something else already holds `port`, so a caller scanning a port
	// range can tell "try the next one" from a failure every port would hit.
	using NewConnectionHandler = std::function<void(std::unique_ptr<Connection>)>;
	bool listen(uint16_t port, const std::string &alpn, const std::string &cert_file,
		const std::string &key_file, NewConnectionHandler on_new_connection, bool *address_in_use = nullptr);

	// Public for the same reason as Stream::Impl/Connection::Impl above:
	// only fully defined in src/transport/transport.cpp.
	struct Impl;

private:
	std::unique_ptr<Impl> impl_;
};

} // namespace gdp
