// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "present/overlay_renderer.hpp"

#include "rect_vert_spv.h"
#include "rect_frag_spv.h"
#include "text_vert_spv.h"
#include "text_frag_spv.h"
#include "cursor_vert_spv.h"
#include "cursor_frag_spv.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace spectre {

namespace {

// rect.vert's push_constant block.
struct RectPushConstants {
	float rect[4];
	float color[4];
};

// text.vert's push_constant block.
struct TextPushConstants {
	float rect[4];
	float uv[4];
	float color[4];
};

// Window pixels -> NDC rect (x0, y0, w, h).
void pixel_rect_to_ndc(float x, float y, float w, float h, VkExtent2D extent, float out[4]) {
	float win_w = (float)extent.width;
	float win_h = (float)extent.height;
	out[0] = (x / win_w) * 2.0f - 1.0f;
	out[1] = (y / win_h) * 2.0f - 1.0f;
	out[2] = (w / win_w) * 2.0f;
	out[3] = (h / win_h) * 2.0f;
}

} // namespace

OverlayRenderer::~OverlayRenderer() {
	if (!dev_) {
		return;
	}
	VkDevice device = dev_->device();
	splash_texture_.destroy(device);
	cursor_texture_.destroy(device);
	font_texture_.destroy(device);
	if (cursor_pipeline_) vkDestroyPipeline(device, cursor_pipeline_, nullptr);
	if (cursor_pipeline_layout_) vkDestroyPipelineLayout(device, cursor_pipeline_layout_, nullptr);
	if (cursor_descriptor_pool_) vkDestroyDescriptorPool(device, cursor_descriptor_pool_, nullptr);
	if (cursor_set_layout_) vkDestroyDescriptorSetLayout(device, cursor_set_layout_, nullptr);
	if (cursor_sampler_) vkDestroySampler(device, cursor_sampler_, nullptr);
	if (text_pipeline_) vkDestroyPipeline(device, text_pipeline_, nullptr);
	if (text_pipeline_layout_) vkDestroyPipelineLayout(device, text_pipeline_layout_, nullptr);
	if (text_descriptor_pool_) vkDestroyDescriptorPool(device, text_descriptor_pool_, nullptr);
	if (text_set_layout_) vkDestroyDescriptorSetLayout(device, text_set_layout_, nullptr);
	if (text_sampler_) vkDestroySampler(device, text_sampler_, nullptr);
	if (rect_pipeline_) vkDestroyPipeline(device, rect_pipeline_, nullptr);
	if (rect_pipeline_layout_) vkDestroyPipelineLayout(device, rect_pipeline_layout_, nullptr);
}

bool OverlayRenderer::init(VulkanDevice &device) {
	dev_ = &device;
	return create_rect_pipeline() && create_text_pipeline() && create_cursor_pipeline();
}

void OverlayRenderer::set_ui(const UiDrawList &list) {
	ui_ = list;
}

bool OverlayRenderer::create_rect_pipeline() {
	// Push-constant-driven solid quad, alpha-blended on top of the video
	// (translucent menu/stats backdrops).
	VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(RectPushConstants)};
	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	if (!vk_check(vkCreatePipelineLayout(dev_->device(), &layout_info, nullptr, &rect_pipeline_layout_),
			"vkCreatePipelineLayout(rect)")) {
		return false;
	}

	VulkanDevice::PipelineDesc desc;
	desc.vert_spv = rect_vert_spv;
	desc.vert_words = sizeof(rect_vert_spv) / sizeof(uint32_t);
	desc.frag_spv = rect_frag_spv;
	desc.frag_words = sizeof(rect_frag_spv) / sizeof(uint32_t);
	desc.layout = rect_pipeline_layout_;
	desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	desc.premultiplied_blend = true;
	desc.what = "vkCreateGraphicsPipelines(rect)";
	return dev_->create_pipeline(desc, &rect_pipeline_);
}

bool OverlayRenderer::create_text_pipeline() {
	VkDevice device = dev_->device();
	// LINEAR: the atlas is rasterized 2x oversampled (ui/font.cpp) and
	// each glyph quad is drawn at half the atlas rect's size, so the
	// bilinear average is what produces the anti-aliasing.
	if (!dev_->create_sampler(VK_FILTER_LINEAR, nullptr, &text_sampler_, "text sampler") ||
		!dev_->create_texture_sets(text_sampler_, 1, &text_set_layout_, &text_descriptor_pool_, &text_set_,
			"text descriptor set")) {
		return false;
	}

	VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TextPushConstants)};
	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &text_set_layout_;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	if (!vk_check(vkCreatePipelineLayout(device, &layout_info, nullptr, &text_pipeline_layout_),
			"vkCreatePipelineLayout(text)")) {
		return false;
	}

	VulkanDevice::PipelineDesc desc;
	desc.vert_spv = text_vert_spv;
	desc.vert_words = sizeof(text_vert_spv) / sizeof(uint32_t);
	desc.frag_spv = text_frag_spv;
	desc.frag_words = sizeof(text_frag_spv) / sizeof(uint32_t);
	desc.layout = text_pipeline_layout_;
	desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	desc.premultiplied_blend = true;
	desc.what = "vkCreateGraphicsPipelines(text)";
	return dev_->create_pipeline(desc, &text_pipeline_);
}

bool OverlayRenderer::set_font(const UiFont &font) {
	if (!font.valid()) {
		return false;
	}
	// The command buffer is idle between presents, but the previous atlas
	// may still be referenced by the last submitted frame's descriptor --
	// the one-frame-in-flight model already waited for it (vkQueueWaitIdle
	// in VulkanPresenter's do_present()), so replacing it here is safe.
	font_texture_.destroy(dev_->device());
	font_ = UiFont();
	const int width = font.atlas_width();
	const int height = font.atlas_height();
	bool ok = create_sampled_texture(
		VK_FORMAT_R8_UNORM, (uint32_t)width, (uint32_t)height,
		[&](uint8_t *mapped, size_t row_pitch) {
			for (int y = 0; y < height; y++) {
				memcpy(mapped + (size_t)y * row_pitch, font.atlas().data() + (size_t)y * width,
					(size_t)width);
			}
		},
		&font_texture_, "font atlas");
	if (!ok) {
		return false;
	}
	dev_->write_texture_set(text_set_, font_texture_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	font_ = font;
	return true;
}

bool OverlayRenderer::create_cursor_pipeline() {
	// Positioned quad, plain sampler (no ycbcr), premultiplied-alpha
	// blended on top of everything else. The splash bitmap
	// (set_splash_bitmap()) is the exact same shape of draw -- one textured quad -- so it
	// shares this sampler/layout/pipeline, just via a second descriptor set
	// out of the same pool holding its own texture.
	VkDevice device = dev_->device();
	VkDescriptorSet sets[2] = {};
	if (!dev_->create_sampler(VK_FILTER_LINEAR, nullptr, &cursor_sampler_, "cursor/splash sampler") ||
		!dev_->create_texture_sets(cursor_sampler_, 2, &cursor_set_layout_, &cursor_descriptor_pool_, sets,
			"cursor/splash descriptor sets")) {
		return false;
	}
	cursor_set_ = sets[0];
	splash_set_ = sets[1];

	// cursor.vert's push_constant block: one vec4 rect.
	VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, 4 * sizeof(float)};
	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &cursor_set_layout_;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	if (!vk_check(vkCreatePipelineLayout(device, &layout_info, nullptr, &cursor_pipeline_layout_),
			"vkCreatePipelineLayout(cursor)")) {
		return false;
	}

	VulkanDevice::PipelineDesc desc;
	desc.vert_spv = cursor_vert_spv;
	desc.vert_words = sizeof(cursor_vert_spv) / sizeof(uint32_t);
	desc.frag_spv = cursor_frag_spv;
	desc.frag_words = sizeof(cursor_frag_spv) / sizeof(uint32_t);
	desc.layout = cursor_pipeline_layout_;
	desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; // same quad as the rect
	desc.premultiplied_blend = true;
	desc.what = "vkCreateGraphicsPipelines(cursor)";
	return dev_->create_pipeline(desc, &cursor_pipeline_);
}

bool OverlayRenderer::create_sampled_texture(VkFormat format, uint32_t width, uint32_t height,
	const std::function<void(uint8_t *mapped, size_t row_pitch)> &fill, OwnedImage *out, const char *what) {
	VkDevice device = dev_->device();
	auto fail = [&]() {
		out->destroy(device);
		return false;
	};

	// Host-visible linear image, written directly via a mapped pointer --
	// simpler than a staging buffer + transfer, and correctness matters
	// far more than upload speed for textures this small that change
	// rarely (the cursor) or never (the font atlas).
	if (!dev_->create_host_image(format, width, height, VK_IMAGE_LAYOUT_PREINITIALIZED, &out->image,
			&out->memory, what)) {
		return fail();
	}

	VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
	VkSubresourceLayout layout;
	vkGetImageSubresourceLayout(device, out->image, &subresource, &layout);

	void *mapped = nullptr;
	if (!vk_check(vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &mapped), what)) {
		return fail();
	}
	fill((uint8_t *)mapped + layout.offset, (size_t)layout.rowPitch);
	vkUnmapMemory(device, out->memory);

	// One-off layout transition on the shared command buffer (idle between
	// present() calls -- this is only ever invoked from init or the
	// stream session's main loop, never mid-frame).
	bool transitioned = dev_->submit_one_time([&](VkCommandBuffer cmd) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = out->image;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
			nullptr, 0, nullptr, 1, &barrier);
	});
	if (!transitioned) {
		return fail();
	}

	if (!dev_->create_image_view(out->image, format, nullptr, &out->view, what)) {
		return fail();
	}
	return true;
}

void OverlayRenderer::set_cursor_shape(uint32_t width, uint32_t height, int32_t hotspot_x, int32_t hotspot_y,
	const uint8_t *argb8888) {
	cursor_texture_.destroy(dev_->device());
	if (width == 0 || height == 0) {
		return;
	}
	// VK_FORMAT_B8G8R8A8_UNORM matches DRM_FORMAT_ARGB8888's byte order
	// exactly, so the rows are copied as-is.
	bool ok = create_sampled_texture(
		VK_FORMAT_B8G8R8A8_UNORM, width, height,
		[&](uint8_t *mapped, size_t row_pitch) {
			for (uint32_t y = 0; y < height; y++) {
				memcpy(mapped + (size_t)y * row_pitch, argb8888 + (size_t)y * width * 4, (size_t)width * 4);
			}
		},
		&cursor_texture_, "cursor texture");
	if (!ok) {
		return;
	}
	dev_->write_texture_set(cursor_set_, cursor_texture_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	cursor_width_ = width;
	cursor_height_ = height;
	cursor_hotspot_x_ = hotspot_x;
	cursor_hotspot_y_ = hotspot_y;
}

bool OverlayRenderer::set_splash_bitmap(uint32_t width, uint32_t height, const uint8_t *bgra8888) {
	// Set once (VulkanPresenter::init, before the pipeline's command buffer
	// has ever been used) and never replaced -- no need for the
	// destroy-then-recreate dance set_cursor_shape does per call.
	bool ok = create_sampled_texture(
		VK_FORMAT_B8G8R8A8_UNORM, width, height,
		[&](uint8_t *mapped, size_t row_pitch) {
			for (uint32_t y = 0; y < height; y++) {
				memcpy(mapped + (size_t)y * row_pitch, bgra8888 + (size_t)y * width * 4, (size_t)width * 4);
			}
		},
		&splash_texture_, "splash bitmap");
	if (!ok) {
		return false;
	}
	dev_->write_texture_set(splash_set_, splash_texture_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	splash_width_ = width;
	splash_height_ = height;
	return true;
}

void OverlayRenderer::set_cursor_position(float x_px, float y_px, bool visible) {
	cursor_pos_x_ = x_px;
	cursor_pos_y_ = y_px;
	cursor_visible_ = visible;
}

void OverlayRenderer::set_cursor_scale(float scale) {
	cursor_scale_ = scale > 0.0f ? scale : 1.0f;
}

void OverlayRenderer::record_text(VkCommandBuffer cmd, VkExtent2D extent, const UiTextPrim &text) {
	const float inv_w = 1.0f / (float)font_.atlas_width();
	const float inv_h = 1.0f / (float)font_.atlas_height();
	const float scale = text.scale;
	float pen = text.x;
	const float baseline = text.y + (float)font_.ascent() * scale;
	for (char c : text.text) {
		const UiFont::Glyph &g = font_.glyph(c);
		if (g.x1 > g.x0 && g.y1 > g.y0) {
			// Snap the quad origin to whole pixels (stb's align_to_integer)
			// so a glyph never straddles a half-texel and blurs.
			float qx = std::floor(pen + g.xoff * scale + 0.5f);
			float qy = std::floor(baseline + g.yoff * scale + 0.5f);
			TextPushConstants push{};
			pixel_rect_to_ndc(qx, qy, (g.xoff2 - g.xoff) * scale, (g.yoff2 - g.yoff) * scale, extent,
				push.rect);
			push.uv[0] = g.x0 * inv_w;
			push.uv[1] = g.y0 * inv_h;
			push.uv[2] = (g.x1 - g.x0) * inv_w;
			push.uv[3] = (g.y1 - g.y0) * inv_h;
			push.color[0] = text.color.r;
			push.color[1] = text.color.g;
			push.color[2] = text.color.b;
			push.color[3] = text.color.a;
			vkCmdPushConstants(cmd, text_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push),
				&push);
			vkCmdDraw(cmd, 4, 1, 0, 0);
		}
		pen += g.xadvance * scale;
	}
}

void OverlayRenderer::record_draw(VkCommandBuffer cmd, VkExtent2D extent) {
	if (!ui_.rects.empty()) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rect_pipeline_);
		for (const UiRectPrim &r : ui_.rects) {
			RectPushConstants push{};
			pixel_rect_to_ndc(r.x, r.y, r.w, r.h, extent, push.rect);
			// rect.frag writes the color straight through; the blend
			// state expects premultiplied alpha.
			push.color[0] = r.color.r * r.color.a;
			push.color[1] = r.color.g * r.color.a;
			push.color[2] = r.color.b * r.color.a;
			push.color[3] = r.color.a;
			vkCmdPushConstants(cmd, rect_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push),
				&push);
			vkCmdDraw(cmd, 4, 1, 0, 0);
		}
	}

	if (!ui_.texts.empty() && font_.valid()) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, text_pipeline_);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, text_pipeline_layout_, 0, 1, &text_set_,
			0, nullptr);
		for (const UiTextPrim &t : ui_.texts) {
			record_text(cmd, extent, t);
		}
	}

	if (cursor_texture_.view && cursor_visible_) {
		// Bitmap size and hotspot both scale (set_cursor_scale) so the tip
		// stays under the pointer; the position itself is already in
		// window pixels and is not scaled. Drawn last so the pointer stays
		// visible over the menu.
		float scale = cursor_scale_;
		float x_px = cursor_pos_x_ - (float)cursor_hotspot_x_ * scale;
		float y_px = cursor_pos_y_ - (float)cursor_hotspot_y_ * scale;
		float cursor_rect[4];
		pixel_rect_to_ndc(x_px, y_px, (float)cursor_width_ * scale, (float)cursor_height_ * scale, extent,
			cursor_rect);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cursor_pipeline_);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cursor_pipeline_layout_, 0, 1,
			&cursor_set_, 0, nullptr);
		vkCmdPushConstants(cmd, cursor_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(cursor_rect),
			cursor_rect);
		vkCmdDraw(cmd, 4, 1, 0, 0);
	}
}

void OverlayRenderer::record_splash(VkCommandBuffer cmd, VkExtent2D extent) {
	if (!splash_texture_.view) {
		return;
	}
	// Cap the drawn size to a fraction of the window so the bitmap doesn't
	// dwarf a small window, but never upscale past its own pixels (would
	// just look soft). The bitmap is 256px for headroom on a large or HiDPI
	// window (see client/spectre/CMakeLists.txt's "Splash bitmap" comment).
	float max_dim = std::min((float)extent.width, (float)extent.height) * 0.4f;
	float scale = std::min(1.0f, max_dim / (float)std::max(splash_width_, splash_height_));
	float w = (float)splash_width_ * scale;
	float h = (float)splash_height_ * scale;
	float x = ((float)extent.width - w) * 0.5f;
	float y = ((float)extent.height - h) * 0.5f;

	float rect[4];
	pixel_rect_to_ndc(x, y, w, h, extent, rect);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cursor_pipeline_);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cursor_pipeline_layout_, 0, 1, &splash_set_,
		0, nullptr);
	vkCmdPushConstants(cmd, cursor_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(rect), rect);
	vkCmdDraw(cmd, 4, 1, 0, 0);
}

} // namespace spectre
