// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The session layer's view of what hosts it: ScreencastHost (a real
// compositor running top-level, captured and driven with wraith as its
// client -- screencast/screencast_host.hpp, docs/design/capture-backends.md).
// GdpSession and SessionServices are written against this interface rather
// than the host itself, and main.cpp drives it through start()/run()/
// terminate().
#pragma once

// Plain Wayland protocol enums (wl_keyboard_key_state and friends), safe
// to include from C++ without extern "C".
#include <wayland-server-protocol.h>

#include <cstdint>

struct wl_event_loop;

namespace wraith {

class SessionServices;
struct LaunchSpec;

// Synthetic input injection, the target of the GDP input stream
// (gdp-spec.md §8). The screencast backends implement it against the
// captured compositor's seat (screencast/remote_session.hpp).
class InputSink {
public:
	virtual ~InputSink() = default;

	virtual void inject_key(uint32_t time_msec, uint32_t keycode, enum wl_keyboard_key_state state) = 0;
	virtual void inject_motion(uint32_t time_msec, double dx, double dy) = 0;
	virtual void inject_motion_absolute(uint32_t time_msec, double x, double y) = 0;
	virtual void inject_button(uint32_t time_msec, uint32_t button, enum wl_pointer_button_state state) = 0;
	virtual void inject_axis(uint32_t time_msec, enum wl_pointer_axis orientation, double delta) = 0;

	// A newly-attached spectre has never seen a CursorShape: forgets any
	// de-dup state and resends the current cursor image (or hidden, or the
	// default arrow) unconditionally.
	virtual void resend_cursor_shape() = 0;
};

class SessionHost {
public:
	virtual ~SessionHost() = default;

	// Brings the host up without entering its event loop -- the session
	// leader and the capture/input bring-up -- so main.cpp can report
	// readiness to ghostd only once everything that can fail has. run()
	// then dispatches until terminate().
	virtual bool start(const LaunchSpec *spec) = 0;
	virtual void run() = 0;
	virtual void terminate() = 0;

	virtual struct wl_event_loop *event_loop() const = 0;
	virtual uint32_t output_width() const = 0;
	virtual uint32_t output_height() const = 0;

	// SessionHello.displays asked for a resolution other than the
	// current one (gdp_session.cpp's accept_session()). ScreencastHost
	// reopens capture against the running compositor at the new size, or
	// declines if that compositor can't resize (screencast_host.hpp).
	// False means the size is unchanged; callers re-read output_width()/
	// output_height() either way.
	virtual bool request_resize(uint32_t width, uint32_t height) = 0;

	// Encodes the current picture again, even with no organic damage, on a
	// desktop that has gone still: SessionServices skipped a frame while
	// the send queue was backed up (GdpSession::should_send_frame()), or a
	// keyframe was asked for (GdpSession::request_keyframe()), and no new
	// frame would ever come to carry either. Asks for no keyframe itself.
	virtual void redeliver_frame() = 0;

	// No viewer to encode for (SessionServices::set_viewer_attached()):
	// stop capturing, so the compositor stops copying its output for
	// nobody, and capture afresh when one attaches. May be called from
	// inside a frame callback; the host applies it later.
	virtual void set_capture_paused(bool paused) = 0;

	// The render node backing this host's frames, for EncoderConfig::drm_fd
	// (SessionServices::ensure_encoder). -1 if none is available.
	virtual int render_drm_fd() const = 0;

	virtual InputSink &input() = 0;

	// A client-requested logout (LogoutRequest): end the desktop
	// session gracefully and exit once its leader is gone.
	virtual void request_logout() = 0;

	// True once the host is shutting down because the desktop session
	// itself ended, as opposed to any other reason to stop.
	virtual bool session_ended() const = 0;

	virtual SessionServices &session() = 0;
};

} // namespace wraith
