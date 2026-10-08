// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// ExtRemoteSession: the RemoteSession (remote_session.hpp) for
// `Backend=screencast-ext` (docs/design/capture-backends.md): a
// compositor exporting the standard ext_image_copy_capture_v1 runs top-
// level as the session leader and wraith is a plain Wayland client of it
// -- frames through ExtImageCopyCapture, input through VirtualInput. No
// D-Bus at all; the whole handshake is "connect and bind globals".
//
// Against a compositor lacking one of the required globals, open() fails
// cleanly and names it.
#pragma once

#include "screencast/remote_session.hpp"

#include <memory>

namespace wraith {

class ExtRemoteSession : public RemoteSession {
public:
	ExtRemoteSession();
	~ExtRemoteSession() override;
	ExtRemoteSession(const ExtRemoteSession &) = delete;
	ExtRemoteSession &operator=(const ExtRemoteSession &) = delete;

	// Connects to the leader's socket (the user manager's WAYLAND_DISPLAY,
	// else wayland-0) and binds: wl_output, wl_seat (+ its wl_pointer for
	// the cursor session), ext_output_image_capture_source_manager_v1,
	// ext_image_copy_capture_manager_v1, zwp_linux_dmabuf_v1 and/or
	// wl_shm, zwlr_virtual_pointer_manager_v1,
	// zwp_virtual_keyboard_manager_v1. False, cleanly, while the socket
	// isn't there yet; false with the missing interface logged once if
	// the compositor lacks one of the required globals.
	//
	// Also resizes the compositor's first output to config.width x
	// config.height via zwlr_output_manager_v1, if the compositor exports
	// it (best-effort, not in the required list above -- unlike
	// screencast-kwin/-gnome, ext has no other way to make the captured
	// size match what the client asked for, since the compositor's own
	// output size is otherwise whatever it started with; see
	// ext_remote_session.cpp's resize_output_to_config()).
	bool open(const Config &config) override;
	void close() override;

	// --- RemoteSession ---
	std::unique_ptr<FrameSource> make_frame_source() override;
	std::unique_ptr<ScreencastInput> make_input_sink() override;
	// ext-data-control-v1 if the compositor exports it (labwc/wlroots do);
	// null otherwise.
	std::unique_ptr<ClipboardSink> make_clipboard_sink() override;
	const char *name() const override { return "ext"; }
	// True until an open() finds the compositor lacks a usable
	// zwlr_output_manager_v1 (or rejects the mode).
	bool sizes_output_on_open() const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
