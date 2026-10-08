// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// The public transport API (transport.hpp) over ngtcp2.
//
// Threading model: each Transport runs one network thread (NetLoop) that
// owns its sockets and every ngtcp2_conn, and does all packet I/O and
// timer work. What *is* under the host's control is where *application*
// code runs: the network thread never calls an on_* callback itself. It
// copies what it has to report and posts a closure onto the EventQueue,
// signalling an eventfd; Transport::dispatch() -- called by the host,
// whenever *it* pumps its own event loop -- runs those closures on the
// calling thread. The other direction works the same way: a host call
// that needs ngtcp2 (Stream::send, close, opening a stream) posts a task to
// the network thread. The one shortcut is the datagram queue, which
// send_datagram() fills directly under a mutex.
//
// Lifetime model (Stream and Connection): the closures posted for the host
// can run long after the host has destroyed the Stream or Connection they
// were meant for -- e.g. a batch of received data already queued when the
// host tears the connection down from inside one of them. So each Impl is
// owned by shared_ptr, and every such closure captures only a weak_ptr to
// it. Delivering an event means: lock the weak_ptr, then check Impl::self
// -- the public wrapper's back-pointer, which its destructor nulls. Either
// failing means the target is gone and the event is dropped. `self` is
// only ever touched on the host thread (set in the wrapper's constructor,
// cleared in its destructor, read from dispatched closures), so it needs
// no lock; the network thread never looks at it.
//
// The on_* std::function is copied before being invoked so the host may
// destroy the wrapper -- and with it the std::function itself -- from
// inside the callback without pulling the rug out from under the call.
//
// The network thread holds its own shared_ptr to each live connection, so
// destroying a Connection only asks for the close: the CONNECTION_CLOSE
// goes out, the closing period runs, and the network thread lets go after.
#include "gdp/transport.hpp"

#include "gdp/clock.hpp"
#include "gdp/framing.hpp"

#include "quic_conn.hpp"
#include "server_demux.hpp"

#include "socket_platform.hpp"

#include <algorithm>
#include <cstdio>

namespace gdp {

// --- Stream ---

Stream::Stream(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {
	impl_->self = this;
}

Stream::~Stream() {
	impl_->self = nullptr;
	// Dropping a stream on its own aborts it.
	// Not when its whole connection is going: that close covers it.
	auto conn = impl_->conn.lock();
	if (conn && conn->self) {
		impl_->core->loop.post([conn, id = impl_->id]() { conn->stream_abort(id); });
	}
}

bool Stream::send_message(const google::protobuf::MessageLite &msg) {
	std::vector<uint8_t> out;
	if (!encode_frame(msg, &out)) {
		return false;
	}
	send(out.data(), out.size());
	return true;
}

void Stream::send(const uint8_t *data, size_t len, bool fin) {
	auto conn = impl_->conn.lock();
	if (!conn || conn->terminated.load(std::memory_order_acquire) || (len == 0 && !fin)) {
		return;
	}
	std::vector<uint8_t> copy(data, data + len);
	impl_->core->loop.post([conn, id = impl_->id, copy = std::move(copy), fin]() mutable {
		conn->stream_send(id, std::move(copy), fin);
	});
}

void Stream::close() {
	auto conn = impl_->conn.lock();
	if (!conn) {
		return;
	}
	impl_->core->loop.post([conn, id = impl_->id]() { conn->stream_abort(id); });
}

// --- Connection ---

Connection::Connection(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {
	impl_->self = this;
}

Connection::~Connection() {
	impl_->self = nullptr;
	impl_->control_stream.reset();
	impl_->input_stream.reset();
	// The close shuts down with the code of the last close(), if any, so a
	// close(code) straight before this still puts `code` on the wire. The
	// task holds the connection until the network thread takes over.
	impl_->core->loop.post(
		[impl = impl_, code = impl_->close_code]() { impl->close_locally(code, now_ns()); });
}

ConnectionState Connection::state() const {
	return impl_->state;
}

const std::string &Connection::shutdown_reason() const {
	return impl_->shutdown_reason;
}

uint64_t Connection::shutdown_error_code() const {
	return impl_->shutdown_error_code;
}

std::string Connection::remote_host() const {
	return impl_->net.remote.host();
}

uint16_t Connection::remote_port() const {
	return impl_->net.remote.port();
}

const std::string &Connection::peer_certificate_sha256() const {
	return impl_->peer_cert_sha256;
}

bool Connection::peer_certificate_ca_verified() const {
	return impl_->peer_cert_ca_verified;
}

uint32_t Connection::rtt_us() const {
	return impl_->rtt_us.load(std::memory_order_relaxed);
}

uint16_t Connection::path_mtu() const {
	return impl_->path_mtu.load(std::memory_order_relaxed);
}

uint16_t Connection::max_datagram_size() const {
	return impl_->max_datagram_size.load(std::memory_order_relaxed);
}

Connection::DatagramStats Connection::datagram_stats() const {
	std::lock_guard<std::mutex> lock(impl_->datagram_mutex);
	DatagramStats stats = impl_->datagram_stats;
	uint64_t oldest = UINT64_MAX;
	if (!impl_->priority_queue.empty()) {
		oldest = impl_->priority_queue.front().enqueued_us;
	}
	if (!impl_->queue.empty()) {
		oldest = std::min(oldest, impl_->queue.front().enqueued_us);
	}
	if (oldest != UINT64_MAX) {
		stats.oldest_queued_age_us = monotonic_us() - oldest;
	}
	return stats;
}

namespace {

// Client-initiated bidirectional stream IDs go 0, 4, 8, ... in the order
// the streams are opened (the low 2 bits encode direction and initiator);
// gdp-spec.md §2.2 fixes 0 = control, 4 = input.
Stream *open_client_stream(Connection::Impl *impl, std::unique_ptr<Stream> &slot, int64_t id) {
	auto stream = std::make_shared<Stream::Impl>();
	stream->core = impl->core;
	stream->conn = impl->weak_self;
	stream->id = id;
	stream->weak_self = stream;
	slot = std::make_unique<Stream>(stream);
	impl->core->loop.post([conn = impl->weak_self.lock(), id, weak = std::weak_ptr<Stream::Impl>(stream)]() {
		conn->open_stream(id, weak);
	});
	return slot.get();
}

} // namespace

Stream *Connection::open_control_stream() {
	if (impl_->is_server || impl_->control_stream) {
		return nullptr;
	}
	return open_client_stream(impl_.get(), impl_->control_stream, 0);
}

Stream *Connection::open_input_stream() {
	if (impl_->is_server || !impl_->control_stream || impl_->input_stream) {
		return nullptr;
	}
	return open_client_stream(impl_.get(), impl_->input_stream, 4);
}

bool Connection::send_datagram(const uint8_t *data, size_t len, bool priority) {
	if (impl_->terminated.load(std::memory_order_acquire) ||
		impl_->datagrams_refused.load(std::memory_order_relaxed)) {
		return false;
	}
	// Refused up front: queued, it could only be dropped unseen once its
	// turn came.
	uint16_t max_len = impl_->max_datagram_size.load(std::memory_order_relaxed);
	if (max_len != 0 && len > max_len) {
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(impl_->datagram_mutex);
		auto &q = priority ? impl_->priority_queue : impl_->queue;
		q.push_back({std::vector<uint8_t>(data, data + len), monotonic_us()});
		impl_->datagram_stats.queued_datagrams++;
		impl_->datagram_stats.queued_bytes += len;
	}
	impl_->core->loop.wake();
	return true;
}

size_t Connection::pacing_burst_bytes(uint64_t bytes_per_s) {
	constexpr size_t kMin = 2 * 1500;
	constexpr size_t kMax = 24 * 1024;
	return (size_t)std::clamp<uint64_t>(bytes_per_s / 1000, kMin, kMax);
}

void Connection::set_pacing_rate(uint64_t bytes_per_s) {
	{
		std::lock_guard<std::mutex> lock(impl_->datagram_mutex);
		if (bytes_per_s == impl_->pacing_bytes_per_s) {
			return;
		}
		if (impl_->pacing_bytes_per_s == 0) {
			// Starting (again): a full bucket, so the first frame isn't held.
			impl_->pacing_tokens = pacing_burst_bytes(bytes_per_s);
			impl_->pacing_refilled_us = monotonic_us();
		} else {
			impl_->refill_pacing_tokens(monotonic_us());
		}
		impl_->pacing_bytes_per_s = bytes_per_s;
		impl_->pacing_tokens = std::min(impl_->pacing_tokens, (double)pacing_burst_bytes(bytes_per_s));
	}
	impl_->core->loop.wake();
}

void Connection::set_keep_alive(uint32_t interval_ms) {
	impl_->core->loop.post([impl = impl_, interval_ms]() {
		if (impl->net.conn) {
			ngtcp2_conn_set_keep_alive_timeout(impl->net.conn,
				interval_ms ? interval_ms * NGTCP2_MILLISECONDS : UINT64_MAX);
		}
	});
}

void Connection::close(uint64_t error_code) {
	impl_->close_code = error_code;
	impl_->core->loop.post([impl = impl_, error_code]() { impl->close_locally(error_code, now_ns()); });
}

// --- Client endpoint ---

namespace {

// A client connection and the connected socket only it uses.
class ClientEndpoint : public NetLoop::Endpoint {
public:
	ClientEndpoint(std::shared_ptr<Connection::Impl> conn, NetLoop *loop)
		: conn_(std::move(conn)), loop_(loop) {}
	~ClientEndpoint() override { conn_->net.socket = nullptr; }

	UdpSocket socket;
	TlsContext tls;

	socket_t fd() const override { return socket.fd(); }
	bool wants_write() const override { return conn_->wants_write(); }
	void on_readable(uint64_t now) override {
		int err = socket.receive(256, [&](const UdpSocket::Packet &pkt) { conn_->read_packet(pkt, now); });
		if (err) {
			conn_->socket_error(err, now);
		}
	}
	void service(uint64_t now) override {
		conn_->service(now);
		if (conn_->finished(now)) {
			loop_->remove(this);
		}
	}
	uint64_t next_deadline() const override { return conn_->next_deadline(); }

private:
	std::shared_ptr<Connection::Impl> conn_;
	NetLoop *loop_;
};

// Network thread: resolves `host`, then starts the handshake. A failure
// reaches the host as an ordinary shutdown.
void start_connect(const std::shared_ptr<Connection::Impl> &conn, const std::string &host, uint16_t port,
	const std::string &alpn, const CaTrust &ca, CongestionControl cc) {
	uint64_t now = now_ns();
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo *results = nullptr;
	std::string port_str = std::to_string(port);
	if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &results) != 0 || !results) {
		conn->terminate("host not found", 0, 0, now);
		return;
	}
	auto endpoint = std::make_shared<ClientEndpoint>(conn, &conn->core->loop);
	bool connected = false;
	for (addrinfo *ai = results; ai; ai = ai->ai_next) {
		conn->net.remote.set(ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
		if (endpoint->socket.connect_to(conn->net.remote)) {
			connected = true;
			break;
		}
	}
	freeaddrinfo(results);
	if (!connected) {
		conn->terminate("host unreachable", 0, 0, now);
		return;
	}
	if (!endpoint->tls.init_client(alpn, ca) ||
		!conn->start_client(&endpoint->socket, host, endpoint->tls, cc)) {
		conn->terminate("TLS error", 0, 0, now);
		return;
	}
	conn->core->loop.add(endpoint);
}

} // namespace

// --- Transport ---

struct Transport::Impl {
	std::shared_ptr<TransportCore> core = std::make_shared<TransportCore>();
	std::vector<std::shared_ptr<AcceptTarget>> accept_targets;
};

Transport::Transport() : impl_(std::make_unique<Impl>()) {
	net_init();
}

Transport::~Transport() {
	// Sends whatever closes are already asked for, then stops. Nothing
	// waits out a closing period: the peer hears the CONNECTION_CLOSE, and
	// if that's lost, its idle timeout.
	impl_->core->loop.stop();
	impl_->core->events.clear();
}

void Transport::set_congestion_control(CongestionControl cc) {
	impl_->core->congestion_control = cc;
}

int Transport::notify_fd() const {
	return impl_->core->events.fd();
}

size_t Transport::dispatch() {
	return impl_->core->events.drain();
}

std::unique_ptr<Connection> Transport::connect(const std::string &host, uint16_t port,
	const std::string &alpn, const CaTrust &ca) {
	auto conn = std::make_shared<Connection::Impl>(impl_->core, false);
	conn->weak_self = conn;
	auto wrapper = std::make_unique<Connection>(conn);
	impl_->core->loop.post([conn, host, port, alpn, ca, cc = impl_->core->congestion_control]() {
		start_connect(conn, host, port, alpn, ca, cc);
	});
	return wrapper;
}

bool Transport::listen(uint16_t port, const std::string &alpn, const std::string &cert_file,
	const std::string &key_file, NewConnectionHandler on_new_connection, bool *address_in_use) {
	auto target = std::make_shared<AcceptTarget>();
	target->on_new_connection = std::move(on_new_connection);
	auto demux = std::make_shared<ServerDemux>(impl_->core, target, impl_->core->congestion_control);
	if (!demux->listen(port, alpn, cert_file, key_file, address_in_use)) {
		return false;
	}
	impl_->accept_targets.push_back(std::move(target));
	impl_->core->loop.post([core = impl_->core.get(), demux]() { core->loop.add(demux); });
	return true;
}

} // namespace gdp
