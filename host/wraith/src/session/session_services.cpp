// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/session_services.hpp"

#include "encode/encoder_factory.hpp"
#include "util/clock.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include "gdp/negotiation.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <signal.h>
#include <unistd.h>

namespace wraith {

SessionServices::SessionServices(SessionHost &host) : host_(host) {}

SessionServices::~SessionServices() {
	// Event sources come off the loop before the objects they dispatch
	// into go away.
	close_gdp_session();

	audio_pipeline_source_.reset();
	audio_pipeline_.reset();
	microphone_.reset();

	close_encoder();
	if (encode_out_) {
		fclose(encode_out_);
		encode_out_ = nullptr;
	}
}

void SessionServices::close_gdp_session() {
	local_login_source_.reset();
	gdp_session_source_.reset();
	gdp_session_.reset();
}

void SessionServices::set_clipboard(std::unique_ptr<ClipboardSink> clipboard) {
	// An active session's callback points at whatever is being replaced,
	// so drop the session's hold on it first.
	if (clipboard_) {
		clipboard_->on_local_change = nullptr;
	}
	clipboard_ = std::move(clipboard);
	// On the screencast backends this lands *after* a client has already
	// attached and negotiated, so the session wires itself to the new sink
	// here rather than only at SessionAccept.
	if (gdp_session_) {
		gdp_session_->clipboard_sink_changed();
	}
}

const std::vector<std::string> &SessionServices::supported_video_codecs() {
	return wraith::supported_video_codecs();
}

uint32_t SessionServices::encode_bitrate_bps() const {
	if (session_codec_ != gdp::VideoCodec::Pyrowave) {
		return encode_bitrate_bps_;
	}
	double bps = config().encode.pyrowave_bpp * host_.output_width() * host_.output_height() * 60.0;
	return (uint32_t)std::min(bps, (double)UINT32_MAX);
}

bool SessionServices::set_session_codec(const std::string &codec_token, bool refine) {
	gdp::VideoCodec codec = gdp::video_codec_from_token(codec_token);
	if (codec == gdp::VideoCodec::Unknown) {
		WLOG_ERROR("encoder: negotiated codec \"%s\" is not one this build knows", codec_token.c_str());
		return false;
	}
	if (encoder_ && codec == session_codec_ && refine == session_refine_) {
		return true;
	}

	// The encoder is opened before any client connects (start_encoder() /
	// start_gdp_session()) and EncoderConfig is fixed at open() time, so
	// a different codec or refine setting means closing it and opening a
	// fresh one.
	session_codec_ = codec;
	session_refine_ = refine;
	close_encoder();
	return ensure_encoder();
}

bool SessionServices::ensure_encoder() {
	if (encoder_) {
		return true;
	}

	EncoderConfig config;
	config.codec = session_codec_;
	config.width = host_.output_width();
	config.height = host_.output_height();
	config.bitrate_bps = encode_bitrate_bps();
	config.framerate_num = 60;
	config.framerate_den = 1;
	config.gop_size = encode_gop_size_;
	config.drm_fd = host_.render_drm_fd();

	// Backend selection -- the hardware backends (VA-API, NVENC) in table
	// order, the software fallback, and what -F does to them -- lives in
	// the factory's table (encode/encoder_factory.cpp), which logs the path
	// it took.
	EncoderSelection selection = create_encoder(config, force_software_encoder_, session_refine_);
	if (!selection) {
		return false;
	}
	encoder_ = std::move(selection.encoder);
	encoder_name_ = std::move(selection.backend_name);
	// Opened at the -b ceiling; the rate controller may have moved on.
	if (target_bitrate_bps_ != 0) {
		encoder_->set_bitrate(target_bitrate_bps_);
	}
	encoder_->set_link_profile(link_profile_);
	encoder_->set_refine_paused(refine_paused_);
	if (encoder_->completion_fd() >= 0) {
		encoder_done_source_.reset(wl_event_loop_add_fd(
			host_.event_loop(), encoder_->completion_fd(), WL_EVENT_READABLE,
			[](int, uint32_t, void *data) {
				auto *self = static_cast<SessionServices *>(data);
				if (self->encoder_) {
					self->drain_encoded_packets();
					// Frames skipped while it was busy: the newest goes now.
					if (self->have_skipped_damage_ && self->encoder_->ready_for_frame()) {
						self->handle_catch_up_timer();
					}
				}
				return 0;
			},
			this));
	}
	return true;
}

void SessionServices::close_encoder() {
	reset_idle_pump();
	encoder_done_source_.reset();
	if (encoder_) {
		encoder_->close();
		encoder_.reset();
	}
	encoder_name_.clear();
}

// Idempotent, same rationale as ensure_encoder above.
bool SessionServices::ensure_audio_pipeline() {
	if (audio_pipeline_) {
		return true;
	}

	auto pipeline = std::make_unique<AudioPipeline>();
	AudioPipelineConfig config;

	if (!pipeline->open(config)) {
		WLOG_ERROR("audio: failed to open PipeWire capture + Opus encoder");
		return false;
	}

	audio_pipeline_ = std::move(pipeline);
	WLOG_INFO("audio: capturing at %u Hz, %u ch, %ums frames (sink \"%s\")", config.sample_rate_hz,
		config.channels, config.frame_ms, config.node_name.c_str());
	return true;
}

bool SessionServices::ensure_microphone() {
	if (microphone_) {
		return true;
	}
	auto mic = std::make_unique<MicrophoneSource>();
	std::string node_name = "gdp-mic";
	if (!mic->open(gdp::microphone_format(), node_name)) {
		WLOG_ERROR("audio: failed to open the virtual microphone");
		return false;
	}
	microphone_ = std::move(mic);
	WLOG_INFO("audio: virtual microphone \"%s\" ready", node_name.c_str());
	return true;
}

bool SessionServices::start_encoder(const char *output_path) {
	if (!ensure_encoder()) {
		return false;
	}

	encode_out_ = fopen(output_path, "wb");
	if (!encode_out_) {
		WLOG_ERROR("encoder: failed to open %s for writing", output_path);
		return false;
	}

	WLOG_INFO("encoder: also writing to %s", output_path);
	return true;
}

bool SessionServices::start_gdp_session(uint16_t port, TokenValidator token_validator,
	const std::string &cert_file, const std::string &key_file, const std::string &control_socket,
	bool gamepads, bool raw_controllers, bool *address_in_use) {
	if (address_in_use) {
		*address_in_use = false;
	}
	if (!ensure_encoder()) {
		return false;
	}
	if (!ensure_audio_pipeline()) {
		// No PipeWire on this host, say: the session runs video-only, and
		// SessionAccept carries no AudioConfig.
		WLOG_ERROR("audio: continuing without session audio");
	}
	if (!ensure_microphone()) {
		WLOG_ERROR("audio: continuing without a microphone");
	}

	auto session = std::make_unique<GdpSession>(host_, std::move(token_validator), control_socket, gamepads,
		raw_controllers);
	bool in_use = false;
	if (!session->listen(port, cert_file, key_file, &in_use)) {
		if (in_use) {
			WLOG_INFO("gdp: port %u is already in use", port);
		} else {
			WLOG_ERROR("gdp: failed to listen on port %u", port);
		}
		if (address_in_use) {
			*address_in_use = in_use;
		}
		return false;
	}

	gdp_session_ = std::move(session);
	gdp_session_source_.reset(wl_event_loop_add_fd(
		host_.event_loop(), gdp_session_->notify_fd(), WL_EVENT_READABLE,
		[](int, uint32_t, void *data) {
			static_cast<GdpSession *>(data)->dispatch();
			return 0;
		},
		gdp_session_.get()));

	local_login_source_.reset(wl_event_loop_add_signal(
		host_.event_loop(), SIGUSR1,
		[](int, void *data) {
			auto *self = static_cast<SessionServices *>(data);
			WLOG_INFO("session: ghostd ended this session for a console login, logging out");
			if (self->gdp_session_) {
				self->gdp_session_->end_for_local_login();
			}
			self->host_.request_logout();
			return 0;
		},
		this));

	if (audio_pipeline_) {
		audio_pipeline_source_.reset(wl_event_loop_add_fd(
			host_.event_loop(), audio_pipeline_->notify_fd(), WL_EVENT_READABLE,
			[](int, uint32_t, void *data) {
				static_cast<SessionServices *>(data)->drain_audio_packets();
				return 0;
			},
			this));
	}

	WLOG_INFO("gdp: session listening on port %u", port);
	return true;
}

void SessionServices::drain_audio_packets() {
	for (auto &packet : audio_pipeline_->poll()) {
		if (gdp_session_ && gdp_session_->active()) {
			gdp_session_->send_audio_packet(packet.data.data(), packet.data.size());
		}
	}
}

void SessionServices::request_keyframe() {
	if (encoder_) {
		encoder_->request_keyframe();
	}
}

void SessionServices::frames_lost(const std::vector<int64_t> &pts_us) {
	if (!encoder_) {
		return;
	}
	encoder_->frames_lost(pts_us);
	if (encoder_->wants_idle_pump() && encoder_->has_pending_work() && idle_pump_source_) {
		arm_idle_pump_timer();
	}
}

void SessionServices::request_repair_keyframe() {
	if (encoder_) {
		encoder_->request_repair_keyframe();
	}
}

void SessionServices::set_bitrate(uint32_t bitrate_bps) {
	target_bitrate_bps_ = bitrate_bps;
	if (encoder_) {
		encoder_->set_bitrate(bitrate_bps);
	}
}

void SessionServices::set_refine_paused(bool paused) {
	if (paused == refine_paused_) {
		return;
	}
	refine_paused_ = paused;
	if (encoder_) {
		encoder_->set_refine_paused(paused);
		host_.redeliver_frame();
	}
}

void SessionServices::set_link_profile(LinkProfile profile) {
	link_profile_ = profile;
	if (encoder_) {
		encoder_->set_link_profile(profile);
	}
}

void SessionServices::drain_encoded_packets() {
	for (const auto &packet : encoder_->poll()) {
		if (encode_out_) {
			fwrite(packet.data.data(), 1, packet.data.size(), encode_out_);
		}
		if (gdp_session_ && gdp_session_->active()) {
			gdp_session_->send_video_packet(packet.keyframe, packet.data.data(), packet.data.size(),
				packet.pts_us);
		}
	}
	if (encode_out_) {
		fflush(encode_out_);
	}
}

void SessionServices::set_viewer_attached(bool attached) {
	if (audio_pipeline_) {
		audio_pipeline_->set_encoding(attached);
	}
	if (!encode_out_) {
		host_.set_capture_paused(!attached);
	}
	if (!attached) {
		reset_idle_pump();
		catch_up_timer_.reset();
	}
}

bool SessionServices::admit_frame(const DamageRegion *damage) {
	// Nobody to send it to (no viewer attached, no -e dump): not encoded
	// at all. Whatever changed meanwhile is owed in full to the first
	// frame a viewer gets -- the attach's keyframe re-delivery.
	if (!encode_out_ && !(gdp_session_ && gdp_session_->active())) {
		skipped_damage_full_ = true;
		skipped_damage_.rects.clear();
		have_skipped_damage_ = true;
		return false;
	}
	// An encoder still busy with the last frame (asynchronous VA-API slower
	// than the screen) takes no new one: the frame is skipped like one the
	// network can't take, and the newest re-delivered when the encode
	// finishes (the completion handler in ensure_encoder()), never queued.
	bool encoder_busy = encoder_ && !encoder_->ready_for_frame();
	if (!encoder_busy && (!gdp_session_ || !gdp_session_->active() || gdp_session_->should_send_frame())) {
		return true;
	}
	if (!damage) {
		skipped_damage_full_ = true;
		skipped_damage_.rects.clear();
	} else if (!skipped_damage_full_) {
		skipped_damage_.rects.insert(skipped_damage_.rects.end(), damage->rects.begin(), damage->rects.end());
	}
	have_skipped_damage_ = true;
	if (!encoder_busy) {
		arm_catch_up_timer();
	}
	return false;
}

void SessionServices::arm_catch_up_timer() {
	// Polled rather than signalled: the backlog drains on the transport's own
	// schedule, and a few ms of lag past that is noise next to the
	// backlog limit (tens of ms) that caused the skip.
	constexpr int kCatchUpPollMs = 4;
	if (!catch_up_timer_) {
		catch_up_timer_.reset(wl_event_loop_add_timer(
			host_.event_loop(),
			[](void *data) {
				static_cast<SessionServices *>(data)->handle_catch_up_timer();
				return 0; // one-shot; rearmed below if still backed up
			},
			this));
	}
	wl_event_source_timer_update(catch_up_timer_.get(), kCatchUpPollMs);
}

void SessionServices::handle_catch_up_timer() {
	if (!have_skipped_damage_) {
		return; // an organic frame already went out and carried it
	}
	if (gdp_session_ && gdp_session_->active() && !gdp_session_->send_queue_clear()) {
		arm_catch_up_timer();
		return;
	}
	// The host re-encodes what's on screen now, back through admit_frame(),
	// which merges the skipped damage in.
	host_.redeliver_frame();
}

void SessionServices::encode_dmabuf(const DmabufFrame &frame, int64_t pts_us) {
	if (!encoder_ || !admit_frame(nullptr)) {
		return;
	}
	push_dmabuf(frame, pts_us);
}

void SessionServices::clear_skipped_damage() {
	have_skipped_damage_ = false;
	skipped_damage_full_ = false;
	skipped_damage_.rects.clear();
}

void SessionServices::push_dmabuf(const DmabufFrame &frame, int64_t pts_us) {
	// A dmabuf encoder takes whole frames and has no use for damage.
	clear_skipped_damage();
	if (!encoder_->push(frame, pts_us)) {
		return;
	}
	drain_encoded_packets();
}

void SessionServices::encode_cpu(const uint8_t *xrgb, uint32_t width, uint32_t height, uint32_t stride,
	int64_t pts_us, const DamageRegion *damage) {
	if (admit_cpu_frame(damage)) {
		push_cpu(xrgb, width, height, stride, pts_us, damage);
	}
}

bool SessionServices::admit_cpu_frame(const DamageRegion *damage) {
	if (!encoder_) {
		return false;
	}
	if (!admit_frame(damage)) {
		// The caller may let go of the previous frame's pixels now (a
		// screencast releases the buffer it was holding), and the pump
		// must not read a frame older than what is on screen anyway.
		forget_frame_pixels();
		return false;
	}
	return true;
}

// Frames the gate turned away changed things this one's own damage doesn't
// mention; they are owed to the encoder with it. The debt itself stays
// until the caller clears it.
const DamageRegion *SessionServices::with_skipped_damage(const DamageRegion *damage,
	DamageRegion *merged) const {
	if (!have_skipped_damage_) {
		return damage;
	}
	if (skipped_damage_full_ || !damage) {
		return nullptr;
	}
	merged->rects = skipped_damage_.rects;
	merged->rects.insert(merged->rects.end(), damage->rects.begin(), damage->rects.end());
	return merged;
}

void SessionServices::push_cpu(const uint8_t *xrgb, uint32_t width, uint32_t height, uint32_t stride,
	int64_t pts_us, const DamageRegion *damage) {
	DamageRegion merged;
	damage = with_skipped_damage(damage, &merged);
	clear_skipped_damage();
	if (!encoder_->push_cpu(xrgb, width, height, stride, pts_us, damage)) {
		return;
	}
	drain_encoded_packets();
	cpu_pump_source_ = CpuTileSource(xrgb, width, height, stride);
	service_idle_pump(&cpu_pump_source_);
}

void SessionServices::push_tiled(const DmabufFrame &frame, TileSource &tiles, int64_t pts_us,
	const DamageRegion *damage) {
	DamageRegion merged;
	bool pushed = encoder_->push_tiled(frame, tiles, pts_us, with_skipped_damage(damage, &merged));
	// A source failure hands the frame back for push_cpu(), which still owes
	// the encoder everything skipped.
	if (!pushed && tiles.failed()) {
		return;
	}
	clear_skipped_damage();
	if (!pushed) {
		return;
	}
	drain_encoded_packets();
	service_idle_pump(&tiles);
}

void SessionServices::service_idle_pump(TileSource *tiles) {
	if (!encoder_ || !encoder_->wants_idle_pump()) {
		return;
	}

	if (refine_paused_) {
		// Paused: the pump has nothing to do until a resume, which
		// starts the tile tracker over from the next push anyway; nothing
		// may pump older pixels after it.
		forget_frame_pixels();
		return;
	}

	// Remember what was just pushed so the pump ticks that follow can hand
	// the encoder the same frame again (it copies settled tiles out of it;
	// see Encoder::pump()) -- in place, by encode_cpu()'s contract, not
	// copied. Kept even once nothing is pending: a reported loss re-arms
	// tiles on a settled desktop (frames_lost()).
	idle_pump_source_ = tiles;

	if (encoder_->has_pending_work()) {
		arm_idle_pump_timer();
	} else {
		// Converged -- nothing left for a pump tick to accomplish. Let the
		// timer lapse rather than rearming it; the next organic push (real
		// damage, or a screencast source's own delivery) will call this
		// again and re-arm if something actually changed.
		idle_pump_timer_.reset();
	}
}

void SessionServices::arm_idle_pump_timer() {
	// One 60 Hz frame per tick. tile_tracker.hpp's settle_us is measured
	// in time, so the tick rate only bounds how late a due tile goes out:
	// with an 80 ms settle, text lands exact 80-96 ms after the last
	// keystroke. A tick with nothing due hashes nothing and encodes
	// nothing, and the timer only runs while there is still something to
	// settle (service_idle_pump() above disarms it the moment there
	// isn't), so this is not a steady-state cost.
	constexpr int kIdlePumpIntervalMs = 16;
	if (!idle_pump_timer_) {
		idle_pump_timer_.reset(wl_event_loop_add_timer(
			host_.event_loop(),
			[](void *data) {
				static_cast<SessionServices *>(data)->handle_idle_pump_timer();
				return 0; // one-shot; handle_idle_pump_timer() rearms if needed
			},
			this));
	}
	wl_event_source_timer_update(idle_pump_timer_.get(), kIdlePumpIntervalMs);
}

void SessionServices::handle_idle_pump_timer() {
	if (!encoder_ || !encoder_->wants_idle_pump() || !idle_pump_source_) {
		return;
	}
	// Settling tiles is exactly the kind of extra traffic a backed-up send
	// queue can't take; try again next tick. And not while a skipped
	// frame's damage is still owed, either: the encoder hasn't seen those
	// changes, so to its wall-clock settle timer the screen has simply gone
	// still, and a pump now would ship everything the skipped frames
	// touched as "settled" lossless tiles -- on busy content a burst that
	// backs the queue up again and skips more frames. The catch-up
	// re-delivery pushes that damage first (handle_catch_up_timer()).
	if (have_skipped_damage_ ||
		(gdp_session_ && gdp_session_->active() && !gdp_session_->send_queue_clear())) {
		arm_idle_pump_timer();
		return;
	}

	int64_t pts_us = monotonic_now_us();

	// A pump, not a push: nothing is encoded. The encoder advances its
	// settle state over the last frame and, if any tiles are due, queues a
	// layer-only packet for the drain below. A GPU source that can't read
	// the frame any more stops the pump; its host sees failed() and falls
	// back to read-back on the next frame.
	if (!encoder_->pump(*idle_pump_source_, pts_us)) {
		if (idle_pump_source_->failed()) {
			forget_frame_pixels();
		}
		return;
	}
	drain_encoded_packets();

	if (encoder_->has_pending_work()) {
		arm_idle_pump_timer();
	}
	// else: converged: don't rearm, go quiet until the next organic push.
}

void SessionServices::forget_frame_pixels() {
	idle_pump_timer_.reset();
	idle_pump_source_ = nullptr;
}

void SessionServices::reset_idle_pump() {
	idle_pump_timer_.reset();
	idle_pump_source_ = nullptr;
	cpu_pump_source_ = CpuTileSource();
}

} // namespace wraith
