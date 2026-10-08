// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The Vulkan plumbing every presenter component shares: instance, surface,
// the physical device (pinned to the decoder's DRM render node on Linux),
// logical device + graphics/present queue, the swapchain, and the single
// command buffer + semaphore pair the one-frame-in-flight present loop
// uses. Also the small helpers (memory-type lookup, host-visible linear
// images, samplers, texture descriptor sets, shader modules, graphics
// pipelines) that VideoImageSource, LosslessPlane, OverlayRenderer and
// VulkanPresenter would otherwise each reimplement. Knows nothing about
// video frames, cursors, or UI -- see vulkan_presenter.hpp for how the
// pieces fit together.
#pragma once

#include "log.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace spectre {

// What FFmpeg's Vulkan hwcontext (AVVulkanDeviceContext) needs to decode on
// VulkanDevice's own device -- the Vulkan Video decode path (decoder.hpp).
// Every pointer refers to storage VulkanDevice owns, valid for its life.
struct VulkanDecodeDevice {
	PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	// The feature chain the device was created with.
	const VkPhysicalDeviceFeatures2 *features = nullptr;
	const char *const *instance_extensions = nullptr;
	int instance_extension_count = 0;
	const char *const *device_extensions = nullptr;
	int device_extension_count = 0;
	// The presenter's graphics queue family (FFmpeg also wants it for its
	// compute/transfer work -- frame layout setup) with its capability
	// flags, and the family holding the one decode queue. May be the same.
	uint32_t graphics_family = 0;
	VkQueueFlags graphics_flags = 0;
	uint32_t decode_family = 0;
	// The codecs that queue decodes, narrowed to the ones whose
	// VK_KHR_video_decode_* extension got enabled.
	VkVideoCodecOperationFlagsKHR decode_ops = 0;
	// The presenter's queue on graphics_family, and the create infos the
	// instance and device were made from -- what PyroWave's borrowed-device
	// API reads (pyrowave_decode.hpp); FFmpeg doesn't need them.
	VkQueue graphics_queue = VK_NULL_HANDLE;
	const VkInstanceCreateInfo *instance_create_info = nullptr;
	const VkDeviceCreateInfo *device_create_info = nullptr;
};

// Logs `what` and returns false on any VkResult other than VK_SUCCESS.
inline bool vk_check(VkResult r, const char *what) {
	if (r != VK_SUCCESS) {
		SLOG_ERROR("vulkan_device: %s failed (VkResult=%d)", what, (int)r);
		return false;
	}
	return true;
}

// A 2D image with its own dedicated memory and one view -- every texture
// spectre creates or imports whole (as opposed to the per-plane dmabuf
// import) is one of these. destroy() is a no-op on an empty one.
struct OwnedImage {
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;

	void destroy(VkDevice device) {
		if (view) vkDestroyImageView(device, view, nullptr);
		if (image) vkDestroyImage(device, image, nullptr);
		if (memory) vkFreeMemory(device, memory, nullptr);
		*this = OwnedImage{};
	}
};

class VulkanDevice {
public:
	VulkanDevice() = default;
	~VulkanDevice();
	VulkanDevice(const VulkanDevice &) = delete;
	VulkanDevice &operator=(const VulkanDevice &) = delete;

	// `drm_render_node` must be the same node Decoder::open() uses -- on
	// Linux the physical device is picked by matching this against each
	// Vulkan device's VK_EXT_physical_device_drm render node (major/minor of
	// the node's stat(), compared to the reported renderMajor/renderMinor).
	// Ignored elsewhere. On macOS the device is MoltenVK's, reached through
	// the loader's portability enumeration (portability_instance_setup()).
	// Creates everything through the swapchain, command buffer, and
	// semaphores; on failure the destructor cleans up whatever got made.
	bool init(SDL_Window *window, const char *drm_render_node);

	VkPhysicalDevice physical_device() const { return physical_device_; }
	VkDevice device() const { return device_; }
	VkQueue queue() const { return queue_; }
	uint32_t queue_family_index() const { return queue_family_index_; }
	VkCommandBuffer command_buffer() const { return command_buffer_; }
	VkSemaphore image_available() const { return image_available_; }
	VkSemaphore render_finished() const { return render_finished_; }
#if defined(__linux__)
	// Loaded via vkGetDeviceProcAddr rather than linked directly -- not
	// every loader exports extension entry points as plain symbols.
	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties() const { return GetMemoryFdPropertiesKHR_; }
	// True if a VASurface decoded on init()'s render node can be imported
	// here: the device has to be the one behind that node *and* carry the
	// dmabuf-import extensions. False on a GPU-less client, where the node
	// is missing and init() falls back to whatever the loader offered
	// (lavapipe, which without a render node lacks
	// VK_EXT_image_drm_format_modifier anyway). Presenting still works; it
	// is VA-API decode that has to be skipped. Valid after init().
	bool supports_dmabuf_import() const { return matched_render_node_ && dmabuf_import_; }
#elif defined(_WIN32)
	// True if this device can import D3D11 textures
	// (VK_KHR_external_memory_win32 present *and* the implementation
	// reports an adapter LUID). Both are preconditions for the D3D11VA
	// decode path: without them there is no way to pair a D3D11 device with
	// this one, nor to sample what it produced, so Decoder skips D3D11VA.
	// See video_image_source.hpp.
	bool supports_d3d11_import() const {
		return GetMemoryWin32HandlePropertiesKHR_ != nullptr && luid_valid_;
	}
	// LUID of the adapter this device runs on (VkPhysicalDeviceIDProperties),
	// which is how the matching DXGI adapter is found for the D3D11 device.
	// Only meaningful when supports_d3d11_import() is true.
	const uint8_t *device_luid() const { return luid_; }
	PFN_vkGetMemoryWin32HandlePropertiesKHR get_memory_win32_handle_properties() const {
		return GetMemoryWin32HandlePropertiesKHR_;
	}
#endif

	// The Vulkan Video decode path's view of this device, or null when it
	// has no usable decode queue (no VK_KHR_video_decode_queue, none of the
	// codec extensions, or no timeline semaphores / synchronization2 --
	// lavapipe, older GPUs, older drivers). Valid after init().
	const VulkanDecodeDevice *decode_device() const {
		return decode_device_valid_ ? &decode_device_ : nullptr;
	}
	// The same view for PyroWave's compute decode (pyrowave_decode.hpp),
	// which needs no decode queue: decode_family is graphics_family and
	// decode_ops 0. Null unless the device has what PyroWave's shaders do
	// (supports_pyrowave()) and isn't a CPU implementation. Valid after
	// init().
	const VulkanDecodeDevice *compute_device() const {
		return compute_device_valid_ ? &compute_device_ : nullptr;
	}
	// VkPhysicalDeviceProperties::vendorID (0x10de is NVIDIA).
	uint32_t vendor_id() const { return vendor_id_; }

	// Instance setup every VkInstance spectre makes needs on this platform,
	// added to `extensions` and returned as VkInstanceCreateInfo::flags. On
	// macOS the only driver is MoltenVK, a portability (non-conformant)
	// implementation, which the loader hides -- and vkCreateInstance fails
	// with VK_ERROR_INCOMPATIBLE_DRIVER -- unless the instance enables
	// VK_KHR_portability_enumeration and asks for portability drivers.
	// Nothing elsewhere.
	static VkInstanceCreateFlags portability_instance_setup(std::vector<const char *> *extensions);

	// The pieces of init() that only look at a physical device, for
	// decode/codec_support.cpp: it has to judge what the presenter's GPU can
	// decode before connecting, which is before there's a window to init()
	// with. Each is what init() itself uses, so both land on the same answer.
	//
	// The physical device init() binds to for `drm_render_node`, or null if
	// the instance has none. *matched_render_node says whether it is the
	// node's own GPU (always false off Linux, which alone has render nodes).
	static VkPhysicalDevice choose_physical_device(VkInstance instance, const char *drm_render_node,
		bool *matched_render_node);
	// Picks the decode queue family, the codecs it can decode, and the video
	// extensions create_device() must add. False (with the reason logged)
	// if the device can't do Vulkan Video decode.
	static bool probe_video_decode(VkPhysicalDevice device, uint32_t *decode_family,
		VkVideoCodecOperationFlagsKHR *decode_ops, std::vector<const char *> *extensions);
	// Whether init() on `device` would end up able to import the platform
	// decoder's frames -- supports_dmabuf_import() / supports_d3d11_import().
	// Always true on macOS, whose VideoToolbox frames are copied in rather
	// than imported (video_image_source.hpp).
	static bool can_import_native_frames(VkPhysicalDevice device, bool matched_render_node);
	// Whether `device` can run PyroWave's decode shaders (subgroup size
	// control, 16-bit ints, 8-bit storage, timeline semaphores,
	// synchronization2) and sample its 3-plane 4:2:0 output, and is a real
	// GPU: lavapipe has the features but decodes wrongly.
	// `why`, if given, is set to the first check that failed (a string
	// literal) when this returns false.
	static bool supports_pyrowave(VkPhysicalDevice device, const char **why = nullptr);
	// Whether PyroWave can write the decoded frame's three planes in place,
	// through R8 storage views of the planar image. Where it can't (Intel's
	// ANV), PyrowaveDecode decodes into plain R8 images and copies them in
	// -- supports_pyrowave() accepts those devices too.
	static bool pyrowave_writes_planes_directly(VkPhysicalDevice device);

	VkSwapchainKHR swapchain() const { return swapchain_; }
	VkFormat swapchain_format() const { return swapchain_format_; }
	VkExtent2D swapchain_extent() const { return swapchain_extent_; }
	VkImage swapchain_image(uint32_t index) const { return swapchain_images_[index]; }
	VkImageView swapchain_view(uint32_t index) const { return swapchain_views_[index]; }

	// Waits for the device to go idle, then drops and rebuilds the swapchain
	// at the window's current pixel size. For window resizes and
	// VK_ERROR_OUT_OF_DATE_KHR.
	bool recreate_swapchain();
	// vkDeviceWaitIdle, a no-op before init().
	void wait_idle();

	// Index of the first memory type in `type_bits` whose property flags
	// include all of `required`; UINT32_MAX if none does.
	uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags required) const;

	// A linear-tiled, sampled, host-visible+coherent 2D image with its own
	// dedicated allocation, bound and ready for vkMapMemory. Used for
	// everything written from the CPU: the cursor, splash and font atlas
	// textures, the lossless refinement plane, and the software-decode
	// staging image. `what` names the caller in log lines.
	bool create_host_image(VkFormat format, uint32_t width, uint32_t height, VkImageLayout initial_layout,
		VkImage *image_out, VkDeviceMemory *memory_out, const char *what);

	// A plain 2D color view with identity swizzle. `pnext` is for a
	// VkSamplerYcbcrConversionInfo on the NV12 video images; nullptr
	// otherwise.
	bool create_image_view(VkImage image, VkFormat format, const void *pnext, VkImageView *view_out,
		const char *what);

	// A clamp-to-edge sampler with `filter` for both min and mag. `pnext`
	// is for a VkSamplerYcbcrConversionInfo on the NV12 video sampler;
	// nullptr otherwise.
	bool create_sampler(VkFilter filter, const void *pnext, VkSampler *sampler_out, const char *what);

	// A fragment-stage combined-image-sampler layout (one binding, with
	// `sampler` immutable) and a pool holding `count` sets of it, allocated
	// into `sets_out`. The texture draws (text, cursor, splash, lossless
	// plane) are all this shape.
	bool create_texture_sets(VkSampler sampler, uint32_t count, VkDescriptorSetLayout *layout_out,
		VkDescriptorPool *pool_out, VkDescriptorSet *sets_out, const char *what);
	// Points `set` at `view` (in `layout`).
	void write_texture_set(VkDescriptorSet set, VkImageView view, VkImageLayout layout);

	VkShaderModule load_shader(const uint32_t *code, size_t words);

	// The draw pipelines (video, rect, text, cursor, lossless plane) differ
	// only in shaders, layout, topology, and whether they blend --
	// everything else (no vertex input, dynamic viewport/scissor, dynamic
	// rendering onto the swapchain format) is fixed here.
	struct PipelineDesc {
		const uint32_t *vert_spv = nullptr;
		size_t vert_words = 0;
		const uint32_t *frag_spv = nullptr;
		size_t frag_words = 0;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		// Premultiplied-alpha blend: dst = src.rgb*1 + dst.rgb*(1-src.a).
		bool premultiplied_blend = false;
		const char *what = "pipeline";
	};
	bool create_pipeline(const PipelineDesc &desc, VkPipeline *pipeline_out);

	// Records `record` into the shared command buffer, submits it, and
	// blocks until it completes. For one-off work between frames (the
	// command buffer is idle between present() calls -- never call this
	// mid-frame). False if any step failed, in which case the recorded
	// work may not have run.
	bool submit_one_time(const std::function<void(VkCommandBuffer)> &record);

private:
	bool pick_physical_device(const char *drm_render_node);
	bool create_device();
	bool create_swapchain();
	void destroy_swapchain();

	SDL_Window *window_ = nullptr;

	VkInstance instance_ = VK_NULL_HANDLE;
	VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	uint32_t queue_family_index_ = 0;
	VkQueue queue_ = VK_NULL_HANDLE;
	VkSurfaceKHR surface_ = VK_NULL_HANDLE;
	uint32_t vendor_id_ = 0;

	// Kept for decode_device_ and compute_device_: the instance extensions
	// (copied -- SDL owns the originals), the device extensions, and the
	// feature chain.
	std::vector<std::string> instance_extension_names_;
	std::vector<const char *> instance_extensions_;
	std::vector<const char *> device_extensions_;
	VkPhysicalDeviceFeatures2 features_{};
	VkPhysicalDeviceVulkan11Features features11_{};
	VkPhysicalDeviceVulkan12Features features12_{};
	VkPhysicalDeviceVulkan13Features features13_{};
	VulkanDecodeDevice decode_device_;
	bool decode_device_valid_ = false;
	VulkanDecodeDevice compute_device_;
	bool compute_device_valid_ = false;
	// The create infos behind instance_ and device_, kept alive for
	// compute_device_ (PyroWave reads them for the device's whole life).
	VkApplicationInfo app_info_{};
	VkInstanceCreateInfo instance_info_{};
	float queue_priority_ = 1.0f;
	std::vector<VkDeviceQueueCreateInfo> queue_infos_;
	VkDeviceCreateInfo device_info_{};
#if defined(__linux__)
	PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR_ = nullptr;
	bool matched_render_node_ = false;
	bool dmabuf_import_ = false;
#elif defined(_WIN32)
	PFN_vkGetMemoryWin32HandlePropertiesKHR GetMemoryWin32HandlePropertiesKHR_ = nullptr;
	uint8_t luid_[VK_LUID_SIZE] = {};
	bool luid_valid_ = false;
#endif

	VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
	VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchain_extent_{};
	std::vector<VkImage> swapchain_images_;
	std::vector<VkImageView> swapchain_views_;

	VkCommandPool command_pool_ = VK_NULL_HANDLE;
	VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
	VkSemaphore image_available_ = VK_NULL_HANDLE;
	VkSemaphore render_finished_ = VK_NULL_HANDLE;
};

} // namespace spectre
