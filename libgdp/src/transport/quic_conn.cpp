// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "quic_conn.hpp"

#include "gdp/clock.hpp"
#include "gdp/error_codes.hpp"
#include "gdp/version.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gdp {

namespace {

constexpr uint64_t kIdleTimeout = 30 * NGTCP2_SECONDS;
// No reply to the handshake for this long is "timed out".
constexpr uint64_t kHandshakeTimeout = 10 * NGTCP2_SECONDS;
// Flow control. ngtcp2 grows each window from the initial size towards its
// maximum as the peer uses it up.
constexpr uint64_t kStreamWindow = 1 << 20;
constexpr uint64_t kMaxStreamWindow = 16 << 20;
constexpr uint64_t kConnWindow = 4 << 20;
constexpr uint64_t kMaxConnWindow = 32 << 20;
// Every GDP peer accepts datagram frames this large.
constexpr uint64_t kMaxDatagramFrameSize = 65535;
// Path MTU discovery's probes, as IP MTUs, largest first so a clean
// 1500-byte path confirms with one probe. The UDP payload each one means
// depends on the IP header (see probes_for()).
constexpr uint16_t kMtuProbes[] = {1500, 1492, 1454, 1420, 1400, 1360, 1280};
// A datagram ngtcp2 declared lost stays tracked this long in case its ack
// turns up after all (a spurious loss), then is forgotten.
constexpr uint64_t kLostDatagramMemory = 2 * NGTCP2_SECONDS;

// The words for each shutdown cause (see describe_ccerr()), as spectre
// shows them.
const char *const kNoResponse = "timed out (no response from host)";

size_t ip_udp_overhead(bool ipv4) {
	return ipv4 ? 20 + 8 : 40 + 8;
}

const uint16_t *probes_for(bool ipv4) {
	static const auto make = [](size_t overhead) {
		std::array<uint16_t, std::size(kMtuProbes)> probes{};
		for (size_t i = 0; i < probes.size(); i++) {
			probes[i] = static_cast<uint16_t>(kMtuProbes[i] - overhead);
		}
		return probes;
	};
	static const auto v4 = make(ip_udp_overhead(true));
	static const auto v6 = make(ip_udp_overhead(false));
	return ipv4 ? v4.data() : v6.data();
}

void random_bytes(uint8_t *dest, size_t len) {
	RAND_bytes(dest, static_cast<int>(len));
}

void rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *) {
	random_bytes(dest, destlen);
}

// GDP_QUIC_LOG=1 in the environment: ngtcp2's own trace, to stderr.
bool quic_log_enabled() {
	static const bool enabled = getenv("GDP_QUIC_LOG") != nullptr;
	return enabled;
}

void log_printf(void *, const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	fputs("gdp quic: ", stderr);
	vfprintf(stderr, format, ap);
	fputc('\n', stderr);
	va_end(ap);
}

ngtcp2_conn *get_conn(ngtcp2_crypto_conn_ref *ref) {
	return static_cast<Connection::Impl *>(ref->user_data)->net.conn;
}

// A CONNECTION_CLOSE, received or our own, in the words shutdown_reason()
// has always used. Application closes are handled by the caller.
std::string describe_ccerr(const ngtcp2_ccerr &err) {
	switch (err.type) {
	case NGTCP2_CCERR_TYPE_TRANSPORT:
		if (err.error_code == NGTCP2_NO_ERROR) {
			return std::string();
		}
		if ((err.error_code & ~uint64_t{0xff}) == NGTCP2_CRYPTO_ERROR) {
			uint8_t alert = static_cast<uint8_t>(err.error_code & 0xff);
			switch (alert) {
			case 40: // handshake_failure
				return "handshake failed";
			case 120: // no_application_protocol
				return std::string("ALPN negotiation failed (peer doesn't speak ") + kAlpn + ")";
			case 42: // bad_certificate
			case 43: // unsupported_certificate
			case 44: // certificate_revoked
			case 45: // certificate_expired
			case 46: // certificate_unknown
			case 48: // unknown_ca
				return "bad TLS certificate";
			default: return "TLS error";
			}
		}
		{
			char buf[64];
			snprintf(buf, sizeof(buf), "connection error (transport error 0x%llx)",
				(unsigned long long)err.error_code);
			return buf;
		}
	case NGTCP2_CCERR_TYPE_VERSION_NEGOTIATION: return "QUIC version negotiation failed";
	default: return std::string();
	}
}

SockAddr to_sockaddr(const ngtcp2_addr &addr) {
	SockAddr sa;
	if (addr.addr && addr.addrlen) {
		sa.set(addr.addr, addr.addrlen);
	}
	return sa;
}

ngtcp2_path make_path(SockAddr &local, SockAddr &remote) {
	ngtcp2_path path{};
	path.local.addr = local.get();
	path.local.addrlen = local.len;
	path.remote.addr = remote.get();
	path.remote.addrlen = remote.len;
	return path;
}

void make_settings(ngtcp2_settings *settings, bool ipv4, CongestionControl cc, uint64_t now) {
	ngtcp2_settings_default(settings);
	settings->initial_ts = now;
	settings->cc_algo = cc == CongestionControl::kBbr ? NGTCP2_CC_ALGO_BBR : NGTCP2_CC_ALGO_CUBIC;
	settings->max_tx_udp_payload_size = kMtuProbes[0] - ip_udp_overhead(ipv4);
	settings->pmtud_probes = probes_for(ipv4);
	settings->pmtud_probeslen = std::size(kMtuProbes);
	settings->handshake_timeout = kHandshakeTimeout;
	settings->max_window = kMaxConnWindow;
	settings->max_stream_window = kMaxStreamWindow;
	if (quic_log_enabled()) {
		settings->log_printf = log_printf;
	}
}

void make_params(ngtcp2_transport_params *params, bool is_server) {
	ngtcp2_transport_params_default(params);
	params->initial_max_data = kConnWindow;
	params->initial_max_stream_data_bidi_local = kStreamWindow;
	params->initial_max_stream_data_bidi_remote = kStreamWindow;
	// Server: gdp-spec.md §2.2's two client-opened streams (control, input);
	// the lobby uses one. A client accepts none.
	params->initial_max_streams_bidi = is_server ? 2 : 0;
	params->initial_max_streams_uni = 0;
	params->max_idle_timeout = kIdleTimeout;
	params->max_datagram_frame_size = kMaxDatagramFrameSize;
	params->max_udp_payload_size = kMaxUdpPayload;
}

} // namespace

void generate_server_cid(ngtcp2_cid *cid) {
	cid->datalen = kCidLen;
	random_bytes(cid->data, kCidLen);
}

// --- ngtcp2 callbacks ---

struct ConnCallbacks {
	using Impl = Connection::Impl;

	static Impl *impl(void *user_data) { return static_cast<Impl *>(user_data); }

	static int get_new_connection_id(ngtcp2_conn *, ngtcp2_cid *cid, uint8_t *token, size_t cidlen,
		void *ud) {
		Impl *c = impl(ud);
		if (c->is_server) {
			generate_server_cid(cid);
			ngtcp2_crypto_generate_stateless_reset_token(token, c->net.static_secret,
				c->net.static_secret_len, cid);
			if (c->net.cids) {
				c->net.cids->add_cid(*cid, c);
			}
		} else {
			cid->datalen = cidlen;
			random_bytes(cid->data, cidlen);
			random_bytes(token, NGTCP2_STATELESS_RESET_TOKENLEN);
		}
		return 0;
	}

	static int remove_connection_id(ngtcp2_conn *, const ngtcp2_cid *cid, void *ud) {
		Impl *c = impl(ud);
		if (c->net.cids) {
			c->net.cids->remove_cid(*cid);
		}
		return 0;
	}

	static int handshake_completed(ngtcp2_conn *, void *ud) {
		Impl *c = impl(ud);
		c->net.handshake_completed = true;
		c->update_path_figures();
		if (c->is_server) {
			if (c->net.on_handshake) {
				c->net.on_handshake();
			}
		} else {
			// Pinned before the application sends anything
			// (ClientConnection checks it at kConnected), so it's enough to
			// capture it here, without failing the handshake over it.
			std::string fingerprint = c->net.tls.peer_certificate_sha256();
			bool ca_verified = c->net.tls.peer_certificate_ca_verified();
			c->core->events.post([weak = c->weak_self, fingerprint = std::move(fingerprint), ca_verified]() {
				auto impl = weak.lock();
				if (impl) {
					impl->peer_cert_sha256 = fingerprint;
					impl->peer_cert_ca_verified = ca_verified;
				}
			});
		}
		c->post_state(ConnectionState::kConnected);
		return 0;
	}

	static int recv_stream_data(ngtcp2_conn *conn, uint32_t, int64_t stream_id, uint64_t, const uint8_t *data,
		size_t datalen, void *ud, void *) {
		Impl *c = impl(ud);
		// Consumed straight away (copied out for dispatch()), so the credit
		// goes straight back.
		ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
		ngtcp2_conn_extend_max_offset(conn, datalen);
		if (datalen == 0) {
			return 0;
		}
		auto it = c->net.streams.find(stream_id);
		if (it == c->net.streams.end()) {
			return 0;
		}
		std::vector<uint8_t> copy(data, data + datalen);
		c->core->events.post([weak = it->second.stream, copy = std::move(copy)]() {
			auto stream = weak.lock();
			if (!stream || !stream->self) {
				return;
			}
			auto on_data = stream->self->on_data;
			if (on_data) {
				on_data(copy.data(), copy.size());
			}
		});
		return 0;
	}

	static int acked_stream_data_offset(ngtcp2_conn *, int64_t stream_id, uint64_t offset, uint64_t datalen,
		void *ud, void *) {
		Impl *c = impl(ud);
		auto it = c->net.streams.find(stream_id);
		if (it == c->net.streams.end()) {
			return 0;
		}
		Impl::NetStream &s = it->second;
		uint64_t acked_end = offset + datalen;
		while (!s.chunks.empty() && s.chunks_offset + s.chunks.front().size() <= acked_end) {
			s.chunks_offset += s.chunks.front().size();
			s.chunks.pop_front();
		}
		return 0;
	}

	// Server: the client opened a stream. Identified by QUIC stream ID (0 =
	// control, 4 = input per gdp-spec.md §2.2) rather than arrival order,
	// which QUIC doesn't guarantee matches the order they were opened in.
	static int stream_open(ngtcp2_conn *, int64_t stream_id, void *ud) {
		Impl *c = impl(ud);
		if (!c->is_server || (stream_id != 0 && stream_id != 4)) {
			// gdp-spec.md §2.2, "Additional streams": no stream kind is
			// defined yet, so refuse it (RESET_STREAM + STOP_SENDING with
			// UNSUPPORTED) instead of leaving the peer's data unread.
			ngtcp2_conn_shutdown_stream(c->net.conn, 0, stream_id,
				static_cast<uint64_t>(ErrorCode::kUnsupported));
			return 0;
		}
		bool is_control = stream_id == 0;
		auto stream = std::make_shared<Stream::Impl>();
		stream->core = c->core;
		stream->conn = c->weak_self;
		stream->id = stream_id;
		stream->weak_self = stream;
		Impl::NetStream &ns = c->net.streams[stream_id];
		ns.stream = stream;
		ns.opened = true;
		ns.any_sent = true; // the peer's stream: nothing of ours to announce
		// The public Stream is made on the host thread; until then this
		// closure is what keeps its Impl alive.
		c->core->events.post([weak = c->weak_self, stream, is_control]() {
			auto impl = weak.lock();
			if (!impl || !impl->self) {
				return;
			}
			auto &slot = is_control ? impl->control_stream : impl->input_stream;
			if (slot) {
				return;
			}
			slot = std::make_unique<Stream>(stream);
			auto handler = is_control ? impl->self->on_control_stream : impl->self->on_input_stream;
			if (handler) {
				handler(slot.get());
			}
		});
		return 0;
	}

	static int extend_max_local_streams_bidi(ngtcp2_conn *, uint64_t, void *ud) {
		impl(ud)->try_open_pending_streams();
		return 0;
	}

	static int recv_datagram(ngtcp2_conn *, uint32_t, const uint8_t *data, size_t datalen, void *ud) {
		Impl *c = impl(ud);
		uint64_t arrival_us = monotonic_us();
		std::vector<uint8_t> copy(data, data + datalen);
		c->core->events.post([weak = c->weak_self, copy = std::move(copy), arrival_us]() {
			auto impl = weak.lock();
			if (!impl || !impl->self) {
				return;
			}
			auto on_datagram = impl->self->on_datagram;
			if (on_datagram) {
				on_datagram(copy.data(), copy.size(), arrival_us);
			}
		});
		return 0;
	}

	static int ack_datagram(ngtcp2_conn *, uint64_t dgram_id, void *ud) {
		Impl *c = impl(ud);
		auto it = c->net.dgrams_in_flight.find(dgram_id);
		if (it == c->net.dgrams_in_flight.end()) {
			return 0;
		}
		{
			// Includes acks for ones declared lost first: the loss was
			// spurious, but it stays counted as a loss too.
			std::lock_guard<std::mutex> lock(c->datagram_mutex);
			c->datagram_stats.acked_datagrams++;
			c->datagram_stats.acked_bytes += it->second.len;
		}
		c->net.dgrams_in_flight.erase(it);
		return 0;
	}

	static int lost_datagram(ngtcp2_conn *, uint64_t dgram_id, void *ud) {
		Impl *c = impl(ud);
		auto it = c->net.dgrams_in_flight.find(dgram_id);
		if (it == c->net.dgrams_in_flight.end() || it->second.lost) {
			return 0;
		}
		it->second.lost = true;
		std::lock_guard<std::mutex> lock(c->datagram_mutex);
		c->datagram_stats.lost_datagrams++;
		c->datagram_stats.lost_bytes += it->second.len;
		return 0;
	}

	static int recv_stateless_reset(ngtcp2_conn *, const ngtcp2_pkt_stateless_reset *, void *ud) {
		impl(ud)->net.stateless_reset = true;
		return 0;
	}

	static ngtcp2_callbacks make(bool is_server) {
		ngtcp2_callbacks cb{};
		if (is_server) {
			cb.recv_client_initial = ngtcp2_crypto_recv_client_initial_cb;
		} else {
			cb.client_initial = ngtcp2_crypto_client_initial_cb;
			cb.recv_retry = ngtcp2_crypto_recv_retry_cb;
			cb.extend_max_local_streams_bidi = extend_max_local_streams_bidi;
			cb.recv_stateless_reset = recv_stateless_reset;
		}
		cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
		cb.encrypt = ngtcp2_crypto_encrypt_cb;
		cb.decrypt = ngtcp2_crypto_decrypt_cb;
		cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
		cb.update_key = ngtcp2_crypto_update_key_cb;
		cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
		cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
		cb.get_path_challenge_data = ngtcp2_crypto_get_path_challenge_data_cb;
		cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
		cb.rand = rand_cb;
		cb.get_new_connection_id = get_new_connection_id;
		cb.remove_connection_id = remove_connection_id;
		cb.handshake_completed = handshake_completed;
		cb.recv_stream_data = recv_stream_data;
		cb.acked_stream_data_offset = acked_stream_data_offset;
		cb.stream_open = stream_open;
		cb.recv_datagram = recv_datagram;
		cb.ack_datagram = ack_datagram;
		cb.lost_datagram = lost_datagram;
		return cb;
	}
};

// --- Connection::Impl ---

Connection::Impl::Impl(std::shared_ptr<TransportCore> core_, bool is_server_)
	: core(std::move(core_)), is_server(is_server_) {
	net.conn_ref.get_conn = get_conn;
	net.conn_ref.user_data = this;
}

Connection::Impl::~Impl() {
	if (net.conn) {
		ngtcp2_conn_del(net.conn);
	}
}

bool Connection::Impl::start_client(UdpSocket *socket, const std::string &server_name,
	const TlsContext &tls_ctx, CongestionControl cc) {
	net.socket = socket;
	net.local = socket->local();
	uint64_t now = now_ns();

	ngtcp2_cid dcid;
	ngtcp2_cid scid;
	dcid.datalen = kCidLen;
	random_bytes(dcid.data, dcid.datalen);
	scid.datalen = kCidLen;
	random_bytes(scid.data, scid.datalen);

	ngtcp2_callbacks callbacks = ConnCallbacks::make(false);
	ngtcp2_settings settings;
	make_settings(&settings, net.remote.is_ipv4(), cc, now);
	ngtcp2_transport_params params;
	make_params(&params, false);
	ngtcp2_path path = make_path(net.local, net.remote);
	int rv = ngtcp2_conn_client_new(&net.conn, &dcid, &scid, &path, NGTCP2_PROTO_VER_V1, &callbacks,
		&settings, &params, nullptr, this);
	if (rv != 0) {
		fprintf(stderr, "gdp: ngtcp2_conn_client_new failed: %s\n", ngtcp2_strerror(rv));
		net.conn = nullptr;
		return false;
	}
	if (!net.tls.init_client(tls_ctx, &net.conn_ref, server_name)) {
		return false;
	}
	ngtcp2_conn_set_tls_native_handle(net.conn, net.tls.native_handle());
	return true;
}

bool Connection::Impl::start_server(UdpSocket *socket, const UdpSocket::Packet &first,
	const ngtcp2_pkt_hd &hd, const ngtcp2_cid *odcid, ngtcp2_token_type token_type, const TlsContext &tls_ctx,
	CidRegistry *cids, const uint8_t *static_secret, size_t static_secret_len, CongestionControl cc,
	uint64_t now) {
	net.socket = socket;
	net.cids = cids;
	net.static_secret = static_secret;
	net.static_secret_len = static_secret_len;
	net.local = first.local;
	net.remote = first.remote;

	ngtcp2_cid scid;
	generate_server_cid(&scid);

	ngtcp2_callbacks callbacks = ConnCallbacks::make(true);
	ngtcp2_settings settings;
	make_settings(&settings, net.remote.is_ipv4(), cc, now);
	settings.token = hd.token;
	settings.tokenlen = hd.tokenlen;
	settings.token_type = token_type;
	ngtcp2_transport_params params;
	make_params(&params, true);
	if (odcid) {
		params.original_dcid = *odcid;
		params.retry_scid = hd.dcid;
		params.retry_scid_present = 1;
	} else {
		params.original_dcid = hd.dcid;
	}
	params.original_dcid_present = 1;
	params.stateless_reset_token_present = 1;
	ngtcp2_crypto_generate_stateless_reset_token(params.stateless_reset_token, static_secret,
		static_secret_len, &scid);

	ngtcp2_path path = make_path(net.local, net.remote);
	int rv = ngtcp2_conn_server_new(&net.conn, &hd.scid, &scid, &path, hd.version, &callbacks, &settings,
		&params, nullptr, this);
	if (rv != 0) {
		fprintf(stderr, "gdp: ngtcp2_conn_server_new failed: %s\n", ngtcp2_strerror(rv));
		net.conn = nullptr;
		return false;
	}
	cids->add_cid(scid, this);
	if (!net.tls.init_server(tls_ctx, &net.conn_ref)) {
		return false;
	}
	ngtcp2_conn_set_tls_native_handle(net.conn, net.tls.native_handle());
	return true;
}

void Connection::Impl::post_state(ConnectionState new_state) {
	core->events.post([weak = weak_self, new_state]() {
		auto impl = weak.lock();
		if (!impl || !impl->self) {
			return;
		}
		impl->state = new_state;
		auto on_state_changed = impl->self->on_state_changed;
		if (on_state_changed) {
			on_state_changed(new_state);
		}
	});
}

void Connection::Impl::terminate(const std::string &reason, uint64_t peer_code, uint64_t linger_ns,
	uint64_t now) {
	if (net.linger_until) {
		return;
	}
	terminated.store(true, std::memory_order_release);
	net.linger_until = now + std::max<uint64_t>(linger_ns, 1);
	drop_queued_datagrams();
	core->events.post([weak = weak_self, reason, peer_code]() {
		auto impl = weak.lock();
		if (!impl) {
			return;
		}
		impl->shutdown_reason = reason;
		impl->shutdown_error_code = peer_code;
	});
	post_state(ConnectionState::kShutdown);
}

void Connection::Impl::fail(const ngtcp2_ccerr &err, const std::string &reason, uint64_t now) {
	if (net.linger_until || !net.conn) {
		return;
	}
	uint8_t buf[kMaxUdpPayload];
	ngtcp2_path_storage ps;
	ngtcp2_path_storage_zero(&ps);
	ngtcp2_pkt_info pi;
	ngtcp2_ssize n = ngtcp2_conn_write_connection_close(net.conn, &ps.path, &pi, buf, sizeof(buf), &err, now);
	if (n > 0) {
		net.close_packet.assign(buf, buf + n);
		send_packet(buf, static_cast<size_t>(n), ps.path);
	}
	terminate(reason, 0, 3 * ngtcp2_conn_get_pto(net.conn), now);
}

void Connection::Impl::close_locally(uint64_t error_code, uint64_t now) {
	ngtcp2_ccerr err;
	ngtcp2_ccerr_default(&err);
	ngtcp2_ccerr_set_application_error(&err, error_code, nullptr, 0);
	// A local close explains itself: no reason.
	fail(err, std::string(), now);
}

void Connection::Impl::socket_error(int err, uint64_t now) {
	// An ICMP error only ends a connection whose handshake hasn't finished:
	// past that point one could be spoofed to kill a
	// live session, and a real outage shows up as the idle timeout anyway.
	if (net.handshake_completed || net.linger_until) {
		return;
	}
	switch (err) {
	case ECONNREFUSED: terminate("connection refused", 0, 0, now); break;
	case EHOSTUNREACH:
	case ENETUNREACH: terminate("host unreachable", 0, 0, now); break;
	default: break;
	}
}

bool Connection::Impl::read_packet(const UdpSocket::Packet &pkt, uint64_t now) {
	if (!net.conn) {
		return false;
	}
	if (pkt.len == 0) {
		return true; // see ServerDemux::handle_packet()
	}
	if (net.linger_until) {
		// Closing period: answer what still arrives with our
		// CONNECTION_CLOSE again, in case the first one was lost -- but
		// only the 1st, 2nd, 4th, 8th... packet, the backoff RFC 9000
		// §10.2.1 suggests, so a peer can't draw one per packet.
		uint64_t n = ++net.packets_while_closing;
		if (!net.close_packet.empty() && (n & (n - 1)) == 0) {
			ngtcp2_path path = make_path(net.local, net.remote);
			send_packet(net.close_packet.data(), net.close_packet.size(), path);
		}
		return false;
	}
	SockAddr local = pkt.local;
	SockAddr remote = pkt.remote.len ? pkt.remote : net.remote;
	ngtcp2_path path = make_path(local, remote);
	ngtcp2_pkt_info pi{};
	int rv = ngtcp2_conn_read_pkt(net.conn, &path, &pi, pkt.data, pkt.len, now);
	if (rv == 0) {
		return true;
	}
	switch (rv) {
	case NGTCP2_ERR_DRAINING: {
		// The peer closed. Its application error code is gdp-spec.md §12's
		// table (error_codes.hpp) -- 0 is a normal close (already explained
		// by whatever protocol message preceded it: LobbyError, Redirect,
		// SessionReject), so shutdown_reason stays empty for that; any
		// other code is the peer's only way to say why it hung up without
		// a protocol message (auth failure, a frame violation).
		const ngtcp2_ccerr *err = ngtcp2_conn_get_ccerr(net.conn);
		uint64_t linger = 3 * ngtcp2_conn_get_pto(net.conn);
		if (net.stateless_reset) {
			terminate("connection error (Operation canceled)", 0, linger, now);
		} else if (err->type == NGTCP2_CCERR_TYPE_APPLICATION) {
			std::string reason =
				err->error_code ? "closed by peer: " + describe_error_code(err->error_code) : "";
			terminate(reason, err->error_code, linger, now);
		} else {
			terminate(describe_ccerr(*err), 0, linger, now);
		}
		return false;
	}
	case NGTCP2_ERR_DROP_CONN:
	case NGTCP2_ERR_RETRY:
		// Server, mid-handshake: the packet can't start this connection.
		// Nobody has seen it yet; let it go without a word.
		terminate(std::string(), 0, 0, now);
		return false;
	case NGTCP2_ERR_CRYPTO: {
		ngtcp2_ccerr err;
		ngtcp2_ccerr_default(&err);
		ngtcp2_ccerr_set_tls_alert(&err, ngtcp2_conn_get_tls_alert(net.conn), nullptr, 0);
		fail(err, describe_ccerr(err), now);
		return false;
	}
	default: {
		ngtcp2_ccerr err;
		ngtcp2_ccerr_default(&err);
		ngtcp2_ccerr_set_liberr(&err, rv, nullptr, 0);
		fail(err, std::string("connection error (") + ngtcp2_strerror(rv) + ")", now);
		return false;
	}
	}
}

void Connection::Impl::service(uint64_t now) {
	if (!net.conn || net.linger_until) {
		return;
	}
	if (now >= ngtcp2_conn_get_expiry(net.conn)) {
		int rv = ngtcp2_conn_handle_expiry(net.conn, now);
		if (rv == NGTCP2_ERR_IDLE_CLOSE) {
			// Silent, as the idle timeout is: nothing goes on the wire.
			// Before the handshake completes that means nobody answered.
			terminate(net.handshake_completed ? "connection error (Timer expired)" : kNoResponse, 0, 0, now);
			return;
		}
		if (rv == NGTCP2_ERR_HANDSHAKE_TIMEOUT) {
			terminate(kNoResponse, 0, 0, now);
			return;
		}
		if (rv != 0) {
			ngtcp2_ccerr err;
			ngtcp2_ccerr_default(&err);
			ngtcp2_ccerr_set_liberr(&err, rv, nullptr, 0);
			fail(err, std::string("connection error (") + ngtcp2_strerror(rv) + ")", now);
			return;
		}
	}
	flush(now);

	if (now >= net.next_prune_ns) {
		net.next_prune_ns = now + kLostDatagramMemory;
		for (auto it = net.dgrams_in_flight.begin(); it != net.dgrams_in_flight.end();) {
			if (it->second.lost && now - it->second.sent_ns > kLostDatagramMemory) {
				it = net.dgrams_in_flight.erase(it);
			} else {
				++it;
			}
		}
	}
}

uint64_t Connection::Impl::next_deadline() const {
	if (!net.conn) {
		return UINT64_MAX;
	}
	if (net.linger_until) {
		return net.linger_until;
	}
	return std::min(ngtcp2_conn_get_expiry(net.conn), net.pacer_wakeup);
}

bool Connection::Impl::finished(uint64_t now) const {
	return net.linger_until && now >= net.linger_until;
}

void Connection::Impl::update_path_figures() {
	if (!net.conn || !net.handshake_completed) {
		return;
	}
	ngtcp2_conn_info info;
	ngtcp2_conn_get_conn_info(net.conn, &info);
	rtt_us.store(static_cast<uint32_t>(info.smoothed_rtt / NGTCP2_MICROSECONDS), std::memory_order_relaxed);

	size_t payload = ngtcp2_conn_get_path_max_tx_udp_payload_size(net.conn);
	path_mtu.store(static_cast<uint16_t>(payload + ip_udp_overhead(net.remote.is_ipv4())),
		std::memory_order_relaxed);

	const ngtcp2_transport_params *remote = ngtcp2_conn_get_remote_transport_params(net.conn);
	if (!remote || remote->max_datagram_frame_size == 0) {
		datagrams_refused.store(true, std::memory_order_relaxed);
		max_datagram_size.store(0, std::memory_order_relaxed);
		return;
	}
	// A 1-RTT packet around one DATAGRAM frame, at its largest: the short
	// header byte, the peer's connection ID, a 4-byte packet number, the
	// AEAD tag, and the frame's type byte plus a 2-byte length.
	constexpr size_t kFrameHeader = 1 + 2;
	size_t overhead = 1 + ngtcp2_conn_get_dcid(net.conn)->datalen + 4 + 16 + kFrameHeader;
	size_t max = payload > overhead ? payload - overhead : 0;
	if (remote->max_datagram_frame_size < max + kFrameHeader) {
		max = remote->max_datagram_frame_size > kFrameHeader ? remote->max_datagram_frame_size - kFrameHeader
															 : 0;
	}
	max_datagram_size.store(static_cast<uint16_t>(std::min<size_t>(max, UINT16_MAX)),
		std::memory_order_relaxed);
}

void Connection::Impl::refill_pacing_tokens(uint64_t now_us) {
	double earned = (double)pacing_bytes_per_s * (double)(now_us - pacing_refilled_us) / 1e6;
	pacing_tokens =
		std::min(pacing_tokens + earned, (double)Connection::pacing_burst_bytes(pacing_bytes_per_s));
	pacing_refilled_us = now_us;
}

void Connection::Impl::drop_queued_datagrams() {
	std::lock_guard<std::mutex> lock(datagram_mutex);
	datagram_stats.canceled_datagrams += priority_queue.size() + queue.size();
	datagram_stats.queued_datagrams = 0;
	datagram_stats.queued_bytes = 0;
	priority_queue.clear();
	queue.clear();
}

// --- Streams (network thread) ---

void Connection::Impl::open_stream(int64_t id, std::weak_ptr<Stream::Impl> stream) {
	if (!net.conn || net.linger_until) {
		return;
	}
	net.streams[id].stream = std::move(stream);
	net.pending_open.push_back(id);
	try_open_pending_streams();
}

void Connection::Impl::try_open_pending_streams() {
	while (!net.pending_open.empty()) {
		int64_t id;
		int rv = ngtcp2_conn_open_bidi_stream(net.conn, &id, nullptr);
		if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED) {
			return; // extend_max_local_streams_bidi calls back here
		}
		int64_t want = net.pending_open.front();
		net.pending_open.pop_front();
		if (rv != 0) {
			net.streams.erase(want);
			continue;
		}
		// Stream IDs go 0, 4, 8, ... in the order streams are opened, and
		// the host numbered them in that same order (open_*_stream()).
		net.streams[want].opened = true;
		(void)id;
	}
}

void Connection::Impl::stream_send(int64_t id, std::vector<uint8_t> data, bool fin) {
	if (net.linger_until) {
		return;
	}
	auto it = net.streams.find(id);
	if (it == net.streams.end() || it->second.fin_requested) {
		return;
	}
	NetStream &s = it->second;
	if (!data.empty()) {
		s.end += data.size();
		s.chunks.push_back(std::move(data));
	}
	if (fin) {
		s.fin_requested = true;
	}
}

void Connection::Impl::stream_abort(int64_t id) {
	if (!net.conn || net.linger_until) {
		return;
	}
	auto it = net.streams.find(id);
	if (it == net.streams.end()) {
		return;
	}
	if (it->second.opened) {
		ngtcp2_conn_shutdown_stream(net.conn, 0, id, 0);
	}
	net.streams.erase(it);
}

// --- Writing ---

bool Connection::Impl::send_packet(const uint8_t *data, size_t len, const ngtcp2_path &path) {
	if (!net.socket) {
		return true;
	}
	SockAddr local = to_sockaddr(path.local);
	SockAddr remote = path.remote.addrlen ? to_sockaddr(path.remote) : net.remote;
	int err = net.socket->send(data, len, local.len ? &local : nullptr, &remote);
	if (err == 0) {
		return true;
	}
	if (err == EAGAIN || err == EWOULDBLOCK || err == ENOBUFS) {
		net.blocked_packet.assign(data, data + len);
		net.blocked_local = local;
		net.blocked_remote = remote;
		return false;
	}
	if (!is_server && (err == ECONNREFUSED || err == EHOSTUNREACH || err == ENETUNREACH)) {
		net.pending_socket_error = err;
	}
	// Anything else (EMSGSIZE for a PMTU probe past the local interface's
	// MTU, a transient route error) loses the packet, which QUIC recovers.
	return true;
}

bool Connection::Impl::send_blocked_packet() {
	int err = net.socket->send(net.blocked_packet.data(), net.blocked_packet.size(),
		net.blocked_local.len ? &net.blocked_local : nullptr, &net.blocked_remote);
	if (err == EAGAIN || err == EWOULDBLOCK || err == ENOBUFS) {
		return false;
	}
	net.blocked_packet.clear();
	return true;
}

void Connection::Impl::flush(uint64_t now) {
	if (!net.blocked_packet.empty() && !send_blocked_packet()) {
		return;
	}
	update_path_figures();
	net.pacer_wakeup = UINT64_MAX;
	for (auto &entry : net.streams) {
		entry.second.blocked = false;
	}

	uint8_t buf[kMaxUdpPayload];
	size_t max_payload = ngtcp2_conn_get_path_max_tx_udp_payload_size(net.conn);
	size_t max_packets = std::max<size_t>(1, ngtcp2_conn_get_send_quantum(net.conn) / max_payload);
	size_t written = 0;
	ngtcp2_path_storage ps;
	ngtcp2_path_storage_zero(&ps);
	ngtcp2_pkt_info pi;
	while (written < max_packets) {
		ngtcp2_ssize n = write_packet(&ps.path, &pi, buf, sizeof(buf), now);
		net.in_packet.clear();
		if (n < 0) {
			ngtcp2_ccerr err;
			ngtcp2_ccerr_default(&err);
			ngtcp2_ccerr_set_liberr(&err, static_cast<int>(n), nullptr, 0);
			fail(err, std::string("connection error (") + ngtcp2_strerror(static_cast<int>(n)) + ")", now);
			return;
		}
		if (n == 0) {
			break;
		}
		written++;
		if (!send_packet(buf, static_cast<size_t>(n), ps.path)) {
			break;
		}
	}
	if (written) {
		ngtcp2_conn_update_pkt_tx_time(net.conn, now);
	}
	if (net.pending_socket_error) {
		int err = net.pending_socket_error;
		net.pending_socket_error = 0;
		socket_error(err, now);
	}
}

ngtcp2_ssize Connection::Impl::write_packet(ngtcp2_path *path, ngtcp2_pkt_info *pi, uint8_t *buf,
	size_t buflen, uint64_t now) {
	for (;;) {
		// Datagrams first: priority ones, then the rest as
		// the pacer allows. Peeked rather than popped -- only this thread
		// pops, and pushes elsewhere never move an element of a deque --
		// and taken off the queue only once ngtcp2 has packed it.
		const QueuedDatagram *next = nullptr;
		bool priority = false;
		uint64_t now_us = monotonic_us();
		{
			std::lock_guard<std::mutex> lock(datagram_mutex);
			// One larger than the path now allows (it shrank) can never go:
			// ngtcp2 would just keep reporting that it didn't fit.
			uint16_t max = max_datagram_size.load(std::memory_order_relaxed);
			for (auto *q : {&priority_queue, &queue}) {
				while (!q->empty() && max && q->front().data.size() > max) {
					datagram_stats.canceled_datagrams++;
					datagram_stats.queued_datagrams--;
					datagram_stats.queued_bytes -= q->front().data.size();
					q->pop_front();
				}
			}
			if (!priority_queue.empty()) {
				next = &priority_queue.front();
				priority = true;
			} else if (!queue.empty()) {
				if (pacing_bytes_per_s != 0) {
					refill_pacing_tokens(now_us);
				}
				if (pacing_bytes_per_s == 0 || pacing_tokens > 0) {
					next = &queue.front();
				} else {
					uint64_t wait_us = (uint64_t)(-pacing_tokens * 1e6 / (double)pacing_bytes_per_s) + 1;
					net.pacer_wakeup = std::min(net.pacer_wakeup, now + wait_us * NGTCP2_MICROSECONDS);
				}
			}
		}
		if (next) {
			int accepted = 0;
			uint64_t dgram_id = net.next_dgram_id++;
			ngtcp2_vec vec{const_cast<uint8_t *>(next->data.data()), next->data.size()};
			ngtcp2_ssize n = ngtcp2_conn_writev_datagram(net.conn, path, pi, buf, buflen, &accepted,
				NGTCP2_WRITE_DATAGRAM_FLAG_MORE, dgram_id, &vec, 1, now);
			if (accepted || n == NGTCP2_ERR_INVALID_ARGUMENT || n == NGTCP2_ERR_INVALID_STATE) {
				std::lock_guard<std::mutex> lock(datagram_mutex);
				auto &q = priority ? priority_queue : queue;
				size_t len = q.front().data.size();
				datagram_stats.queued_datagrams--;
				datagram_stats.queued_bytes -= len;
				if (accepted) {
					datagram_stats.sent_datagrams++;
					datagram_stats.sent_bytes += len;
					if (!priority && pacing_bytes_per_s != 0) {
						pacing_tokens -= (double)len;
					}
					net.dgrams_in_flight[dgram_id] = {static_cast<uint32_t>(len), false, now};
					net.in_packet.push_back(std::move(q.front()));
				} else {
					// Larger than the peer's max_datagram_frame_size, or the
					// peer takes no datagrams at all.
					datagram_stats.canceled_datagrams++;
				}
				q.pop_front();
			}
			if (n == NGTCP2_ERR_WRITE_MORE || n == NGTCP2_ERR_INVALID_ARGUMENT ||
				n == NGTCP2_ERR_INVALID_STATE) {
				continue;
			}
			return n;
		}

		// Then stream data, whenever no datagram is ready to go (the queue
		// is empty, or the pacer is holding it). The control stream is
		// small and seldom busy; datagrams going first means a large
		// message on it (a big clipboard) can't stall the video.
		NetStream *stream = nullptr;
		int64_t stream_id = -1;
		for (auto &[id, s] : net.streams) {
			if (!s.opened || s.blocked) {
				continue;
			}
			if (!s.any_sent || s.written < s.end || (s.fin_requested && !s.fin_sent)) {
				stream = &s;
				stream_id = id;
				break;
			}
		}
		if (stream) {
			ngtcp2_vec vecs[16];
			size_t count = 0;
			uint64_t covered = 0;
			uint64_t pos = stream->chunks_offset;
			for (auto &chunk : stream->chunks) {
				uint64_t chunk_end = pos + chunk.size();
				if (chunk_end > stream->written) {
					if (count == std::size(vecs)) {
						break;
					}
					uint64_t skip = stream->written > pos ? stream->written - pos : 0;
					vecs[count].base = chunk.data() + skip;
					vecs[count].len = chunk.size() - skip;
					covered += vecs[count].len;
					count++;
				}
				pos = chunk_end;
			}
			uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
			bool all = stream->written + covered == stream->end;
			if (stream->fin_requested && all) {
				flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
			}
			ngtcp2_ssize accepted = -1;
			ngtcp2_ssize n = ngtcp2_conn_writev_stream(net.conn, path, pi, buf, buflen, &accepted, flags,
				stream_id, vecs, count, now);
			if (accepted >= 0) {
				// An empty write that went out still announces the stream, and
				// the server identifies the streams by their existence, not
				// their data.
				stream->any_sent = true;
				stream->written += static_cast<uint64_t>(accepted);
				if ((flags & NGTCP2_WRITE_STREAM_FLAG_FIN) && stream->written == stream->end) {
					stream->fin_sent = true;
				}
			}
			switch (n) {
			case NGTCP2_ERR_WRITE_MORE: continue;
			case NGTCP2_ERR_STREAM_DATA_BLOCKED: stream->blocked = true; continue;
			case NGTCP2_ERR_STREAM_SHUT_WR:
			case NGTCP2_ERR_STREAM_NOT_FOUND:
				stream->any_sent = true;
				stream->written = stream->end;
				stream->fin_sent = true;
				continue;
			default:
				// If nothing was accepted and nothing written, the stream
				// is congestion-limited like everything else: done for now.
				return n;
			}
		}

		// Nothing of ours left: ngtcp2's own frames (acks, retransmissions,
		// probes), or just finishing a packet started above.
		return ngtcp2_conn_writev_stream(net.conn, path, pi, buf, buflen, nullptr, 0, -1, nullptr, 0, now);
	}
}

} // namespace gdp
