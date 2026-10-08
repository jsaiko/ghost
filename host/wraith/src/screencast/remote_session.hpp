// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// RemoteSession: the compositor-specific half of a screencast backend
// (docs/design/capture-backends.md). ScreencastHost is generic: it owns
// the session leader, the retry-until-ready loop, GDP, the encoder,
// audio and teardown, and holds a FrameSource and a ScreencastInput it
// obtained from one of these. What a RemoteSession does to obtain them is
// its own business -- mutter's private D-Bus API (GnomeRemoteSession),
// kwin's zkde_screencast Wayland protocol plus its EIS D-Bus method
// (KwinRemoteSession), a plain Wayland connection speaking
// ext_image_copy_capture_v1 (ExtRemoteSession).
#pragma once

#include "screencast/frame_source.hpp"
#include "session/clipboard_sync.hpp"
#include "session/session_host.hpp"

#include <cstdint>
#include <functional>
#include <memory>

struct wl_event_loop;

namespace wraith {

// An InputSink into a compositor wraith is a client of, plus the
// lifecycle bits ScreencastHost needs around it. Implementations: EiInput
// (libei; GNOME, KDE) and VirtualInput (zwlr_virtual_pointer_v1 +
// zwp_virtual_keyboard_v1; ext). A RemoteSession returns these already open;
// the host sets the two callbacks afterwards, so implementations must
// read them at fire time, not copy them at open.
class ScreencastInput : public InputSink {
public:
	virtual void close() = 0;
	// Fires once if the connection drops from under us (the compositor's
	// input service ending, or a restart). The host decides whether/when
	// to ask its RemoteSession for a fresh one.
	std::function<void()> on_disconnected;
	// Supplied by the host: the cached last-cursor-bitmap replay lives
	// there (fed by the FrameSource's cursor callbacks), not here.
	std::function<void()> resend_cursor_shape_cb;
};

class RemoteSession {
public:
	virtual ~RemoteSession() = default;

	struct Config {
		struct wl_event_loop *event_loop = nullptr;
		// The virtual output's pixel size (wraith's -o): what the
		// compositor is asked to render at, and what absolute input is
		// scaled into.
		uint32_t width = 0;
		uint32_t height = 0;
	};

	// Establishes whatever the compositor needs before capture and input
	// can be created. Retried by the host on a timer while the compositor
	// is still starting, so a false here must be cheap and side-effect
	// free enough to try again 250ms later.
	virtual bool open(const Config &config) = 0;
	virtual void close() = 0;
	// The session dying out from under us (the compositor closed it, or
	// the connection carrying it dropped). Capture and input are assumed
	// gone with it.
	std::function<void()> on_closed;

	// Valid after open() returned true. Returns an *unopened* FrameSource
	// bound to this session's capture target; the host wires callbacks
	// and open()s it with its own Params.
	virtual std::unique_ptr<FrameSource> make_frame_source() = 0;
	// Valid after open() returned true. Returns an already-open input
	// sink, or null if the compositor's input service isn't ready yet
	// (retryable). Called again after on_disconnected for a fresh one.
	virtual std::unique_ptr<ScreencastInput> make_input_sink() = 0;
	// Valid after open() returned true. Returns an already-open clipboard
	// sink, or null when this compositor exposes no mechanism for one --
	// which is not an error: the host logs it once and the session simply
	// runs without the "clipboard" capability (session/gdp_session.cpp).
	// Called once per open(), unlike make_input_sink(); there is no
	// reconnect path, a dropped clipboard goes with the whole session.
	// The default is "no mechanism", so a backend opts in by overriding.
	virtual std::unique_ptr<ClipboardSink> make_clipboard_sink() { return nullptr; }
	// For log lines: the compositor's name ("mutter", "kwin", ...).
	virtual const char *name() const = 0;
	// Whether open() makes the compositor's output Config's width x height
	// itself, so a new size needs only close() and open() again against
	// the same running compositor (ScreencastHost::request_resize()). A
	// session that can't is never resized.
	virtual bool sizes_output_on_open() const { return false; }
};

} // namespace wraith
