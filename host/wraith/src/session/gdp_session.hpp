// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The session-connection side of GDP (gdp-spec.md §6-§11), wraith's end.
//
// Two ways to reach here (main.cpp): direct-connect mode (a static
// shared-secret token, no ghostd) and ghostd-mediated mode (a session
// secret from host/proto/control.proto's SessionInit, verified per
// gdp-spec.md §4.8 via session/token.cpp). Either way, all this class
// needs is "is this presented token good?" -- the caller supplies that as
// a predicate so this class doesn't need to know which mode produced it.
#pragma once

#include "gdp/clipboard.hpp"
#include "gdp/clock.hpp"
#include "gdp/framing.hpp"
#include "gdp/gamepad.hpp"
#include "gdp/transport.hpp"
#include "session/gamepad_devices.hpp"
#include "session/raw_controllers.hpp"
#include "session/path_rate_estimator.hpp"
#include "session/rate_controller.hpp"
#include "session/session_host.hpp"
#include "util/event_source.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gdp::session {
class ClipboardData;
class ControlEnvelope;
class GamepadConnect;
class GamepadDisconnect;
class GamepadState;
class HidConnect;
class HostInputEnvelope;
class SessionHello;
class StreamStats;
} // namespace gdp::session

namespace wraith {

using TokenValidator = std::function<bool(const std::string &token)>;

// Handles exactly one active spectre connection (the viewer) at a time
// -- there are no multi-viewer sessions. A connection only becomes the
// viewer once its SessionHello's token checks out; until then it waits in
// a short list of pending connections (kMaxPendingPerSource per address,
// kMaxPending in all; a source over its share loses its oldest) with
// kHelloDeadlineMs to send that hello, so a peer that completes the QUIC
// handshake and then idles can't hold the session, and one address
// can't evict another's (gdp-spec.md §6.3). Every pre-auth failure
// closes with AUTH_FAILED and nothing more specific; the log gets the
// real reason. A second peer whose token *is* good, while a viewer is
// attached, gets ALREADY_CONNECTED -- unless its hello says take_over
// (gdp-spec.md §6.4), when the viewer goes with
// TAKEN_OVER and it attaches instead.
class GdpSession {
public:
	// `control_socket` is the -G control socket path, empty under -l.
	// wraith reports the viewer attaching and detaching there
	// (ViewerAttached/ViewerDetached, host/proto/control.proto).
	// `gamepads` is false when this host can't create virtual input
	// devices (no -G at all, or ghostd said uinput/the udev rule is
	// missing -- SessionInit.uinput_available): then the session never
	// offers the "gamepad" capability, and a client's gamepad messages
	// are dropped. `raw_controllers` is the same for raw HID controllers
	// (SessionInit.uhid_available): the "hid" capability, and "gamepad"
	// with it even without uinput.
	GdpSession(SessionHost &host, TokenValidator token_validator, const std::string &control_socket = "",
		bool gamepads = false, bool raw_controllers = false);
	~GdpSession();

	// `*address_in_use`: see gdp::Transport::listen().
	bool listen(uint16_t port, const std::string &cert_file, const std::string &key_file,
		bool *address_in_use = nullptr);

	// Integrate into wraith's own event loop: add notify_fd() as a
	// WL_EVENT_READABLE source and call dispatch() when it fires.
	int notify_fd() const;
	void dispatch();

	// The session is being ended because its user logged in at the
	// console: closes the viewer with ENDED_BY_LOCAL_LOGIN now, before the
	// logout finishes, and gives any later connection whose token checks
	// out the same code instead of the desktop that is going away.
	void end_for_local_login();

	// True once SessionHello's token has been validated and SessionAccept
	// sent. SessionServices only calls send_video_packet() while this is
	// true.
	bool active() const { return authenticated_; }

	// What the active session negotiated (gdp-spec.md §6.5): the video
	// codec chosen from SessionHello.codecs, and the capabilities both ends
	// support. Empty when !active().
	const std::string &video_codec() const { return video_codec_; }
	const std::vector<std::string> &capabilities() const { return capabilities_; }

	// Support report (`wraith --report`, gdp-spec.md §7.11). What this
	// side knows about the current or most recent session -- the codecs
	// each side offered, what was chosen, why -- as plain text.
	std::string status_text() const;
	// Asks the attached client for its DiagnosticsReport; `done` gets the
	// text, or a sentence saying why there is none (no viewer, the viewer
	// left), exactly once -- unless cancel_client_diagnostics() comes
	// first, which drops it without calling. False (and `done` never
	// called) if one is already waiting or no client is attached. The
	// client may never answer (it may be gone): the caller owns the
	// timeout.
	bool request_client_diagnostics(std::function<void(std::string)> done);
	void cancel_client_diagnostics() { diagnostics_done_ = nullptr; }

	// Slices `data` into video datagrams per gdp-spec.md §9 and sends
	// them on the active connection, at the largest slice the connection
	// currently allows (gdp::Connection::max_datagram_size()). The header
	// reports how long ago `capture_pts_us` (monotonic_now_us()'s clock)
	// was as the frame's host latency. No-op if !active().
	void send_video_packet(bool keyframe, const uint8_t *data, size_t len, int64_t capture_pts_us);

	// Sends one Opus frame as an audio datagram per gdp-spec.md §10,
	// ahead of any video still queued. Always one datagram (never sliced --
	// a 10ms Opus frame is far under gdp::kFallbackDatagramPayload). No-op
	// if !active().
	void send_audio_packet(const uint8_t *data, size_t len);

	// The send-queue gate (session/rate_controller.hpp): false while what
	// the transport still holds unsent has waited past the network profile's
	// limit, meaning the frame about to be encoded should be skipped --
	// queued behind that backlog it would only arrive late, and add to it.
	// SessionServices asks before every encode and re-delivers the latest
	// frame once this turns true again. Always true with no session.
	bool should_send_frame();
	// The same test without counting a skipped frame: for polling whether
	// a skipped frame can be re-delivered yet.
	bool send_queue_clear();

	// Sends a CursorShape control message (gdp-spec.md §7.4).
	// `argb8888` must be width*height*4 bytes, premultiplied,
	// DRM_FORMAT_ARGB8888 layout (ScreencastHost's capture callbacks are
	// the callers). No-op if !active().
	void send_cursor_shape(uint32_t width, uint32_t height, int32_t hotspot_x, int32_t hotspot_y,
		const uint8_t *argb8888);

	// Sends a CursorShape with an empty 0x0 image -- the convention spectre's
	// overlay renderer already uses to mean "hide the cursor" (see
	// OverlayRenderer::set_cursor_shape). No-op if !active().
	void send_cursor_hidden();

	// Sends a CursorPosition control message: `x`/`y` are 0..1 over the
	// output (see session.proto). No-op if !active().
	void send_cursor_position(double x, double y);

	// Sends the local clipboard's text as ClipboardData (gdp-spec.md §7.9).
	// No-op unless a session is active *and* negotiated the
	// "clipboard" capability, or when `utf8` is empty, over the size cap,
	// or the very text this peer just sent (gdp::ClipboardEcho). Wired to
	// the host's ClipboardSink::on_local_change for the life of a session.
	void send_clipboard(const std::string &utf8);

	// SessionServices got a different ClipboardSink (or its first one).
	// On the screencast backends that happens seconds *after* a client has
	// attached -- the captured compositor isn't up when ghostd redirects
	// it -- so an active session wires itself to the new sink here, push
	// of the current text included, rather than only at SessionAccept.
	// No-op unless a session is active and negotiated "clipboard".
	void clipboard_sink_changed();

private:
	// One QUIC connection and its per-connection stream state. Starts in
	// pending_ and moves to active_ once its token checks out. Always
	// heap-allocated and never copied, so the callbacks wired to its
	// address stay valid across that move.
	struct Peer {
		GdpSession *owner = nullptr;
		uint64_t id = 0; // for log lines only
		// The peer's address, what the pending cap counts by.
		std::string source;
		std::unique_ptr<gdp::Connection> conn;
		gdp::Stream *control = nullptr;
		gdp::FrameReader control_reader;
		gdp::FrameReader input_reader;
		// The input stream, once the client opened it: the host's side
		// of it carries HostInputEnvelope (gdp-spec.md §8.4).
		gdp::Stream *input = nullptr;
		// Pending only: fires kHelloDeadlineMs after the handshake.
		EventSource hello_deadline;
	};

	void handle_new_connection(std::unique_ptr<gdp::Connection> conn);
	void handle_control_data(Peer *peer, const uint8_t *data, size_t len);
	void handle_input_data(Peer *peer, const uint8_t *data, size_t len);
	void handle_peer_shutdown(Peer *peer);
	// A pending peer's first control message. Returns false once `peer`
	// has been closed (and destroyed), so the caller must stop draining;
	// true once it is the viewer and SessionAccept went out.
	bool handle_hello(Peer *peer, const gdp::session::ControlEnvelope &env);
	// ResolutionChange from the active client (gdp-spec.md §7.2).
	void handle_resolution_change(const gdp::session::ResolutionChange &change);
	// The one output's descriptor, as SessionAccept and DisplaysChanged
	// carry it.
	void fill_output(gdp::session::OutputDescriptor *output) const;
	// Closes a pending peer with AUTH_FAILED, logging `reason`, and drops it.
	void reject_pending(Peer *peer, const char *reason);
	// Removes `peer` from pending_, destroying it unless `out` takes it.
	void remove_pending(Peer *peer, std::unique_ptr<Peer> *out = nullptr);
	// Closes the viewer's connection with `error_code` (gdp-spec.md §12 /
	// gdp::ErrorCode; 0 for a normal close) and clears all session state.
	// Pending peers are untouched.
	void reset_session(uint64_t error_code = 0);
	// Tells ghostd a viewer attached or detached (no-op under -l).
	void report_viewer(bool attached);
	// SessionHello handling once the token has been validated: codec and
	// capability negotiation, then SessionAccept. Returns false (having
	// closed the connection) if there's no usable codec.
	bool accept_session(const gdp::session::SessionHello &hello);
	// Sends `env` on the control stream, logging (never silently dropping)
	// an encode failure -- which can't happen for our own messages.
	void send_control(const gdp::session::ControlEnvelope &env);

	// The optional capabilities this *host* supports right now
	// (gdp-spec.md §6.7): the build-time list plus "clipboard" if the
	// backend actually gave SessionServices a ClipboardSink. Computed per
	// session rather than static because whether the mechanism exists is
	// only known once the backend has opened.
	std::vector<std::string> supported_capabilities() const;
	// Called from accept_session() once capabilities are settled: hooks
	// the host's clipboard sink up to send_clipboard() and pushes its
	// current text once, so the first paste after connecting isn't stale.
	// No-op if "clipboard" wasn't negotiated.
	void start_clipboard_sync();
	// The inverse, from reset_session(): the sink outlives the session, so
	// its callback must not survive it.
	void stop_clipboard_sync();
	// An incoming ClipboardData: validates mime/size, then applies it to
	// the local clipboard. Dropped, never forwarded, if anything is off.
	void handle_clipboard_data(const gdp::session::ClipboardData &clipboard);

	// The three gamepad input messages (gdp-spec.md §8.5).
	// Each is a no-op unless the session negotiated "gamepad"; a client
	// that sends them anyway is ignored, not disconnected, matching how
	// an un-negotiated ClipboardData is treated.
	bool gamepads_negotiated() const;
	bool microphone_negotiated() const;
	void handle_datagram(Peer *peer, const uint8_t *data, size_t len);
	void handle_gamepad_connect(const gdp::session::GamepadConnect &connect);
	void handle_gamepad_disconnect(const gdp::session::GamepadDisconnect &disconnect);
	void handle_gamepad_state(const gdp::session::GamepadState &state);
	// Raw HID controllers (gdp-spec.md §8.6): only with both "gamepad"
	// and "hid" in effect and uhid on this host.
	bool raw_negotiated() const;
	void handle_hid_connect(const gdp::session::HidConnect &connect);
	// Toward the client on the input stream; dropped without a viewer.
	void send_host_input(const gdp::session::HostInputEnvelope &env);

	// A StatsReport for our stream: feeds the rate controller (the
	// transport's own datagram counters plus what spectre measured) and
	// applies its target to the encoder, then asks for a keyframe if the
	// report shows a lost frame no keyframe has already superseded.
	void handle_stream_stats(uint32_t rtt_us, const gdp::session::StreamStats &stats);
	// Requests a keyframe from the encoder and remembers which frame_id it
	// can come as, earliest, so a loss it will repair doesn't ask again.
	// `repair` asks for the video's IDR alone (Encoder::
	// request_repair_keyframe()): the lost frames' refinement layers were
	// already handed over through report_lost_frames().
	//
	// Either way the keyframe is only the encoder's *next* frame, and on a
	// desktop that has gone still -- a screencast compositor sends nothing
	// until something repaints -- no next frame would come. So the request
	// also has the host re-encode the current picture (redeliver_frame()),
	// from an idle callback: this runs inside send_video_packet() too,
	// which a synchronous re-encode would re-enter.
	void request_keyframe(bool repair = false);
	// Hands the encoder the pts of every frame in `new_loss` (bit i is frame
	// `highest` - i), so lossless refinement can redo what their layers
	// did (Encoder::frames_lost()). A frame too old for sent_frames_ is
	// answered with a plain request_keyframe(), whose plane reset covers it.
	void report_lost_frames(uint32_t highest, uint64_t new_loss);
	// A lost frame (up to frame_id `newest_lost`) needs repairing: requests
	// a keyframe now, or -- inside the network profile's minimum interval
	// since the last loss-driven one -- owes it for
	// service_owed_keyframe() to request once the interval is up.
	void repair_loss(uint32_t newest_lost);
	void service_owed_keyframe();
	// Paces the active connection's datagrams for the rate controller's
	// current target ([network] pacing_multiplier / pacing_floor_mbps).
	void apply_pacing();

	SessionHost &host_;
	TokenValidator token_validator_;
	gdp::Transport transport_;

	// The viewer, from the moment its token checks out (accept_session()
	// runs with it already here), and the connections still waiting to
	// authenticate, oldest first.
	std::unique_ptr<Peer> active_;
	std::vector<std::unique_ptr<Peer>> pending_;
	uint64_t next_peer_id_ = 1;
	// Set by end_for_local_login(); no connection becomes the viewer after.
	bool ended_by_local_login_ = false;
	gdp::SessionClock clock_;
	// True once SessionAccept has gone out to active_: what gates every
	// send, since active_ is already set during accept_session().
	bool authenticated_ = false;
	std::string video_codec_;
	std::vector<std::string> capabilities_;
	// How the last SessionHello was answered, for status_text(): kept past
	// the session, since a negotiation that failed is what a report is for.
	std::string negotiation_summary_;
	std::function<void(std::string)> diagnostics_done_;
	// Per-session, reset with it: which text crossed the wire in each
	// direction most recently, so neither end's own write comes back as a
	// fresh local change (gdp/clipboard.hpp).
	gdp::ClipboardEcho clipboard_echo_;
	// Clipboard text from the client that arrived before the host had a
	// sink to put it in -- the screencast startup window again (the client
	// attaches seconds before the captured compositor is up). Applied when
	// the sink appears; dropped with the session otherwise.
	std::string pending_clipboard_text_;
	// The client's forwarded controllers, or null on a host that can't
	// create them. Per-client, like the clipboard echo state: every slot
	// is disconnected in reset_session(), so a client going away unplugs
	// its pads from the session's point of view.
	std::unique_ptr<GamepadDevices> gamepads_;
	std::unique_ptr<RawControllers> raw_;
	// The -G socket, for ViewerAttached/ViewerDetached; empty under -l.
	std::string control_socket_;
	uint32_t next_frame_id_ = 0;
	uint16_t audio_seq_ = 0;

	// Single output: there is no multi-display support.
	static constexpr uint8_t kStreamId = 0;

	// Per session: made in accept_session(), dropped in reset_session().
	std::unique_ptr<RateController> rate_;
	// The transport's datagram counters at the previous report, for deltas.
	gdp::Connection::DatagramStats last_datagram_stats_;
	// The worst send backlog should_send_frame() saw since the previous
	// report, and the frames it skipped: the gate samples far more often
	// than reports come, and a backlog that cleared just before one still
	// happened.
	uint64_t max_queue_age_since_report_ = 0;
	uint32_t frames_skipped_since_report_ = 0;
	// The pacing rate apply_pacing() last set, bits/s; 0 when off.
	uint64_t pacing_bps_ = 0;
	// The rate apply_pacing() last logged, so only real moves are.
	uint64_t logged_pacing_bps_ = 0;
	// The path's slowest hop, from spectre's frame trains.
	PathRateEstimator path_rate_;
	// Last slice size used, so a change (MTU discovery) is logged once.
	size_t slice_payload_ = 0;
	// wraith.toml's network.rate_trace: log every report's measurements
	// and target, not just cuts (the netem harness's view into the
	// controller).
	bool rate_trace_ = false;
	// Where the previous report's loss bitmap was anchored, so the next one
	// can be masked down to the frames acked since (handle_stream_stats).
	bool have_stats_ = false;
	uint32_t last_highest_frame_id_acked_ = 0;
	// Keyframe bookkeeping: the frame_id of the newest keyframe sent, and
	// the earliest frame_id an outstanding request can come as. A reported
	// loss needs a keyframe only if neither a sent nor a requested one
	// comes after it. Refinement's layer is repaired separately and at
	// once (report_lost_frames()), so it doesn't wait on this.
	bool have_keyframe_ = false;
	uint32_t last_keyframe_frame_id_ = 0;
	bool keyframe_pending_ = false;
	uint32_t keyframe_requested_at_ = 0;
	// repair_loss()'s rate limit: when the last loss-driven keyframe was
	// asked for, and a repair still owed (for losses up to
	// owed_newest_lost_) until the interval allows another.
	uint64_t last_repair_us_ = 0;
	bool repair_owed_ = false;
	uint32_t owed_newest_lost_ = 0;
	// request_keyframe()'s re-encode of the current picture, from an idle
	// callback.
	IdleCoalescer keyframe_redeliver_;
	// The pts each recent frame_id was sent with, indexed by frame_id
	// modulo the size: a loss bitmap covers the last 64 frames, and a
	// report can arrive a few hundred ms late on a congested link.
	struct SentFrame {
		uint32_t frame_id = 0;
		int64_t pts_us = 0;
		bool valid = false;
	};
	static constexpr size_t kSentFrames = 256;
	std::vector<SentFrame> sent_frames_ = std::vector<SentFrame>(kSentFrames);
};

} // namespace wraith
