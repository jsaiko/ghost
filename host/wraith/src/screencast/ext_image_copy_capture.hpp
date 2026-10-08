// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// ExtImageCopyCapture: FrameSource (frame_source.hpp) over the standard
// ext_image_copy_capture_v1 protocol (docs/design/capture-backends.md#ext):
// wraith as a plain Wayland client of any compositor that exports it
// (wlroots 0.19, Smithay/COSMIC, niri, Hyprland, ...). kwin and mutter
// don't export it, which is why they have backends of their own.
//
// Unlike PipeWireCapture, wraith owns the buffers: the session advertises
// a size, a DRM device and dmabuf formats (or shm formats), this class
// allocates a small pool on that device through gbm, imports each buffer
// with linux-dmabuf, and runs the pull loop create_frame -> attach_buffer
// -> damage_buffer(full) -> capture -> ready. After the first frame the
// compositor may wait indefinitely for content to change (the spec), so
// an idle desktop yields nothing, same as PipeWire; on_buffer_removed
// never fires (nobody can withdraw wraith's own buffers). Cursor: the
// protocol's pointer cursor session over a wl_pointer of the seat. A
// headless compositor (labwc for the LXQt session) has no pointer at all
// until wraith's own virtual pointer appears, which happens after this
// source opens, so the cursor session is created lazily from the seat's
// capabilities events rather than once at open().
#pragma once

#include "screencast/frame_source.hpp"

#include <cstdint>
#include <memory>

struct wl_output;
struct wl_seat;
struct wl_shm;
struct zwp_linux_dmabuf_v1;
struct ext_output_image_capture_source_manager_v1;
struct ext_image_copy_capture_manager_v1;

namespace wraith {

class WaylandClient;

class ExtImageCopyCapture : public FrameSource {
public:
	// Globals the ExtRemoteSession bound and keeps alive for as long as
	// this object exists; nothing here is destroyed by this class, except
	// `seat`, a wl_seat proxy bound for this object alone (a proxy takes
	// one listener, and the ExtRemoteSession already listens on its own)
	// which close() destroys.
	struct Globals {
		WaylandClient *client = nullptr;
		struct wl_output *output = nullptr;
		struct ext_output_image_capture_source_manager_v1 *source_manager = nullptr;
		struct ext_image_copy_capture_manager_v1 *capture_manager = nullptr;
		struct zwp_linux_dmabuf_v1 *dmabuf = nullptr; // may be null: shm/CPU path only
		struct wl_shm *shm = nullptr;                 // may be null if dmabuf is not
		struct wl_seat *seat = nullptr;               // may be null: no cursor session ever
	};

	explicit ExtImageCopyCapture(const Globals &globals);
	~ExtImageCopyCapture() override;
	ExtImageCopyCapture(const ExtImageCopyCapture &) = delete;
	ExtImageCopyCapture &operator=(const ExtImageCopyCapture &) = delete;

	// Creates the output capture source and session, round-trips for its
	// constraints (buffer_size, dmabuf_device/formats or shm formats),
	// allocates the pool and submits the first capture. Frames then
	// arrive on the event loop as ready events.
	bool open(const Params &params) override;
	void close() override;
	void release(void *release_token) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
