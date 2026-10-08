// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// FrameSource: the frame-producing half of a screencast backend, as
// ScreencastHost consumes it (docs/design/capture-backends.md). A
// RemoteSession hands the host one of these already bound to the
// compositor-specific thing it captures from (a PipeWire node id, an
// ext_image_copy_capture session); the host wires the callbacks, then
// open()s it. Implementations: PipeWireCapture (GNOME, KDE) and
// ExtImageCopyCapture (ext).
//
// Runs on the caller's wl_event_loop: an implementation adds its own fds
// as sources, no dedicated thread. Callers must not block in any callback
// -- frames arrive at up to 60Hz off the same loop that also services GDP.
#pragma once

#include "encode/encoder.hpp" // DmabufFrame

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct wl_event_loop;

namespace wraith {

class FrameSource {
public:
	virtual ~FrameSource() = default;

	struct Params {
		struct wl_event_loop *event_loop = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		// Encoder::supported_import_modifiers(DRM_FORMAT_XRGB8888) --
		// offered as the dmabuf format's acceptable modifier set. Capture
		// still works with this empty (dmabuf import modifiers unknown
		// or unavailable); an implementation always offers its CPU
		// fallback alongside regardless.
		std::vector<uint64_t> import_modifiers;
	};

	// Fires once per frame, off the event loop given in Params, after
	// open() has connected and frames have started. Exactly one of
	// on_dmabuf_frame/on_cpu_frame fires per frame, matching whichever
	// path the producer negotiated -- never a mix mid-session.
	// A non-null `release_token` must eventually reach release() below,
	// exactly once (FrameHold's buffer-holding rule is the caller's, not
	// enforced here); until then the frame stays valid -- a dmabuf's fds,
	// or a CPU frame's pixels. on_dmabuf_frame always has one. on_cpu_frame
	// has one when the producer lends its own mapped buffer (PipeWire's
	// memfd/MemPtr), and nullptr when the pixels are only valid for the
	// callback's duration (a temporary mapping) and need no release.
	// `damage` (both): what the producer repainted since its last frame,
	// from its damage metadata, or null when it sent none.
	std::function<void(const DmabufFrame &frame, int64_t pts_us, void *release_token,
		const DamageRegion *damage)>
		on_dmabuf_frame;
	std::function<void(const uint8_t *xrgb, uint32_t width, uint32_t height, uint32_t stride, int64_t pts_us,
		const DamageRegion *damage, void *release_token)>
		on_cpu_frame;
	// Cursor image in wraith's own ARGB8888-premultiplied convention
	// (gdp_session.hpp's send_cursor_shape); any channel swap from the
	// producer's format is the implementation's job, not the caller's.
	std::function<void(uint32_t width, uint32_t height, int32_t hotspot_x, int32_t hotspot_y,
		const uint8_t *argb8888_premultiplied)>
		on_cursor_shape;
	// 0..1 over the captured output. "Only while relative motion is
	// driving the pointer" is the caller's rule to apply, not this
	// class's.
	std::function<void(double x, double y)> on_cursor_position;
	std::function<void()> on_cursor_hidden;
	// The producer erroring out or going away under us; open() itself
	// reports failure via its own return value, not this.
	std::function<void(const std::string &error)> on_closed;
	// The producer withdrew this buffer (PipeWire's remove_buffer, or an
	// implementation tearing its pool down): the token is dead from here
	// on -- its backing memory is freed and the dmabuf fds it carried are
	// closed -- so a holder must drop it (FrameHold::forget) and anything
	// derived from that frame. Fires on the event loop, before the buffer
	// memory is gone. release() on a withdrawn token is a no-op
	// regardless, so a caller that ignores this only leaks its own
	// bookkeeping, never crashes. An implementation that owns its own
	// buffers may never fire it.
	std::function<void(void *release_token)> on_buffer_removed;

	// Callbacks above are read at open() time (an implementation may copy
	// them for its own C callbacks), so they must be set before this.
	// Frames only start once the producer is actually streaming; this
	// call reports the connection setup only.
	virtual bool open(const Params &params) = 0;
	virtual void close() = 0;
	// Gives the buffer identified by `release_token` back to the
	// producer. Exactly one release() per delivery that carried a token;
	// never for an on_cpu_frame with a null one. Ignored for a withdrawn
	// token.
	virtual void release(void *release_token) = 0;
};

} // namespace wraith
