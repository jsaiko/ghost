// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "present/vulkan_device.hpp"

#include "log.hpp"

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <bit>
#include <cstring>
#ifdef __linux__
#include <sys/stat.h>
#include <sys/sysmacros.h>
#endif

namespace spectre {

namespace {

bool has_device_extension(VkPhysicalDevice device, const char *name) {
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> props(count);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &count, props.data());
	for (const auto &p : props) {
		if (strcmp(p.extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

#ifdef __linux__
// Only needed to pull the decoder's VASurface into Vulkan. They come as a
// set or not at all: lavapipe on a box with no render node advertises three
// of the four but not VK_EXT_image_drm_format_modifier, and naming an
// extension the driver lacks fails vkCreateDevice outright
// (VK_ERROR_EXTENSION_NOT_PRESENT) rather than degrading. Without the full
// set there is no import path, so Decoder skips VA-API; Vulkan Video and
// software decode need none of them (software uploads through a
// host-visible image).
const char *const kDmabufExtensions[] = {
	VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
	VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
	VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};
#endif

} // namespace

VulkanDevice::~VulkanDevice() {
	wait_idle();
	destroy_swapchain();
	if (render_finished_) vkDestroySemaphore(device_, render_finished_, nullptr);
	if (image_available_) vkDestroySemaphore(device_, image_available_, nullptr);
	if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
	if (device_) vkDestroyDevice(device_, nullptr);
	if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
	if (instance_) vkDestroyInstance(instance_, nullptr);
}

void VulkanDevice::wait_idle() {
	if (device_ != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(device_);
	}
}

bool VulkanDevice::init(SDL_Window *window, const char *drm_render_node) {
	window_ = window;

	if (!SDL_Vulkan_LoadLibrary(nullptr)) {
		SLOG_ERROR("vulkan_device: SDL_Vulkan_LoadLibrary failed: %s", SDL_GetError());
		return false;
	}

	uint32_t sdl_ext_count = 0;
	char const *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
	// Copied: decode_device() and compute_device() hand them on to FFmpeg
	// and PyroWave, which have no business pointing into SDL's storage.
	instance_extension_names_.assign(sdl_exts, sdl_exts + sdl_ext_count);
	for (const std::string &name : instance_extension_names_) {
		instance_extensions_.push_back(name.c_str());
	}
	VkInstanceCreateFlags instance_flags = portability_instance_setup(&instance_extensions_);

	// Members, not locals: compute_device() hands PyroWave a pointer.
	app_info_ = {};
	app_info_.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info_.pApplicationName = "spectre";
	app_info_.apiVersion = VK_API_VERSION_1_3;

	instance_info_ = {};
	instance_info_.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info_.flags = instance_flags;
	instance_info_.pApplicationInfo = &app_info_;
	instance_info_.enabledExtensionCount = (uint32_t)instance_extensions_.size();
	instance_info_.ppEnabledExtensionNames = instance_extensions_.data();

	if (!vk_check(vkCreateInstance(&instance_info_, nullptr, &instance_), "vkCreateInstance")) {
		return false;
	}

	if (!SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) {
		SLOG_ERROR("vulkan_device: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
		return false;
	}

	if (!pick_physical_device(drm_render_node)) return false;
	if (!create_device()) return false;
	if (!create_swapchain()) return false;

	VkCommandPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family_index_;
	if (!vk_check(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_), "vkCreateCommandPool")) {
		return false;
	}

	VkCommandBufferAllocateInfo cmd_alloc{};
	cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd_alloc.commandPool = command_pool_;
	cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_alloc.commandBufferCount = 1;
	if (!vk_check(vkAllocateCommandBuffers(device_, &cmd_alloc, &command_buffer_),
			"vkAllocateCommandBuffers")) {
		return false;
	}

	VkSemaphoreCreateInfo sem_info{};
	sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	if (!vk_check(vkCreateSemaphore(device_, &sem_info, nullptr, &image_available_), "vkCreateSemaphore") ||
		!vk_check(vkCreateSemaphore(device_, &sem_info, nullptr, &render_finished_), "vkCreateSemaphore")) {
		return false;
	}
	return true;
}

bool VulkanDevice::pick_physical_device(const char *drm_render_node) {
	bool matched = false;
	physical_device_ = choose_physical_device(instance_, drm_render_node, &matched);
#ifdef __linux__
	matched_render_node_ = matched;
#endif
	return physical_device_ != VK_NULL_HANDLE;
}

VkPhysicalDevice VulkanDevice::choose_physical_device(VkInstance instance, const char *drm_render_node,
	bool *matched_render_node) {
	*matched_render_node = false;
#ifdef __linux__
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());
	if (devices.empty()) {
		SLOG_ERROR("vulkan_device: no Vulkan physical devices found");
		return VK_NULL_HANDLE;
	}

	struct stat st;
	if (stat(drm_render_node, &st) == 0) {
		uint32_t want_major = major(st.st_rdev);
		uint32_t want_minor = minor(st.st_rdev);
		for (VkPhysicalDevice dev : devices) {
			VkPhysicalDeviceDrmPropertiesEXT drm_props{};
			drm_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
			VkPhysicalDeviceProperties2 props2{};
			props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			props2.pNext = &drm_props;
			vkGetPhysicalDeviceProperties2(dev, &props2);

			if (drm_props.hasRender && (uint32_t)drm_props.renderMajor == want_major &&
				(uint32_t)drm_props.renderMinor == want_minor) {
				*matched_render_node = true;
				SLOG_INFO("vulkan_device: using %s", props2.properties.deviceName);
				return dev;
			}
		}
	}

	// No render node (a client with no GPU at all -- a VM, a container, or a
	// nested session on a headless host) or nothing matching it. Present on
	// whatever device the loader does offer, which on a GPU-less box is
	// Mesa's lavapipe: slower, but it satisfies every extension and feature
	// create_device() asks for, so the whole present path works unchanged.
	// *matched_render_node stays false, which is what tells Decoder to skip
	// the VA-API attempt -- a VASurface from some other GPU could not be
	// imported into this device anyway. Vulkan Video decode is still
	// possible if this device has it, since that decodes on the device
	// itself (decode_device()).
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(devices[0], &props);
	SLOG_INFO("vulkan_device: no Vulkan device matches render node %s -- falling back to %s (no VA-API)",
		drm_render_node, props.deviceName);
	return devices[0];
#else
	// No DRM render nodes on Windows or macOS to pin the device choice to --
	// just take the first device the loader lists (create_device() checks
	// it has a graphics+present queue family). The D3D11VA decode path
	// runs the other way round from Linux's: rather than the render node
	// picking the GPU for both, this pick wins and Decoder opens D3D11 on
	// whichever DXGI adapter matches its LUID (see create_device()).
	// VideoToolbox picks its own engine and hands back frames the CPU can
	// read, so on macOS there is nothing to pair at all.
	(void)drm_render_node;
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	if (count == 0) {
		SLOG_ERROR("vulkan_device: no Vulkan physical devices found");
		return VK_NULL_HANDLE;
	}
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(devices[0], &props);
	SLOG_INFO("vulkan_device: using %s", props.deviceName);
	return devices[0];
#endif
}

bool VulkanDevice::can_import_native_frames(VkPhysicalDevice device, bool matched_render_node) {
#if defined(__linux__)
	if (!matched_render_node) {
		return false;
	}
	for (const char *name : kDmabufExtensions) {
		if (!has_device_extension(device, name)) {
			return false;
		}
	}
	return true;
#elif defined(_WIN32)
	(void)matched_render_node;
	if (!has_device_extension(device, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)) {
		return false;
	}
	VkPhysicalDeviceIDProperties id_props{};
	id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
	VkPhysicalDeviceProperties2 props2{};
	props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props2.pNext = &id_props;
	vkGetPhysicalDeviceProperties2(device, &props2);
	return id_props.deviceLUIDValid == VK_TRUE;
#else
	(void)device;
	(void)matched_render_node;
	return true;
#endif
}

bool VulkanDevice::supports_pyrowave(VkPhysicalDevice device, const char **why) {
	auto fail = [why](const char *reason) {
		if (why) {
			*why = reason;
		}
		return false;
	};
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(device, &props);
	if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
		return fail("a CPU implementation (it decodes PyroWave wrongly)");
	}
	if (props.apiVersion < VK_API_VERSION_1_3) {
		return fail("the driver is older than Vulkan 1.3");
	}
	VkPhysicalDeviceVulkan13Features f13{};
	f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	VkPhysicalDeviceVulkan12Features f12{};
	f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	f12.pNext = &f13;
	VkPhysicalDeviceFeatures2 f{};
	f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	f.pNext = &f12;
	vkGetPhysicalDeviceFeatures2(device, &f);
	if (!f13.subgroupSizeControl) {
		return fail("no subgroupSizeControl");
	}
	if (!f13.synchronization2) {
		return fail("no synchronization2");
	}
	if (!f.features.shaderInt16) {
		return fail("no shaderInt16");
	}
	if (!f12.storageBuffer8BitAccess) {
		return fail("no storageBuffer8BitAccess");
	}
	if (!f12.timelineSemaphore) {
		return fail("no timelineSemaphore");
	}
	// The decode target: one 3-plane 4:2:0 image, written plane by plane
	// through R8 storage views and sampled whole through a ycbcr
	// conversion with centred chroma.
	VkFormatProperties fmt{};
	vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM, &fmt);
	VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
		VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT |
		VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
	if ((fmt.optimalTilingFeatures & need) != need) {
		return fail("the 3-plane 4:2:0 image format can't be sampled with a ycbcr conversion");
	}
	if (pyrowave_writes_planes_directly(device)) {
		return true;
	}
	// The staged path: PyroWave writes three R8 images, which are copied
	// into the planes of an image that is only sampled and copied to.
	VkFormatProperties r8{};
	vkGetPhysicalDeviceFormatProperties(device, VK_FORMAT_R8_UNORM, &r8);
	VkFormatFeatureFlags r8_need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	if ((r8.optimalTilingFeatures & r8_need) != r8_need) {
		return fail("no R8 storage images to decode into");
	}
	if (!(fmt.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
		return fail("the 3-plane 4:2:0 image can't be copied into");
	}
	VkPhysicalDeviceImageFormatInfo2 staged{};
	staged.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	staged.format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
	staged.type = VK_IMAGE_TYPE_2D;
	staged.tiling = VK_IMAGE_TILING_OPTIMAL;
	staged.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkImageFormatProperties2 staged_out{};
	staged_out.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	if (vkGetPhysicalDeviceImageFormatProperties2(device, &staged, &staged_out) != VK_SUCCESS) {
		return fail("the 3-plane 4:2:0 image can't be sampled and copied into");
	}
	return true;
}

bool VulkanDevice::pyrowave_writes_planes_directly(VkPhysicalDevice device) {
	VkPhysicalDeviceImageFormatInfo2 info{};
	info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	info.format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
	info.type = VK_IMAGE_TYPE_2D;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
	info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	VkImageFormatProperties2 out{};
	out.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	return vkGetPhysicalDeviceImageFormatProperties2(device, &info, &out) == VK_SUCCESS;
}

VkInstanceCreateFlags VulkanDevice::portability_instance_setup(std::vector<const char *> *extensions) {
#ifdef __APPLE__
	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> props(count);
	vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
	for (const auto &p : props) {
		if (strcmp(p.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
			extensions->push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			return VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		}
	}
	// A loader too old to know about portability drivers lists MoltenVK like
	// any other, so there is nothing to ask for.
#else
	(void)extensions;
#endif
	return 0;
}

bool VulkanDevice::probe_video_decode(VkPhysicalDevice device, uint32_t *decode_family,
	VkVideoCodecOperationFlagsKHR *decode_ops, std::vector<const char *> *extensions) {
	if (!has_device_extension(device, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) ||
		!has_device_extension(device, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME)) {
		SLOG_INFO("vulkan_device: no %s -- Vulkan Video decode unavailable",
			VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
		return false;
	}

	// The codecs wraith can send (gdp/video_codec.hpp). A decode queue
	// advertising an operation is no use without its extension enabled.
	struct CodecExtension {
		const char *name;
		VkVideoCodecOperationFlagBitsKHR op;
	};
	static const CodecExtension kCodecs[] = {
		{VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR},
		{VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR},
		{VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME, VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR},
	};
	std::vector<const char *> codec_extensions;
	VkVideoCodecOperationFlagsKHR extension_ops = 0;
	for (const CodecExtension &codec : kCodecs) {
		if (has_device_extension(device, codec.name)) {
			codec_extensions.push_back(codec.name);
			extension_ops |= codec.op;
		}
	}
	if (extension_ops == 0) {
		SLOG_INFO("vulkan_device: no H.264/H.265/AV1 decode extension -- Vulkan Video decode unavailable");
		return false;
	}

	VkPhysicalDeviceVulkan13Features supported13{};
	supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	VkPhysicalDeviceVulkan12Features supported12{};
	supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	supported12.pNext = &supported13;
	VkPhysicalDeviceFeatures2 supported{};
	supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	supported.pNext = &supported12;
	vkGetPhysicalDeviceFeatures2(device, &supported);
	if (!supported12.timelineSemaphore || !supported13.synchronization2) {
		SLOG_INFO(
			"vulkan_device: no timeline semaphores/synchronization2 -- Vulkan Video decode unavailable");
		return false;
	}

	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties2(device, &count, nullptr);
	std::vector<VkQueueFamilyVideoPropertiesKHR> video(count);
	std::vector<VkQueueFamilyProperties2> families(count);
	for (uint32_t i = 0; i < count; i++) {
		video[i] = {};
		video[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
		families[i] = {};
		families[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
		families[i].pNext = &video[i];
	}
	vkGetPhysicalDeviceQueueFamilyProperties2(device, &count, families.data());

	// A driver may split codecs across several decode families; take the
	// one covering the most of ours.
	int best_codecs = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (!(families[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR)) {
			continue;
		}
		VkVideoCodecOperationFlagsKHR ops = video[i].videoCodecOperations & extension_ops;
		int codecs = std::popcount(ops);
		if (codecs > best_codecs) {
			best_codecs = codecs;
			*decode_family = i;
			*decode_ops = ops;
		}
	}
	if (best_codecs == 0) {
		SLOG_INFO("vulkan_device: no decode queue for H.264/H.265/AV1 -- Vulkan Video decode unavailable");
		return false;
	}

	extensions->push_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
	extensions->push_back(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
	extensions->insert(extensions->end(), codec_extensions.begin(), codec_extensions.end());
	SLOG_INFO("vulkan_device: Vulkan Video decode available (queue family %u:%s%s%s)", *decode_family,
		(*decode_ops & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR) ? " h264" : "",
		(*decode_ops & VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR) ? " h265" : "",
		(*decode_ops & VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR) ? " av1" : "");
	return true;
}

bool VulkanDevice::create_device() {
	uint32_t family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, nullptr);
	std::vector<VkQueueFamilyProperties> families(family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &family_count, families.data());

	queue_family_index_ = UINT32_MAX;
	for (uint32_t i = 0; i < family_count; i++) {
		VkBool32 present_support = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &present_support);
		if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support) {
			queue_family_index_ = i;
			break;
		}
	}
	if (queue_family_index_ == UINT32_MAX) {
		SLOG_ERROR("vulkan_device: no graphics+present queue family");
		return false;
	}

	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(physical_device_, &props);
	vendor_id_ = props.vendorID;

	// Vulkan Video decode (decoder.hpp): an extra queue on the decode family
	// plus the extensions below. FFmpeg also runs its frame-setup work on a
	// compute+transfer queue, which it gets by sharing the graphics one.
	uint32_t decode_family = UINT32_MAX;
	VkVideoCodecOperationFlagsKHR decode_ops = 0;
	std::vector<const char *> video_extensions;
	bool want_decode = false;
	if (!(families[queue_family_index_].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
		SLOG_INFO("vulkan_device: graphics queue family lacks compute -- Vulkan Video decode unavailable");
	} else {
		want_decode = probe_video_decode(physical_device_, &decode_family, &decode_ops, &video_extensions);
	}

	std::vector<VkDeviceQueueCreateInfo> &queue_infos = queue_infos_;
	queue_infos.clear();
	VkDeviceQueueCreateInfo queue_info{};
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = queue_family_index_;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &queue_priority_;
	queue_infos.push_back(queue_info);
	if (want_decode && decode_family != queue_family_index_) {
		queue_info.queueFamilyIndex = decode_family;
		queue_infos.push_back(queue_info);
	}
	// PyroWave's compute decode (pyrowave_decode.hpp) shares the graphics
	// queue and wants a few more features, all cheap to enable.
	bool want_pyrowave = false;
#ifdef SPECTRE_HAVE_PYROWAVE
	const char *pyrowave_why = nullptr;
	if (!(families[queue_family_index_].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
		pyrowave_why = "the graphics queue can't do compute";
	} else {
		want_pyrowave = supports_pyrowave(physical_device_, &pyrowave_why);
	}
	if (!want_pyrowave) {
		SLOG_INFO("vulkan_device: no PyroWave decode on this GPU: %s", pyrowave_why);
	}
#else
	SLOG_INFO("vulkan_device: no PyroWave decode: this spectre was built without it");
#endif

	std::vector<const char *> extensions = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	};

	// A portability implementation (MoltenVK) lists this, and the spec
	// requires enabling it whenever it's listed: it's how the application
	// acknowledges the features such a driver may lack.
	if (has_device_extension(physical_device_, "VK_KHR_portability_subset")) {
		extensions.push_back("VK_KHR_portability_subset");
	}

#ifdef __linux__
	dmabuf_import_ = true;
	for (const char *name : kDmabufExtensions) {
		if (!has_device_extension(physical_device_, name)) {
			SLOG_INFO("vulkan_device: no %s -- hardware decode unavailable", name);
			dmabuf_import_ = false;
			break;
		}
	}
	if (dmabuf_import_) {
		extensions.insert(extensions.end(), std::begin(kDmabufExtensions), std::end(kDmabufExtensions));
	}
#endif

#ifdef _WIN32
	// The Windows counterpart of the dmabuf-import extensions above: what
	// lets video_image_source.cpp import the NV12 texture the D3D11VA decoder
	// wrote. Like the Linux set this is optional -- a device without it
	// still presents fine, it just leaves Decoder without D3D11VA (see
	// supports_d3d11_import()).
	bool want_d3d11_import =
		has_device_extension(physical_device_, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
	if (want_d3d11_import) {
		extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
	}

	VkPhysicalDeviceIDProperties id_props{};
	id_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
	VkPhysicalDeviceProperties2 props2{};
	props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props2.pNext = &id_props;
	vkGetPhysicalDeviceProperties2(physical_device_, &props2);
	luid_valid_ = id_props.deviceLUIDValid == VK_TRUE;
	if (luid_valid_) {
		memcpy(luid_, id_props.deviceLUID, VK_LUID_SIZE);
	}
#endif

	extensions.insert(extensions.end(), video_extensions.begin(), video_extensions.end());

	// Members rather than locals: decode_device() hands FFmpeg this chain.
	features13_ = {};
	features13_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	features13_.dynamicRendering = VK_TRUE;
	// Decode only: FFmpeg's barriers are vkCmdPipelineBarrier2, and every
	// frame it hands out is guarded by a timeline semaphore.
	features13_.synchronization2 = want_decode || want_pyrowave ? VK_TRUE : VK_FALSE;

	features12_ = {};
	features12_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features12_.timelineSemaphore = want_decode || want_pyrowave ? VK_TRUE : VK_FALSE;
	features12_.pNext = &features13_;
	if (want_pyrowave) {
		// What PyroWave's shaders use; fp16 and int8 only where there is
		// any (its decode runs without them).
		VkPhysicalDeviceVulkan13Features s13{};
		s13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		VkPhysicalDeviceVulkan12Features s12{};
		s12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		s12.pNext = &s13;
		VkPhysicalDeviceFeatures2 s{};
		s.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		s.pNext = &s12;
		vkGetPhysicalDeviceFeatures2(physical_device_, &s);
		features13_.subgroupSizeControl = VK_TRUE;
		features13_.computeFullSubgroups = s13.computeFullSubgroups;
		features12_.storageBuffer8BitAccess = VK_TRUE;
		features12_.uniformAndStorageBuffer8BitAccess = s12.uniformAndStorageBuffer8BitAccess;
		features12_.shaderFloat16 = s12.shaderFloat16;
		features12_.shaderInt8 = s12.shaderInt8;
	}

	features11_ = {};
	features11_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	features11_.samplerYcbcrConversion = VK_TRUE;
	features11_.pNext = &features12_;

	features_ = {};
	features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features_.pNext = &features11_;
	features_.features.shaderInt16 = want_pyrowave ? VK_TRUE : VK_FALSE;

	device_extensions_ = std::move(extensions);
	device_info_ = {};
	device_info_.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info_.pNext = &features_;
	device_info_.queueCreateInfoCount = (uint32_t)queue_infos.size();
	device_info_.pQueueCreateInfos = queue_infos.data();
	device_info_.enabledExtensionCount = (uint32_t)device_extensions_.size();
	device_info_.ppEnabledExtensionNames = device_extensions_.data();

	if (!vk_check(vkCreateDevice(physical_device_, &device_info_, nullptr, &device_), "vkCreateDevice")) {
		return false;
	}
	vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);

	if (want_decode) {
		decode_device_.get_instance_proc_addr = vkGetInstanceProcAddr;
		decode_device_.instance = instance_;
		decode_device_.physical_device = physical_device_;
		decode_device_.device = device_;
		decode_device_.features = &features_;
		decode_device_.instance_extensions = instance_extensions_.data();
		decode_device_.instance_extension_count = (int)instance_extensions_.size();
		decode_device_.device_extensions = device_extensions_.data();
		decode_device_.device_extension_count = (int)device_extensions_.size();
		decode_device_.graphics_family = queue_family_index_;
		// Transfer is implied by graphics/compute, so a driver need not
		// report it -- but FFmpeg looks for the bit when picking a queue.
		decode_device_.graphics_flags =
			(families[queue_family_index_].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) |
			VK_QUEUE_TRANSFER_BIT;
		decode_device_.decode_family = decode_family;
		decode_device_.decode_ops = decode_ops;
		decode_device_.graphics_queue = queue_;
		decode_device_.instance_create_info = &instance_info_;
		decode_device_.device_create_info = &device_info_;
		decode_device_valid_ = true;
	}
	if (want_pyrowave) {
		compute_device_.get_instance_proc_addr = vkGetInstanceProcAddr;
		compute_device_.instance = instance_;
		compute_device_.physical_device = physical_device_;
		compute_device_.device = device_;
		compute_device_.features = &features_;
		compute_device_.instance_extensions = instance_extensions_.data();
		compute_device_.instance_extension_count = (int)instance_extensions_.size();
		compute_device_.device_extensions = device_extensions_.data();
		compute_device_.device_extension_count = (int)device_extensions_.size();
		compute_device_.graphics_family = queue_family_index_;
		compute_device_.graphics_flags =
			(families[queue_family_index_].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) |
			VK_QUEUE_TRANSFER_BIT;
		compute_device_.decode_family = queue_family_index_;
		compute_device_.decode_ops = 0;
		compute_device_.graphics_queue = queue_;
		compute_device_.instance_create_info = &instance_info_;
		compute_device_.device_create_info = &device_info_;
		compute_device_valid_ = true;
	}

#if defined(__linux__)
	if (dmabuf_import_) {
		GetMemoryFdPropertiesKHR_ =
			(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR");
		if (!GetMemoryFdPropertiesKHR_) {
			SLOG_ERROR("vulkan_device: vkGetMemoryFdPropertiesKHR not available");
			return false;
		}
	}
#elif defined(_WIN32)
	if (want_d3d11_import) {
		GetMemoryWin32HandlePropertiesKHR_ = (PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(
			device_, "vkGetMemoryWin32HandlePropertiesKHR");
	}
	if (!supports_d3d11_import()) {
		SLOG_INFO("vulkan_device: no D3D11 texture import (%s) -- hardware decode unavailable",
			want_d3d11_import ? "device reports no adapter LUID"
							  : VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME " unsupported");
	}
#endif
	return true;
}

bool VulkanDevice::create_swapchain() {
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

	uint32_t format_count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(format_count);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());

	VkSurfaceFormatKHR chosen = formats[0];
	for (const auto &f : formats) {
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			chosen = f;
			break;
		}
	}
	swapchain_format_ = chosen.format;

	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(window_, &w, &h);
	swapchain_extent_.width = std::clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width);
	swapchain_extent_.height =
		std::clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);

	uint32_t image_count = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && image_count > caps.maxImageCount) {
		image_count = caps.maxImageCount;
	}

	VkSwapchainCreateInfoKHR swap_info{};
	swap_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swap_info.surface = surface_;
	swap_info.minImageCount = image_count;
	swap_info.imageFormat = chosen.format;
	swap_info.imageColorSpace = chosen.colorSpace;
	swap_info.imageExtent = swapchain_extent_;
	swap_info.imageArrayLayers = 1;
	swap_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	swap_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swap_info.preTransform = caps.currentTransform;
	swap_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swap_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	swap_info.clipped = VK_TRUE;

	if (!vk_check(vkCreateSwapchainKHR(device_, &swap_info, nullptr, &swapchain_), "vkCreateSwapchainKHR")) {
		return false;
	}

	uint32_t actual_count = 0;
	vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, nullptr);
	swapchain_images_.resize(actual_count);
	vkGetSwapchainImagesKHR(device_, swapchain_, &actual_count, swapchain_images_.data());

	swapchain_views_.resize(actual_count);
	for (uint32_t i = 0; i < actual_count; i++) {
		if (!create_image_view(swapchain_images_[i], swapchain_format_, nullptr, &swapchain_views_[i],
				"swapchain")) {
			return false;
		}
	}
	return true;
}

void VulkanDevice::destroy_swapchain() {
	for (VkImageView v : swapchain_views_) {
		vkDestroyImageView(device_, v, nullptr);
	}
	swapchain_views_.clear();
	swapchain_images_.clear();
	if (swapchain_) {
		vkDestroySwapchainKHR(device_, swapchain_, nullptr);
		swapchain_ = VK_NULL_HANDLE;
	}
}

bool VulkanDevice::recreate_swapchain() {
	wait_idle();
	destroy_swapchain();
	return create_swapchain();
}

uint32_t VulkanDevice::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags required) const {
	VkPhysicalDeviceMemoryProperties mem_props;
	vkGetPhysicalDeviceMemoryProperties(physical_device_, &mem_props);
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((type_bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & required) == required) {
			return i;
		}
	}
	return UINT32_MAX;
}

bool VulkanDevice::create_host_image(VkFormat format, uint32_t width, uint32_t height,
	VkImageLayout initial_layout, VkImage *image_out, VkDeviceMemory *memory_out, const char *what) {
	VkImageCreateInfo image_info{};
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = format;
	image_info.extent = {width, height, 1};
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_LINEAR;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = initial_layout;
	if (!vk_check(vkCreateImage(device_, &image_info, nullptr, image_out), what)) {
		return false;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(device_, *image_out, &requirements);
	uint32_t memory_type_index = find_memory_type(requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (memory_type_index == UINT32_MAX) {
		SLOG_ERROR("vulkan_device: no host-visible memory type for %s", what);
		return false;
	}

	VkMemoryAllocateInfo alloc_info{};
	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.allocationSize = requirements.size;
	alloc_info.memoryTypeIndex = memory_type_index;
	if (!vk_check(vkAllocateMemory(device_, &alloc_info, nullptr, memory_out), what)) {
		return false;
	}
	return vk_check(vkBindImageMemory(device_, *image_out, *memory_out, 0), what);
}

bool VulkanDevice::create_image_view(VkImage image, VkFormat format, const void *pnext, VkImageView *view_out,
	const char *what) {
	VkImageViewCreateInfo view_info{};
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.pNext = pnext;
	view_info.image = image;
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = format;
	view_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
	view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	return vk_check(vkCreateImageView(device_, &view_info, nullptr, view_out), what);
}

bool VulkanDevice::create_sampler(VkFilter filter, const void *pnext, VkSampler *sampler_out,
	const char *what) {
	VkSamplerCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	info.pNext = pnext;
	info.magFilter = filter;
	info.minFilter = filter;
	info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	return vk_check(vkCreateSampler(device_, &info, nullptr, sampler_out), what);
}

bool VulkanDevice::create_texture_sets(VkSampler sampler, uint32_t count, VkDescriptorSetLayout *layout_out,
	VkDescriptorPool *pool_out, VkDescriptorSet *sets_out, const char *what) {
	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	binding.pImmutableSamplers = &sampler;

	VkDescriptorSetLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout_info.bindingCount = 1;
	layout_info.pBindings = &binding;
	if (!vk_check(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, layout_out), what)) {
		return false;
	}

	VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count};
	VkDescriptorPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = count;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	if (!vk_check(vkCreateDescriptorPool(device_, &pool_info, nullptr, pool_out), what)) {
		return false;
	}

	std::vector<VkDescriptorSetLayout> layouts(count, *layout_out);
	VkDescriptorSetAllocateInfo alloc_info{};
	alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc_info.descriptorPool = *pool_out;
	alloc_info.descriptorSetCount = count;
	alloc_info.pSetLayouts = layouts.data();
	return vk_check(vkAllocateDescriptorSets(device_, &alloc_info, sets_out), what);
}

void VulkanDevice::write_texture_set(VkDescriptorSet set, VkImageView view, VkImageLayout layout) {
	VkDescriptorImageInfo image_info{};
	image_info.imageView = view;
	image_info.imageLayout = layout;
	VkWriteDescriptorSet write{};
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

VkShaderModule VulkanDevice::load_shader(const uint32_t *code, size_t words) {
	VkShaderModuleCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	info.codeSize = words * sizeof(uint32_t);
	info.pCode = code;
	VkShaderModule module = VK_NULL_HANDLE;
	if (!vk_check(vkCreateShaderModule(device_, &info, nullptr, &module), "vkCreateShaderModule")) {
		return VK_NULL_HANDLE;
	}
	return module;
}

bool VulkanDevice::create_pipeline(const PipelineDesc &desc, VkPipeline *pipeline_out) {
	VkShaderModule vert = load_shader(desc.vert_spv, desc.vert_words);
	VkShaderModule frag = load_shader(desc.frag_spv, desc.frag_words);
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
		SLOG_ERROR("vulkan_device: %s: shader module creation failed", desc.what);
		vkDestroyShaderModule(device_, vert, nullptr);
		vkDestroyShaderModule(device_, frag, nullptr);
		return false;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	// No vertex buffers anywhere: every shader derives its geometry from
	// gl_VertexIndex (a fullscreen triangle or a 4-vertex strip quad).
	VkPipelineVertexInputStateCreateInfo vertex_input{};
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	VkPipelineInputAssemblyStateCreateInfo input_assembly{};
	input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	input_assembly.topology = desc.topology;

	VkPipelineViewportStateCreateInfo viewport_state{};
	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.viewportCount = 1;
	viewport_state.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo raster{};
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState blend_attachment{};
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if (desc.premultiplied_blend) {
		blend_attachment.blendEnable = VK_TRUE;
		blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
		blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}

	VkPipelineColorBlendStateCreateInfo blend_state{};
	blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend_state.attachmentCount = 1;
	blend_state.pAttachments = &blend_attachment;

	VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic_state{};
	dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.dynamicStateCount = 2;
	dynamic_state.pDynamicStates = dynamic_states;

	VkPipelineRenderingCreateInfo rendering_info{};
	rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
	rendering_info.colorAttachmentCount = 1;
	rendering_info.pColorAttachmentFormats = &swapchain_format_;

	VkGraphicsPipelineCreateInfo pipeline_info{};
	pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.pNext = &rendering_info;
	pipeline_info.stageCount = 2;
	pipeline_info.pStages = stages;
	pipeline_info.pVertexInputState = &vertex_input;
	pipeline_info.pInputAssemblyState = &input_assembly;
	pipeline_info.pViewportState = &viewport_state;
	pipeline_info.pRasterizationState = &raster;
	pipeline_info.pMultisampleState = &multisample;
	pipeline_info.pColorBlendState = &blend_state;
	pipeline_info.pDynamicState = &dynamic_state;
	pipeline_info.layout = desc.layout;

	bool ok =
		vk_check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, pipeline_out),
			desc.what);
	vkDestroyShaderModule(device_, vert, nullptr);
	vkDestroyShaderModule(device_, frag, nullptr);
	return ok;
}

bool VulkanDevice::submit_one_time(const std::function<void(VkCommandBuffer)> &record) {
	VkCommandBufferBeginInfo begin_info{};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (!vk_check(vkResetCommandBuffer(command_buffer_, 0), "vkResetCommandBuffer(one-time)") ||
		!vk_check(vkBeginCommandBuffer(command_buffer_, &begin_info), "vkBeginCommandBuffer(one-time)")) {
		return false;
	}
	record(command_buffer_);
	if (!vk_check(vkEndCommandBuffer(command_buffer_), "vkEndCommandBuffer(one-time)")) {
		return false;
	}

	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command_buffer_;
	return vk_check(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(one-time)") &&
		vk_check(vkQueueWaitIdle(queue_), "vkQueueWaitIdle(one-time)");
}

} // namespace spectre
