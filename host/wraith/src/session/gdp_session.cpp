// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/gdp_session.hpp"

#include "session/session_services.hpp"
#include "session/hid_evdev.hpp"
#include "session/raw_controllers.hpp"
#include "session/seat_client.hpp"

#include "gdp/clipboard.hpp"
#include "gdp/datagram.hpp"
#include "gdp/error_codes.hpp"
#include "gdp/framing.hpp"
#include "gdp/negotiation.hpp"
#include "gdp/refine.hpp"
#include "gdp/version.hpp"
#include "gdp/video_codec.hpp"
#include "session.pb.h"

#include "util/clock.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <pwd.h>
#include <unistd.h>

namespace wraith {

namespace {

// The rate controller's profile (never AUTO: it resolves that itself) as
// wraith.toml's per-profile settings name it.
LinkProfile link_profile_of(gdp::session::NetworkProfile profile) {
	switch (profile) {
	case gdp::session::NETWORK_PROFILE_LAN: return LinkProfile::kLan;
	case gdp::session::NETWORK_PROFILE_MOBILE: return LinkProfile::kMobile;
	default: return LinkProfile::kInternet;
	}
}

// Optional protocol extensions this build understands unconditionally
// (gdp-spec.md §6.7). "clipboard" is *not* here: whether
// this host can sync a clipboard at all depends on the backend's
// mechanism having opened, so GdpSession::supported_capabilities() adds
// it per session.
// Lossless refinement is unconditional: every encoder backend has a
// push_cpu() path for the wrapper to feed (encode/refine/refine_encoder.hpp).
const std::vector<std::string> kSupportedCapabilities = {gdp::kCapabilityRefine};

// gdp-spec.md §6.3. spectre sends SessionHello
// the moment its connection is up, so a real client is pending for about
// one round trip: the deadline is only ever reached by something that
// isn't going to authenticate. The pending cap is per source address:
// a source already holding kMaxPendingPerSource has its oldest evicted,
// never another source's, so one address that keeps handshaking (the
// port takes a QUIC handshake from anyone) can't shut a real client at
// another address out. kMaxPending bounds the whole table; past it the
// busiest source loses its oldest. Pre-auth frames are capped at
// gdp::kMaxPreAuthFrameSize.
constexpr int kHelloDeadlineMs = 5000;
constexpr size_t kMaxPendingPerSource = 2;
constexpr size_t kMaxPending = 16;
// A ResolutionChange outside these, or odd (4:2:0 needs even sizes), is
// answered with the current size. Whether the encoder can open at an
// accepted size is only known by trying (handle_resolution_change()).
constexpr uint32_t kMinOutputSide = 320;
constexpr uint32_t kMaxOutputSide = 8192;

// Whether a client-requested output size is one the host will apply:
// even, and within the sides above (both SessionHello.displays and
// ResolutionChange go through this).
bool valid_output_size(uint32_t width, uint32_t height) {
	return width >= kMinOutputSide && height >= kMinOutputSide && width <= kMaxOutputSide &&
		height <= kMaxOutputSide && width % 2 == 0 && height % 2 == 0;
}

std::vector<std::string> to_vector(const google::protobuf::RepeatedPtrField<std::string> &field) {
	return std::vector<std::string>(field.begin(), field.end());
}

std::string join(const std::vector<std::string> &list) {
	std::string out;
	for (const auto &s : list) {
		out += out.empty() ? s : ", " + s;
	}
	return out.empty() ? "(none)" : out;
}

} // namespace

GdpSession::GdpSession(SessionHost &host, TokenValidator token_validator, const std::string &control_socket,
	bool gamepads, bool raw_controllers)
	: host_(host), token_validator_(std::move(token_validator)), control_socket_(control_socket) {
	if (!control_socket.empty() && gamepads) {
		gamepads_ = std::make_unique<GamepadDevices>(control_socket);
	}
	if (!control_socket.empty() && raw_controllers) {
		raw_ = std::make_unique<RawControllers>(control_socket, host_.event_loop(),
			[this](const gdp::session::HostInputEnvelope &env) { send_host_input(env); });
	}
	// QUIC's own congestion controller, under wraith's rate control: CUBIC
	// unless wraith.toml says network.congestion_control = "bbr"
	// (docs/design/transport-and-rate-control.md#congestion-control).
	// The algorithm is fixed when a connection is accepted, before
	// SessionHello says what kind of link it is, so it can't follow the
	// network profile.
	rate_trace_ = config().network.rate_trace;
	if (config().network.congestion_control == "bbr") {
		WLOG_INFO("gdp: QUIC congestion control: BBR (wraith.toml)");
		transport_.set_congestion_control(gdp::CongestionControl::kBbr);
	} else {
		WLOG_INFO("gdp: QUIC congestion control: CUBIC");
	}
}

GdpSession::~GdpSession() {
	// An attached client learns *why* from the close code (gdp-spec.md
	// §12): SESSION_ENDED when wraith is going away because the desktop
	// session did (a LogoutRequest, or the desktop's own logout), so it can
	// report an ordinary end of session rather than a lost connection.
	reset_session(host_.session_ended() ? static_cast<uint64_t>(gdp::ErrorCode::kSessionEnded) : 0);
	pending_.clear();
}

bool GdpSession::listen(uint16_t port, const std::string &cert_file, const std::string &key_file,
	bool *address_in_use) {
	return transport_.listen(
		port, gdp::kAlpn, cert_file, key_file,
		[this](std::unique_ptr<gdp::Connection> conn) { handle_new_connection(std::move(conn)); },
		address_in_use);
}

int GdpSession::notify_fd() const {
	return transport_.notify_fd();
}

void GdpSession::dispatch() {
	transport_.dispatch();
}

// Also reached from inside handle_control_data()/handle_input_data()'s
// feed_and_drain callbacks, i.e. while the reader being replaced below is
// still on the stack. Safe only because those callbacks return false right
// after calling this and FrameReader::feed_and_drain() then returns without
// touching its state again (libgdp's contract for a handler that closed
// the connection); libgdp likewise copies each on_* callback before
// invoking it, so destroying the Peer (and its Connection) from inside one
// is fine too. reject_pending()/remove_pending() rely on the same contract.
void GdpSession::reset_session(uint64_t error_code) {
	if (active_) {
		active_->conn->close(error_code);
		active_.reset();
		report_viewer(false);
	}
	if (diagnostics_done_) {
		auto done = std::move(diagnostics_done_);
		diagnostics_done_ = nullptr;
		done("the client disconnected before answering");
	}
	stop_clipboard_sync();
	pending_clipboard_text_.clear();
	if (gamepads_) {
		// The pads belong to the client that forwarded them: unplug them
		// from the session rather than leaving devices behind for the
		// next client (or for nobody) to inherit.
		gamepads_->disconnect_all();
	}
	if (raw_) {
		raw_->disconnect_all();
	}
	if (authenticated_) {
		host_.session().set_viewer_attached(false);
	}
	authenticated_ = false;
	video_codec_.clear();
	capabilities_.clear();
	clipboard_echo_.reset();
	rate_.reset();
}

void GdpSession::end_for_local_login() {
	ended_by_local_login_ = true;
	reset_session(static_cast<uint64_t>(gdp::ErrorCode::kEndedByLocalLogin));
}

void GdpSession::handle_new_connection(std::unique_ptr<gdp::Connection> conn) {
	// Never refused outright, even with a viewer attached: whether this
	// is the real user (who then gets ALREADY_CONNECTED) or a stranger
	// (AUTH_FAILED) is only known from its hello. See the class comment.
	const std::string source = conn->remote_host();
	auto from_source = [&](const std::string &host) {
		return std::count_if(pending_.begin(), pending_.end(),
			[&host](const std::unique_ptr<Peer> &p) { return p->source == host; });
	};
	auto oldest_from = [&](const std::string &host) -> Peer * {
		auto it = std::find_if(pending_.begin(), pending_.end(),
			[&host](const std::unique_ptr<Peer> &p) { return p->source == host; });
		return it == pending_.end() ? nullptr : it->get();
	};
	if (from_source(source) >= static_cast<long>(kMaxPendingPerSource)) {
		reject_pending(oldest_from(source),
			"evicted: too many connections from this address waiting to authenticate");
	} else if (pending_.size() >= kMaxPending) {
		std::string busiest;
		long busiest_count = 0;
		for (const auto &p : pending_) {
			long n = from_source(p->source);
			if (n > busiest_count) {
				busiest_count = n;
				busiest = p->source;
			}
		}
		reject_pending(oldest_from(busiest), "evicted: too many connections waiting to authenticate");
	}

	auto owned = std::make_unique<Peer>();
	Peer *peer = owned.get();
	peer->owner = this;
	peer->id = next_peer_id_++;
	peer->source = source;
	peer->conn = std::move(conn);
	peer->control_reader.set_max_frame_size(gdp::kMaxPreAuthFrameSize);

	peer->conn->on_control_stream = [this, peer](gdp::Stream *s) {
		peer->control = s;
		s->on_data = [this, peer](const uint8_t *data, size_t len) { handle_control_data(peer, data, len); };
	};
	peer->conn->on_input_stream = [this, peer](gdp::Stream *s) {
		peer->input = s;
		s->on_data = [this, peer](const uint8_t *data, size_t len) { handle_input_data(peer, data, len); };
	};
	peer->conn->on_datagram = [this, peer](const uint8_t *data, size_t len, uint64_t) {
		handle_datagram(peer, data, len);
	};
	peer->conn->on_state_changed = [this, peer](gdp::ConnectionState state) {
		if (state == gdp::ConnectionState::kShutdown) {
			handle_peer_shutdown(peer);
		}
	};

	peer->hello_deadline.reset(wl_event_loop_add_timer(
		host_.event_loop(),
		[](void *data) {
			auto *p = static_cast<Peer *>(data);
			p->owner->reject_pending(p, "no SessionHello within the deadline");
			return 0;
		},
		peer));
	wl_event_source_timer_update(peer->hello_deadline.get(), kHelloDeadlineMs);

	pending_.push_back(std::move(owned));
	WLOG_INFO("gdp: connection #%llu from %s waiting for SessionHello (%zu pending)",
		(unsigned long long)peer->id, peer->source.c_str(), pending_.size());
}

bool GdpSession::microphone_negotiated() const {
	return host_.session().microphone() && gdp::has_capability(capabilities_, gdp::kCapabilityMicrophone);
}

// gdp-spec.md §3.2: a datagram on a channel that was not negotiated is dropped
// silently, as is anything from a connection that has not authenticated.
void GdpSession::handle_datagram(Peer *peer, const uint8_t *data, size_t len) {
	if (peer != active_.get() || !authenticated_ || !microphone_negotiated()) {
		return;
	}
	if (gdp::peek_channel(data, len) != gdp::DatagramChannel::kMicrophone) {
		return;
	}
	gdp::AudioDatagramHeader hdr;
	const uint8_t *payload;
	size_t payload_len;
	if (!gdp::AudioDatagramHeader::decode(data, len, &hdr, &payload, &payload_len,
			gdp::DatagramChannel::kMicrophone)) {
		return;
	}
	host_.session().microphone()->on_packet(hdr.seq, payload, payload_len);
}

void GdpSession::handle_peer_shutdown(Peer *peer) {
	if (peer == active_.get()) {
		reset_session();
	} else {
		remove_pending(peer);
	}
}

void GdpSession::reject_pending(Peer *peer, const char *reason) {
	WLOG_INFO("gdp: connection #%llu rejected before authentication: %s", (unsigned long long)peer->id,
		reason);
	peer->conn->close(static_cast<uint64_t>(gdp::ErrorCode::kAuthFailed));
	remove_pending(peer);
}

void GdpSession::remove_pending(Peer *peer, std::unique_ptr<Peer> *out) {
	auto it = std::find_if(pending_.begin(), pending_.end(),
		[peer](const std::unique_ptr<Peer> &p) { return p.get() == peer; });
	if (it == pending_.end()) {
		return;
	}
	std::unique_ptr<Peer> owned = std::move(*it);
	pending_.erase(it);
	if (out) {
		*out = std::move(owned);
	}
}

bool GdpSession::handle_hello(Peer *peer, const gdp::session::ControlEnvelope &env) {
	// gdp-spec.md §6.3: SessionHello must be the first message. Whatever
	// goes wrong before the token checks out, the peer only learns
	// AUTH_FAILED.
	if (!env.has_hello()) {
		char reason[96];
		snprintf(reason, sizeof(reason),
			"first control message was ControlEnvelope field %d, not SessionHello", (int)env.msg_case());
		reject_pending(peer, reason);
		return false;
	}
	if (!token_validator_(env.hello().token())) {
		reject_pending(peer, "invalid token");
		return false;
	}
	if (ended_by_local_login_) {
		// A reconnect racing the logout: it proved who it is, so it may
		// know why it can't have the session.
		WLOG_INFO("gdp: connection #%llu authenticated after a console login ended the session",
			(unsigned long long)peer->id);
		peer->conn->close(static_cast<uint64_t>(gdp::ErrorCode::kEndedByLocalLogin));
		remove_pending(peer);
		return false;
	}
	if (active_ && env.hello().take_over()) {
		// gdp-spec.md §6.4: the same user, from
		// somewhere else, who asked for the other viewer to go. The token
		// checked out, so this really is that user.
		WLOG_INFO("gdp: connection #%llu authenticated and takes over from connection #%llu",
			(unsigned long long)peer->id, (unsigned long long)active_->id);
		reset_session(static_cast<uint64_t>(gdp::ErrorCode::kTakenOver));
	}
	if (active_) {
		// The same user again, from somewhere else: say so, since this
		// peer has proven who it is. See the class comment.
		WLOG_INFO("gdp: connection #%llu authenticated, but connection #%llu is already the viewer",
			(unsigned long long)peer->id, (unsigned long long)active_->id);
		peer->conn->close(static_cast<uint64_t>(gdp::ErrorCode::kAlreadyConnected));
		remove_pending(peer);
		return false;
	}

	remove_pending(peer, &active_);
	active_->hello_deadline.reset();
	active_->control_reader.set_max_frame_size(gdp::kMaxFrameSize);
	WLOG_INFO("gdp: connection #%llu authenticated", (unsigned long long)peer->id);
	report_viewer(true);
	return accept_session(env.hello());
}

void GdpSession::report_viewer(bool attached) {
	if (control_socket_.empty()) {
		return;
	}
	// Best-effort and blocking, like create_device(): one short local
	// connection, written and closed. ghostd only passes it on to Veil
	// (docs/design/veil.md), so a failure costs a stale
	// "viewer attached" on Veil's Devices page, nothing more.
	std::string error;
	if (!SeatClient::report_viewer(control_socket_, attached, &error)) {
		WLOG_ERROR("gdp: reporting the viewer %s to ghostd failed: %s", attached ? "attaching" : "detaching",
			error.c_str());
	}
}

void GdpSession::fill_output(gdp::session::OutputDescriptor *output) const {
	output->set_stream_id(kStreamId);
	output->mutable_display()->set_width(host_.output_width());
	output->mutable_display()->set_height(host_.output_height());
	output->mutable_display()->set_refresh_mhz(60000);
	output->mutable_display()->set_scale(1.0);
	// What the encoder really does (gdp-spec.md §7.3): the desktop
	// is sRGB, converted to 4:2:0 with BT.601 limited-range coefficients,
	// except for PyroWave's own full-range BT.709.
	const bool pyrowave = video_codec_ == gdp::kVideoCodecPyrowave;
	auto *color = output->mutable_color();
	color->set_matrix(pyrowave ? gdp::session::COLOR_MATRIX_BT709 : gdp::session::COLOR_MATRIX_BT601);
	color->set_range(pyrowave ? gdp::session::COLOR_RANGE_FULL : gdp::session::COLOR_RANGE_LIMITED);
	color->set_primaries(gdp::session::COLOR_PRIMARIES_BT709);
	color->set_transfer(gdp::session::COLOR_TRANSFER_SRGB);
	color->set_bit_depth(8);
	color->set_chroma(gdp::session::CHROMA_420);
}

// gdp-spec.md §7.2: resize the output, reopen the
// encoder at the new size (its first frame is a keyframe), and answer
// with DisplaysChanged -- sent before any frame at the new size, and sent
// whether or not the size changed, so the client always learns the
// outcome.
void GdpSession::handle_resolution_change(const gdp::session::ResolutionChange &change) {
	uint32_t width = change.display().width();
	uint32_t height = change.display().height();
	uint32_t old_width = host_.output_width();
	uint32_t old_height = host_.output_height();
	SessionServices &services = host_.session();
	bool refine = gdp::has_capability(capabilities_, gdp::kCapabilityRefine);

	if (change.stream_id() != kStreamId || !valid_output_size(width, height)) {
		WLOG_ERROR("gdp: ignoring resolution change to %ux%u on stream %u (even sizes %u..%u only)", width,
			height, change.stream_id(), kMinOutputSide, kMaxOutputSide);
	} else if (width != old_width || height != old_height) {
		if (!host_.request_resize(width, height)) {
			WLOG_ERROR("gdp: client asked for %ux%u, but this session can't resize -- staying at %ux%u",
				width, height, old_width, old_height);
		} else {
			services.close_encoder();
			if (services.set_session_codec(video_codec_, refine)) {
				WLOG_INFO("gdp: resized output to %ux%u mid-session (was %ux%u)", width, height, old_width,
					old_height);
			} else {
				// The encoder won't open at that size (a hardware limit):
				// go back to the old one, which it just ran at.
				WLOG_ERROR("gdp: no %s encoder at %ux%u, going back to %ux%u", video_codec_.c_str(), width,
					height, old_width, old_height);
				host_.request_resize(old_width, old_height);
				services.close_encoder();
				if (!services.set_session_codec(video_codec_, refine)) {
					WLOG_ERROR("gdp: the %s encoder won't reopen at %ux%u either", video_codec_.c_str(),
						old_width, old_height);
					reset_session(static_cast<uint64_t>(gdp::ErrorCode::kNoCommonCodec));
					return;
				}
			}
		}
	}

	gdp::session::ControlEnvelope env;
	fill_output(env.mutable_displays_changed()->add_outputs());
	send_control(env);
	if (host_.output_width() != old_width || host_.output_height() != old_height) {
		// The reopened encoder's first frame is a keyframe anyway; this
		// also sends one on a desktop that is sitting still.
		request_keyframe();
	}
}

void GdpSession::send_control(const gdp::session::ControlEnvelope &env) {
	if (!active_ || !active_->control) {
		return;
	}
	if (!active_->control->send_message(env)) {
		WLOG_ERROR("gdp: failed to encode control message");
	}
}

void GdpSession::handle_control_data(Peer *peer, const uint8_t *data, size_t len) {
	gdp::session::ControlEnvelope env;
	bool ok = peer->control_reader.feed_and_drain(data, len, &env, [&] {
		if (peer != active_.get()) {
			// No SessionReject either way: a rejected attempt gets
			// nothing back but the §12 code on CONNECTION_CLOSE.
			return handle_hello(peer, env);
		}

		if (env.has_keyframe_request()) {
			request_keyframe();
		} else if (env.has_ping()) {
			gdp::session::ControlEnvelope pong_env;
			pong_env.mutable_pong()->set_nonce(env.ping().nonce());
			send_control(pong_env);
		} else if (env.has_stats()) {
			for (const auto &stream_stats : env.stats().streams()) {
				if (stream_stats.stream_id() == kStreamId) {
					handle_stream_stats(env.stats().rtt_us(), stream_stats);
					break;
				}
			}
		} else if (env.has_clipboard()) {
			handle_clipboard_data(env.clipboard());
		} else if (env.has_refine_pause()) {
			// gdp-spec.md §7.8: only on a session
			// that negotiated it; dropped otherwise.
			if (gdp::has_capability(capabilities_, gdp::kCapabilityRefine)) {
				host_.session().set_refine_paused(env.refine_pause().paused());
			}
		} else if (env.has_diagnostics_report()) {
			if (diagnostics_done_) {
				auto done = std::move(diagnostics_done_);
				diagnostics_done_ = nullptr;
				done(env.diagnostics_report().text());
			}
		} else if (env.has_logout_request()) {
			// A client-requested logout (gdp-spec.md §7.10). No
			// reply: the connection closing with SESSION_ENDED (see ~GdpSession) is the
			// completion signal. Whoever holds a validated token *is*
			// the session's user, so there's nothing further to check.
			WLOG_INFO("gdp: client requested logout");
			host_.request_logout();
		} else if (env.has_resolution_change()) {
			handle_resolution_change(env.resolution_change());
		}
		return true;
	});
	if (!ok) {
		if (peer != active_.get()) {
			reject_pending(peer,
				peer->control_reader.last_result() == gdp::FrameReader::Result::kTooLarge
					? "pre-auth frame over the size limit"
					: "unparseable pre-auth frame");
			return;
		}
		// gdp-spec.md §6: close the connection on a frame violation, with
		// the code saying which kind.
		reset_session(peer->control_reader.error_code());
	}
}

bool GdpSession::accept_session(const gdp::session::SessionHello &hello) {
	// gdp-spec.md §6.6: the first of spectre's
	// preferences this host can actually encode -- which takes opening an
	// encoder to establish, hence the loop below over every common codec
	// rather than a single pick.
	std::vector<std::string> offered_codecs = to_vector(hello.codecs());
	std::vector<std::string> common =
		gdp::common_video_codecs(offered_codecs, SessionServices::supported_video_codecs());
	// PyroWave is a wired-LAN codec: never through a gateway, which would
	// decrypt and re-encrypt several hundred Mbit/s and may face the
	// internet on its other side (gdp-spec.md §6.6).
	if (hello.via_gateway()) {
		auto pyrowave = std::find(common.begin(), common.end(), gdp::kVideoCodecPyrowave);
		if (pyrowave != common.end()) {
			common.erase(pyrowave);
			WLOG_INFO("gdp: not using pyrowave: the session comes through a gateway");
		}
	}
	negotiation_summary_ = "client offered codecs: " + join(offered_codecs) +
		"\nclient decoders: " + join(to_vector(hello.decoders())) +
		"\nhost can encode: " + join(SessionServices::supported_video_codecs()) +
		"\nconnection: " + (hello.via_gateway() ? "via a gateway (pyrowave is never used)" : "direct");
	video_codec_.clear();
	capabilities_ = gdp::negotiate_capabilities(to_vector(hello.capabilities()), supported_capabilities());
	bool refine = gdp::has_capability(capabilities_, gdp::kCapabilityRefine);

	SessionServices &services = host_.session();
	// gdp-spec.md §7.8: every connection starts unpaused, whatever the
	// last client left it at.
	services.set_refine_paused(false);

	// SessionHello.displays: spectre's requested resolution (spectre-qt's
	// Settings dialog, or -r on the spectre CLI), gdp-spec.md §6.2. Only
	// the first entry matters -- wraith is single-output -- and only a
	// change is acted on: ScreencastHost::request_resize() restarts its
	// capture whatever the size, so it must not be called
	// for a size that already matches. SessionAccept reports the actual
	// size either way (SessionHost::request_resize()).
	//
	// Done before the codec is chosen: the encoder's size is fixed at
	// open(), so a successful resize closes it and the codec loop below
	// opens it once, at the new size.
	if (hello.displays_size() > 0) {
		const auto &requested = hello.displays(0);
		if (requested.width() == 0 && requested.height() == 0) {
			// No preference.
		} else if (!valid_output_size(requested.width(), requested.height())) {
			// A token holder's size, so bounded like a ResolutionChange:
			// 1x1 or 65535x65535 would stick to the output after the
			// encoder failed on it, and fail every reconnect after.
			WLOG_ERROR("gdp: ignoring the requested %ux%u (even sizes %u..%u only), staying at %ux%u",
				requested.width(), requested.height(), kMinOutputSide, kMaxOutputSide, host_.output_width(),
				host_.output_height());
		} else if (requested.width() != host_.output_width() || requested.height() != host_.output_height()) {
			if (host_.request_resize(requested.width(), requested.height())) {
				services.close_encoder();
				WLOG_INFO("gdp: resized output to %ux%u (client requested %ux%u)", host_.output_width(),
					host_.output_height(), requested.width(), requested.height());
			} else {
				WLOG_ERROR("gdp: client requested %ux%u, but this session can't resize -- staying at %ux%u",
					requested.width(), requested.height(), host_.output_width(), host_.output_height());
			}
		}
	}

	// The encoder was opened before any client existed, on the default
	// codec; point it at what the client asked for. Having a backend
	// registered for a codec isn't proof it opens on this host (no
	// hardware entrypoint for the profile, docs/design/encoding.md),
	// so walk the client's preferences until one does. If none opens
	// there is no video the client could decode, and the connection is
	// closed with the §12 code rather than a SessionReject, so the reason
	// survives even if a reject message would race the close.
	for (const std::string &candidate : common) {
		if (services.set_session_codec(candidate, refine)) {
			video_codec_ = candidate;
			break;
		}
		WLOG_INFO("gdp: no encoder opened for %s, trying the client's next preference", candidate.c_str());
	}
	if (video_codec_.empty()) {
		WLOG_ERROR("gdp: no usable video codec (client offered: %s; host supports: %s)",
			join(offered_codecs).c_str(), join(SessionServices::supported_video_codecs()).c_str());
		reset_session(static_cast<uint64_t>(gdp::ErrorCode::kNoCommonCodec));
		return false;
	}
	WLOG_INFO("gdp: session codec %s (client decoders: %s), capabilities: %s, %s", video_codec_.c_str(),
		join(to_vector(hello.decoders())).c_str(), join(capabilities_).c_str(),
		hello.via_gateway() ? "via gateway" : "direct");

	authenticated_ = true;
	services.set_viewer_attached(true);
	clock_.reset(); // session clock zeroes at SessionAccept, gdp-spec.md §11
	next_frame_id_ = 0;
	audio_seq_ = 0;
	have_stats_ = false;
	have_keyframe_ = false;
	keyframe_pending_ = false;
	repair_owed_ = false;
	last_repair_us_ = 0;
	std::fill(sent_frames_.begin(), sent_frames_.end(), SentFrame{});
	slice_payload_ = 0;
	max_queue_age_since_report_ = 0;
	frames_skipped_since_report_ = 0;
	last_datagram_stats_ = active_->conn->datagram_stats();

	// The rate controller starts from the profile the client asked for --
	// or, for AUTO, the one the handshake RTT suggests -- and sets the
	// encoder's starting rate from it: the encoder was opened at the -b
	// ceiling, which a far-away client's link may not carry.
	uint32_t handshake_rtt_us = active_->conn->rtt_us();
	rate_ = std::make_unique<RateController>(services.encode_bitrate_bps(), hello.network_profile(),
		handshake_rtt_us, gdp::monotonic_us());
	path_rate_ = PathRateEstimator{}; // a new client is a new path
	logged_pacing_bps_ = 0;
	services.set_bitrate(rate_->target_bps());
	services.set_link_profile(link_profile_of(rate_->params().kind));
	apply_pacing();
	WLOG_INFO(
		"gdp: network profile %s%s (handshake RTT %.1f ms), starting at %.1f Mbps of %.1f, paced at %.0f Mbps",
		rate_->params().name, rate_->auto_profile() ? " (auto)" : "", handshake_rtt_us / 1000.0,
		rate_->target_bps() / 1e6, services.encode_bitrate_bps() / 1e6, pacing_bps_ / 1e6);

	// A newly-attached spectre's decoder has no reference frame yet, so
	// the next packet it receives must be a keyframe. request_keyframe()
	// also re-encodes the current picture, so an idle desktop sends it too.
	request_keyframe();

	// A newly-attached spectre has never seen a CursorShape, even if
	// wraith's own cursor state hasn't changed since some earlier
	// session -- force a resend rather than suppressing it as a
	// no-op change.
	host_.input().resend_cursor_shape();

	gdp::session::ControlEnvelope accept_env;
	auto *accept = accept_env.mutable_accept();
	fill_output(accept->add_outputs());
	accept->set_codec(video_codec_);
	accept->set_via_gateway(hello.via_gateway());
	accept->set_bitrate_ceiling_bps(services.encode_bitrate_bps());
	// Host info (gdp-spec.md §6.5): purely for spectre's own UI.
	accept->set_encoder(services.encoder_name());
	accept->set_network_profile(rate_->params().kind);
	if (const struct passwd *pw = getpwuid(getuid())) {
		accept->set_host_user(pw->pw_name);
	}
	char hostname[256] = {};
	if (gethostname(hostname, sizeof(hostname) - 1) == 0) {
		accept->set_host_name(hostname);
	}
	for (const auto &name : capabilities_) {
		accept->add_capabilities(name);
	}
	// AudioConfig is only sent when audio capture actually opened
	// (gdp-spec.md §6.8) -- a host with no PipeWire, or one
	// where the capture failed to open, still gets working video;
	// spectre simply never receives an audio() field and stays
	// silent (session_client.cpp checks has_audio()).
	if (AudioPipeline *audio_pipeline = services.audio_pipeline()) {
		const auto &audio_config = audio_pipeline->config();
		auto *audio = accept->mutable_audio();
		audio->set_codec(audio_config.codec);
		audio->set_sample_rate_hz(audio_config.sample_rate_hz);
		audio->set_channels(audio_config.channels);
		audio->set_frame_ms(audio_config.frame_ms);
	}
	if (microphone_negotiated()) {
		const gdp::AudioFormat &mic = services.microphone()->format();
		auto *config = accept->mutable_microphone();
		config->set_codec(mic.codec);
		config->set_sample_rate_hz(mic.sample_rate_hz);
		config->set_channels(mic.channels);
		config->set_frame_ms(mic.frame_ms);
		services.microphone()->reset();
	}

	send_control(accept_env);

	// After SessionAccept: the client only starts acting on ClipboardData
	// once it knows the capability is in effect, and the initial push
	// below is an ordinary ClipboardData.
	start_clipboard_sync();
	return true;
}

std::string GdpSession::status_text() const {
	std::string text = "viewer attached: ";
	text += authenticated_ ? "yes\n" : "no\n";
	text += negotiation_summary_.empty() ? "no client has connected yet\n" : negotiation_summary_ + "\n";
	if (authenticated_) {
		const SessionServices &services = host_.session();
		text += "session codec: " + video_codec_ + "\n";
		text += "capabilities: " + join(capabilities_) + "\n";
		text += "encoder: " + services.encoder_name() + "\n";
		text += "output: " + std::to_string(host_.output_width()) + "x" +
			std::to_string(host_.output_height()) + "\n";
	}
	return text;
}

bool GdpSession::request_client_diagnostics(std::function<void(std::string)> done) {
	if (!authenticated_ || !active_ || diagnostics_done_) {
		return false;
	}
	diagnostics_done_ = std::move(done);
	gdp::session::ControlEnvelope env;
	env.mutable_diagnostics_request();
	send_control(env);
	return true;
}

std::vector<std::string> GdpSession::supported_capabilities() const {
	std::vector<std::string> supported = kSupportedCapabilities;
	// clipboard_available(), not clipboard(): a screencast host's sink
	// only exists once the captured compositor is up, seconds after the
	// first client attaches, and a capability missed at SessionAccept can
	// never be added to that session.
	if (host_.session().clipboard_available()) {
		supported.push_back(gdp::kCapabilityClipboard);
	}
	// Unlike the clipboard, this is known before any client attaches:
	// ghostd answered it in SessionInit, so there is no startup race to
	// work around.
	if (gamepads_ || raw_) {
		supported.push_back(gdp::kCapabilityGamepad);
	}
	if (raw_) {
		supported.push_back(gdp::kCapabilityHid);
	}
	// Opened with the host's other services, before any client attaches.
	if (host_.session().microphone()) {
		supported.push_back(gdp::kCapabilityMicrophone);
	}
	return supported;
}

void GdpSession::clipboard_sink_changed() {
	if (!authenticated_) {
		return;
	}
	start_clipboard_sync();
}

void GdpSession::start_clipboard_sync() {
	ClipboardSink *clipboard = host_.session().clipboard();
	if (!clipboard || !gdp::has_capability(capabilities_, gdp::kCapabilityClipboard)) {
		// No sink yet is normal on a screencast backend: the session was
		// accepted before its compositor came up, and set_clipboard() will
		// call back here once it has (clipboard_sink_changed()).
		return;
	}
	clipboard->on_local_change = [this](const std::string &utf8) { send_clipboard(utf8); };
	if (!pending_clipboard_text_.empty()) {
		// The client copied something while the host still had no sink;
		// its text wins over the push below, being the newer of the two.
		std::string text = std::move(pending_clipboard_text_);
		pending_clipboard_text_.clear();
		clipboard_echo_.note_applied(text);
		clipboard->set_text(text);
		return;
	}
	// gdp-spec.md §7.9: the host pushes once on attach, since
	// the desktop's clipboard can have changed while nothing was
	// connected and the client would otherwise paste something stale.
	// The client does not push the other way -- connecting must not
	// clobber the desktop's clipboard with the client's.
	send_clipboard(clipboard->current_text());
}

void GdpSession::stop_clipboard_sync() {
	if (ClipboardSink *clipboard = host_.session().clipboard()) {
		clipboard->on_local_change = nullptr;
	}
}

void GdpSession::send_clipboard(const std::string &utf8) {
	if (!authenticated_ || !gdp::has_capability(capabilities_, gdp::kCapabilityClipboard)) {
		return;
	}
	if (!clipboard_echo_.should_send(utf8)) {
		// Empty, over the cap, or the very text this peer just sent us --
		// forwarding that back is how a copy ping-pongs forever.
		return;
	}
	clipboard_echo_.note_sent(utf8);
	gdp::session::ControlEnvelope env;
	auto *clipboard = env.mutable_clipboard();
	clipboard->set_mime_type(gdp::kClipboardMimeText);
	clipboard->set_data(utf8);
	send_control(env);
}

void GdpSession::handle_clipboard_data(const gdp::session::ClipboardData &clipboard) {
	if (!gdp::has_capability(capabilities_, gdp::kCapabilityClipboard)) {
		// A peer sending what it never negotiated: dropped rather than
		// acted on, whatever it claims (gdp-spec.md §6.7).
		return;
	}
	if (clipboard.mime_type() != gdp::kClipboardMimeText) {
		WLOG_INFO("clipboard: dropping ClipboardData with mime \"%s\" (v1 carries text/plain only)",
			clipboard.mime_type().c_str());
		return;
	}
	const std::string &text = clipboard.data();
	if (!clipboard_echo_.should_apply(text)) {
		return;
	}
	ClipboardSink *sink = host_.session().clipboard();
	if (!sink) {
		// Negotiated, but the backend's sink isn't up yet (the screencast
		// startup window). Held rather than dropped: start_clipboard_sync()
		// applies it the moment the sink arrives.
		pending_clipboard_text_ = text;
		return;
	}
	// Recorded *before* the local write: every mechanism reports a
	// selection-owner change for our own write, and that must not come
	// back as a fresh local change.
	clipboard_echo_.note_applied(text);
	sink->set_text(text);
}

bool GdpSession::gamepads_negotiated() const {
	return (gamepads_ || raw_) && gdp::has_capability(capabilities_, gdp::kCapabilityGamepad);
}

bool GdpSession::raw_negotiated() const {
	return raw_ && gamepads_negotiated() && gdp::has_capability(capabilities_, gdp::kCapabilityHid);
}

void GdpSession::handle_gamepad_connect(const gdp::session::GamepadConnect &connect) {
	if (!gamepads_negotiated()) {
		return;
	}
	// A slot holds one controller, standard or raw: this replaces either.
	if (raw_) {
		raw_->disconnect(connect.pad_index());
	}
	// A create that fails (ghostd refused, the index is out of range, no
	// uinput) is logged by the layer that knows why and the slot stays
	// empty: the rest of the session, that pad included, keeps working --
	// the client just gets no device for it.
	if (gamepads_) {
		gamepads_->connect(connect.pad_index(), connect.name());
	} else {
		WLOG_ERROR("gamepad: no uinput on this host, slot %u stays empty", connect.pad_index());
	}
}

void GdpSession::handle_gamepad_disconnect(const gdp::session::GamepadDisconnect &disconnect) {
	if (!gamepads_negotiated()) {
		return;
	}
	if (gamepads_) {
		gamepads_->disconnect(disconnect.pad_index());
	}
	if (raw_) {
		raw_->disconnect(disconnect.pad_index());
	}
}

void GdpSession::handle_gamepad_state(const gdp::session::GamepadState &state) {
	if (!gamepads_negotiated() || !gamepads_) {
		return;
	}
	// The wire's arrays are whatever the client sent: a shorter one
	// leaves the rest at 0/false, a longer one is truncated. Neither is
	// an error -- it's how a client built against a different
	// gdp/gamepad.hpp revision degrades instead of desynchronising.
	gdp::GamepadSnapshot snapshot;
	int axes = std::min<int>(state.axes_size(), (int)gdp::kGamepadAxisCount);
	for (int i = 0; i < axes; i++) {
		snapshot.axes[i] = state.axes(i);
	}
	int buttons = std::min<int>(state.buttons_size(), (int)gdp::kGamepadButtonCount);
	for (int i = 0; i < buttons; i++) {
		snapshot.buttons[i] = state.buttons(i);
	}
	gamepads_->update(state.pad_index(), snapshot);
}

void GdpSession::handle_hid_connect(const gdp::session::HidConnect &connect) {
	if (!raw_negotiated()) {
		return;
	}
	if (gamepads_) {
		gamepads_->disconnect(connect.pad_index());
	}
	raw_->connect(connect);
}

void GdpSession::send_host_input(const gdp::session::HostInputEnvelope &env) {
	if (!authenticated_ || !active_ || !active_->input) {
		return;
	}
	if (!active_->input->send_message(env)) {
		WLOG_ERROR("gdp: failed to encode a host input message");
	}
}

void GdpSession::request_keyframe(bool repair) {
	if (repair) {
		host_.session().request_repair_keyframe();
	} else {
		host_.session().request_keyframe();
	}
	if (!keyframe_pending_) {
		keyframe_pending_ = true;
		keyframe_requested_at_ = next_frame_id_;
	}
	keyframe_redeliver_.schedule(host_.event_loop(), [this] { host_.redeliver_frame(); });
}

void GdpSession::handle_stream_stats(uint32_t rtt_us, const gdp::session::StreamStats &stats) {
	SessionServices &services = host_.session();
	if (!rate_ || !active_) {
		return;
	}

	// Bit i of the bitmap is frame (highest_frame_id_acked - i). Frames
	// newer than the previous report's anchor are the bits below `advance`;
	// only those can carry loss this report is the first to tell us about.
	// The first report has no anchor, so everything in it counts.
	uint32_t highest = stats.highest_frame_id_acked();
	uint64_t new_loss = stats.loss_bitmap();
	if (have_stats_) {
		uint32_t advance = highest - last_highest_frame_id_acked_;
		new_loss = advance >= 64 ? new_loss : new_loss & ((uint64_t{1} << advance) - 1);
	}
	have_stats_ = true;
	last_highest_frame_id_acked_ = highest;

	uint64_t report_us = gdp::monotonic_us();
	bool path_changed = path_rate_.expire(report_us);
	for (const gdp::session::FrameTrain &train : stats.trains()) {
		path_rate_.add(train.bytes(), train.span_us(), report_us);
		path_changed = true;
	}
	if (path_changed) {
		apply_pacing();
	}

	gdp::Connection::DatagramStats now_stats = active_->conn->datagram_stats();
	RateSample sample;
	sample.now_us = report_us;
	sample.rtt_us = rtt_us ? rtt_us : active_->conn->rtt_us();
	sample.sent_bytes = now_stats.sent_bytes - last_datagram_stats_.sent_bytes;
	sample.acked_datagrams = now_stats.acked_datagrams - last_datagram_stats_.acked_datagrams;
	sample.lost_datagrams = now_stats.lost_datagrams - last_datagram_stats_.lost_datagrams;
	sample.queue_age_us = std::max(max_queue_age_since_report_, now_stats.oldest_queued_age_us);
	sample.frames_skipped = frames_skipped_since_report_;
	if (stats.interval_us() != 0) {
		sample.has_receiver_stats = true;
		sample.interval_us = stats.interval_us();
		sample.bytes_received = stats.bytes_received();
		sample.delay_samples = stats.delay_samples();
		sample.delay_min_us = stats.delay_min_us();
		sample.delay_avg_us = stats.delay_avg_us();
	}
	last_datagram_stats_ = now_stats;
	uint32_t skipped = frames_skipped_since_report_;
	max_queue_age_since_report_ = 0;
	frames_skipped_since_report_ = 0;

	const char *profile_before = rate_->params().name;
	uint32_t before = rate_->target_bps();
	bool changed = rate_->on_sample(sample);
	if (rate_trace_) {
		uint64_t dt_us = std::max<uint64_t>(1, sample.interval_us);
		WLOG_INFO(
			"gdp: rate %s target %.1f Mbps, sent %.1f, received %.1f, paced %.0f, path %.1f, queuing %.1f ms, "
			"backlog %.1f ms, lost %llu/%llu, skipped %u, rtt %.1f ms%s%s",
			rate_->params().name, rate_->target_bps() / 1e6, sample.sent_bytes * 8.0 / (double)dt_us,
			sample.has_receiver_stats ? sample.bytes_received * 8.0 / (double)dt_us : 0.0, pacing_bps_ / 1e6,
			path_rate_.estimate_bps() / 1e6, rate_->queuing_delay_us() / 1000.0, sample.queue_age_us / 1000.0,
			(unsigned long long)sample.lost_datagrams,
			(unsigned long long)(sample.lost_datagrams + sample.acked_datagrams), skipped,
			sample.rtt_us / 1000.0, *rate_->last_reason() ? ", " : "", rate_->last_reason());
	}
	if (changed) {
		services.set_bitrate(rate_->target_bps());
		apply_pacing();
		// Climbing happens every clean report; only cuts are worth a line.
		if (rate_->target_bps() < before) {
			WLOG_INFO("gdp: bitrate %.1f -> %.1f Mbps (%s: queuing %.1f ms, backlog %.1f ms, "
					  "lost %llu/%llu datagrams, rtt %.1f ms, %u frames skipped)",
				before / 1e6, rate_->target_bps() / 1e6, rate_->last_reason(),
				rate_->queuing_delay_us() / 1000.0, sample.queue_age_us / 1000.0,
				(unsigned long long)sample.lost_datagrams,
				(unsigned long long)(sample.lost_datagrams + sample.acked_datagrams), sample.rtt_us / 1000.0,
				skipped);
		}
	} else if (std::strcmp(rate_->last_reason(), "rebase") == 0) {
		WLOG_INFO("gdp: queuing delay of %.1f ms survived two cuts -- taking it as the path's new base",
			rate_->queuing_delay_us() / 1000.0);
	}
	if (rate_->params().name != profile_before) {
		WLOG_INFO("gdp: network profile %s -> %s (auto)", profile_before, rate_->params().name);
		services.set_link_profile(link_profile_of(rate_->params().kind));
		gdp::session::ControlEnvelope env;
		env.mutable_network_profile_changed()->set_profile(rate_->params().kind);
		send_control(env);
	}

	// Lost-frame recovery, separate from rate: a lost frame corrupts every
	// P-frame referencing it until the next keyframe, so one is needed.
	// Its refinement layer is repaired at once, on its own: that costs
	// only what the frame carried, so there is nothing to rate-limit.
	if (new_loss != 0) {
		report_lost_frames(highest, new_loss);
		repair_loss(highest - (uint32_t)__builtin_ctzll(new_loss));
	}
	service_owed_keyframe();
}

void GdpSession::report_lost_frames(uint32_t highest, uint64_t new_loss) {
	std::vector<int64_t> lost_pts;
	for (uint64_t bits = new_loss; bits != 0; bits &= bits - 1) {
		uint32_t frame_id = highest - (uint32_t)__builtin_ctzll(bits);
		const SentFrame &sent = sent_frames_[frame_id % kSentFrames];
		if (!sent.valid || sent.frame_id != frame_id) {
			request_keyframe();
			return;
		}
		lost_pts.push_back(sent.pts_us);
	}
	host_.session().frames_lost(lost_pts);
}

void GdpSession::repair_loss(uint32_t newest_lost) {
	if (repair_owed_ && (int32_t)(newest_lost - owed_newest_lost_) <= 0) {
		return; // an owed repair already covers it
	}
	// A keyframe sent, or already asked for, after the newest lost frame
	// repairs it. Asking again for every report that still shows the loss
	// would stack keyframes, each far bigger than a P-frame, onto a link
	// that just proved it can't keep up.
	bool covered_by_sent = have_keyframe_ && (int32_t)(last_keyframe_frame_id_ - newest_lost) > 0;
	bool covered_by_pending = keyframe_pending_ && (int32_t)(keyframe_requested_at_ - newest_lost) > 0;
	if (covered_by_sent || covered_by_pending) {
		return;
	}
	repair_owed_ = true;
	owed_newest_lost_ = newest_lost;
	service_owed_keyframe();
}

void GdpSession::service_owed_keyframe() {
	if (!repair_owed_ || !rate_) {
		return;
	}
	// A keyframe that went out on its own (the GOP's) since the loss did
	// the repair already.
	if (have_keyframe_ && (int32_t)(last_keyframe_frame_id_ - owed_newest_lost_) > 0) {
		repair_owed_ = false;
		return;
	}
	uint64_t now = gdp::monotonic_us();
	if (last_repair_us_ != 0 && now - last_repair_us_ < rate_->params().keyframe_min_interval_us) {
		return;
	}
	last_repair_us_ = now;
	repair_owed_ = false;
	request_keyframe(/*repair=*/true);
}

void GdpSession::apply_pacing() {
	// Once spectre has timed a few large frames, a path slower than the
	// fixed rate is paced at its estimate: a backlog then drains no faster
	// than the path's slowest hop carries it. Any more than that, sustained
	// for a keyframe's backlog, fills a shallow router queue until it drops
	// (docs/design/transport-and-rate-control.md#pacing).
	//
	// Never below the rate controller's target, though, and that is how a
	// faster path is found: frames paced at the estimate arrive at the
	// estimate, so it can't rise on its own, but the rate controller keeps
	// climbing while reports are clean, pacing follows it past the
	// estimate, and frames that then arrive as fast as they were paced
	// raise the estimate too. A path that can't carry the climb shows it
	// in delay and loss, which the rate controller already answers.
	//
	// The estimate never raises pacing past the fixed rate, which a LAN
	// (where that hop is the fastest) already runs clean.
	const auto &network = config().network;
	uint64_t paced = 0;
	if (network.pacing_multiplier != 0) {
		uint64_t target = rate_->target_bps();
		paced = std::max<uint64_t>((uint64_t)network.pacing_multiplier * target,
			(uint64_t)network.pacing_floor_mbps * 1'000'000);
		if (path_rate_.estimate_bps() != 0) {
			paced = std::min(paced, std::max(path_rate_.estimate_bps(), (uint64_t)target));
		}
	}
	// Only a real move is worth a line: the estimate shifts a little with
	// every large frame.
	if (logged_pacing_bps_ == 0 || paced * 10 < logged_pacing_bps_ * 8 ||
		paced * 10 > logged_pacing_bps_ * 12) {
		if (logged_pacing_bps_ != 0) {
			WLOG_INFO("gdp: pacing %.0f -> %.0f Mbps (path %.1f Mbps from %zu frames, target %.1f Mbps)",
				logged_pacing_bps_ / 1e6, paced / 1e6, path_rate_.estimate_bps() / 1e6, path_rate_.samples(),
				rate_->target_bps() / 1e6);
		}
		logged_pacing_bps_ = paced;
	}
	pacing_bps_ = paced;
	active_->conn->set_pacing_rate(pacing_bps_ / 8);
}

bool GdpSession::send_queue_clear() {
	if (!authenticated_ || !rate_ || !active_) {
		return true;
	}
	uint64_t age = active_->conn->datagram_stats().oldest_queued_age_us;
	max_queue_age_since_report_ = std::max(max_queue_age_since_report_, age);
	return rate_->should_send_frame(age);
}

bool GdpSession::should_send_frame() {
	if (send_queue_clear()) {
		return true;
	}
	frames_skipped_since_report_++;
	return false;
}

void GdpSession::handle_input_data(Peer *peer, const uint8_t *data, size_t len) {
	if (peer != active_.get() || !authenticated_) {
		// The input stream is wired up at connection time, before any
		// SessionHello has been checked -- input arriving before (or
		// instead of) a valid hello must never reach the seat. Dropped
		// rather than treated as a violation: QUIC doesn't order the input
		// stream against the control stream, so a legitimate spectre's
		// first events can land here before its hello has been processed.
		return;
	}
	gdp::session::InputEnvelope env;
	bool ok = peer->input_reader.feed_and_drain(data, len, &env, [&] {
		InputSink &input = host_.input();
		uint32_t time_msec = (uint32_t)(env.client_time_us() / 1000);
		if (env.has_key()) {
			uint32_t evdev = hid_usage_to_evdev(env.key().hid_usage());
			if (evdev != 0) {
				input.inject_key(time_msec, evdev,
					env.key().state() == gdp::session::KEY_STATE_PRESSED ? WL_KEYBOARD_KEY_STATE_PRESSED
																		 : WL_KEYBOARD_KEY_STATE_RELEASED);
			}
		} else if (env.has_pointer_motion()) {
			const auto &m = env.pointer_motion();
			if (m.absolute()) {
				input.inject_motion_absolute(time_msec, m.dx(), m.dy());
			} else {
				input.inject_motion(time_msec, m.dx(), m.dy());
			}
		} else if (env.has_pointer_button()) {
			const auto &b = env.pointer_button();
			input.inject_button(time_msec, b.button(),
				b.state() == gdp::session::KEY_STATE_PRESSED ? WL_POINTER_BUTTON_STATE_PRESSED
															 : WL_POINTER_BUTTON_STATE_RELEASED);
		} else if (env.has_pointer_axis()) {
			const auto &a = env.pointer_axis();
			if (a.vertical() != 0.0) {
				input.inject_axis(time_msec, WL_POINTER_AXIS_VERTICAL_SCROLL, a.vertical());
			}
			if (a.horizontal() != 0.0) {
				input.inject_axis(time_msec, WL_POINTER_AXIS_HORIZONTAL_SCROLL, a.horizontal());
			}
		} else if (env.has_gamepad_connect()) {
			handle_gamepad_connect(env.gamepad_connect());
		} else if (env.has_gamepad_disconnect()) {
			handle_gamepad_disconnect(env.gamepad_disconnect());
		} else if (env.has_gamepad()) {
			handle_gamepad_state(env.gamepad());
		} else if (env.has_hid_connect()) {
			handle_hid_connect(env.hid_connect());
		} else if (env.has_hid_input()) {
			if (raw_negotiated()) {
				raw_->input(env.hid_input());
			}
		} else if (env.has_hid_get_report_reply()) {
			if (raw_negotiated()) {
				raw_->get_report_reply(env.hid_get_report_reply());
			}
		} else if (env.has_hid_set_report_reply()) {
			if (raw_negotiated()) {
				raw_->set_report_reply(env.hid_set_report_reply());
			}
		}
		// TouchEvent: ignored; wraith has no uinput touch device.
		return true;
	});
	if (!ok) {
		reset_session(peer->input_reader.error_code());
	}
}

void GdpSession::send_cursor_shape(uint32_t width, uint32_t height, int32_t hotspot_x, int32_t hotspot_y,
	const uint8_t *argb8888) {
	if (!authenticated_) {
		return;
	}
	if (width > gdp::kMaxCursorDim || height > gdp::kMaxCursorDim) {
		WLOG_INFO("gdp: not sending a %ux%u cursor, over the %u-pixel cap", width, height,
			gdp::kMaxCursorDim);
		return;
	}
	gdp::session::ControlEnvelope env;
	auto *shape = env.mutable_cursor_shape();
	shape->set_width(width);
	shape->set_height(height);
	shape->set_hotspot_x((uint32_t)hotspot_x);
	shape->set_hotspot_y((uint32_t)hotspot_y);
	shape->set_argb8888(argb8888, (size_t)width * (size_t)height * 4);
	send_control(env);
}

void GdpSession::send_cursor_hidden() {
	if (!authenticated_) {
		return;
	}
	gdp::session::ControlEnvelope env;
	env.mutable_cursor_shape();
	send_control(env);
}

void GdpSession::send_cursor_position(double x, double y) {
	if (!authenticated_) {
		return;
	}
	gdp::session::ControlEnvelope env;
	auto *pos = env.mutable_cursor_position();
	pos->set_x(x);
	pos->set_y(y);
	send_control(env);
}

void GdpSession::send_video_packet(bool keyframe, const uint8_t *data, size_t len, int64_t capture_pts_us) {
	if (!authenticated_) {
		return;
	}

	gdp::VideoDatagramHeader hdr;
	hdr.stream_id = kStreamId;
	hdr.frame_id = next_frame_id_++;
	sent_frames_[hdr.frame_id % kSentFrames] = SentFrame{hdr.frame_id, capture_pts_us, true};
	hdr.flags = keyframe ? gdp::VideoDatagramHeader::kFlagKeyframe : 0;
	hdr.pts = clock_.now_us();
	hdr.host_latency = gdp::VideoDatagramHeader::host_latency_from_us(monotonic_now_us() - capture_pts_us);
	if (keyframe) {
		have_keyframe_ = true;
		last_keyframe_frame_id_ = hdr.frame_id;
		if (keyframe_pending_ && (int32_t)(hdr.frame_id - keyframe_requested_at_) >= 0) {
			keyframe_pending_ = false;
		}
	}

	// A repair owed from a lost frame, now that its interval may be up;
	// the request applies to the encoder's next frame.
	service_owed_keyframe();

	// Read per frame, not once: QUIC starts at its minimum MTU and MTU
	// discovery raises the limit over the first round trips (the fallback
	// covers the moment before it's known). Every slice of one frame uses
	// the same size.
	uint16_t max_datagram = active_->conn->max_datagram_size();
	size_t payload = gdp::slice_payload_for(max_datagram);
	if (payload != slice_payload_) {
		WLOG_INFO("gdp: video slices now %zu bytes (max datagram %u, path MTU %u)", payload, max_datagram,
			active_->conn->path_mtu());
		slice_payload_ = payload;
	}
	gdp::slice_video_frame(hdr, data, len, payload, [this](const uint8_t *datagram, size_t datagram_len) {
		return active_->conn->send_datagram(datagram, datagram_len);
	});
}

void GdpSession::send_audio_packet(const uint8_t *data, size_t len) {
	if (!authenticated_) {
		return;
	}

	gdp::AudioDatagramHeader hdr;
	hdr.seq = audio_seq_++;
	hdr.pts = clock_.now_us();

	std::vector<uint8_t> datagram(1 + gdp::AudioDatagramHeader::kSize + len);
	hdr.encode(datagram.data());
	std::memcpy(datagram.data() + 1 + gdp::AudioDatagramHeader::kSize, data, len);

	active_->conn->send_datagram(datagram.data(), datagram.size(), /*priority=*/true);
}

} // namespace wraith
