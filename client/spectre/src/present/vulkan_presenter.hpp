// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Presents decoded frames via Vulkan. The façade the stream session talks
// to; the work is split across:
// - VulkanDevice (vulkan_device.hpp): instance/device/queue/swapchain and
//   the shared command buffer + semaphores.
// - VideoImageSource (video_image_source.hpp): turns an AVFrame into a
//   sampled view, by GPU import (hardware decode) or CPU upload (software
//   fallback) -- through a sampler per image format (NV12 + ycbcr on Linux
//   and macOS, BGRA on Windows, I420 + ycbcr for PyroWave).
// - OverlayRenderer (overlay_renderer.hpp): spectre's own UI (toolbar,
//   menu, toasts, statistics) and the cursor overlay drawn on top.
// What's left here is the video pipeline itself and the per-frame
// orchestration in present(): acquire the frame image, acquire a
// swapchain image, record video + overlays, submit, present, wait.
#pragma once

#include "ui/draw_list.hpp"
#include "ui/font.hpp"

#include "gdp/refine.hpp"

#include <cstdint>
#include <memory>

struct SDL_Window;
struct AVFrame;

namespace spectre {

struct VulkanDecodeDevice;

class VulkanPresenter {
public:
	VulkanPresenter();
	~VulkanPresenter();
	VulkanPresenter(const VulkanPresenter &) = delete;
	VulkanPresenter &operator=(const VulkanPresenter &) = delete;

	// `drm_render_node` must be the same node Decoder::open() uses -- see
	// VulkanDevice::init for how it selects the physical device. Also
	// uploads the splash bitmap present_splash() draws (see its comment).
	bool init(SDL_Window *window, const char *drm_render_node);

	// This device as the Vulkan Video decode backend needs it (Decoder's
	// HardwareDecode::vulkan), or null when it can't decode -- see
	// VulkanDevice::decode_device(). Valid after init(), for as long as the
	// presenter lives.
	const VulkanDecodeDevice *decode_device() const;
	// This device as PyroWave's compute decode needs it (HardwareDecode::
	// compute), or null when it can't run it -- see
	// VulkanDevice::compute_device().
	const VulkanDecodeDevice *compute_device() const;
	// VkPhysicalDeviceProperties::vendorID of the device presenting.
	uint32_t vendor_id() const;

#if defined(__linux__)
	// False when VA-API's frames can't be imported into this device --
	// see VulkanDevice::supports_dmabuf_import(). The VA-API backend has
	// to be skipped in that case. Valid after init().
	bool supports_dmabuf_import() const;
#elif defined(_WIN32)
	// LUID (8 bytes) of the adapter the Vulkan device ended up on, for
	// Decoder::set_adapter_luid() -- hardware decode has to happen on this
	// same GPU for its texture to be shareable into Vulkan at all. Null when
	// this device can't import D3D11 textures, in which case there's no
	// point opening a D3D11VA decoder. Valid after init().
	const uint8_t *adapter_luid() const;
#endif

	// Presents `frame` with the current UI (set_ui) and cursor overlays on
	// top. `frame` is any format VideoImageSource::acquire() takes; the
	// path is picked per call from frame->format (video_image_source.hpp).
	// One frame in flight at a time (vkQueueWaitIdle after submit) -- see
	// docs/design/spectre-client.md#limitations.
	// Returns false when the frame wasn't drawn: either this frame alone
	// couldn't be imported/uploaded or there is no usable swapchain right
	// now (recoverable, try the next one; the cause is logged where it
	// happened) or the device/swapchain returned an error (not
	// recoverable; failed() is then true and every later call is a no-op).
	bool present(AVFrame *frame);

	// Replaces the UI drawn over the video (ui/draw_list.hpp) from the
	// next present()/redraw() on. StreamSession rebuilds it before each
	// present and calls redraw() when only the UI changed.
	void set_ui(const UiDrawList &list);
	// Uploads the UI font's atlas (OverlayRenderer::set_font). Call between
	// presents; text draws nothing until it has been called once.
	bool set_font(const UiFont &font);

	// True once a queue or swapchain operation has failed with something
	// other than a resize (VK_ERROR_DEVICE_LOST etc.). Callers should
	// stop the stream rather than keep presenting.
	bool failed() const;

	// Re-presents whatever frame present() last drew, with no new decode
	// involved. For SDL_EVENT_WINDOW_EXPOSED: without a compositor, a
	// swapchain image's obscured region is undefined once something else
	// draws over it (VkSwapchainCreateInfoKHR::clipped=VK_TRUE), and
	// present() only runs when a fresh network frame decodes -- so a
	// window dragged across spectre and back would otherwise leave a
	// stale hole until the next real video frame happens to arrive.
	// Before the first present(), falls back to present_splash() instead
	// of no-op'ing, for the same reason.
	void redraw();

	// Applies one frame's lossless refinement layer to the overlay plane
	// drawn over the video (present/lossless_plane.hpp). `width`/`height`
	// are the session's display size, which sizes the plane. Call before
	// the present() for the frame the layer arrived with; a session that
	// didn't negotiate refinement never calls this at all.
	void apply_lossless_update(const gdp::RefineLayer &layer, uint32_t width, uint32_t height);

	// Makes the whole plane transparent again (a new decoder, whose stream
	// shares nothing with what the plane holds). Harmless when it is
	// already empty.
	void clear_lossless_plane();

	// The session's negotiated picture size (SessionAccept's display).
	// present() shows only this much of a decoded frame that is larger:
	// AV1 cannot signal a crop, so wraith pads a desktop whose width or
	// height is not a multiple of 8 and the padding decodes as picture.
	// Without the crop that padding is stretched into view and the video
	// lands a fraction of a pixel off the lossless refinement plane,
	// which is sized to the display. H.264 and H.265 crop in the decoder,
	// so for them this is a no-op.
	void set_display_size(uint32_t width, uint32_t height);

	// Where the picture (the video and the lossless plane over it) is
	// drawn: the `width`x`height` rectangle at `x`,`y` in window pixels,
	// stretched to fill it. It may hang off the window's edges (the
	// actual-size view, panned); nothing is drawn in the top `clip_top`
	// rows either way, which belong to StreamSession's docked toolbar.
	// A zero width (the default) fills the window below `clip_top`. Takes
	// effect from the next present()/redraw().
	void set_video_placement(float x, float y, float width, float height, uint32_t clip_top);

	// Call when the window is resized (destroys and recreates the
	// swapchain at the new extent on the next present).
	void notify_resized();

	// Presents the splash bitmap (spectre's app icon, centered) instead of
	// a video frame -- there's a real gap between the window appearing and
	// the first decoded frame (session accept, keyframe request/decode,
	// first network round trip), during which the swapchain would
	// otherwise show whatever undefined pixels the compositor last had
	// there. Call once right after init(); redraw() also falls back to
	// this automatically (see its comment) so SDL_EVENT_WINDOW_EXPOSED
	// keeps showing it too, for as long as present() hasn't been called
	// yet. A no-op once failed() is true.
	void present_splash();

	// Uploads a new cursor image (gdp-spec.md §7.4's CursorShape --
	// SessionClient::on_cursor_shape). `argb8888` is width*height*4
	// premultiplied bytes, same DRM_FORMAT_ARGB8888 layout wraith sends
	// its cursor images in (see GdpSession::send_cursor_shape). Safe to call
	// between present() calls (never mid-frame); replaces whatever texture
	// was set before.
	void set_cursor_shape(uint32_t width, uint32_t height, int32_t hotspot_x, int32_t hotspot_y,
		const uint8_t *argb8888);

	// Moves/shows/hides the cursor overlay. `x_px`/`y_px` are window-
	// relative pixel coordinates of the pointer tip (before the hotspot
	// offset, which present() applies). StreamSession feeds it the local
	// pointer in absolute mode and wraith's CursorPosition reports in
	// relative/captured mode (see StreamSession::relative_mouse_).
	void set_cursor_position(float x_px, float y_px, bool visible);

	// Draws the cursor overlay at `scale` times the bitmap's own size (and
	// hotspot). wraith's cursor bitmaps are drawn for the remote output,
	// and the quad is sized in swapchain pixels, so StreamSession feeds
	// this the window-to-stream stretch factor: the pointer keeps the size
	// the host gave it relative to the video, and follows window resizes
	// (and HiDPI swapchains) along with everything else in the picture.
	// Default 1.0.
	void set_cursor_scale(float scale);

	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace spectre
