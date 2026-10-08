// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The client half of the lossless refinement layer (gdp/refine.hpp).
//
// A persistent full-frame BGRA texture drawn over the decoded video, and
// under spectre's own UI and cursor. Where its alpha is 255 the viewer
// sees the host's exact source pixels; where it is 0 they see the codec's
// base layer through it. wraith's tile tracker keeps it honest: a tile
// arrives when a region has settled, and a clear (or a whole-plane reset)
// arrives the moment that region starts changing again, so the plane never
// shows pixels the host has since replaced.
//
// Like the video, it draws the whole clip volume, so the viewport
// VulkanPresenter sets (set_video_placement()) places the two alike and
// they line up pixel for pixel.
#pragma once

#include "present/vulkan_device.hpp"

#include "gdp/refine.hpp"

#include <cstdint>

namespace spectre {

class LosslessPlane {
public:
	LosslessPlane() = default;
	~LosslessPlane();
	LosslessPlane(const LosslessPlane &) = delete;
	LosslessPlane &operator=(const LosslessPlane &) = delete;

	// Creates the pipeline (not the texture -- that waits for the first
	// frame, since nothing here knows the video size until then).
	// `device` must outlive this object.
	bool init(VulkanDevice &device);

	// Applies one frame's layer, sizing (or resizing) the plane to
	// `width`x`height` if needed. Called between present() calls, never
	// mid-frame. A layer referring to pixels outside the frame is clipped
	// rather than rejected -- it costs one comparison and means a host
	// that miscounts tiles can't write past the image.
	void apply(const gdp::RefineLayer &layer, uint32_t width, uint32_t height);

	// Makes the whole plane transparent (the image itself is kept). Also
	// what a layer's whole-plane reset does. A no-op before the first
	// apply() has created the image.
	void clear();

	// Records the host-write -> shader-read barrier for whatever apply()
	// wrote since the last frame. Must be recorded before the render pass
	// begins, like VideoImageSource::record_acquire_barrier.
	void record_upload_barrier(VkCommandBuffer cmd);

	// Draws the plane over the video if any tile is showing. Call inside
	// the render pass, after the video quad and before the UI/cursor
	// overlays.
	void record_draw(VkCommandBuffer cmd);

	void shutdown();

private:
	bool ensure_image(uint32_t width, uint32_t height);
	void destroy_image();

	VulkanDevice *dev_ = nullptr;

	VkSampler sampler_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet set_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;

	// Host-visible linear image kept mapped for the life of the session:
	// apply() writes single tiles into it up to 60 times a second, so a
	// map/unmap pair per frame (what OverlayRenderer's rarely-changing
	// textures do) would be pure overhead. Held in VK_IMAGE_LAYOUT_GENERAL
	// rather than SHADER_READ_ONLY_OPTIMAL for the same reason: the host
	// keeps writing to it between draws.
	OwnedImage texture_;
	uint8_t *mapped_ = nullptr;
	size_t row_pitch_ = 0;
	uint32_t width_ = 0;
	uint32_t height_ = 0;

	// A tile has been applied since the plane was last cleared, i.e. there
	// is something to draw.
	bool has_content_ = false;
	// Something was written since the last record_upload_barrier().
	bool dirty_ = false;
};

} // namespace spectre
