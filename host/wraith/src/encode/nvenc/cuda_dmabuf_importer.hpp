// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Gets a dmabuf's pixels into CUDA memory NVENC can encode from, without a
// trip through host memory -- the NVENC backend's zero-copy path.
//
// CUDA has no dmabuf import of its own. Vulkan does
// (VK_EXT_external_memory_dma_buf + VK_EXT_image_drm_format_modifier, as
// spectre's video_image_source.cpp uses the other way round), and CUDA can
// import memory Vulkan *allocated* (an OPAQUE_FD export, the documented
// Vulkan-CUDA interop). So: Vulkan-allocated, CUDA-mapped linear buffers
// (one per frame the encoder keeps in flight) live for the life of the
// encoder; each capture buffer is
// imported once as a VkImage (cached) and each frame copied into it on the GPU
// (vkCmdCopyImageToBuffer -- which is also what untiles NVIDIA's
// block-linear layouts), and NVENC reads that buffer as a CUDA device
// pointer. One GPU copy per frame, the same count VA-API's VPP pass costs.
//
// The copy preserves bytes, so the buffer holds the dmabuf's own packed
// 32-bit RGB layout and NVENC does the RGB -> YUV conversion.
//
// The EGL/GL route (import into a GL texture, cuGraphicsGLRegisterImage)
// would work too, and is what some NVIDIA capture tools use; this one
// needs no GL context, and Vulkan is already on every box wraith runs on.
#pragma once

#include "encode/encoder.hpp"
#include "encode/nvenc/nvenc_runtime.hpp"

#include <sys/stat.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace wraith {

class CudaDmabufImporter {
public:
	~CudaDmabufImporter();

	// Sets up Vulkan on the physical device that *is* `device` (matched by
	// UUID, the interop rule) and `buffers` shared width x height buffers. The
	// caller's CUDA context must be current. False, having logged why,
	// if any piece of it is missing -- the encoder then falls back to its
	// CPU upload path.
	bool init(CUdevice device, uint32_t width, uint32_t height, int buffers);

	// The modifiers a `drm_format` dmabuf can be imported and copied from
	// on this device (Encoder::supported_import_modifiers). Empty for a
	// format the copy can't take.
	std::vector<uint64_t> import_modifiers(uint32_t drm_format) const;

	// Copies `frame` into shared buffer `index` and waits for the copy to
	// finish, so the caller may encode from device_ptr(index) -- and release
	// the dmabuf -- as soon as this returns. The CUDA context must be
	// current. False on failure, logged.
	bool copy(const DmabufFrame &frame, int index);

	// Shared buffer `index`: `pitch` bytes per row, `height` rows, packed
	// 32-bit pixels in the byte order of the last frame copied into it.
	CUdeviceptr device_ptr(int index) const { return shared_[(size_t)index].cuda_ptr; }
	uint32_t pitch() const { return pitch_; }

private:
	bool create_instance_and_device(CUdevice device);
	struct Shared {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		CUexternalMemory cuda_memory = nullptr;
		CUdeviceptr cuda_ptr = 0;
	};
	bool create_shared_buffer(Shared *shared);
	void destroy();

	struct Import {
		dev_t dev = 0;
		ino_t ino = 0;
		uint32_t format = 0;
		uint64_t modifier = 0;
		uint32_t offset = 0, stride = 0;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint64_t last_used = 0;
	};
	// Capture pools are 3 (ext) to a handful (PipeWire) of buffers; the
	// rest of the room is for a renegotiation's leftovers.
	static constexpr size_t kMaxImports = 8;
	Import *find_import(const DmabufFrame &frame, const struct stat &st);
	void release_import(Import &import);

	// Waits (on the GPU) for whoever last wrote the dmabuf, via the
	// kernel's implicit-sync fences: NVIDIA's Vulkan driver does not
	// honour implicit sync on imported dmabufs by itself. False if the
	// kernel or driver can't express it; the copy then goes ahead
	// unsynchronised, as VA-API's import does.
	bool import_implicit_fence(int dmabuf_fd);

	uint32_t width_ = 0, height_ = 0, pitch_ = 0;

	VkInstance instance_ = VK_NULL_HANDLE;
	VkPhysicalDevice physical_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	uint32_t queue_family_ = 0;
	VkQueue queue_ = VK_NULL_HANDLE;
	VkCommandPool command_pool_ = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
	VkFence fence_ = VK_NULL_HANDLE;

	// VK_EXT_queue_family_foreign: acquire from FOREIGN (the dmabuf's
	// producer is another driver or process) rather than EXTERNAL.
	bool have_foreign_queue_ = false;
	// Binary semaphore the dmabuf's implicit fence is imported into (a
	// temporary SYNC_FD import, consumed by the submit that waits on it).
	// VK_NULL_HANDLE when the driver can't import sync files.
	VkSemaphore implicit_fence_ = VK_NULL_HANDLE;

	PFN_vkGetMemoryFdKHR get_memory_fd_ = nullptr;
	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties_ = nullptr;
	PFN_vkImportSemaphoreFdKHR import_semaphore_fd_ = nullptr;

	std::vector<Shared> shared_;

	std::vector<Import> imports_;
	uint64_t use_counter_ = 0;
};

} // namespace wraith
