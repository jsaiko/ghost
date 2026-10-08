// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Everything drawn on top of the video each frame: spectre's own UI (the
// toolbar, session menu, statistics overlay, toasts and scroll indicators,
// delivered as a UiDrawList of filled rectangles and text --
// ui/draw_list.hpp) and the cursor overlay
// (gdp-spec.md §7.4's CursorShape -- spectre always draws the
// server-supplied pointer image itself rather than the client OS's
// generic arrow). Owns three pipelines (solid rect, font-atlas text,
// cursor -- which the splash bitmap also draws through) and their three
// textures (font atlas, cursor, splash); VulkanPresenter calls
// record_draw() inside its render pass after the video quad.
#pragma once

#include "present/vulkan_device.hpp"
#include "ui/draw_list.hpp"
#include "ui/font.hpp"

#include <cstdint>
#include <functional>

namespace spectre {

class OverlayRenderer {
public:
	OverlayRenderer() = default;
	~OverlayRenderer();
	OverlayRenderer(const OverlayRenderer &) = delete;
	OverlayRenderer &operator=(const OverlayRenderer &) = delete;

	// Creates the pipelines. `device` must outlive this object and already
	// have its swapchain (the pipelines are built against its format).
	// Text draws nothing until set_font() has supplied an atlas.
	bool init(VulkanDevice &device);

	// Uploads `font`'s atlas (replacing any previous one) and keeps its
	// glyph metrics for record_draw(). Safe between present() calls
	// (never mid-frame); StreamSession calls it at startup and whenever
	// the display scale changes the font size.
	bool set_font(const UiFont &font);

	// Replaces the UI drawn on top of the video from the next
	// record_draw() on (copied; the caller's list can be rebuilt freely).
	// An empty list draws nothing but the cursor.
	void set_ui(const UiDrawList &list);

	// Uploads a new cursor image (SessionClient::on_cursor_shape).
	// `argb8888` is width*height*4 premultiplied bytes, same
	// DRM_FORMAT_ARGB8888 layout wraith sends its cursor images in (see
	// GdpSession::send_cursor_shape). Safe to call between present() calls
	// (never mid-frame); replaces whatever texture was set before. A 0x0
	// shape just hides the cursor.
	void set_cursor_shape(uint32_t width, uint32_t height, int32_t hotspot_x, int32_t hotspot_y,
		const uint8_t *argb8888);

	// Moves/shows/hides the cursor overlay. `x_px`/`y_px` are window-
	// relative pixel coordinates of the pointer tip (before the hotspot
	// offset, which record_draw() applies).
	void set_cursor_position(float x_px, float y_px, bool visible);

	// Draws the cursor at `scale` times the bitmap's own size (and hotspot)
	// -- see VulkanPresenter::set_cursor_scale for why. Default 1.0.
	void set_cursor_scale(float scale);

	// Uploads the splash bitmap once at startup (VulkanPresenter::init) --
	// a pre-baked, already-premultiplied VK_FORMAT_B8G8R8A8_UNORM image
	// (see client/spectre/CMakeLists.txt's "Splash bitmap" comment), same texture
	// format and upload path as the cursor. Never replaced afterwards.
	bool set_splash_bitmap(uint32_t width, uint32_t height, const uint8_t *bgra8888);

	// Records the UI (rects, then text) and, if a texture is set and the
	// cursor is visible, the cursor quad on top. Must be called inside an
	// active dynamic-rendering pass on `cmd` with viewport and scissor
	// already set to `extent`.
	void record_draw(VkCommandBuffer cmd, VkExtent2D extent);

	// Draws the splash bitmap centered in `extent`, capped to a fraction of
	// the window so it doesn't dwarf a small window. Called by
	// VulkanPresenter::present_splash() instead of the video quad, before
	// any frame has decoded (see its comment for why one is needed at
	// all). Must be called inside an active dynamic-rendering pass, same as
	// record_draw().
	void record_splash(VkCommandBuffer cmd, VkExtent2D extent);

private:
	bool create_rect_pipeline();
	bool create_text_pipeline();
	bool create_cursor_pipeline();
	// Host-visible sampled texture, PREINITIALIZED -> SHADER_READ_ONLY,
	// filled by `fill(mapped, row_pitch)`; the font atlas, cursor and
	// splash all go through this. On failure everything created is
	// destroyed and false returned.
	bool create_sampled_texture(VkFormat format, uint32_t width, uint32_t height,
		const std::function<void(uint8_t *mapped, size_t row_pitch)> &fill, OwnedImage *out,
		const char *what);
	void record_text(VkCommandBuffer cmd, VkExtent2D extent, const UiTextPrim &text);

	VulkanDevice *dev_ = nullptr;

	VkPipelineLayout rect_pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline rect_pipeline_ = VK_NULL_HANDLE;

	// Text: the font atlas (ui/font.hpp) as an R8 texture, linearly
	// sampled (it's rasterized 2x oversampled); one draw per glyph.
	VkSampler text_sampler_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout text_set_layout_ = VK_NULL_HANDLE;
	VkDescriptorPool text_descriptor_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet text_set_ = VK_NULL_HANDLE;
	VkPipelineLayout text_pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline text_pipeline_ = VK_NULL_HANDLE;
	OwnedImage font_texture_;
	UiFont font_; // metrics for record_text() (a copy of what set_font() uploaded)

	UiDrawList ui_;

	// Cursor overlay: a plain (non-ycbcr) sampled texture, uploaded rarely
	// (only when the shape changes) rather than per frame like the video
	// image.
	VkSampler cursor_sampler_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout cursor_set_layout_ = VK_NULL_HANDLE;
	VkDescriptorPool cursor_descriptor_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet cursor_set_ = VK_NULL_HANDLE;
	VkPipelineLayout cursor_pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline cursor_pipeline_ = VK_NULL_HANDLE;

	OwnedImage cursor_texture_;
	uint32_t cursor_width_ = 0;
	uint32_t cursor_height_ = 0;
	int32_t cursor_hotspot_x_ = 0;
	int32_t cursor_hotspot_y_ = 0;

	float cursor_pos_x_ = 0.0f;
	float cursor_pos_y_ = 0.0f;
	bool cursor_visible_ = false;
	float cursor_scale_ = 1.0f;

	// Splash bitmap (see set_splash_bitmap): a second descriptor set drawn
	// through the same cursor_pipeline_/cursor_pipeline_layout_ -- it's
	// just another textured, premultiplied-alpha quad, same shader. Set
	// once at startup and never replaced, unlike the cursor texture.
	VkDescriptorSet splash_set_ = VK_NULL_HANDLE;
	OwnedImage splash_texture_;
	uint32_t splash_width_ = 0;
	uint32_t splash_height_ = 0;
};

} // namespace spectre
