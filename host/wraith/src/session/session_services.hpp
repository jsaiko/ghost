// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The session-facing services hung off the host (ScreencastHost): the
// video encoder, the GDP session connection, session
// audio and the clipboard sink. All are host-scoped rather than
// per-connection: the encoder and audio pipeline stay open once a session
// is possible, independent of whether one is actually attached, but only
// encode while a viewer is (set_viewer_attached()).
#pragma once

#include "audio/audio_pipeline.hpp"
#include "audio/microphone_source.hpp"
#include "encode/encoder.hpp"
#include "encode/refine/tile_source.hpp"
#include "session/clipboard_sync.hpp"
#include "session/gdp_session.hpp"
#include "session/session_host.hpp"
#include "util/event_source.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace wraith {

class SessionServices {
public:
	explicit SessionServices(SessionHost &host);
	~SessionServices();

	SessionServices(const SessionServices &) = delete;
	SessionServices &operator=(const SessionServices &) = delete;

	// Encoder tuning; read when the encoder is first opened, so set these
	// before start_encoder()/start_gdp_session().
	void set_encode_bitrate_bps(uint32_t bitrate_bps) { encode_bitrate_bps_ = bitrate_bps; }
	void set_encode_gop_size(uint32_t gop_size) { encode_gop_size_ = gop_size; }
	// -F: skip every hardware backend (VA-API, NVENC) in ensure_encoder()
	// and go straight to the software (x264) fallback, to exercise that
	// path on a host that does have a working hardware encoder.
	void set_force_software_encoder(bool force) { force_software_encoder_ = force; }

	// The ceiling the bitrate controller (GdpSession) adapts under:
	// wraith.toml's encode.max_bitrate_mbps, or -b -- except on a pyrowave
	// session, whose ceiling is encode.pyrowave_bpp of the output at 60 fps
	// (an intra-only codec needs several times the others' rate).
	uint32_t encode_bitrate_bps() const;

	// Video codecs this host can offer a session (gdp-spec.md §6.6's codec
	// negotiation; GdpSession picks from these against SessionHello.codecs)
	// -- whatever encode/encoder_factory.cpp has a backend registered for.
	static const std::vector<std::string> &supported_video_codecs();

	// Point the encoder at a codec negotiation offered, reopening it if
	// that isn't the one currently open (GdpSession::accept_session(),
	// which walks the client's preferences until one opens). Returns false
	// if no backend for the codec opens.
	//
	// Until this is called the encoder runs on the default codec (plain
	// h264), which is what -e's file dump and a session that never gets
	// past hello both want.
	//
	// `refine` is whether the session negotiated the "refine" capability
	// (gdp-spec.md §7.8): the encoder is then
	// wrapped in encode/refine/refine_encoder.hpp's decorator. A session
	// without it gets the bare backend and none of refinement's costs.
	bool set_session_codec(const std::string &codec_token, bool refine);

	// -e: encode every composited frame to Annex-B H.264 appended to
	// `output_path`. The dump is whatever the open encoder produces, so a
	// session that then negotiates another codec, or lossless refinement,
	// turns the rest of the file into that instead of plain H.264 -- the
	// debug dump is a dump, not a muxer, and the two flags are not meant
	// to be combined with a non-default session.
	bool start_encoder(const char *output_path);

	// -l / -G: listen for a GDP session (gdp-spec.md §6). Implies the
	// encoder, and session audio if the host can provide it.
	// `control_socket` is the -G control socket, empty under -l; `gamepads`
	// says whether ghostd can create virtual input devices on this host,
	// which decides whether the session offers the "gamepad" capability
	// (session/gamepad_devices.hpp); `raw_controllers` the same for uhid
	// and "hid" (session/raw_controllers.hpp). On failure,
	// `*address_in_use` (if given) says whether it was only because `port`
	// is taken -- the one failure a port scan moves past.
	bool start_gdp_session(uint16_t port, TokenValidator token_validator, const std::string &cert_file,
		const std::string &key_file, const std::string &control_socket = "", bool gamepads = false,
		bool raw_controllers = false, bool *address_in_use = nullptr);

	// Encodes one captured frame (docs/design/capture-backends.md) and
	// fans the resulting packets out to the dump file and/or the live
	// session. No-op if no encoder is open.
	void encode_dmabuf(const DmabufFrame &frame, int64_t pts_us);
	// `xrgb` must stay valid until the next encode_cpu() call or
	// forget_frame_pixels(), whichever comes first: lossless refinement's
	// idle pump re-reads the last pushed frame in place rather than copying
	// it (33 MB a frame at 4K). A caller that holds the producer's buffer
	// until the next frame arrives meets that (ScreencastHost); one whose
	// buffer goes away before then calls forget_frame_pixels() first.
	void encode_cpu(const uint8_t *xrgb, uint32_t width, uint32_t height, uint32_t stride, int64_t pts_us,
		const DamageRegion *damage);
	// encode_cpu() in two halves, for a caller whose pixels cost something
	// to produce (a dmabuf read-back): admit first, and only produce and
	// push them if admitted -- a frame the gate turns away isn't worth
	// reading. push_cpu() carries encode_cpu()'s contract on `xrgb`.
	bool admit_cpu_frame(const DamageRegion *damage);
	void push_cpu(const uint8_t *xrgb, uint32_t width, uint32_t height, uint32_t stride, int64_t pts_us,
		const DamageRegion *damage);
	// The other way in, for a takes_tiled_dmabuf() encoder (refinement over
	// a hardware base) on a host that can read its dmabufs on the GPU
	// (screencast/dmabuf_tile_source.hpp): after admit_cpu_frame(), the
	// dmabuf and a TileSource bound to it. `tiles` carries encode_cpu()'s
	// contract on `xrgb` -- the idle pump reads the frame through it until
	// the next push or forget_frame_pixels(). If it fails (tiles.failed()),
	// nothing was pushed and the caller re-sends the frame through
	// push_cpu().
	void push_tiled(const DmabufFrame &frame, TileSource &tiles, int64_t pts_us, const DamageRegion *damage);
	// The frame last given to encode_cpu() or push_tiled() is about to go
	// away (the producer withdrew the buffer, or the capture is closing):
	// stop pumping it. The next push brings a fresh one.
	void forget_frame_pixels();

	// Closes the open encoder, if any. gdp_session.cpp's accept_session()
	// calls this after a successful SessionHost::request_resize(): the
	// encoder's size is fixed at open() and it has no resize of its own,
	// so the set_session_codec() that follows opens a fresh one at the
	// host's new output_width()/output_height().
	void close_encoder();

	bool has_encoder() const { return encoder_ != nullptr; }
	// Whether the open encoder wants CPU pixels for the next frame (asked
	// per frame: refinement pausing turns it off) -- for a capture that
	// gets dmabufs and can read them back (screencast/dmabuf_reader.hpp).
	bool wants_cpu_frame() const { return encoder_ && encoder_->wants_cpu_frame(); }
	// Whether it would rather have the dmabuf and a GPU TileSource
	// (push_tiled()); asked first, per frame, by a host that has one.
	bool wants_tiled_dmabuf() const { return encoder_ && encoder_->takes_tiled_dmabuf(); }
	// Encoder::supported_import_modifiers, for a capture source that gets
	// to propose a dmabuf modifier (PipeWireCapture). Empty if no encoder
	// is open yet -- callers fall back to the MemFd/CPU path either way.
	std::vector<uint64_t> supported_import_modifiers(uint32_t drm_format) const {
		return encoder_ ? encoder_->supported_import_modifiers(drm_format) : std::vector<uint64_t>{};
	}
	// Which backend the factory picked: "vaapi", "nvenc", "pyrowave",
	// "software", "vaapi+refine", ... (SessionAccept.encoder, gdp-spec.md §6.5);
	// empty while no encoder is open.
	const std::string &encoder_name() const { return encoder_name_; }
	void request_keyframe();
	// Encoder::frames_lost() / request_repair_keyframe(). frames_lost()
	// also wakes the idle pump, so a repair on a desktop that has gone
	// still goes out as a layer-only frame rather than waiting for the
	// next change.
	void frames_lost(const std::vector<int64_t> &pts_us);
	void request_repair_keyframe();
	// The rate controller's target and the session's network profile.
	// Both are remembered and handed to any encoder opened later (a
	// codec fallback, a resize), not just the one open now.
	void set_bitrate(uint32_t bitrate_bps);
	void set_link_profile(LinkProfile profile);
	// The client's RefinePause (Encoder::set_refine_paused()). Also asks
	// the host for a fresh frame, so the reset that wipes the client's
	// lossless plane -- or, resuming, the first frame the tracker sees --
	// goes out now rather than whenever the screen next changes.
	void set_refine_paused(bool paused);

	// GdpSession's viewer authenticating (true) or going away (false).
	// While none is attached nothing is encoded (admit_frame()), the idle
	// pump stops, session audio isn't encoded either and the host pauses
	// capture (unless -e is dumping the stream) -- a detached session
	// lives until logout and has no one to send any of it to. The encoder
	// itself stays open: a capture that opens meanwhile still
	// gets its import modifiers from it (supported_import_modifiers()).
	void set_viewer_attached(bool attached);

	// Tears the GDP session down explicitly, before this object itself is.
	// ~GdpSession()'s stop_clipboard_sync() calls back through
	// host_.session() (SessionHost::session(), which returns a reference
	// through the very unique_ptr the owner, ScreencastHost, is about to
	// reset()); unique_ptr::reset() nulls that pointer *before*
	// running ~SessionServices(), so gdp_session_'s own destructor member
	// running from inside ~SessionServices() would hit that call with
	// host_.session() already null. Owners must call this before resetting
	// their session_ pointer. Idempotent (the destructor also calls it).
	void close_gdp_session();

	// Null unless wraith was started with -l/-G.
	GdpSession *gdp_session() const { return gdp_session_.get(); }
	// Null unless a GDP session was started and PipeWire capture opened.
	AudioPipeline *audio_pipeline() const { return audio_pipeline_.get(); }
	// Null unless the virtual PipeWire microphone opened: a session then
	// offers the "microphone" capability (gdp-spec.md §10.1). Like the
	// audio pipeline it lives as long as the host, not the connection.
	MicrophoneSource *microphone() const { return microphone_.get(); }

	// Clipboard sync (gdp-spec.md §7.9), compositor-
	// scoped like the encoder and audio above rather than per-connection:
	// the local clipboard changes whether or not a client is attached, and
	// a newly authenticated session pushes whatever it currently holds.
	// Null when the host has no mechanism for it -- a screencast backend
	// whose compositor exports none -- in which case no session ever
	// negotiates the "clipboard" capability (session/gdp_session.cpp).
	//
	// May be set (or replaced) while a session is already active: on the
	// screencast backends the sink only exists once the captured
	// compositor is up, which is seconds after the first client can
	// attach. Doing so re-runs that session's clipboard wiring, including
	// the one-shot push of the current text.
	void set_clipboard(std::unique_ptr<ClipboardSink> clipboard);
	ClipboardSink *clipboard() const { return clipboard_.get(); }

	// The screencast hosts call this before their compositor is up, to say
	// a sink is coming (every screencast backend has a mechanism; whether
	// this particular compositor exports it is only known once it opens).
	// Without it, a client attaching during that window -- which is the
	// normal case, since ghostd redirects it the moment wraith reports
	// ready -- would negotiate a session with no "clipboard" capability
	// and never get it back: capabilities are fixed at SessionAccept.
	// Cleared again if the backend turns out to have no mechanism, so
	// later sessions don't offer what this host can't do.
	void set_clipboard_expected(bool expected) { clipboard_expected_ = expected; }
	// What GdpSession offers on: a sink now, or the promise of one.
	bool clipboard_available() const { return clipboard_ != nullptr || clipboard_expected_; }

private:
	// Idempotent: both -e and -l want an open encoder, and either may come
	// first (or both -- writing to a file and streaming to a live session
	// simultaneously is a useful debug configuration, not a conflict).
	bool ensure_encoder();
	bool ensure_audio_pipeline();
	bool ensure_microphone();
	void drain_audio_packets();
	// Fans out packets already sitting in encoder_->poll() to the dump
	// file and/or the live session -- the part encode_dmabuf() and
	// encode_cpu() share once the frame itself has been pushed.
	void drain_encoded_packets();

	// The send-queue gate in front of every encode (GdpSession::
	// should_send_frame()). A frame it turns away isn't encoded: its
	// damage (`damage`, null meaning unknown/everything) is kept for the
	// next frame that is, and a catch-up timer is armed to have the host
	// re-deliver the current picture (SessionHost::redeliver_frame()) as
	// soon as the backlog clears, since on a desktop that has gone still
	// nothing else would ever send what the skipped frame carried.
	bool admit_frame(const DamageRegion *damage);
	void arm_catch_up_timer();
	void handle_catch_up_timer();
	// The frame about to be pushed carries every skipped frame's damage.
	void clear_skipped_damage();
	// `damage` with every skipped frame's merged in (into `merged`, or null
	// for unknown). Doesn't clear the debt; clear_skipped_damage() does.
	const DamageRegion *with_skipped_damage(const DamageRegion *damage, DamageRegion *merged) const;
	// encode_dmabuf() past the gate.
	void push_dmabuf(const DmabufFrame &frame, int64_t pts_us);

	// The idle pump (docs/design/refinement.md): for a
	// wants_idle_pump() encoder, keeps re-pushing the last frame on a
	// timer while has_pending_work() says there's still something to
	// settle, and goes quiet the moment it says there isn't. Needed
	// because SessionServices only ever sees a frame when the host has one
	// to give it -- real compositor damage, or a screencast source's own
	// delivery -- and a desktop that stops changing produces neither, so
	// nothing would otherwise call the encoder again to let it finish
	// settling. Every wants_idle_pump()==false encoder (everything but
	// lossless refinement) never touches any of this.
	//
	// Called after every real push with the frame that was just pushed --
	// remembers it and arms/disarms the timer based on has_pending_work()
	// right after.
	void service_idle_pump(TileSource *tiles);
	void arm_idle_pump_timer();
	void handle_idle_pump_timer();
	// Drops the cached frame and disarms the timer -- called whenever the
	// encoder is closed or replaced, so a stale frame at the wrong
	// dimensions never gets pushed into a freshly (re)opened one.
	void reset_idle_pump();

	SessionHost &host_;

	std::unique_ptr<Encoder> encoder_;
	// The codec the open encoder serves; set by set_session_codec() once a
	// session negotiates one.
	gdp::VideoCodec session_codec_ = gdp::VideoCodec::H264;
	bool session_refine_ = false;
	FILE *encode_out_ = nullptr;
	uint32_t encode_bitrate_bps_ = 80'000'000;
	// Last set_bitrate() (0 before the first) and set_link_profile().
	uint32_t target_bitrate_bps_ = 0;
	LinkProfile link_profile_ = LinkProfile::kLan;
	bool refine_paused_ = false;
	uint32_t encode_gop_size_ = 600; // EncoderConfig::gop_size says why
	bool force_software_encoder_ = false;
	std::string encoder_name_;

	// See service_idle_pump()'s comment: the last pushed frame, read
	// through cpu_pump_source_ (pixels the caller owns, a
	// screencast's held buffer -- see encode_cpu()'s contract) or through
	// the caller's own GPU source (push_tiled()). Only ever set while the
	// open encoder's wants_idle_pump() is true.
	CpuTileSource cpu_pump_source_;
	TileSource *idle_pump_source_ = nullptr;
	EventSource idle_pump_timer_;

	// What frames admit_frame() turned away changed, owed to the next one
	// encoded: `skipped_damage_full_` when any of them had unknown damage,
	// else the union (as a rect list) of theirs.
	bool have_skipped_damage_ = false;
	bool skipped_damage_full_ = false;
	DamageRegion skipped_damage_;
	// Armed while a skipped frame is owed a re-delivery.
	EventSource catch_up_timer_;

	// Declared before gdp_session_ so it outlives it: an active GdpSession
	// holds a callback into the sink, and ~SessionServices tears the
	// session down explicitly before any member destructor runs anyway.
	std::unique_ptr<ClipboardSink> clipboard_;
	bool clipboard_expected_ = false;

	std::unique_ptr<GdpSession> gdp_session_;
	EventSource gdp_session_source_;
	// An asynchronous encoder's completion_fd(): packets are drained as
	// they finish, not only right after the next push.
	EventSource encoder_done_source_;
	// SIGUSR1, from ghostd: this user logged in at the console, so the
	// viewer is closed with ENDED_BY_LOCAL_LOGIN and the desktop logs out
	// (docs/design/login-and-sessions.md). main() blocks the signal.
	EventSource local_login_source_;

	// Bridges AudioPipeline's PipeWire-thread-produced packets back onto
	// wraith's main thread, the only thread send_audio_packet() is called
	// from (drain_audio_packets()).
	std::unique_ptr<AudioPipeline> audio_pipeline_;
	EventSource audio_pipeline_source_;
	std::unique_ptr<MicrophoneSource> microphone_;
};

} // namespace wraith
