// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "present/lossless_plane.hpp"

#include "cursor_frag_spv.h"
#include "cursor_vert_spv.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace spectre {

namespace {

// Tile pixels are wraith's DRM_FORMAT_XRGB8888 (memory order B,G,R,X) with
// the X byte dropped on the wire (gdp/refine.hpp). VK_FORMAT_B8G8R8A8_UNORM
// reads that byte order, so they go in with no swizzle, as
// OverlayRenderer's cursor texture does with wraith's ARGB8888 cursor
// images; unpacking adds an alpha of 0xff, which the shader blends with.
constexpr VkFormat kPlaneFormat = VK_FORMAT_B8G8R8A8_UNORM;

} // namespace

LosslessPlane::~LosslessPlane() {
	shutdown();
}

bool LosslessPlane::init(VulkanDevice &device) {
	dev_ = &device;
	VkDevice vk = dev_->device();

	// The cursor pipeline's exact shape -- positioned quad, plain sampler,
	// premultiplied-alpha blend -- so it reuses cursor.vert/cursor.frag
	// rather than carrying a second copy of the same two shaders. The
	// plane's alpha is only ever 0 or 255, where premultiplied and
	// straight alpha agree, so the shared blend state is correct for both.
	//
	// NEAREST, unlike the cursor's LINEAR: the whole point of this plane
	// is byte-exact pixels, and at any window size other than 1:1 a linear
	// filter would blur precisely what it exists to keep sharp.
	if (!dev_->create_sampler(VK_FILTER_NEAREST, nullptr, &sampler_, "lossless plane sampler") ||
		!dev_->create_texture_sets(sampler_, 1, &set_layout_, &descriptor_pool_, &set_,
			"lossless plane descriptor set")) {
		return false;
	}

	VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, 4 * sizeof(float)};
	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &set_layout_;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	if (!vk_check(vkCreatePipelineLayout(vk, &layout_info, nullptr, &pipeline_layout_),
			"vkCreatePipelineLayout(lossless plane)")) {
		return false;
	}

	VulkanDevice::PipelineDesc desc;
	desc.vert_spv = cursor_vert_spv;
	desc.vert_words = sizeof(cursor_vert_spv) / sizeof(uint32_t);
	desc.frag_spv = cursor_frag_spv;
	desc.frag_words = sizeof(cursor_frag_spv) / sizeof(uint32_t);
	desc.layout = pipeline_layout_;
	desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	desc.premultiplied_blend = true;
	desc.what = "vkCreateGraphicsPipelines(lossless plane)";
	return dev_->create_pipeline(desc, &pipeline_);
}

bool LosslessPlane::ensure_image(uint32_t width, uint32_t height) {
	if (texture_.image && width == width_ && height == height_) {
		return true;
	}
	destroy_image();
	if (width == 0 || height == 0) {
		return false;
	}

	VkDevice vk = dev_->device();
	if (!dev_->create_host_image(kPlaneFormat, width, height, VK_IMAGE_LAYOUT_PREINITIALIZED, &texture_.image,
			&texture_.memory, "lossless plane image")) {
		destroy_image();
		return false;
	}

	VkImageSubresource subresource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
	VkSubresourceLayout layout;
	vkGetImageSubresourceLayout(vk, texture_.image, &subresource, &layout);

	void *mapped = nullptr;
	if (!vk_check(vkMapMemory(vk, texture_.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
			"vkMapMemory(lossless plane)")) {
		destroy_image();
		return false;
	}
	mapped_ = (uint8_t *)mapped + layout.offset;
	row_pitch_ = (size_t)layout.rowPitch;
	width_ = width;
	height_ = height;
	clear();

	// PREINITIALIZED -> GENERAL once, and it stays there: the host keeps
	// writing tiles into this image between draws, which
	// SHADER_READ_ONLY_OPTIMAL doesn't allow. Per-frame visibility is the
	// host-write barrier in record_upload_barrier().
	bool transitioned = dev_->submit_one_time([&](VkCommandBuffer cmd) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = texture_.image;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
			nullptr, 0, nullptr, 1, &barrier);
	});
	if (!transitioned) {
		destroy_image();
		return false;
	}

	if (!dev_->create_image_view(texture_.image, kPlaneFormat, nullptr, &texture_.view,
			"lossless plane view")) {
		destroy_image();
		return false;
	}
	dev_->write_texture_set(set_, texture_.view, VK_IMAGE_LAYOUT_GENERAL);
	return true;
}

void LosslessPlane::clear() {
	if (!mapped_) {
		return;
	}
	memset(mapped_, 0, row_pitch_ * height_);
	has_content_ = false;
	dirty_ = true;
}

void LosslessPlane::apply(const gdp::RefineLayer &layer, uint32_t width, uint32_t height) {
	if (!dev_ || layer.empty()) {
		return;
	}
	if (!ensure_image(width, height)) {
		return;
	}

	if (layer.reset) {
		clear();
	}

	// Clipping, not rejecting: `layer` came off the wire, and a rect that
	// runs off the frame should cost the pixels it can't reach, nothing
	// more.
	auto clip = [&](const gdp::RefineRect &rect, uint32_t *w_out, uint32_t *h_out) {
		if (rect.x >= width_ || rect.y >= height_) {
			return false;
		}
		*w_out = std::min<uint32_t>(rect.width, width_ - rect.x);
		*h_out = std::min<uint32_t>(rect.height, height_ - rect.y);
		return *w_out > 0 && *h_out > 0;
	};

	for (const gdp::RefineRect &rect : layer.clears) {
		uint32_t w = 0, h = 0;
		if (!clip(rect, &w, &h)) {
			continue;
		}
		for (uint32_t row = 0; row < h; row++) {
			memset(mapped_ + (size_t)(rect.y + row) * row_pitch_ + (size_t)rect.x * 4, 0, (size_t)w * 4);
		}
		dirty_ = true;
	}

	// Tile pixels are concatenated in tile order, each tightly packed at
	// width*3 (gdp/refine.hpp). The parser already checked the total length
	// against the rects, so walking the two in step cannot run off the
	// end -- but the offset is still bounds-checked, since this is the
	// only thing standing between a wire payload and a write into the
	// mapped image.
	size_t offset = 0;
	for (const gdp::RefineRect &rect : layer.tiles) {
		size_t tile_bytes = (size_t)rect.width * rect.height * gdp::kRefineBytesPerPixel;
		if (offset + tile_bytes > layer.tile_pixels.size()) {
			break;
		}
		const uint8_t *src = layer.tile_pixels.data() + offset;
		offset += tile_bytes;

		uint32_t w = 0, h = 0;
		if (!clip(rect, &w, &h)) {
			continue;
		}
		for (uint32_t row = 0; row < h; row++) {
			uint8_t *dst = mapped_ + (size_t)(rect.y + row) * row_pitch_ + (size_t)rect.x * 4;
			// The wire drops the X byte; this plane's alpha is what decides
			// whether the viewer sees the tile or the video under it, and
			// unpack puts it back as opaque.
			gdp::refine_unpack_row(src + (size_t)row * rect.width * gdp::kRefineBytesPerPixel, dst, w);
		}
		has_content_ = true;
		dirty_ = true;
	}
}

void LosslessPlane::record_upload_barrier(VkCommandBuffer cmd) {
	if (!texture_.image || !dirty_) {
		return;
	}
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = texture_.image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
		nullptr, 0, nullptr, 1, &barrier);
	dirty_ = false;
}

void LosslessPlane::record_draw(VkCommandBuffer cmd) {
	if (!pipeline_ || !texture_.image || !has_content_) {
		return;
	}
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set_, 0, nullptr);
	// The whole clip volume: the video pipeline draws a fullscreen
	// triangle, so the plane covers exactly the same pixels.
	float rect[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
	vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(rect), rect);
	vkCmdDraw(cmd, 4, 1, 0, 0);
}

void LosslessPlane::destroy_image() {
	if (!dev_) {
		return;
	}
	VkDevice vk = dev_->device();
	if (mapped_) {
		vkUnmapMemory(vk, texture_.memory);
		mapped_ = nullptr;
	}
	texture_.destroy(vk);
	row_pitch_ = 0;
	width_ = 0;
	height_ = 0;
	has_content_ = false;
	dirty_ = false;
}

void LosslessPlane::shutdown() {
	if (!dev_) {
		return;
	}
	VkDevice vk = dev_->device();
	destroy_image();
	if (pipeline_) {
		vkDestroyPipeline(vk, pipeline_, nullptr);
		pipeline_ = VK_NULL_HANDLE;
	}
	if (pipeline_layout_) {
		vkDestroyPipelineLayout(vk, pipeline_layout_, nullptr);
		pipeline_layout_ = VK_NULL_HANDLE;
	}
	if (descriptor_pool_) {
		vkDestroyDescriptorPool(vk, descriptor_pool_, nullptr);
		descriptor_pool_ = VK_NULL_HANDLE;
		set_ = VK_NULL_HANDLE;
	}
	if (set_layout_) {
		vkDestroyDescriptorSetLayout(vk, set_layout_, nullptr);
		set_layout_ = VK_NULL_HANDLE;
	}
	if (sampler_) {
		vkDestroySampler(vk, sampler_, nullptr);
		sampler_ = VK_NULL_HANDLE;
	}
	dev_ = nullptr;
}

} // namespace spectre
