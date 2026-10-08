// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/screencast_host.hpp"

#include "util/clock.hpp"
#include "util/log.hpp"
#include "util/render_node.hpp"

#include <libdrm/drm_fourcc.h>

#include <algorithm>
#include <cstdlib>

#include <unistd.h>

namespace wraith {

namespace {

// A compositor's startup takes a few seconds; retried every 250ms for up
// to 30s before giving up.
constexpr int kOpenRetryMs = 250;
constexpr int kOpenMaxRetries = 120;

} // namespace

ScreencastHost::ScreencastHost() = default;

ScreencastHost::~ScreencastHost() {
	terminate();
	// An open or input-reconnect retry can still be armed -- the input
	// half drops as the desktop logs out, just before the loop stops. As a
	// member it would be removed after wl_event_loop_destroy() below, and
	// wl_event_source_remove() on a timer writes into its loop (the timer
	// heap, the destroy list): heap corruption at exit.
	open_retry_timer_.reset();
	capture_reopen_timer_.reset();
	capture_pause_apply_.cancel();
	// Drop anything still held before the capture/PipeWire connection
	// that owns those buffers goes away.
	if (session_) {
		session_->forget_frame_pixels(); // the idle pump may point into a held buffer
	}
	// Its imports hold capture fds, and its EGL display lives on
	// render_drm_fd_, closed below. The tile source reads through it.
	tile_source_.reset();
	reader_.reset();
	for (void *token : frame_hold_.drain()) {
		if (frame_source_) {
			frame_source_->release(token);
		}
	}
	frame_source_.reset();
	input_.reset();
	// Before the RemoteSession: a clipboard sink holds that session's
	// Wayland connection or sd-bus (screencast/data_control_clipboard.cpp,
	// gnome_clipboard.cpp) and drops the selection through it on the way
	// out. SessionServices would otherwise destroy it after. session_ is
	// null if init() failed before creating it.
	if (session_) {
		session_->set_clipboard(nullptr);
	}
	remote_session_.reset();
	// Before session_.reset(): see SessionServices::close_gdp_session().
	if (session_) {
		session_->close_gdp_session();
		session_.reset();
	}
	session_process_.terminate();
	if (render_drm_fd_ >= 0) {
		::close(render_drm_fd_);
	}
	cursor_position_send_.cancel();
	reopen_without_reader_.cancel();
	if (event_loop_) {
		wl_event_loop_destroy(event_loop_);
	}
}

bool ScreencastHost::init(Options options) {
	width_ = options.width;
	height_ = options.height;
	if (!options.remote_session) {
		WLOG_ERROR("screencast: no RemoteSession given");
		return false;
	}
	remote_session_ = std::move(options.remote_session);
	input_proxy_.host = this;

	event_loop_ = wl_event_loop_create();
	if (!event_loop_) {
		WLOG_ERROR("screencast: wl_event_loop_create failed");
		return false;
	}

	render_drm_fd_ = open_render_node();
	if (render_drm_fd_ < 0) {
		WLOG_ERROR("screencast: no DRM render node found for the encoder");
		// Not fatal by itself -- the encoder factory skips the backends
		// that need a render node (VA-API, NVENC) and tries the rest, down
		// to the software encoder.
	}

	session_ = std::make_unique<SessionServices>(*this);
	// Every screencast backend has a clipboard mechanism, but which one
	// this compositor exports isn't known until the RemoteSession opens --
	// seconds after the first client can attach, and capabilities are
	// fixed at SessionAccept. Promise it now, retract it in
	// try_open_capture_and_input() if the mechanism turns out to be
	// missing (see SessionServices::set_clipboard_expected).
	session_->set_clipboard_expected(true);
	return true;
}

bool ScreencastHost::start(const LaunchSpec *spec) {
	if (!spec) {
		WLOG_ERROR("screencast: a screencast backend needs a session leader to capture");
		return false;
	}
	// No host compositor for the leader to nest in. Cleared here rather than relying on wraith's own
	// inherited environment already being unset: a stale WAYLAND_DISPLAY left in systemd --user's environment
	// block by an earlier session for the same uid would otherwise reach the compositor and make it nest
	// (gnome-shell does, even when started with --headless).
	unsetenv("WAYLAND_DISPLAY");
	unsetenv("DISPLAY");

	if (!start_leader(event_loop_, *spec, width_, height_)) {
		return false;
	}
	arm_open_retry();
	return true;
}

void ScreencastHost::arm_open_retry() {
	open_retries_ = 0;
	open_retry_timer_.reset(wl_event_loop_add_timer(
		event_loop_,
		[](void *data) {
			auto *host = static_cast<ScreencastHost *>(data);
			if (host->try_open_capture_and_input()) {
				host->open_retry_timer_.reset();
				return 0;
			}
			if (++host->open_retries_ >= kOpenMaxRetries) {
				WLOG_ERROR("screencast: %s never became ready after %ds, giving up",
					host->remote_session_->name(), kOpenMaxRetries * kOpenRetryMs / 1000);
				host->open_retry_timer_.reset();
				host->terminate();
				return 0;
			}
			wl_event_source_timer_update(host->open_retry_timer_.get(), kOpenRetryMs);
			return 0;
		},
		this));
	wl_event_source_timer_update(open_retry_timer_.get(), kOpenRetryMs);
}

void ScreencastHost::run() {
	while (!terminate_requested_) {
		if (wl_event_loop_dispatch(event_loop_, -1) < 0) {
			break;
		}
	}
	// Timestamps the start of teardown in the journal: the gap from here
	// to the unit going inactive is how long an attached client waits for
	// its SESSION_ENDED close after a logout.
	WLOG_INFO("screencast: event loop stopped, shutting down (session %s)",
		session_ended_ ? "ended" : "still running");
}

bool ScreencastHost::try_open_capture_and_input() {
	if (!remote_session_ready_) {
		RemoteSession::Config rconfig;
		rconfig.event_loop = event_loop_;
		rconfig.width = width_;
		rconfig.height = height_;
		remote_session_->on_closed = [this] { on_remote_session_closed(); };
		if (!remote_session_->open(rconfig)) {
			return false;
		}
		remote_session_ready_ = true;

		// Asked for once per open, unlike the input sink: there is no
		// reconnect path for a clipboard, it goes with the whole session.
		// A null is normal (the compositor exports no mechanism) -- the
		// session just runs without the "clipboard" capability.
		if (std::unique_ptr<ClipboardSink> clipboard = remote_session_->make_clipboard_sink()) {
			session_->set_clipboard(std::move(clipboard));
		} else {
			// Retract the promise made in init(): later sessions won't
			// offer the capability. One already accepted in the meantime
			// keeps it, inert -- nothing it sends is acted on and it
			// receives nothing.
			session_->set_clipboard_expected(false);
			WLOG_INFO("screencast: %s exposes no clipboard mechanism, clipboard sync is off",
				remote_session_->name());
		}
	}

	// Not while paused: no viewer is attached (set_capture_paused()).
	if (!capture_paused_ && !open_capture()) {
		return false;
	}

	// Attempted fresh every tick until the RemoteSession hands one back
	// (a null is "the compositor's input service isn't ready yet").
	return open_input();
}

bool ScreencastHost::open_capture() {
	if (capture_ready_) {
		return true;
	}
	frame_source_ = remote_session_->make_frame_source();
	if (!frame_source_) {
		return false;
	}
	FrameSource::Params config;
	config.event_loop = event_loop_;
	config.width = width_;
	config.height = height_;
	// Best-effort: if start_gdp_session()/start_encoder() haven't run
	// yet (main.cpp calls those before run()), there's no encoder to
	// ask and this is just empty -- a FrameSource still offers its
	// CPU fallback path regardless.
	config.import_modifiers = capture_modifiers();
	frame_source_->on_dmabuf_frame = [this](const DmabufFrame &frame, int64_t pts_us, void *token,
										 const DamageRegion *damage) {
		void *to_release = frame_hold_.arrived(token);
		has_last_frame_ = true;
		last_frame_is_dmabuf_ = true;
		last_dmabuf_frame_ = frame; // fds stay valid: this token isn't released below
		last_dmabuf_token_ = token;
		deliver_dmabuf(frame, token, pts_us, damage, true);
		if (to_release) {
			frame_source_->release(to_release);
		}
	};
	frame_source_->on_buffer_removed = [this](void *token) { on_capture_buffer_removed(token); };
	frame_source_->on_cpu_frame = [this](const uint8_t *xrgb, uint32_t w, uint32_t h, uint32_t stride,
									  int64_t pts_us, const DamageRegion *damage, void *token) {
		has_last_frame_ = true;
		last_frame_is_dmabuf_ = false;
		last_cpu_width_ = w;
		last_cpu_height_ = h;
		last_cpu_stride_ = stride;
		// A lent buffer is held, as a dmabuf frame is, rather than copied;
		// only pixels valid for this call alone need a copy of our own.
		void *to_release = nullptr;
		if (token) {
			to_release = frame_hold_.arrived(token);
			last_cpu_held_ = xrgb;
			last_cpu_frame_.clear();
		} else {
			last_cpu_held_ = nullptr;
			last_cpu_frame_.assign(xrgb, xrgb + (size_t)stride * h);
		}
		session_->encode_cpu(xrgb, w, h, stride, pts_us, damage);
		if (to_release) {
			frame_source_->release(to_release);
		}
	};
	frame_source_->on_cursor_shape = [this](uint32_t w, uint32_t h, int32_t hx, int32_t hy,
										 const uint8_t *argb) {
		if (cursor_argb_.empty()) {
			WLOG_INFO("screencast: compositor cursor metadata live (first shape %ux%u hotspot %d,%d)", w, h,
				hx, hy);
		}
		cursor_hidden_ = false;
		cursor_width_ = w;
		cursor_height_ = h;
		cursor_hotspot_x_ = hx;
		cursor_hotspot_y_ = hy;
		cursor_argb_.assign(argb, argb + (size_t)w * h * 4);
		if (GdpSession *gdp = active_session()) {
			gdp->send_cursor_shape(w, h, hx, hy, cursor_argb_.data());
		}
	};
	frame_source_->on_cursor_position = [this](double x, double y) {
		// The compositor is authoritative: snap the self-tracked
		// position to it, correcting any drift the injected-delta
		// tracking below accumulated between frames.
		cursor_track_x_ = x;
		cursor_track_y_ = y;
		if (GdpSession *gdp = active_session()) {
			gdp->send_cursor_position(x, y);
		}
	};
	frame_source_->on_cursor_hidden = [this] {
		cursor_hidden_ = true;
		if (GdpSession *gdp = active_session()) {
			gdp->send_cursor_hidden();
		}
	};
	frame_source_->on_closed = [this](const std::string &error) { on_capture_closed(error); };

	if (!frame_source_->open(config)) {
		frame_source_.reset();
		return false;
	}
	capture_ready_ = true;
	return true;
}

bool ScreencastHost::open_input() {
	std::unique_ptr<ScreencastInput> input = remote_session_->make_input_sink();
	if (!input) {
		return false;
	}
	input->on_disconnected = [this] { on_input_disconnected(); };
	input->resend_cursor_shape_cb = [this] { replay_cursor_shape(); };
	input_ = std::move(input); // replaces a closed predecessor on reconnect
	return true;
}

void ScreencastHost::InputProxy::inject_key(uint32_t time_msec, uint32_t keycode,
	enum wl_keyboard_key_state state) {
	if (host->input_) {
		host->input_->inject_key(time_msec, keycode, state);
	}
}

void ScreencastHost::InputProxy::inject_motion(uint32_t time_msec, double dx, double dy) {
	if (host->input_) {
		host->input_->inject_motion(time_msec, dx, dy);
	}
	// Relative motion means spectre is in capture mode and drawing its
	// cursor from our reports; keep that position fresh without waiting
	// for the next frame's metadata.
	host->track_relative_motion(dx, dy);
}

void ScreencastHost::InputProxy::inject_motion_absolute(uint32_t time_msec, double x, double y) {
	if (host->input_) {
		host->input_->inject_motion_absolute(time_msec, x, y);
	}
	host->note_absolute_motion(x, y);
}

void ScreencastHost::InputProxy::inject_button(uint32_t time_msec, uint32_t button,
	enum wl_pointer_button_state state) {
	if (host->input_) {
		host->input_->inject_button(time_msec, button, state);
	}
}

void ScreencastHost::InputProxy::inject_axis(uint32_t time_msec, enum wl_pointer_axis orientation,
	double delta) {
	if (host->input_) {
		host->input_->inject_axis(time_msec, orientation, delta);
	}
}

void ScreencastHost::InputProxy::resend_cursor_shape() {
	host->replay_cursor_shape();
}

GdpSession *ScreencastHost::active_session() const {
	GdpSession *gdp = session_->gdp_session();
	return gdp && gdp->active() ? gdp : nullptr;
}

void ScreencastHost::replay_cursor_shape() {
	GdpSession *gdp = active_session();
	if (!gdp) {
		return;
	}
	if (cursor_hidden_ || cursor_argb_.empty()) {
		gdp->send_cursor_hidden();
	} else {
		gdp->send_cursor_shape(cursor_width_, cursor_height_, cursor_hotspot_x_, cursor_hotspot_y_,
			cursor_argb_.data());
	}
}

void ScreencastHost::track_relative_motion(double dx, double dy) {
	if (width_ == 0 || height_ == 0) {
		return;
	}
	cursor_track_x_ = std::clamp(cursor_track_x_ + dx / (double)width_, 0.0, 1.0);
	cursor_track_y_ = std::clamp(cursor_track_y_ + dy / (double)height_, 0.0, 1.0);
	schedule_cursor_position_send();
}

void ScreencastHost::note_absolute_motion(double x, double y) {
	// Absolute mode: spectre draws its cursor from its own local pointer
	// and ignores our reports (stream_session.cpp), so there's nothing to
	// send -- just keep the tracked position current for a later switch
	// into relative mode.
	cursor_track_x_ = x;
	cursor_track_y_ = y;
}

void ScreencastHost::schedule_cursor_position_send() {
	// Coalesce a burst of deltas within one loop iteration into a single
	// send.
	cursor_position_send_.schedule(event_loop_, [this] { flush_cursor_position(); });
}

void ScreencastHost::flush_cursor_position() {
	if (GdpSession *gdp = active_session()) {
		gdp->send_cursor_position(cursor_track_x_, cursor_track_y_);
	}
}

void ScreencastHost::redeliver_frame() {
	// A compositor's stream is damage-driven and wraith doesn't own its
	// rendering: the only picture available on an idle desktop is the one
	// held for exactly this purpose, re-pushed through the encoder with a
	// fresh pts.
	if (!has_last_frame_) {
		return;
	}
	int64_t pts_us = monotonic_now_us();
	// The same pixels as last time, so nothing has changed: an empty damage
	// region. (What a frame SessionServices skipped did change, it carries
	// over into this push itself.)
	DamageRegion unchanged;
	if (last_frame_is_dmabuf_) {
		deliver_dmabuf(last_dmabuf_frame_, last_dmabuf_token_, pts_us, &unchanged, false);
	} else {
		const uint8_t *pixels = last_cpu_held_ ? last_cpu_held_ : last_cpu_frame_.data();
		session_->encode_cpu(pixels, last_cpu_width_, last_cpu_height_, last_cpu_stride_, pts_us, &unchanged);
	}
}

bool ScreencastHost::ensure_reader() {
	if (reader_ && reader_->is_open()) {
		return true;
	}
	if (reader_tried_) {
		return false;
	}
	reader_tried_ = true;
	reader_ = std::make_unique<DmabufReader>();
	if (!reader_->open(render_drm_fd_)) {
		WLOG_INFO("screencast: no dmabuf read-back; CPU-frame encoders get memfd frames from the compositor");
		reader_.reset();
		return false;
	}
	if (reader_->can_hash_tiles()) {
		tile_source_ = std::make_unique<DmabufTileSource>(*reader_);
	}
	return true;
}

void ScreencastHost::drop_tile_source() {
	if (!tile_source_) {
		return;
	}
	session_->forget_frame_pixels(); // the idle pump may read through it
	last_delivery_tiled_ = false;
	tile_source_.reset();
}

std::vector<uint64_t> ScreencastHost::capture_modifiers() {
	std::vector<uint64_t> encoder_mods = session_->supported_import_modifiers(DRM_FORMAT_XRGB8888);
	if (!session_->wants_cpu_frame()) {
		return encoder_mods;
	}
	if (!ensure_reader()) {
		return {}; // memfd frames, read back by the compositor
	}
	std::vector<uint64_t> reader_mods = reader_->supported_modifiers(DRM_FORMAT_XRGB8888);
	if (encoder_mods.empty()) {
		return reader_mods;
	}
	// Both kinds of push may follow (refinement pausing goes zero-copy),
	// so prefer modifiers both can import.
	std::vector<uint64_t> both;
	for (uint64_t m : encoder_mods) {
		if (std::find(reader_mods.begin(), reader_mods.end(), m) != reader_mods.end()) {
			both.push_back(m);
		}
	}
	return both.empty() ? reader_mods : both;
}

void ScreencastHost::deliver_dmabuf(const DmabufFrame &frame, void *token, int64_t pts_us,
	const DamageRegion *damage, bool fresh) {
	if (tile_source_ && tile_source_->failed()) {
		// The idle pump's read of the last frame failed.
		WLOG_ERROR("screencast: GPU tile hashing failed; reading frames back for refinement instead");
		drop_tile_source();
	}
	if (session_->wants_tiled_dmabuf() && ensure_reader() && tile_source_) {
		// readback_ lags the frame being encoded from here on: a later
		// read-back must read anew.
		readback_current_ = false;
		if (!session_->admit_cpu_frame(damage)) {
			return; // a catch-up redelivery brings the frame back
		}
		tile_source_->bind(frame, token);
		session_->push_tiled(frame, *tile_source_, pts_us, damage);
		if (!tile_source_->failed()) {
			last_delivery_tiled_ = true;
			return;
		}
		// Nothing was pushed, and the tracker is as it was: the same frame
		// goes out read back, as it will from now on.
		WLOG_ERROR("screencast: GPU tile hashing failed; reading frames back for refinement instead");
		drop_tile_source();
	}
	if (!session_->wants_cpu_frame()) {
		// readback_ now lags the frame being encoded: refinement resuming
		// redelivers this frame (fresh = false), and must read it anew.
		readback_current_ = false;
		session_->encode_dmabuf(frame, pts_us);
		return;
	}
	if (fresh) {
		readback_current_ = false;
	}
	if (!session_->admit_cpu_frame(damage)) {
		return; // not read, then: a catch-up redelivery brings the frame back
	}
	if (!readback_current_) {
		if (!ensure_reader()) {
			return; // nothing to give a CPU encoder; the next capture open asks for memfd
		}
		size_t needed = (size_t)frame.width * frame.height * 4;
		if (readback_.size() != needed) {
			readback_.resize(needed);
		}
		if (!reader_->read(frame, token, readback_.data())) {
			give_up_reader();
			return;
		}
		readback_current_ = true;
	}
	// readback_ stays as it is until the next frame's read, which comes
	// before its push_cpu(): the idle pump's in-place contract holds.
	last_delivery_tiled_ = false;
	session_->push_cpu(readback_.data(), (uint32_t)frame.width, (uint32_t)frame.height,
		(uint32_t)frame.width * 4, pts_us, damage);
}

void ScreencastHost::give_up_reader() {
	reopen_without_reader_.schedule(event_loop_, [this] {
		WLOG_ERROR("screencast: dmabuf read-back failed; asking the compositor for memfd frames instead");
		close_capture_and_input();
		drop_tile_source();
		reader_.reset(); // reader_tried_ stays set: capture_modifiers() offers no dmabufs now
		arm_open_retry();
	});
}

bool ScreencastHost::request_resize(uint32_t width, uint32_t height) {
	if (!remote_session_->sizes_output_on_open()) {
		WLOG_ERROR("screencast: %s can't resize its output -- ignoring request for %ux%u, staying at %ux%u",
			remote_session_->name(), width, height, width_, height_);
		return false;
	}
	// Closed and reopened against the same running compositor, whose
	// output the next open() resizes: the desktop keeps running, its
	// windows just see a mode change, as on a monitor switch.
	close_capture_and_input();
	width_ = width;
	height_ = height;
	WLOG_INFO("screencast: resizing %s's output to %ux%u (client's requested size)", remote_session_->name(),
		width, height);
	// The compositor is already up, so this normally succeeds at once;
	// the retry loop is for one that is still starting.
	if (!try_open_capture_and_input()) {
		arm_open_retry();
	}
	return true;
}

void ScreencastHost::close_capture_and_input() {
	// Every callback below reports its event as fatal in the ordinary
	// case (the leader crashing, the compositor dropping the session) --
	// cleared first so the deliberate teardown they're about to go
	// through doesn't also end the session.
	open_retry_timer_.reset();
	remote_session_->on_closed = nullptr;
	if (input_) {
		input_->on_disconnected = nullptr;
		input_->close();
		input_.reset();
	}
	close_capture();
	// Before the RemoteSession closes, as in the destructor: the sink holds
	// its Wayland connection or sd-bus. The next open() makes a new one.
	session_->set_clipboard(nullptr);
	remote_session_->close();
	remote_session_ready_ = false;
	cursor_hidden_ = true;
	cursor_argb_.clear();
}

void ScreencastHost::close_capture() {
	capture_reopen_timer_.reset();
	if (frame_source_) {
		frame_source_->on_closed = nullptr;
	}
	if (session_) {
		session_->forget_frame_pixels(); // the idle pump may point into a held buffer
	}
	if (reader_) {
		reader_->forget_all();
	}
	readback_current_ = false;
	last_delivery_tiled_ = false;
	for (void *token : frame_hold_.drain()) {
		if (frame_source_) {
			frame_source_->release(token);
		}
	}
	frame_source_.reset();
	capture_ready_ = false;
	has_last_frame_ = false;
	last_cpu_held_ = nullptr;
}

void ScreencastHost::set_capture_paused(bool paused) {
	capture_paused_ = paused;
	// Never inside a frame callback: a viewer can drop from within one
	// (a failed send), and the capture can't close under its own feet.
	capture_pause_apply_.schedule(event_loop_, [this] { apply_capture_paused(); });
}

void ScreencastHost::apply_capture_paused() {
	if (capture_paused_) {
		if (capture_ready_) {
			close_capture();
			WLOG_INFO("screencast: no viewer attached, capture paused");
		}
		return;
	}
	// Before the RemoteSession is open the open retry is running, and it
	// opens the capture itself now that it isn't paused.
	if (!remote_session_ready_ || capture_ready_) {
		return;
	}
	if (open_capture()) {
		WLOG_INFO("screencast: viewer attached, capture resumed");
		return;
	}
	// Retried on the open retry's schedule, and fatal in the end, as a
	// capture that closes under a running session is.
	capture_reopen_retries_ = 0;
	capture_reopen_timer_.reset(wl_event_loop_add_timer(
		event_loop_,
		[](void *data) {
			auto *host = static_cast<ScreencastHost *>(data);
			if (host->capture_paused_ || host->open_capture()) {
				host->capture_reopen_timer_.reset();
				return 0;
			}
			if (++host->capture_reopen_retries_ >= kOpenMaxRetries) {
				host->capture_reopen_timer_.reset();
				host->on_capture_closed("it didn't reopen when a viewer attached");
				return 0;
			}
			wl_event_source_timer_update(host->capture_reopen_timer_.get(), kOpenRetryMs);
			return 0;
		},
		this));
	wl_event_source_timer_update(capture_reopen_timer_.get(), kOpenRetryMs);
}

void ScreencastHost::terminate() {
	terminate_requested_ = true;
}

void ScreencastHost::on_capture_closed(const std::string &error) {
	WLOG_ERROR("screencast: capture closed: %s", error.c_str());
	session_ended_ = true;
	terminate();
}

void ScreencastHost::on_capture_buffer_removed(void *token) {
	if (reader_) {
		reader_->forget(token); // its import holds the buffer's fds
	}
	if (!frame_hold_.forget(token)) {
		return;
	}
	// The held token is always the frame last_dmabuf_frame_ or
	// last_cpu_held_ was taken from, and that buffer is gone now: a
	// redeliver_frame() re-encode would push dead fds or freed memory at
	// the encoder. There is simply no last frame until the next one arrives.
	if (last_frame_is_dmabuf_ || last_cpu_held_) {
		if (last_cpu_held_ || last_delivery_tiled_) {
			session_->forget_frame_pixels(); // the idle pump reads that buffer in place
		}
		has_last_frame_ = false;
		last_cpu_held_ = nullptr;
		last_delivery_tiled_ = false;
	}
}

void ScreencastHost::on_remote_session_closed() {
	// The remote session dying takes capture and input down with it
	// (mutter tears down the PipeWire node and the EIS context together)
	// -- same severity as on_capture_closed, not something to retry in
	// place the way a lone input drop is.
	WLOG_ERROR("screencast: %s remote session closed", remote_session_->name());
	session_ended_ = true;
	terminate();
}

void ScreencastHost::on_input_disconnected() {
	// Unlike capture (if the session's still alive, only the input half
	// needs redoing), reconnecting is just asking the same RemoteSession
	// for a fresh sink, not a whole new session. close() here, replace on
	// success: this runs from inside the old sink's own event handling,
	// which is why it isn't destroyed until then.
	WLOG_INFO("screencast: input connection dropped, reconnecting");
	input_->close();
	open_retries_ = 0;
	open_retry_timer_.reset(wl_event_loop_add_timer(
		event_loop_,
		[](void *data) {
			auto *host = static_cast<ScreencastHost *>(data);
			if (host->open_input()) {
				host->open_retry_timer_.reset();
				return 0;
			}
			if (++host->open_retries_ >= kOpenMaxRetries) {
				WLOG_ERROR("screencast: input never reconnected, giving up");
				host->open_retry_timer_.reset();
				host->session_ended_ = true;
				host->terminate();
				return 0;
			}
			wl_event_source_timer_update(host->open_retry_timer_.get(), kOpenRetryMs);
			return 0;
		},
		this));
	wl_event_source_timer_update(open_retry_timer_.get(), kOpenRetryMs);
}

} // namespace wraith
