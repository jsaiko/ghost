// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// ScreencastHost: the SessionHost (session/session_host.hpp) for the
// `screencast` backends (docs/design/capture-backends.md): a real,
// top-level compositor runs as the session leader and wraith is purely a
// *client* of that session: no Wayland socket of its own, no scene, no
// Xwayland. Frames
// arrive through a FrameSource, input goes out through a ScreencastInput,
// and both come from the one compositor-specific object here, the
// RemoteSession (GnomeRemoteSession, KwinRemoteSession or
// ExtRemoteSession). Everything else -- the session
// leader's lifecycle, the GDP endpoint, the encoder, audio, ghostd
// reporting -- is LeaderHost's and SessionServices', shared by every
// backend.
#pragma once

#include "util/event_source.hpp"
#include "session/session_services.hpp"
#include "screencast/dmabuf_reader.hpp"
#include "screencast/dmabuf_tile_source.hpp"
#include "screencast/frame_hold.hpp"
#include "screencast/frame_source.hpp"
#include "screencast/remote_session.hpp"
#include "session/leader_host.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct wl_event_loop;

namespace wraith {

class ScreencastHost : public LeaderHost {
public:
	struct Options {
		unsigned width = 1920;
		unsigned height = 1080;
		// The compositor-specific half. Required.
		std::unique_ptr<RemoteSession> remote_session;
	};

	ScreencastHost();
	~ScreencastHost() override;
	ScreencastHost(const ScreencastHost &) = delete;
	ScreencastHost &operator=(const ScreencastHost &) = delete;

	bool init(Options options);

	// --- SessionHost lifecycle ---
	// start(): forks `spec` as the session leader (e.g. packaging/ghost/
	// sessions/gnome-screencast execs gnome-shell --headless), without
	// WAYLAND_DISPLAY/DISPLAY (there is no host compositor for it to nest
	// in), and arms the retry timer that
	// keeps asking the RemoteSession to open until the compositor is up
	// (its D-Bus names registered, its socket listening -- whatever the
	// RemoteSession needs), then opens capture and input. Null `spec` is
	// rejected: a screencast host with no leader has nothing to capture.
	// run() then dispatches the event loop until
	// terminate().
	bool start(const LaunchSpec *spec) override;
	void run() override;
	void terminate() override;

	// --- SessionHost ---
	struct wl_event_loop *event_loop() const override { return event_loop_; }
	uint32_t output_width() const override { return width_; }
	uint32_t output_height() const override { return height_; }
	// Closes capture and input, takes the new size, and reopens against
	// the same running compositor, whose output the RemoteSession resizes
	// on open(). Declines when the RemoteSession can't size its output.
	bool request_resize(uint32_t width, uint32_t height) override;
	void redeliver_frame() override;
	// Closes the FrameSource alone -- the RemoteSession, input and the
	// compositor's output (GNOME's virtual monitor) stay -- and makes a
	// new one on resume, whose first frame carries the attach's keyframe.
	// The cursor state is kept for the attach's replay.
	void set_capture_paused(bool paused) override;
	int render_drm_fd() const override { return render_drm_fd_; }
	InputSink &input() override { return input_proxy_; }
	SessionServices &session() override { return *session_; }

private:
	// GDP/debug input arrives whenever a client is attached, which can be
	// before the compositor's input service is up or between a drop and
	// its reconnect; this forwards to the current ScreencastInput and is a
	// silent no-op when there is none. Cursor replay is the host's own
	// state, so that one never depends on the input side at all.
	struct InputProxy final : InputSink {
		ScreencastHost *host = nullptr;
		void inject_key(uint32_t time_msec, uint32_t keycode, enum wl_keyboard_key_state state) override;
		void inject_motion(uint32_t time_msec, double dx, double dy) override;
		void inject_motion_absolute(uint32_t time_msec, double x, double y) override;
		void inject_button(uint32_t time_msec, uint32_t button, enum wl_pointer_button_state state) override;
		void inject_axis(uint32_t time_msec, enum wl_pointer_axis orientation, double delta) override;
		void resend_cursor_shape() override;
	};

	// Arms the timer that polls try_open_capture_and_input() until the
	// compositor is ready.
	void arm_open_retry();
	// Undoes whatever try_open_capture_and_input() has got to so far, with
	// every callback that would report it as fatal cleared first, so the
	// next one starts from scratch (request_resize()).
	void close_capture_and_input();
	// Retried on a timer until all three succeed or kOpenMaxRetries ticks
	// have elapsed: a compositor takes a few seconds after the leader is
	// forked before a RemoteSession can open against it.
	bool try_open_capture_and_input();
	// Makes, wires and opens a FrameSource; true if one is already open.
	bool open_capture();
	// Releases every held frame and closes the FrameSource.
	void close_capture();
	// Asks the RemoteSession for a fresh, open input sink and wires it --
	// the tail of try_open_capture_and_input(), and the whole of
	// on_input_disconnected()'s reconnect.
	bool open_input();
	// The attached GdpSession once it has authenticated, else null: the
	// gate on every send the capture callbacks and cursor replay make.
	GdpSession *active_session() const;
	void on_capture_closed(const std::string &error);
	// FrameSource::on_buffer_removed: drop a withdrawn buffer from
	// frame_hold_ (never re-queue it) and, since it was the newest frame,
	// the keyframe-replay copy of it whose dmabuf fds died with it.
	void on_capture_buffer_removed(void *token);
	void on_remote_session_closed();
	void on_input_disconnected();
	void replay_cursor_shape();
	// Cursor position for spectre's relative-mode (capture) overlay. The
	// compositor only reports position as frame metadata, i.e. throttled
	// to frame delivery, and in relative mode spectre draws its cursor
	// from our reports (it can't use its own grabbed pointer), so on a
	// screen that isn't repainting, frame metadata alone would leave the
	// cursor frozen between frames. wraith tracks position itself from the
	// relative deltas it injects, snaps it to the frame metadata whenever a
	// frame arrives, and sends it independently of frames. Normalized 0..1.
	void track_relative_motion(double dx, double dy);
	void note_absolute_motion(double x, double y);
	void schedule_cursor_position_send();
	void flush_cursor_position();

	struct wl_event_loop *event_loop_ = nullptr;
	unsigned width_ = 1920, height_ = 1080;
	int render_drm_fd_ = -1;
	std::unique_ptr<SessionServices> session_;
	std::unique_ptr<RemoteSession> remote_session_;
	std::unique_ptr<FrameSource> frame_source_;
	std::unique_ptr<ScreencastInput> input_;
	InputProxy input_proxy_;
	FrameHold frame_hold_;
	bool remote_session_ready_ = false;
	bool capture_ready_ = false;
	// The frame FrameHold's current() token belongs to -- safe to re-push
	// through the encoder exactly because that token is by
	// definition not yet released back to the producer's pool, so a dmabuf
	// frame's fds are still that buffer's live contents. Exactly one of
	// the two is populated at a time (has_last_frame_ false until the
	// first frame).
	bool has_last_frame_ = false;
	bool last_frame_is_dmabuf_ = false;
	DmabufFrame last_dmabuf_frame_{};
	void *last_dmabuf_token_ = nullptr;

	// CPU pixels for an encoder that wants them, read back from the
	// compositor's dmabufs here rather than asking it for memfd frames
	// (dmabuf_reader.hpp says why). reader_ opens on first need; a failed
	// open is not retried, and captures then ask for memfd frames.
	std::unique_ptr<DmabufReader> reader_;
	bool reader_tried_ = false;
	bool ensure_reader();
	// The modifiers to offer the compositor: the encoder's own (zero-copy
	// import), or, for an encoder that wants CPU pixels, ones the reader
	// can import -- empty (memfd frames) when there is no reader.
	std::vector<uint64_t> capture_modifiers();
	// Refinement over a hardware encoder hashes on the GPU through this
	// (SessionServices::push_tiled()) and the frame stays a dmabuf. Made
	// with the reader when it can_hash_tiles(); dropped for good -- the
	// session falls back to read-back -- if a GPU read ever fails.
	std::unique_ptr<DmabufTileSource> tile_source_;
	// The newest frame went out through tile_source_: the idle pump reads
	// that dmabuf, not readback_.
	bool last_delivery_tiled_ = false;
	void drop_tile_source();
	// readback_ holds the last frame read back, tightly packed; current
	// while readback_current_ (redeliver_frame() re-pushes it unread).
	std::vector<uint8_t> readback_;
	bool readback_current_ = false;
	// A read-back that fails mid-session drops the reader for good and
	// reopens the capture asking for memfd frames -- from the event loop,
	// since the capture can't be torn down inside its own frame callback.
	IdleCoalescer reopen_without_reader_;
	void give_up_reader();
	// One dmabuf frame to the encoder: with a GPU tile source if it takes
	// one, else read back and pushed as CPU pixels if it wants those
	// (`fresh`: a new frame, not a re-push), else zero-copy.
	void deliver_dmabuf(const DmabufFrame &frame, void *token, int64_t pts_us, const DamageRegion *damage,
		bool fresh);
	// A CPU frame is either held (the producer lent its buffer:
	// last_cpu_held_ points into it, valid while FrameHold has its token)
	// or, from a producer that can't lend, copied into last_cpu_frame_.
	const uint8_t *last_cpu_held_ = nullptr;
	std::vector<uint8_t> last_cpu_frame_;
	uint32_t last_cpu_width_ = 0, last_cpu_height_ = 0, last_cpu_stride_ = 0;
	// Last cursor state the FrameSource reported, replayed by
	// resend_cursor_shape() since that state lives here, not in the input
	// sink (which has none of its own).
	bool cursor_hidden_ = true;
	uint32_t cursor_width_ = 0, cursor_height_ = 0;
	int32_t cursor_hotspot_x_ = 0, cursor_hotspot_y_ = 0;
	std::vector<uint8_t> cursor_argb_;
	double cursor_track_x_ = 0.5, cursor_track_y_ = 0.5; // normalized, centre until first input/frame
	IdleCoalescer cursor_position_send_;
	EventSource open_retry_timer_;
	// set_capture_paused()'s wish, applied from the event loop by
	// apply_capture_paused(); while set, the open retry opens no capture.
	bool capture_paused_ = false;
	IdleCoalescer capture_pause_apply_;
	void apply_capture_paused();
	// Retries open_capture() on resume, apart from open_retry_timer_
	// (which an input reconnect may be using).
	EventSource capture_reopen_timer_;
	int capture_reopen_retries_ = 0;
	int open_retries_ = 0;
	bool terminate_requested_ = false;
};

} // namespace wraith
