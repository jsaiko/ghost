// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The "pyrowave" codec (gdp-spec.md §6.6, docs/design/pyrowave.md): PyroWave, an intra-only wavelet
// codec that runs entirely in Vulkan compute shaders -- well under a
// millisecond to encode a 4K frame on a desktop GPU, at several hundred
// Mbit/s. Built only when pkg-config finds
// libpyrowave-shared (packaging/build-pyrowave.sh).
//
// PyroWave runs on a VkDevice of wraith's own (borrowed through
// pyrowave_create_device), on the GPU the session's render node names, so
// wraith owns the queue every submission goes to. Frames go into one
// optimal BGRA image, and the encode -- PyroWave's "scaled" one, which does
// the RGB -> YCbCr conversion on the GPU too (full-range BT.709, 4:2:0) --
// runs on a worker thread. Two ways in:
//
// - push(): a dmabuf, imported as a VkImage (VK_EXT_image_drm_format_modifier,
//   cached per buffer) and copied into that image on the GPU. The copy
//   waits on the buffer's own fences (DMA_BUF_IOCTL_EXPORT_SYNC_FILE: the
//   compositor may still be drawing it) and push() waits for the copy, so
//   the buffer is the caller's again when it returns. Zero-copy as far as
//   the CPU is concerned, which matters most on a VM whose GPU reads back
//   slowly.
// - push_cpu(): XRGB8888 rows, copied into a persistent host-visible
//   staging buffer and uploaded by the worker. For hosts without the
//   import extensions, and for good after an import fails
//   (wants_cpu_frame() turns true, as NVENC's does). PyroWave's own CPU
//   entry point would create and upload three new images every frame,
//   which is far slower.
//
// Every frame stands alone, so every packet is a keyframe and loss needs
// no repair; the packet is PyroWave's whole bitstream for the frame, its
// sequence header first. Rate control is PyroWave's: each frame gets at
// most the set_bitrate() rate's share of one frame interval.
#pragma once

#include "encode/encoder.hpp"

#include <vulkan/vulkan.h>
// After vulkan.h, which it checks for.
#include <pyrowave.h>

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace wraith {

class PyrowaveEncoder : public Encoder {
public:
	~PyrowaveEncoder() override;

	bool open(const EncoderConfig &config) override;
	bool wants_cpu_frame() const override { return !dmabuf_import_ || import_failed_; }
	bool push(const DmabufFrame &frame, int64_t pts_us) override;
	std::vector<uint64_t> supported_import_modifiers(uint32_t drm_format) const override;
	bool push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride, int64_t pts_us,
		const DamageRegion *damage) override;
	// Every frame is already a keyframe.
	void request_keyframe() override {}
	void set_bitrate(uint32_t bitrate_bps) override;
	void set_asynchronous(bool on) override { asynchronous_ = on; }
	int completion_fd() const override { return wake_fd_; }
	std::vector<EncodedPacket> poll() override;
	void close() override;

	// For tests: the encoder's input image as it is now -- the last frame
	// pushed, either way -- as tightly packed B8G8R8A8 rows. Only on a
	// device that imports dmabufs; waits for the worker and the GPU.
	bool read_back_input(std::vector<uint8_t> *out);

private:
	bool create_device(int drm_fd);
	bool create_resources();
	// The DRM modifiers a B8G8R8A8 dmabuf of the session's size imports
	// with as a copy source, into import_modifiers_.
	void query_import_modifiers();
	// `slot`'s staging buffer to the image (kSlotImported: push() already
	// filled it), encodes it and appends the packet to `out`. Worker
	// thread (or the caller's, synchronously).
	bool encode_slot(int slot, int64_t pts_us, std::vector<EncodedPacket> *out);
	static constexpr int kSlotImported = 2;
	// encode_slot()'s halves: the staging upload (its timeline value out)
	// and the encode of the image once that value is reached.
	bool upload_slot(int slot, uint64_t *signal_value_out);
	bool encode_image(uint64_t signal_value, int64_t pts_us, std::vector<EncodedPacket> *out);
	// Hands `slot` to the worker, or encodes it here when synchronous.
	bool submit_job(int slot, int64_t pts_us, int64_t copy_us);
	// Waits until the worker has nothing in flight; false if stopping.
	bool wait_worker_idle();
	void worker_loop();
	void note_timing(int64_t copy_us, int64_t encode_us, size_t bytes);
	void destroy();

	// push()'s dmabuf imports, one per capture buffer (keyed by the
	// dmabuf's inode, which is the buffer's for as long as it lives).
	struct Import {
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		ino_t inode = 0;
		int32_t width = 0, height = 0;
		uint64_t modifier = 0;
		uint32_t offset[DmabufFrame::kMaxPlanes] = {};
		uint32_t stride[DmabufFrame::kMaxPlanes] = {};
		int n_planes = 0;
		uint64_t last_used = 0;
	};
	Import *find_import(const DmabufFrame &frame);
	bool import_dmabuf(const DmabufFrame &frame, ino_t inode, Import *out);
	void release_import(Import &import);
	// Copies `import` into image_ on the GPU after `frame`'s pending
	// writes, and waits for it.
	bool copy_imported(const DmabufFrame &frame, const Import &import);

	EncoderConfig config_;
	std::atomic<uint32_t> bitrate_bps_{0};

	// wraith's own device, and the one queue PyroWave and the upload share.
	VkInstance instance_ = VK_NULL_HANDLE;
	VkPhysicalDevice physical_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	uint32_t queue_family_ = 0;
	VkQueue queue_ = VK_NULL_HANDLE;
	// What pyrowave_create_device must be able to read for the device's
	// whole life: the create infos and the feature chain behind them.
	VkApplicationInfo app_info_{};
	VkInstanceCreateInfo instance_info_{};
	VkDeviceQueueCreateInfo queue_info_{};
	float queue_priority_ = 1.0f;
	VkPhysicalDeviceVulkan13Features features13_{};
	VkPhysicalDeviceVulkan12Features features12_{};
	VkPhysicalDeviceVulkan11Features features11_{};
	VkPhysicalDeviceFeatures2 features_{};
	VkDeviceCreateInfo device_info_{};
	pyrowave_device_create_queue_info pyro_queue_{};

	pyrowave_device pyro_device_ = nullptr;
	pyrowave_encoder pyro_encoder_ = nullptr;

	VkCommandPool command_pool_ = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
	// Signalled by the upload's submit, waited on by PyroWave's encode.
	VkSemaphore upload_done_ = VK_NULL_HANDLE;
	uint64_t upload_value_ = 0;
	VkImage image_ = VK_NULL_HANDLE;
	VkDeviceMemory image_memory_ = VK_NULL_HANDLE;
	// Two staging buffers: push_cpu() fills one while the worker uploads
	// the other.
	struct Staging {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		uint8_t *mapped = nullptr;
	};
	Staging staging_[2];
	uint32_t pitch_ = 0;
	std::vector<uint8_t> bitstream_;

	// dmabuf input (push()): the device has the extensions, and no import
	// has failed yet.
	bool dmabuf_import_ = false;
	bool import_failed_ = false;
	std::vector<const char *> device_extensions_;
	std::vector<uint64_t> import_modifiers_;
	std::vector<uint32_t> import_plane_counts_; // per import_modifiers_ entry
	std::vector<Import> imports_;
	uint64_t import_clock_ = 0;
	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties_ = nullptr;
	PFN_vkImportSemaphoreFdKHR import_semaphore_fd_ = nullptr;
	VkCommandBuffer copy_command_buffer_ = VK_NULL_HANDLE;
	VkFence copy_done_ = VK_NULL_HANDLE;
	// Holds the frame's pending writes (a sync file, imported temporarily).
	VkSemaphore frame_ready_ = VK_NULL_HANDLE;

	// Asynchronous mode, as X264Encoder's: at most one frame in flight.
	bool asynchronous_ = false;
	int next_slot_ = 0;
	std::thread worker_;
	std::mutex mutex_; // guards the job fields and ready_
	std::condition_variable cv_;
	int job_slot_ = -1;
	int64_t job_pts_us_ = 0;
	int64_t job_copy_us_ = 0;
	bool stop_ = false;
	std::vector<EncodedPacket> ready_;
	int wake_fd_ = -1;

	struct Timing {
		int64_t window_start_us = 0;
		uint32_t frames = 0;
		int64_t copy_sum_us = 0, copy_max_us = 0;
		int64_t encode_sum_us = 0, encode_max_us = 0;
		uint64_t bytes = 0;
	} timing_;
};

} // namespace wraith
