// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "decode/pyrowave_decode.hpp"

#include "present/vulkan_device.hpp"
#include "log.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
}

#include <cstdlib>
#include <cstring>

namespace spectre {

namespace {

constexpr VkFormat kI420Format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;

// A layout change on the whole image, ranges and queues left alone.
void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
	VkPipelineStageFlags src_stage, VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
	VkAccessFlags dst_access) {
	VkImageMemoryBarrier b{};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool pyro_ok(pyrowave_result result, const char *what) {
	if (result != PYROWAVE_SUCCESS) {
		SLOG_ERROR("pyrowave: %s failed (%d)", what, (int)result);
		return false;
	}
	return true;
}

// PyroWave's BitstreamSequenceHeader (its bitstream.md), the first 8 bytes
// of every frame wraith sends: width-1 in bits 0-13 of the first word,
// height-1 in bits 14-27, the "extended" flag in bit 31, and the extended
// code (0, start of frame) in bits 24-25 of the second.
bool frame_size(const uint8_t *data, size_t len, uint32_t *width, uint32_t *height) {
	if (len < 8) {
		return false;
	}
	uint32_t w0, w1;
	memcpy(&w0, data, 4);
	memcpy(&w1, data + 4, 4);
	if (!(w0 >> 31) || ((w1 >> 24) & 3) != 0) {
		return false;
	}
	*width = (w0 & 0x3fff) + 1;
	*height = ((w0 >> 14) & 0x3fff) + 1;
	return true;
}

} // namespace

PyrowaveDecode::~PyrowaveDecode() {
	close();
}

bool PyrowaveDecode::open(const VulkanDecodeDevice &device) {
	dev_ = &device;

	queue_.queue = device.graphics_queue;
	queue_.familyIndex = device.graphics_family;
	queue_.index = 0;
	pyrowave_device_create_info info = {};
	info.GetInstanceProcAddr = device.get_instance_proc_addr;
	info.instance = device.instance;
	info.physical_device = device.physical_device;
	info.device = device.device;
	info.instance_create_info = device.instance_create_info;
	info.device_create_info = device.device_create_info;
	info.queue_info = &queue_;
	info.queue_info_count = 1;
	if (!pyro_ok(pyrowave_create_device(&info, &pyro_device_), "pyrowave_create_device")) {
		pyro_device_ = nullptr;
		return false;
	}
	pyrowave_device_set_queue_type(pyro_device_, VK_QUEUE_GRAPHICS_BIT);
	// SPECTRE_PYROWAVE_STAGED=1 takes the staged path on a GPU that doesn't
	// need it, so it can be tried where the direct one also works.
	const char *force_staged = getenv("SPECTRE_PYROWAVE_STAGED");
	staged_ = (force_staged && *force_staged == '1') ||
		!VulkanDevice::pyrowave_writes_planes_directly(device.physical_device);
	if (staged_) {
		SLOG_INFO("pyrowave: writing through R8 images and copying into the frame's planes");
	}

	// FFmpeg's view of the same device, for its frame pool: just the
	// graphics queue (no decode queue is involved).
	hw_device_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
	if (!hw_device_) {
		SLOG_ERROR("pyrowave: this FFmpeg has no Vulkan hwcontext");
		return false;
	}
	auto *device_ctx = (AVHWDeviceContext *)hw_device_->data;
	auto *vk_ctx = (AVVulkanDeviceContext *)device_ctx->hwctx;
	vk_ctx->get_proc_addr = device.get_instance_proc_addr;
	vk_ctx->inst = device.instance;
	vk_ctx->phys_dev = device.physical_device;
	vk_ctx->act_dev = device.device;
	vk_ctx->device_features = *device.features;
	vk_ctx->enabled_inst_extensions = device.instance_extensions;
	vk_ctx->nb_enabled_inst_extensions = device.instance_extension_count;
	vk_ctx->enabled_dev_extensions = device.device_extensions;
	vk_ctx->nb_enabled_dev_extensions = device.device_extension_count;
	vk_ctx->qf[0].idx = (int)device.graphics_family;
	vk_ctx->qf[0].num = 1;
	vk_ctx->qf[0].flags = (VkQueueFlagBits)device.graphics_flags;
	vk_ctx->nb_qf = 1;
	int ret = av_hwdevice_ctx_init(hw_device_);
	if (ret < 0) {
		char errbuf[64];
		av_strerror(ret, errbuf, sizeof(errbuf));
		SLOG_ERROR("pyrowave: av_hwdevice_ctx_init(Vulkan) failed: %s", errbuf);
		return false;
	}

	VkCommandPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = device.graphics_family;
	if (!vk_check(vkCreateCommandPool(device.device, &pool_info, nullptr, &pool_),
			"vkCreateCommandPool(pyrowave)")) {
		pool_ = VK_NULL_HANDLE;
		return false;
	}
	VkCommandBufferAllocateInfo cb_info{};
	cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cb_info.commandPool = pool_;
	cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cb_info.commandBufferCount = kSlots;
	if (!vk_check(vkAllocateCommandBuffers(device.device, &cb_info, cmd_),
			"vkAllocateCommandBuffers(pyrowave)")) {
		return false;
	}
	VkFenceCreateInfo fence_info{};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (VkFence &fence : fence_) {
		if (!vk_check(vkCreateFence(device.device, &fence_info, nullptr, &fence),
				"vkCreateFence(pyrowave)")) {
			fence = VK_NULL_HANDLE;
			return false;
		}
	}
	return true;
}

bool PyrowaveDecode::set_size(uint32_t width, uint32_t height) {
	if (width == width_ && height == height_ && pyro_decoder_) {
		return true;
	}
	destroy_sized();
	if (width % 2 != 0 || height % 2 != 0) {
		SLOG_ERROR("pyrowave: %ux%u frame isn't even-sized, as 4:2:0 needs", width, height);
		return false;
	}

	pyrowave_decoder_create_info info = {};
	info.device = pyro_device_;
	info.width = (int)width;
	info.height = (int)height;
	info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	// The fragment path is for mobile GPUs with weak compute.
	info.fragment_path = false;
	if (!pyro_ok(pyrowave_decoder_create(&info, &pyro_decoder_), "pyrowave_decoder_create")) {
		pyro_decoder_ = nullptr;
		return false;
	}

	hw_frames_ = av_hwframe_ctx_alloc(hw_device_);
	if (!hw_frames_) {
		return false;
	}
	auto *frames = (AVHWFramesContext *)hw_frames_->data;
	frames->format = AV_PIX_FMT_VULKAN;
	frames->sw_format = AV_PIX_FMT_YUV420P;
	frames->width = (int)width;
	frames->height = (int)height;
	auto *vk_frames = (AVVulkanFramesContext *)frames->hwctx;
	vk_frames->tiling = VK_IMAGE_TILING_OPTIMAL;
	vk_frames->format[0] = kI420Format;
	if (staged_) {
		// Only sampled, and copied into.
		vk_frames->usage =
			(VkImageUsageFlagBits)(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	} else {
		// Storage only through the per-plane R8 views PyroWave writes, which is
		// what MUTABLE_FORMAT + EXTENDED_USAGE allow.
		vk_frames->usage = (VkImageUsageFlagBits)(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
		vk_frames->img_flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	}
	int ret = av_hwframe_ctx_init(hw_frames_);
	if (ret < 0) {
		char errbuf[64];
		av_strerror(ret, errbuf, sizeof(errbuf));
		SLOG_ERROR("pyrowave: av_hwframe_ctx_init(%ux%u I420) failed: %s", width, height, errbuf);
		return false;
	}
	if (staged_ && !open_stage(width, height)) {
		return false;
	}
	width_ = width;
	height_ = height;
	SLOG_INFO("pyrowave: decoding %ux%u", width, height);
	return true;
}

// The staged path's three R8 images per slot, from FFmpeg's pool like every
// other frame here. Written by PyroWave and read by the copy, never handed
// on, so FFmpeg's own semaphore for them goes unused.
bool PyrowaveDecode::open_stage(uint32_t width, uint32_t height) {
	AVBufferRef **contexts[2] = {&stage_luma_frames_, &stage_chroma_frames_};
	const uint32_t sizes[2][2] = {{width, height}, {width / 2, height / 2}};
	for (int i = 0; i < 2; i++) {
		*contexts[i] = av_hwframe_ctx_alloc(hw_device_);
		if (!*contexts[i]) {
			return false;
		}
		auto *frames = (AVHWFramesContext *)(*contexts[i])->data;
		frames->format = AV_PIX_FMT_VULKAN;
		frames->sw_format = AV_PIX_FMT_GRAY8; // VK_FORMAT_R8_UNORM
		frames->width = (int)sizes[i][0];
		frames->height = (int)sizes[i][1];
		auto *vk_frames = (AVVulkanFramesContext *)frames->hwctx;
		vk_frames->tiling = VK_IMAGE_TILING_OPTIMAL;
		vk_frames->usage =
			(VkImageUsageFlagBits)(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		int ret = av_hwframe_ctx_init(*contexts[i]);
		if (ret < 0) {
			char errbuf[64];
			av_strerror(ret, errbuf, sizeof(errbuf));
			SLOG_ERROR("pyrowave: av_hwframe_ctx_init(%ux%u R8) failed: %s", sizes[i][0], sizes[i][1],
				errbuf);
			return false;
		}
	}
	for (auto &slot : stage_) {
		for (int plane = 0; plane < 3; plane++) {
			slot[plane] = av_frame_alloc();
			AVBufferRef *pool = plane == 0 ? stage_luma_frames_ : stage_chroma_frames_;
			if (!slot[plane] || av_hwframe_get_buffer(pool, slot[plane], 0) < 0) {
				SLOG_ERROR("pyrowave: no staging image for plane %d", plane);
				return false;
			}
		}
	}
	return true;
}

bool PyrowaveDecode::decode(const uint8_t *data, size_t len, AVFrame *out) {
	uint32_t width, height;
	if (!frame_size(data, len, &width, &height)) {
		SLOG_ERROR("pyrowave: payload (%zu bytes) doesn't start with a sequence header", len);
		return false;
	}
	if (!set_size(width, height)) {
		return false;
	}
	// Every payload is a whole frame: forget whatever the last one left
	// (PyroWave would otherwise drop a frame whose 3-bit sequence number
	// looks older than the last one, which a run of lost frames can cause).
	pyrowave_decoder_clear(pyro_decoder_);
	if (!pyro_ok(pyrowave_decoder_push_packet(pyro_decoder_, data, len), "push_packet")) {
		return false;
	}
	if (!pyrowave_decoder_decode_is_ready(pyro_decoder_, false)) {
		SLOG_ERROR("pyrowave: frame (%zu bytes) is incomplete", len);
		return false;
	}

	int ret = av_hwframe_get_buffer(hw_frames_, out, 0);
	if (ret < 0) {
		SLOG_ERROR("pyrowave: no frame from the pool (%d)", ret);
		return false;
	}
	auto *frames = (AVHWFramesContext *)hw_frames_->data;
	auto *vk_frames = (AVVulkanFramesContext *)frames->hwctx;
	auto *vkf = (AVVkFrame *)out->data[0];
	VkDevice device = dev_->device;

	const int slot = slot_;
	VkCommandBuffer cmd = cmd_[slot];
	VkFence fence = fence_[slot];
	slot_ = (slot_ + 1) % kSlots;
	vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
	vkResetFences(device, 1, &fence);
	vkResetCommandBuffer(cmd, 0);
	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &begin);

	vk_frames->lock_frame(frames, vkf);

	bool ok = false;
	if (staged_) {
		// Staged: PyroWave writes the three R8 images (GENERAL, for its storage
		// writes), which are then copied into the planes of the output frame.
		// Whatever any of them last held is overwritten whole.
		VkImage stage_images[3];
		for (int plane = 0; plane < 3; plane++) {
			stage_images[plane] = ((AVVkFrame *)stage_[slot][plane]->data[0])->img[0];
			barrier(cmd, stage_images[plane], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
		}
		barrier(cmd, vkf->img[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT);

		pyrowave_gpu_buffers buffers = {};
		for (int plane = 0; plane < 3; plane++) {
			pyrowave_image_view &view = buffers.planes[plane];
			view.image = stage_images[plane];
			// The luma size for every plane: PyroWave halves it for chroma.
			view.width = width;
			view.height = height;
			view.image_format = VK_FORMAT_R8_UNORM;
			view.view_format = VK_FORMAT_R8_UNORM;
			view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			view.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
			view.layout = VK_IMAGE_LAYOUT_GENERAL;
		}
		pyrowave_device_set_command_buffer(pyro_device_, cmd);
		ok = pyro_ok(pyrowave_decoder_decode_gpu_buffer(pyro_decoder_, nullptr, nullptr, &buffers), "decode");
		pyrowave_device_set_command_buffer(pyro_device_, VK_NULL_HANDLE);

		static const VkImageAspectFlags kPlaneAspects[3] = {VK_IMAGE_ASPECT_PLANE_0_BIT,
			VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_ASPECT_PLANE_2_BIT};
		for (int plane = 0; plane < 3; plane++) {
			barrier(cmd, stage_images[plane], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			VkImageCopy copy{};
			copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.dstSubresource = {kPlaneAspects[plane], 0, 0, 1};
			// A plane's own size: chroma is half the luma in each direction.
			copy.extent = {plane == 0 ? width : width / 2, plane == 0 ? height : height / 2, 1};
			vkCmdCopyImage(cmd, stage_images[plane], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vkf->img[0],
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		}
		// Done writing: ready for the presenter's fragment shader.
		barrier(cmd, vkf->img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	} else {
		// Whatever the image last held is overwritten whole: GENERAL, which is
		// what PyroWave's storage writes need.
		VkImageMemoryBarrier to_general{};
		to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		to_general.srcAccessMask = 0;
		to_general.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		to_general.image = vkf->img[0];
		to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
			0, nullptr, 0, nullptr, 1, &to_general);

		pyrowave_gpu_buffers buffers = {};
		static const VkImageAspectFlagBits kPlanes[3] = {VK_IMAGE_ASPECT_PLANE_0_BIT,
			VK_IMAGE_ASPECT_PLANE_1_BIT, VK_IMAGE_ASPECT_PLANE_2_BIT};
		for (int i = 0; i < 3; i++) {
			pyrowave_image_view &view = buffers.planes[i];
			view.image = vkf->img[0];
			// The luma size for every plane: PyroWave halves it for chroma.
			view.width = width;
			view.height = height;
			view.image_format = kI420Format;
			view.view_format = VK_FORMAT_R8_UNORM;
			view.aspect = kPlanes[i];
			view.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
			view.layout = VK_IMAGE_LAYOUT_GENERAL;
		}
		pyrowave_device_set_command_buffer(pyro_device_, cmd);
		ok = pyro_ok(pyrowave_decoder_decode_gpu_buffer(pyro_decoder_, nullptr, nullptr, &buffers), "decode");
		pyrowave_device_set_command_buffer(pyro_device_, VK_NULL_HANDLE);

		// Done writing: ready for the presenter's fragment shader.
		VkImageMemoryBarrier to_read = to_general;
		to_read.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		to_read.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &to_read);
	}
	vkEndCommandBuffer(cmd);

	// FFmpeg's frame protocol: wait for the image's timeline semaphore at
	// its current value, signal the next.
	uint64_t wait_value = vkf->sem_value[0];
	uint64_t signal_value = wait_value + 1;
	VkTimelineSemaphoreSubmitInfo timeline{};
	timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	timeline.waitSemaphoreValueCount = 1;
	timeline.pWaitSemaphoreValues = &wait_value;
	timeline.signalSemaphoreValueCount = 1;
	timeline.pSignalSemaphoreValues = &signal_value;
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.pNext = &timeline;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &vkf->sem[0];
	submit.pWaitDstStageMask = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &vkf->sem[0];
	if (ok) {
		ok = vk_check(vkQueueSubmit(dev_->graphics_queue, 1, &submit, fence), "vkQueueSubmit(pyrowave)");
	}
	if (ok) {
		vkf->sem_value[0] = signal_value;
		vkf->layout[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkf->access[0] = VK_ACCESS_SHADER_READ_BIT;
	} else {
		// Nothing was queued: the fence must still end up signalled for
		// the next wait on this slot.
		VkSubmitInfo empty{};
		empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		vkQueueSubmit(dev_->graphics_queue, 1, &empty, fence);
	}
	vk_frames->unlock_frame(frames, vkf);
	if (!ok) {
		av_frame_unref(out);
		return false;
	}
	out->color_range = AVCOL_RANGE_JPEG;
	out->colorspace = AVCOL_SPC_BT709;
	out->color_primaries = AVCOL_PRI_BT709;
	out->color_trc = AVCOL_TRC_BT709;
	return true;
}

void PyrowaveDecode::destroy_sized() {
	if (dev_ && pool_) {
		vkWaitForFences(dev_->device, kSlots, fence_, VK_TRUE, UINT64_MAX);
	}
	if (pyro_decoder_) {
		pyrowave_decoder_destroy(pyro_decoder_);
		pyro_decoder_ = nullptr;
	}
	// The fences above are why this is safe: nothing still reads these.
	for (auto &slot : stage_) {
		for (AVFrame *&frame : slot) {
			av_frame_free(&frame);
		}
	}
	av_buffer_unref(&stage_luma_frames_);
	av_buffer_unref(&stage_chroma_frames_);
	// Frames still held elsewhere (the presenter's last picture) keep the
	// pool alive through their own references.
	av_buffer_unref(&hw_frames_);
	width_ = height_ = 0;
}

void PyrowaveDecode::close() {
	if (!dev_) {
		return;
	}
	destroy_sized();
	for (VkFence &fence : fence_) {
		if (fence) {
			vkDestroyFence(dev_->device, fence, nullptr);
			fence = VK_NULL_HANDLE;
		}
	}
	if (pool_) {
		vkDestroyCommandPool(dev_->device, pool_, nullptr);
		pool_ = VK_NULL_HANDLE;
	}
	av_buffer_unref(&hw_device_);
	if (pyro_device_) {
		pyrowave_device_destroy(pyro_device_);
		pyro_device_ = nullptr;
	}
	dev_ = nullptr;
}

} // namespace spectre
