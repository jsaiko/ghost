// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The "pyrowave" codec's decode (gdp-spec.md §6.6; wraith's side is
// host/wraith/src/encode/pyrowave/): PyroWave's compute decode on the
// presenter's own VkDevice (VulkanDevice::compute_device(), borrowed
// through pyrowave_create_device()), straight into a frame the presenter
// draws -- nothing leaves the GPU.
//
// Frames come from an FFmpeg Vulkan frames pool (AV_PIX_FMT_VULKAN), so
// they ride the same path as Vulkan Video's: refcounted AVFrames,
// VideoImageSource::acquire_vulkan_frame(), FFmpeg's timeline-semaphore and
// layout protocol. The images are one 3-plane 4:2:0 image each
// (VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM), mutable so PyroWave can write each
// plane through an R8 storage view, sampled whole through VideoImageSource's
// I420 ycbcr sampler.
//
// PyroWave records into a command buffer of ours (pyrowave_device_set_
// command_buffer) between our own layout barriers, and we submit it on the
// presenter's queue -- decode() runs on the same thread as present(), so no
// queue lock is needed (as with FFmpeg's Vulkan decode, decoder.cpp).
//
// Each payload is one whole frame (GDP reassembles; a frame missing a
// datagram never reaches the decoder), PyroWave's sequence header first,
// which carries the frame size: the decoder and pool follow it.
#pragma once

#include <vulkan/vulkan.h>
// After vulkan.h, which it checks for.
#include <pyrowave.h>

#include <cstddef>
#include <cstdint>

struct AVBufferRef;
struct AVFrame;

namespace spectre {

struct VulkanDecodeDevice;

class PyrowaveDecode {
public:
	PyrowaveDecode() = default;
	~PyrowaveDecode();
	PyrowaveDecode(const PyrowaveDecode &) = delete;
	PyrowaveDecode &operator=(const PyrowaveDecode &) = delete;

	// `device` must outlive this object. False, logged, if PyroWave or
	// FFmpeg won't take the device.
	bool open(const VulkanDecodeDevice &device);
	// Decodes one frame's bitstream into a new pool frame, referenced by
	// `out` (which must be empty). False for a bitstream that doesn't parse
	// or isn't a whole frame -- the caller reports the frame lost.
	bool decode(const uint8_t *data, size_t len, AVFrame *out);
	void close();

private:
	bool set_size(uint32_t width, uint32_t height);
	bool open_stage(uint32_t width, uint32_t height);
	void destroy_sized();

	const VulkanDecodeDevice *dev_ = nullptr;
	pyrowave_device_create_queue_info queue_{};
	pyrowave_device pyro_device_ = nullptr;
	pyrowave_decoder pyro_decoder_ = nullptr;
	AVBufferRef *hw_device_ = nullptr;
	AVBufferRef *hw_frames_ = nullptr;
	uint32_t width_ = 0, height_ = 0;

	// The staged path, for a GPU that can't write the planes of the output
	// image in place (VulkanDevice::pyrowave_writes_planes_directly()):
	// PyroWave decodes into three R8 images -- luma size, then chroma at
	// half -- and the result is copied into the output frame's planes. One
	// set of three per command slot, kept for the size's life.
	bool staged_ = false;
	AVBufferRef *stage_luma_frames_ = nullptr;
	AVBufferRef *stage_chroma_frames_ = nullptr;
	AVFrame *stage_[2][3] = {};

	// Two command buffers, each fenced, so recording the next frame never
	// waits on the GPU finishing the last.
	static constexpr int kSlots = 2; // stage_'s first dimension too
	VkCommandPool pool_ = VK_NULL_HANDLE;
	VkCommandBuffer cmd_[kSlots] = {};
	VkFence fence_[kSlots] = {};
	int slot_ = 0;
};

} // namespace spectre
