// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The session-connection side of GDP (gdp-spec.md §6), spectre's end.
// Mirrors wraith's GdpSession (host/wraith/src/session/gdp_session.hpp) but from
// the client role: opens the two streams, sends SessionHello, reassembles
// video datagram slices into coded frames.
#pragma once

#include "gdp/audio_format.hpp"
#include "gdp/clipboard.hpp"
#include "gdp/client_connection.hpp"
#include "gdp/framing.hpp"
#include "gdp/gamepad.hpp"
#include "gdp/video_reassembler.hpp"

#include <atomic>
#include <cstdint>
#include <chrono>
#include "video_color.hpp"

#include <functional>
#include <string>
#include <vector>

namespace gdp::session {
class ClipboardData;
class ControlEnvelope;
class DesktopState;
class FileFetch;
class FileList;
class HidConnect;
class InputEnvelope;
} // namespace gdp::session

namespace spectre {

struct DisplayInfo {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t refresh_mhz = 0;
	double scale = 1.0;
	// OutputDescriptor.color (gdp-spec.md §7.3), with the codec's defaults
	// filled in where the host left a field unspecified. Checked against
	// what SessionHello declared before it reaches on_accepted /
	// on_displays_changed: a depth this client never offered ends the
	// session (UNSUPPORTED) rather than reaching the decoder.
	VideoColor color;
};

// From SessionAccept.audio (gdp-spec.md §6.8, §10). `valid` is false
// when wraith sent no audio field at all -- e.g. its PipeWire capture
// failed to open (SessionServices' audio pipeline setup) -- in which case
// the session is video-only and spectre should not open an audio device
// at all.
struct AudioInfo : gdp::AudioFormat {
	bool valid = false;
};

// What the session negotiated (gdp-spec.md §6.6): the video codec
// wraith picked from the list spectre offered (always one spectre can
// decode -- an accept naming anything else is treated as a protocol
// error and the session is dropped), and the optional capabilities both
// ends support.
struct SessionInfo {
	std::string codec;
	std::vector<std::string> capabilities;
	// gdp-spec.md §6.5: any may be empty.
	std::string encoder;   // wraith's backend: "vaapi"/"nvenc"/"pyrowave"/"software", "+refine" when refined
	std::string host_user; // the remote session's account name
	std::string host_name; // the remote host's hostname
};

class SessionClient : public gdp::ClientConnection {
public:
	SessionClient();
	~SessionClient() override;

	// Connects and sends SessionHello with `token`, but only to a host
	// whose certificate fingerprint (gdp::sha256_hex() of its DER) is
	// exactly `cert_sha256` -- the value ghostd vouched for in
	// Redirect.cert_sha256 (gdp-spec.md §2.3). Any other certificate ends
	// the connection before the token is sent, and on_disconnected reports
	// both fingerprints.
	bool connect(const std::string &host, uint16_t port, const std::string &token,
		const std::string &cert_sha256);

	// Sets the one DisplayDescriptor SessionHello.displays carries
	// (gdp-spec.md §7.2), if any. Must be called before connect() --
	// SessionHello is built and sent from within it. 0x0 asks for no size,
	// and with no refresh rate either omits displays entirely. `scale` is
	// the desktop scale wanted (StreamSession::requested_scale()); 0 sends
	// none, which wraith takes as 100%. `refresh_mhz` is the frame rate
	// wanted; 0 sends none, which wraith takes as 60 Hz.
	void set_requested_display(uint32_t width, uint32_t height, double scale, uint32_t refresh_mhz) {
		requested_width_ = width;
		requested_height_ = height;
		requested_scale_ = scale;
		requested_refresh_mhz_ = refresh_mhz;
	}

	// SessionHello.codecs: the gdp/video_codec.hpp tokens this client can
	// decode, in preference order -- probe_decodable_codecs()'s result
	// (decode/codec_support.hpp). Must be called before connect() and before
	// set_preferred_codec(); wraith's pick is only accepted from this list.
	void set_decodable_codecs(std::vector<std::string> codecs);
	// SessionHello.codec_formats (gdp-spec.md §7.3): the codecs among
	// set_decodable_codecs()'s this client also decodes at 10 bits per
	// component -- probe_decodable_codecs()'s `ten_bit`. The host may then
	// answer with a 10-bit OutputDescriptor.color for one of them; it
	// stays at 8 for the rest. Must be called before connect().
	void set_ten_bit_codecs(std::vector<std::string> codecs);

	// Moves `codec` (a gdp/video_codec.hpp wire token) to the front of
	// SessionHello.codecs, ahead of the default order -- spectre -C. Must
	// be called before connect(), like set_requested_display(). A token
	// this client can't decode is reported and ignored: the rest of the
	// offer is still valid, and a session on the default codec beats no
	// session.
	void set_preferred_codec(const std::string &codec);

	// SessionHello.take_over (gdp-spec.md §6.4): displace the viewer
	// already attached to this user's session instead of being refused with
	// ALREADY_CONNECTED -- spectre -T, set only after the user agreed to it.
	// Must be called before connect().
	void set_take_over(bool take_over) { take_over_ = take_over; }

	// SessionHello.network_profile (gdp-spec.md §6.9): "auto" (the
	// default), "lan", "internet" or "mobile" -- spectre -N. Must be called
	// before connect(). Anything else is reported and ignored, leaving it on
	// auto.
	void set_network_profile(const std::string &profile);
	// What set_network_profile() left in effect (an unknown name is "auto").
	const std::string &network_profile() const { return network_profile_; }

	// Whether to offer the "refine" capability (gdp/refine.hpp) in
	// SessionHello.capabilities; on unless turned off. spectre always
	// offers it and switches the layer with RefinePause (the menu's
	// Lossless row); tools that want the bare bitstream turn it off. Must
	// be called before connect().
	void set_lossless_refinement(bool enabled) { offer_refine_ = enabled; }

	// Whether to offer the "gamepad" capability (gdp/gamepad.hpp) --
	// only with spectre -G. Must be called before connect(). Off means
	// gamepad_enabled() stays false, so no controller is ever forwarded.
	void set_gamepad_forwarding(bool enabled) { offer_gamepad_ = enabled; }
	// Whether to offer "hid" alongside "gamepad" (gdp-spec.md §8.6): raw
	// controllers. Must be called before connect().
	void set_raw_controllers(bool enabled) { offer_hid_ = enabled; }
	// Whether to offer "security-key" (gdp-spec.md §8.7) -- spectre -U.
	// Must be called before connect(). Off means no key is ever opened.
	void set_security_keys(bool enabled) { offer_security_key_ = enabled; }
	// Whether to offer "smartcard" (gdp-spec.md §8.8) -- spectre -Y.
	// Must be called before connect(). Off means no reader is ever opened.
	void set_smartcards(bool enabled) { offer_smartcard_ = enabled; }

	// Whether to offer the "microphone" capability (gdp/audio_format.hpp)
	// -- spectre -M. Must be called before connect(). Off means the
	// microphone is never opened, let alone sent.
	void set_microphone(bool enabled) { offer_microphone_ = enabled; }
	// True once SessionAccept put "microphone" in effect, with the format
	// the host expects in microphone_format().
	bool microphone_enabled() const { return microphone_enabled_; }
	const gdp::AudioFormat &microphone_format() const { return microphone_format_; }
	// One Opus packet of captured audio to the host (datagram channel
	// 0x03). Safe from any thread (the audio capture thread calls it);
	// a no-op unless microphone_enabled().
	void send_microphone_packet(const uint8_t *data, size_t len);

	// Fires once, from dispatch(), when SessionAccept arrives.
	std::function<void(const SessionInfo &session, const DisplayInfo &display, const AudioInfo &audio)>
		on_accepted;
	// Fires at most once: on SessionReject (arg is the reason), a frame
	// violation, or the connection shutting down for any other reason
	// (the peer's gdp-spec.md §12 close code described in words, or an
	// empty string for a plain close) -- at any point, including
	// pre-accept.
	std::function<void(const std::string &reason)> on_disconnected;
	// The peer's gdp-spec.md §12 close code once on_disconnected has fired
	// for the connection shutting down; 0 for a plain close or any other
	// cause.
	uint64_t disconnect_code() const { return disconnect_code_; }
	// Fires once per fully-reassembled coded frame (all slices for a
	// frame_id received). A frame whose slices never all arrive (loss over
	// the unreliable datagram channel) is simply never delivered -- nothing
	// to retransmit, this is a live stream.
	// Returns whether the frame was consumed. A fully reassembled frame the
	// session then throws away (no window yet, a malformed refined
	// container, a decoder error) is reported to wraith as *lost* in the
	// next StatsReport, exactly like one that never arrived: from the
	// host's point of view the two are the same -- the client is not
	// showing what was sent -- and the keyframe (and, on a refined session,
	// the overlay-plane reset) that loss triggers is what puts it right.
	// Without this, a discarded frame that carried a refinement *clear*
	// would leave a stale lossless block over a region that then keeps
	// moving, with nothing left to ever heal it.
	std::function<bool(bool keyframe, const uint8_t *data, size_t len)> on_video_frame;
	// Fires once per received audio datagram (gdp-spec.md §10), in
	// whatever order they arrive over the network -- reordering/loss
	// handling is JitterBuffer's job (audio/jitter_buffer.hpp), not this
	// class's. `data` is one raw Opus packet, valid only for the duration
	// of the callback.
	std::function<void(uint16_t seq, uint32_t pts, const uint8_t *data, size_t len)> on_audio_frame;
	// Fires whenever wraith sends a new cursor image (gdp-spec.md §7.4).
	// `argb8888` is width*height*4 premultiplied bytes, valid only for the
	// duration of the callback.
	std::function<void(uint32_t width, uint32_t height, int32_t hotspot_x, int32_t hotspot_y,
		const uint8_t *argb8888)>
		on_cursor_shape;
	// Fires when wraith reports where its pointer is (gdp-spec.md §7.4):
	// `x`/`y` are 0..1 over the remote output. Only sent while this client
	// is driving the pointer with relative motion.
	std::function<void(double x, double y)> on_cursor_position;
	// Fires when the remote clipboard's text changed (gdp-spec.md §7.9),
	// including the host's one-shot push right after SessionAccept. Only
	// ever fires if the session negotiated the "clipboard" capability;
	// StreamSession wires it to SDL_SetClipboardText. `utf8` is non-empty
	// and within the size cap -- anything else was dropped here.
	std::function<void(const std::string &utf8)> on_clipboard_text;
	// Fires on FileList (gdp-spec.md §7.12): what the host's user copied, the
	// latest list (no files: the clipboard holds something else now). Only
	// with "file" and "clipboard" in effect.
	std::function<void(const gdp::session::FileList &list)> on_file_list;
	// Fires on FileFetch: the host wants one of this client's files, or a
	// folder's contents (file-clipboard, file-share). Answered on a stream of
	// open_file_stream()'s.
	std::function<void(const gdp::session::FileFetch &fetch)> on_file_fetch;
	// Fires on DisplaysChanged (gdp-spec.md §7.2): the output's size now,
	// the answer to send_resolution_change() whether or not the host could
	// change it.
	std::function<void(const DisplayInfo &display)> on_displays_changed;
	// Fires on DiagnosticsRequest (gdp-spec.md §7.11): returns the text to
	// answer with, which SessionClient trims to the size cap. Unset, the
	// request is ignored.
	std::function<std::string()> on_diagnostics_request;
	// Fires on DesktopState (gdp-spec.md §7.13): the remote desktop's state,
	// all of it -- once after SessionAccept, then on each change.
	std::function<void(const gdp::session::DesktopState &state)> on_desktop_state;

	// Input events (gdp-spec.md §8). No-ops before SessionAccept.
	void send_key(uint32_t hid_usage, bool pressed);
	void send_pointer_motion(double dx, double dy, bool absolute);
	void send_pointer_button(uint32_t button, bool pressed);
	void send_pointer_axis(double horizontal, double vertical);
	// Gamepads (gdp-spec.md §8.5). All no-ops unless the session negotiated
	// "gamepad" -- StreamSession checks gamepad_enabled() before even
	// opening a controller, so a host that can't take them costs nothing
	// here. `pad_index` is the slot in
	// [0, gdp::kMaxGamepads) that StreamSession assigned the controller.
	void send_gamepad_connect(uint32_t pad_index, const std::string &name);
	void send_gamepad_disconnect(uint32_t pad_index);
	void send_gamepad_state(uint32_t pad_index, const gdp::GamepadSnapshot &state);
	// True once SessionAccept put the "gamepad" capability in effect.
	bool gamepad_enabled() const { return gamepad_enabled_; }

	// Raw HID controllers (gdp-spec.md §8.6). No-ops unless hid_enabled().
	// Safe from any thread -- a controller's reader and worker threads
	// send straight onto the input stream, so the main loop's pacing adds
	// no latency to them.
	void send_hid_connect(const gdp::session::HidConnect &connect);
	void send_hid_input(uint32_t pad_index, const uint8_t *data, size_t len);
	void send_hid_get_report_reply(uint32_t pad_index, uint32_t request_id, bool failed, const uint8_t *data,
		size_t len);
	void send_hid_set_report_reply(uint32_t pad_index, uint32_t request_id, bool failed);
	// True once SessionAccept put both "gamepad" and "hid" in effect.
	bool hid_enabled() const { return hid_enabled_.load(std::memory_order_acquire); }
	// What the host sends about raw controllers on the input stream
	// (gdp-spec.md §8.4). All fire only from dispatch(). `type` is the
	// wire's HidReportType.
	std::function<void(uint32_t pad_index, const std::string &reason)> on_hid_rejected;
	std::function<void(uint32_t pad_index, const std::string &data)> on_hid_output;
	std::function<void(uint32_t pad_index, uint32_t request_id, uint32_t report_id, int type)>
		on_hid_get_report;
	std::function<void(uint32_t pad_index, uint32_t request_id, uint32_t report_id, int type,
		const std::string &data)>
		on_hid_set_report;

	// Security keys (gdp-spec.md §8.7). No-ops unless
	// security_keys_enabled(); safe from any thread, like the raw
	// controllers' senders, since a key's reader thread sends its packets.
	void send_security_key_connect(uint32_t key_index, const std::string &name, uint32_t vendor_id,
		uint32_t product_id, uint32_t version);
	void send_security_key_disconnect(uint32_t key_index);
	void send_security_key_input(uint32_t key_index, const uint8_t *packet, size_t len);
	// True once SessionAccept put "security-key" in effect.
	bool security_keys_enabled() const { return security_key_enabled_.load(std::memory_order_acquire); }
	// What the host sends about security keys; fire only from dispatch().
	std::function<void(uint32_t key_index, const std::string &reason)> on_security_key_rejected;
	std::function<void(uint32_t key_index, const std::string &packet)> on_security_key_output;

	// Smart cards (gdp-spec.md §8.8). No-ops unless smartcards_enabled();
	// from the main thread, which reads every answer to the host through
	// the card's policy first. `error` is a gdp::session::SmartcardError.
	void send_smartcard_reader_added(uint32_t reader_index, const std::string &name, bool card_present,
		const std::string &atr);
	void send_smartcard_reader_removed(uint32_t reader_index);
	void send_smartcard_card_inserted(uint32_t reader_index, const std::string &atr);
	void send_smartcard_card_removed(uint32_t reader_index);
	void send_smartcard_powered(uint32_t reader_index, uint32_t sequence, int error, const std::string &atr);
	void send_smartcard_response(uint32_t reader_index, uint32_t sequence, int error, const std::string &response);
	// True once SessionAccept put "smartcard" in effect.
	bool smartcards_enabled() const { return smartcard_enabled_; }
	// What the host sends about smart cards; fire only from dispatch().
	// `action` is a gdp::session::SmartcardPowerAction.
	std::function<void(uint32_t reader_index, const std::string &reason)> on_smartcard_rejected;
	std::function<void(uint32_t reader_index, uint32_t sequence, int action)> on_smartcard_power;
	std::function<void(uint32_t reader_index, uint32_t sequence, const std::string &apdu)> on_smartcard_transmit;
	void request_keyframe();
	// Sends the local clipboard's text to wraith (gdp-spec.md §7.9). A
	// no-op before SessionAccept, unless the session negotiated the
	// "clipboard" capability, or when `utf8` is empty, over the size cap, or
	// the very text wraith just sent us (gdp::ClipboardEcho -- SDL raises
	// SDL_EVENT_CLIPBOARD_UPDATE for spectre's own SDL_SetClipboardText,
	// which must not bounce back).
	void send_clipboard(const std::string &utf8);
	// True once SessionAccept put the "clipboard" capability in effect --
	// StreamSession checks it before even reading the local clipboard.
	bool clipboard_enabled() const { return clipboard_enabled_; }
	// Called by StreamSession immediately *before* it hands wraith's text
	// to SDL_SetClipboardText, so the SDL_EVENT_CLIPBOARD_UPDATE that call
	// raises is recognised as our own write rather than a fresh local copy.
	void note_clipboard_applied(const std::string &utf8) { clipboard_echo_.note_applied(utf8); }
	// Whether to offer the "file" capability (gdp/file_transfer.hpp), and
	// with it "file-clipboard" (copied files both ways, folders included);
	// on unless turned off. Must be called before connect().
	void set_file_transfer(bool enabled) { offer_file_ = enabled; }
	// Whether to offer "file-share": this client has folders to share. Must
	// be called before connect().
	void set_file_share(bool enabled) { offer_file_share_ = enabled; }
	// True once SessionAccept put "file" in effect, and the others.
	bool file_enabled() const { return file_enabled_; }
	bool file_clipboard_enabled() const { return file_clipboard_enabled_; }
	bool file_share_enabled() const { return file_share_enabled_; }
	// What this client's user copied, for the host (file-clipboard); a list
	// with no files takes it back. No-op unless file_clipboard_enabled().
	void send_file_list(const gdp::session::FileList &list);
	// A folder this client shares, or (`removed`) stops sharing. No-op unless
	// file_share_enabled().
	void send_file_share(uint64_t share_id, const std::string &name, bool removed);
	// A new "file" stream (gdp-spec.md §7.12) for FileTransfers, or null
	// unless file_enabled().
	std::unique_ptr<gdp::Stream> open_file_stream();
	// Asks wraith to end the desktop session (gdp-spec.md §7.10). No reply:
	// completion is on_disconnected with wraith's SESSION_ENDED close code.
	// No-op before SessionAccept.
	void send_logout_request();
	// Asks wraith to change the output to width x height, at `scale` (0:
	// none) mid-session (gdp-spec.md §7.2); on_displays_changed answers.
	// No-op before SessionAccept.
	void send_resolution_change(uint32_t width, uint32_t height, double scale);
	// Asks wraith to stop or resume building the lossless layer
	// (gdp-spec.md §7.8: RefinePause), the menu's Lossless row. The caller only
	// sends it on a session that negotiated "refine". No-op before
	// SessionAccept.
	void send_refine_pause(bool paused);

	// A read-only snapshot for the statistics overlay -- the same numbers
	// the next StatsReport will carry, without consuming them.
	struct LinkStats {
		uint32_t rtt_us = 0;
		uint32_t lost_recent = 0; // frames never completed, of the trailing...
		uint32_t window_span = 0; // ...this many frame_ids (up to 64), stream 0
		// The one-way delay figures the last StatsReport carried (see
		// ReceiveAccum): an unknown constant offset, so only their movement
		// means anything. delay_samples is 0 before the first report, or if
		// no first slice arrived in its window.
		int32_t delay_min_us = 0;
		int32_t delay_avg_us = 0;
		uint32_t delay_samples = 0;
		// "fec" is in effect, the frames parity has rebuilt this session, and
		// the data slices of the frames finished and how many of them were
		// lost before any recovery (all as of the last StatsReport).
		bool fec = false;
		uint64_t fec_recovered = 0;
		uint64_t slices_expected = 0;
		uint64_t slices_lost = 0;
	};
	LinkStats link_stats() const;

	// Host latency (VideoDatagramHeader::host_latency, capture to send on
	// wraith) of the frames completed since the last call, which starts
	// the next window. samples is 0 when none arrived or the host doesn't
	// report it.
	struct HostLatency {
		uint32_t min_us = 0;
		uint32_t max_us = 0;
		uint64_t sum_us = 0;
		uint32_t samples = 0;
	};
	HostLatency take_host_latency();

	// The network profile wraith's rate control is running with
	// (SessionAccept.network_profile, then each NetworkProfileChanged):
	// "lan", "internet" or "mobile" -- for an AUTO session, what wraith
	// classified the link as. Empty before SessionAccept or from a wraith
	// that doesn't report it.
	const std::string &host_network_profile() const { return host_network_profile_; }
	// Whether a gateway (Veil) relays this session: SessionAccept.via_gateway
	// (gdp-spec.md §6.10). False before SessionAccept.
	bool via_gateway() const { return via_gateway_.load(std::memory_order_relaxed); }

	// Accumulates one sample for the next send_stats_report() (averaged
	// over however many frames land between calls -- StreamSession
	// calls this once per decoded/presented frame and send_stats_report()
	// on a 250ms timer, gdp-spec.md §7.5).
	void record_frame_timing(uint32_t decode_us, uint32_t present_us);

	// Builds and sends a StatsReport: RTT from the QUIC connection's own
	// stats and, once a frame has been seen, stream_id 0's highest frame_id
	// acked, a loss bitmap over the trailing 64 frame_ids, the average
	// decode/present timings accumulated since the last call, and what
	// arrived since then -- bytes and each frame's first-slice one-way
	// delay (gdp-spec.md §7.5). No-op before SessionAccept.
	void send_stats_report();

private:
	// When the last DiagnosticsRequest was answered (zero: never).
	std::chrono::steady_clock::time_point last_diagnostics_{};
	std::vector<std::string> offered_capabilities() const;
	void on_connected() override;
	void on_connection_shutdown() override;
	void handle_control_data(const uint8_t *data, size_t len);
	// HostInputEnvelope frames on the input stream (gdp-spec.md §8.4).
	void handle_input_data(const uint8_t *data, size_t len);
	void handle_datagram(const uint8_t *data, size_t len, uint64_t arrival_us);
	// Fires on_disconnected at most once. A non-zero `error_code`
	// (gdp::ErrorCode) also closes the connection with it, so wraith's
	// log says why spectre hung up.
	void disconnect(const std::string &reason, uint64_t error_code = 0);
	// An incoming ClipboardData: validates mime/size and the negotiated
	// capability, then fires on_clipboard_text. Dropped, never applied, if
	// anything is off.
	void handle_clipboard_data(const gdp::session::ClipboardData &clipboard);
	// SessionAccept handling: checks wraith's codec/capability choices
	// against what was offered, then fires on_accepted. Returns false
	// (having disconnected) on a protocol violation.
	bool handle_accept(const gdp::session::ControlEnvelope &env);

	// Send on the input stream (a no-op before SessionAccept) / the control
	// stream, logging an encode failure -- which can't happen for our own
	// messages -- rather than dropping it silently.
	void send_input(const gdp::session::InputEnvelope &env);
	void send_control(const gdp::session::ControlEnvelope &env);

	std::string token_;
	std::string expected_cert_sha256_;
	std::string presented_cert_sha256_; // what the host actually showed, for the mismatch report
	// SessionHello.codecs in preference order, from set_decodable_codecs().
	std::vector<std::string> offered_codecs_;
	// SessionHello.codec_formats' 10-bit entries, from set_ten_bit_codecs().
	std::vector<std::string> ten_bit_codecs_;
	// The session's codec once accepted, for display_info()'s defaults.
	std::string session_codec_;
	// Whether OutputDescriptor.color stays within the declaration above
	// (gdp-spec.md §7.3); logs what's wrong when it doesn't.
	bool color_declared(const VideoColor &color) const;
	// Empty unless set_preferred_codec() named one of offered_codecs_.
	std::string preferred_codec_;
	std::string network_profile_ = "auto";
	bool take_over_ = false;
	std::string host_network_profile_;
	bool offer_gamepad_ = false;
	bool offer_hid_ = false;
	bool offer_security_key_ = false;
	bool offer_smartcard_ = false;
	bool offer_refine_ = true;
	bool offer_file_ = true;
	bool offer_file_share_ = false;
	bool file_enabled_ = false;
	bool file_clipboard_enabled_ = false;
	bool file_share_enabled_ = false;
	uint32_t requested_width_ = 0;
	uint32_t requested_height_ = 0;
	double requested_scale_ = 0.0;
	uint32_t requested_refresh_mhz_ = 0;
	gdp::Stream *control_ = nullptr;
	gdp::Stream *input_ = nullptr;
	gdp::FrameReader control_reader_;
	gdp::FrameReader input_reader_;
	bool accepted_ = false;
	bool disconnect_reported_ = false;
	uint64_t disconnect_code_ = 0;
	// Clipboard sync state, valid from SessionAccept on: whether the
	// capability is in effect, and which text crossed the wire in each
	// direction most recently (so neither end's own write comes back as a
	// fresh local change -- gdp/clipboard.hpp).
	bool clipboard_enabled_ = false;
	gdp::ClipboardEcho clipboard_echo_;
	bool gamepad_enabled_ = false;
	std::atomic<bool> hid_enabled_{false};
	std::atomic<bool> security_key_enabled_{false};
	bool smartcard_enabled_ = false;
	bool offer_microphone_ = false;
	bool microphone_enabled_ = false;
	gdp::AudioFormat microphone_format_;
	std::atomic<uint16_t> microphone_seq_{0};

	// spectre shows a single output, stream_id 0 (gdp-spec.md §9.1):
	// frames for any other stream are dropped, and everything below
	// describes stream 0 alone.
	gdp::VideoFrameReassembler reassembler_;
	// "fec" in effect: parity datagrams (gdp-spec.md §9.7) go to
	// reassembler_ too. fec_recovered_total_ adds up its frames_recovered,
	// for link_stats(), as the other two do its slice counts.
	bool fec_enabled_ = false;
	std::atomic<uint64_t> fec_recovered_total_{0};
	std::atomic<uint64_t> slices_expected_total_{0};
	std::atomic<uint64_t> slices_lost_total_{0};

	// Sliding ack/loss window (gdp-spec.md §7.5's StatsReport): bit i of
	// completed_mask is set if frame_id (highest_frame_id - i) was fully
	// reassembled. Shifts as new highest
	// frame_ids arrive; a shifted-in bit defaults to 0 (lost) unless that
	// frame_id later completes out of order.
	//
	// window_span is how many of those 64 bits refer to frame_ids at or
	// after the first frame this session ever saw: 1 after the first
	// frame, growing with each shift, saturating at 64. Bits beyond it
	// are frames that never existed, and must be masked out of the
	// reported loss_bitmap -- otherwise every report in roughly the first
	// second of a session claims losses, and wraith answers each one with
	// a keyframe to repair it.
	struct AckState {
		uint32_t highest_frame_id = 0;
		uint64_t completed_mask = 0;
		uint32_t window_span = 0;
		bool has_data = false;
	};
	AckState ack_;

	// record_frame_timing()'s samples since the last StatsReport.
	struct TimingAccum {
		uint64_t decode_us_sum = 0;
		uint64_t present_us_sum = 0;
		uint32_t sample_count = 0;
	};
	TimingAccum timing_;

	// The frame whose datagrams are arriving now, timed for
	// StreamStats.trains (gdp-spec.md §7.5).
	struct TrainTiming {
		bool active = false;
		uint32_t frame_id = 0;
		uint64_t first_us = 0;
		uint64_t last_us = 0;
		uint32_t datagrams = 0;
		uint64_t bytes_after_first = 0;
	};
	TrainTiming train_;
	struct TrainSample {
		uint32_t frame_id;
		uint32_t datagrams;
		uint32_t bytes;
		uint32_t span_us;
	};
	// What arrived on stream 0 since the last StatsReport, for wraith's
	// rate controller. Delays are a first slice's arrival on this client's
	// session clock (zeroed at SessionAccept, like wraith's at sending it)
	// minus its frame's pts: the offset between the two zeroes is unknown
	// but constant, and only the movement matters.
	struct ReceiveAccum {
		uint64_t window_start_us = 0; // gdp::monotonic_us()
		uint64_t bytes = 0;
		int64_t delay_sum_us = 0;
		int32_t delay_min_us = 0;
		uint32_t delay_samples = 0;
		std::vector<TrainSample> trains;
	};
	ReceiveAccum receive_;
	HostLatency host_latency_;
	uint64_t session_epoch_us_ = 0;
	// The gateway's leg to wraith (GatewayPath), added to the QUIC RTT; 0 on
	// a direct session.
	std::atomic<uint32_t> upstream_rtt_us_{0};
	std::atomic<bool> via_gateway_{false};
	// The delay figures the last StatsReport sent, for link_stats().
	int32_t last_delay_min_us_ = 0;
	int32_t last_delay_avg_us_ = 0;
	uint32_t last_delay_samples_ = 0;

	// Records that `frame_id` was seen: `completed` sets its bit, and false
	// (a discarded frame, see on_video_frame) leaves it clear as lost.
	void note_frame_seen(uint32_t frame_id, bool completed);
	// A frame just completed: keeps train_'s timing of it as a
	// StreamStats.trains sample if it was large and clean enough.
	void note_train(uint32_t slice_count);
	// A frame the reassembler completed, from its slices or rebuilt from
	// parity, to the decoder. `host_us`: its host latency, 0 unreported.
	void handle_video_frame(const gdp::VideoFrameReassembler::Frame &frame, uint32_t host_us);
};

} // namespace spectre
