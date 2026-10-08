// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/pyrowave/pyrowave_encoder.hpp"

#include "util/clock.hpp"
#include "util/log.hpp"

#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace wraith {

namespace {

// Room in the bitstream buffer beyond the frame budget: PyroWave lands
// within a few bytes under it, and the sequence header and block headers
// ride inside it.
constexpr size_t kBitstreamSlack = 64 * 1024;

// What push() needs to take a dmabuf: import it (external memory, explicit
// modifiers), hand it back and forth with its producer (foreign queue
// family) and wait on its pending writes (a sync file as a semaphore).
const char *const kDmabufExtensions[] = {
	VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
	VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
	VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
};

// Capture buffers imported at once: a PipeWire stream cycles a handful,
// and a renegotiation's old ones age out.
constexpr size_t kMaxImports = 8;

// How long push() waits for its copy -- which includes the compositor
// finishing the frame -- before calling the GPU path broken.
constexpr uint64_t kCopyTimeoutNs = 1'000'000'000;

bool vk_ok(VkResult result, const char *what) {
	if (result != VK_SUCCESS) {
		WLOG_ERROR("pyrowave: %s failed (VkResult %d)", what, (int)result);
		return false;
	}
	return true;
}

bool pyro_ok(pyrowave_result result, const char *what) {
	if (result != PYROWAVE_SUCCESS) {
		WLOG_ERROR("pyrowave: %s failed (%d)", what, (int)result);
		return false;
	}
	return true;
}

bool has_extension(VkPhysicalDevice physical, const char *name) {
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> available(count);
	vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data());
	return std::any_of(available.begin(), available.end(),
		[name](const VkExtensionProperties &ext) { return strcmp(ext.extensionName, name) == 0; });
}

// The first memory type with all of `want` and none of `avoid`.
uint32_t find_memory_type(VkPhysicalDevice physical, uint32_t type_bits, VkMemoryPropertyFlags want,
	VkMemoryPropertyFlags avoid = 0) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(physical, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
		if ((type_bits & (1u << i)) && (flags & want) == want && (flags & avoid) == 0) {
			return i;
		}
	}
	return UINT32_MAX;
}

} // namespace

PyrowaveEncoder::~PyrowaveEncoder() {
	close();
}

bool PyrowaveEncoder::open(const EncoderConfig &config) {
	config_ = config;
	bitrate_bps_ = config.bitrate_bps;
	if (config.width % 2 != 0 || config.height % 2 != 0) {
		// 4:2:0 needs even dimensions.
		WLOG_INFO("pyrowave: %ux%u is odd-sized; 4:2:0 needs even dimensions", config.width, config.height);
		return false;
	}
	if (!create_device(config.drm_fd) || !create_resources()) {
		destroy();
		return false;
	}

	if (asynchronous_) {
		wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (wake_fd_ < 0) {
			WLOG_ERROR("pyrowave: eventfd failed; encoding on the caller's thread");
			asynchronous_ = false;
		} else {
			stop_ = false;
			worker_ = std::thread([this] { worker_loop(); });
		}
	}

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(physical_, &props);
	WLOG_INFO("pyrowave: %ux%u 4:2:0 on %s, at most %.0f Mbit/s, %s", config.width, config.height,
		props.deviceName, bitrate_bps_.load() / 1e6, dmabuf_import_ ? "dmabuf input" : "CPU frames only");
	return true;
}

bool PyrowaveEncoder::create_device(int drm_fd) {
	// The session's render node picks the GPU, as it does for VA-API.
	struct stat node;
	if (drm_fd < 0 || fstat(drm_fd, &node) != 0) {
		WLOG_INFO("pyrowave: no render node to pick a GPU by");
		return false;
	}

	app_info_.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info_.pApplicationName = "wraith";
	app_info_.apiVersion = VK_API_VERSION_1_3;
	instance_info_.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info_.pApplicationInfo = &app_info_;
	if (!vk_ok(vkCreateInstance(&instance_info_, nullptr, &instance_), "vkCreateInstance")) {
		instance_ = VK_NULL_HANDLE;
		return false;
	}

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance_, &count, nullptr);
	std::vector<VkPhysicalDevice> physicals(count);
	vkEnumeratePhysicalDevices(instance_, &count, physicals.data());
	for (VkPhysicalDevice candidate : physicals) {
		if (!has_extension(candidate, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) {
			continue;
		}
		VkPhysicalDeviceDrmPropertiesEXT drm = {};
		drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
		VkPhysicalDeviceProperties2 props = {};
		props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		props.pNext = &drm;
		vkGetPhysicalDeviceProperties2(candidate, &props);
		if (drm.hasRender && (dev_t)makedev(drm.renderMajor, drm.renderMinor) == node.st_rdev &&
			props.properties.apiVersion >= VK_API_VERSION_1_3 &&
			props.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
			physical_ = candidate;
			break;
		}
	}
	if (physical_ == VK_NULL_HANDLE) {
		WLOG_INFO("pyrowave: no Vulkan 1.3 GPU matches the render node");
		return false;
	}

	vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, families.data());
	bool found = false;
	for (uint32_t i = 0; i < count; i++) {
		// PyroWave wants a graphics queue on a borrowed device; one that
		// also computes covers both its paths.
		VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
		if ((families[i].queueFlags & want) == want) {
			queue_family_ = i;
			found = true;
			break;
		}
	}
	if (!found) {
		WLOG_INFO("pyrowave: GPU has no graphics+compute queue");
		return false;
	}

	// Everything the GPU supports, as PyroWave's own device does (its
	// shaders want subgroup size control, 16-bit ints, 8-bit storage,
	// fp16 where there is any), less robust buffer access, which only
	// costs.
	features13_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	features12_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features12_.pNext = &features13_;
	features11_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
	features11_.pNext = &features12_;
	features_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features_.pNext = &features11_;
	vkGetPhysicalDeviceFeatures2(physical_, &features_);
	features_.features.robustBufferAccess = VK_FALSE;
	if (!features13_.subgroupSizeControl || !features_.features.shaderInt16 ||
		!features12_.storageBuffer8BitAccess || !features12_.timelineSemaphore) {
		WLOG_INFO(
			"pyrowave: GPU lacks subgroup size control, 16-bit ints, 8-bit storage or timeline semaphores");
		return false;
	}

	dmabuf_import_ = std::all_of(std::begin(kDmabufExtensions), std::end(kDmabufExtensions),
		[this](const char *name) { return has_extension(physical_, name); });
	if (dmabuf_import_) {
		device_extensions_.assign(std::begin(kDmabufExtensions), std::end(kDmabufExtensions));
	} else {
		WLOG_INFO("pyrowave: the GPU can't import dmabufs; frames come in read back");
	}

	queue_info_.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info_.queueFamilyIndex = queue_family_;
	queue_info_.queueCount = 1;
	queue_info_.pQueuePriorities = &queue_priority_;
	device_info_.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info_.pNext = &features_;
	device_info_.queueCreateInfoCount = 1;
	device_info_.pQueueCreateInfos = &queue_info_;
	device_info_.enabledExtensionCount = (uint32_t)device_extensions_.size();
	device_info_.ppEnabledExtensionNames = device_extensions_.data();
	if (!vk_ok(vkCreateDevice(physical_, &device_info_, nullptr, &device_), "vkCreateDevice")) {
		device_ = VK_NULL_HANDLE;
		return false;
	}
	vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
	if (dmabuf_import_) {
		get_memory_fd_properties_ = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
			vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR"));
		import_semaphore_fd_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
			vkGetDeviceProcAddr(device_, "vkImportSemaphoreFdKHR"));
		dmabuf_import_ = get_memory_fd_properties_ && import_semaphore_fd_;
	}

	pyro_queue_.queue = queue_;
	pyro_queue_.familyIndex = queue_family_;
	pyro_queue_.index = 0;
	pyrowave_device_create_info info = {};
	info.GetInstanceProcAddr = vkGetInstanceProcAddr;
	info.instance = instance_;
	info.physical_device = physical_;
	info.device = device_;
	info.instance_create_info = &instance_info_;
	info.device_create_info = &device_info_;
	info.queue_info = &pyro_queue_;
	info.queue_info_count = 1;
	// No queue locks: PyroWave submits only inside its own calls, which run
	// on the one thread that encodes, and push()'s copy is submitted only
	// while that thread is idle.
	if (!pyro_ok(pyrowave_create_device(&info, &pyro_device_), "pyrowave_create_device")) {
		pyro_device_ = nullptr;
		return false;
	}
	pyrowave_device_set_queue_type(pyro_device_, VK_QUEUE_GRAPHICS_BIT);

	pyrowave_encoder_create_info enc = {};
	enc.device = pyro_device_;
	enc.width = (int)config_.width;
	enc.height = (int)config_.height;
	enc.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
	if (!pyro_ok(pyrowave_encoder_create(&enc, &pyro_encoder_), "pyrowave_encoder_create")) {
		pyro_encoder_ = nullptr;
		return false;
	}
	return true;
}

bool PyrowaveEncoder::create_resources() {
	pitch_ = config_.width * 4;
	VkDeviceSize size = (VkDeviceSize)pitch_ * config_.height;

	for (Staging &staging : staging_) {
		VkBufferCreateInfo buffer_info = {};
		buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buffer_info.size = size;
		buffer_info.usage =
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT; // dst: read_back_input()
		buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (!vk_ok(vkCreateBuffer(device_, &buffer_info, nullptr, &staging.buffer),
				"vkCreateBuffer(staging)")) {
			staging.buffer = VK_NULL_HANDLE;
			return false;
		}
		VkMemoryRequirements req;
		vkGetBufferMemoryRequirements(device_, staging.buffer, &req);
		// System memory, not VRAM through the PCIe BAR: the CPU writes a
		// whole frame here, and BAR writes ran at ~2 GB/s on an RX 590
		// (17 ms for a 4K frame). The GPU's copy then reads it at bus speed.
		const VkMemoryPropertyFlags host =
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		uint32_t type = find_memory_type(physical_, req.memoryTypeBits,
			host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (type == UINT32_MAX) {
			type = find_memory_type(physical_, req.memoryTypeBits, host, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		}
		if (type == UINT32_MAX) {
			type = find_memory_type(physical_, req.memoryTypeBits, host);
		}
		if (type == UINT32_MAX) {
			WLOG_ERROR("pyrowave: no host-visible memory for the staging buffer");
			return false;
		}
		VkMemoryAllocateInfo alloc = {};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = type;
		if (!vk_ok(vkAllocateMemory(device_, &alloc, nullptr, &staging.memory),
				"vkAllocateMemory(staging)")) {
			staging.memory = VK_NULL_HANDLE;
			return false;
		}
		vkBindBufferMemory(device_, staging.buffer, staging.memory, 0);
		void *mapped = nullptr;
		if (!vk_ok(vkMapMemory(device_, staging.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
				"vkMapMemory(staging)")) {
			return false;
		}
		staging.mapped = static_cast<uint8_t *>(mapped);
	}

	// XRGB8888 is B,G,R,X in memory: BGRA, whose alpha nothing reads.
	VkImageCreateInfo image_info = {};
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
	image_info.extent = {config_.width, config_.height, 1};
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // src: read_back_input()
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!vk_ok(vkCreateImage(device_, &image_info, nullptr, &image_), "vkCreateImage")) {
		image_ = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device_, image_, &req);
	uint32_t type = find_memory_type(physical_, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX) {
		WLOG_ERROR("pyrowave: no device-local memory for the input image");
		return false;
	}
	VkMemoryAllocateInfo alloc = {};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = type;
	if (!vk_ok(vkAllocateMemory(device_, &alloc, nullptr, &image_memory_), "vkAllocateMemory(image)")) {
		image_memory_ = VK_NULL_HANDLE;
		return false;
	}
	vkBindImageMemory(device_, image_, image_memory_, 0);

	VkCommandPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = queue_family_;
	if (!vk_ok(vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_), "vkCreateCommandPool")) {
		command_pool_ = VK_NULL_HANDLE;
		return false;
	}
	VkCommandBufferAllocateInfo cb_info = {};
	cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cb_info.commandPool = command_pool_;
	cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cb_info.commandBufferCount = 1;
	if (!vk_ok(vkAllocateCommandBuffers(device_, &cb_info, &command_buffer_), "vkAllocateCommandBuffers")) {
		return false;
	}

	VkSemaphoreTypeCreateInfo timeline = {};
	timeline.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	timeline.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo sem_info = {};
	sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	sem_info.pNext = &timeline;
	if (!vk_ok(vkCreateSemaphore(device_, &sem_info, nullptr, &upload_done_), "vkCreateSemaphore")) {
		upload_done_ = VK_NULL_HANDLE;
		return false;
	}

	if (dmabuf_import_) {
		cb_info.commandBufferCount = 1;
		if (!vk_ok(vkAllocateCommandBuffers(device_, &cb_info, &copy_command_buffer_),
				"vkAllocateCommandBuffers")) {
			return false;
		}
		VkFenceCreateInfo fence_info = {};
		fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (!vk_ok(vkCreateFence(device_, &fence_info, nullptr, &copy_done_), "vkCreateFence")) {
			copy_done_ = VK_NULL_HANDLE;
			return false;
		}
		VkSemaphoreCreateInfo binary = {};
		binary.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		if (!vk_ok(vkCreateSemaphore(device_, &binary, nullptr, &frame_ready_), "vkCreateSemaphore")) {
			frame_ready_ = VK_NULL_HANDLE;
			return false;
		}
		query_import_modifiers();
	}

	bitstream_.resize(bitrate_bps_.load() / 60 + kBitstreamSlack);
	return true;
}

void PyrowaveEncoder::query_import_modifiers() {
	import_modifiers_.clear();
	import_plane_counts_.clear();
	VkDrmFormatModifierPropertiesListEXT list = {};
	list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
	VkFormatProperties2 props = {};
	props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
	props.pNext = &list;
	vkGetPhysicalDeviceFormatProperties2(physical_, VK_FORMAT_B8G8R8A8_UNORM, &props);
	std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
	list.pDrmFormatModifierProperties = modifiers.data();
	vkGetPhysicalDeviceFormatProperties2(physical_, VK_FORMAT_B8G8R8A8_UNORM, &props);

	for (const VkDrmFormatModifierPropertiesEXT &modifier : modifiers) {
		if (!(modifier.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
			continue;
		}
		// Importable from a dmabuf, as a copy source, at this size.
		VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {};
		modifier_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
		modifier_info.drmFormatModifier = modifier.drmFormatModifier;
		modifier_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkPhysicalDeviceExternalImageFormatInfo external_info = {};
		external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
		external_info.pNext = &modifier_info;
		external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkPhysicalDeviceImageFormatInfo2 info = {};
		info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
		info.pNext = &external_info;
		info.format = VK_FORMAT_B8G8R8A8_UNORM;
		info.type = VK_IMAGE_TYPE_2D;
		info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		VkExternalImageFormatProperties external_props = {};
		external_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
		VkImageFormatProperties2 out = {};
		out.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
		out.pNext = &external_props;
		if (vkGetPhysicalDeviceImageFormatProperties2(physical_, &info, &out) != VK_SUCCESS) {
			continue;
		}
		if (!(external_props.externalMemoryProperties.externalMemoryFeatures &
				VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
			out.imageFormatProperties.maxExtent.width < config_.width ||
			out.imageFormatProperties.maxExtent.height < config_.height) {
			continue;
		}
		import_modifiers_.push_back(modifier.drmFormatModifier);
		import_plane_counts_.push_back(modifier.drmFormatModifierPlaneCount);
	}
	if (import_modifiers_.empty()) {
		WLOG_INFO("pyrowave: no DRM modifier imports as a B8G8R8A8 copy source; frames come in read back");
		dmabuf_import_ = false;
	}
}

std::vector<uint64_t> PyrowaveEncoder::supported_import_modifiers(uint32_t drm_format) const {
	if (wants_cpu_frame() || (drm_format != DRM_FORMAT_XRGB8888 && drm_format != DRM_FORMAT_ARGB8888)) {
		return {};
	}
	return import_modifiers_;
}

bool PyrowaveEncoder::push(const DmabufFrame &frame, int64_t pts_us) {
	if (!pyro_encoder_ || wants_cpu_frame() || frame.n_planes < 1 || (uint32_t)frame.width != config_.width ||
		(uint32_t)frame.height != config_.height) {
		return false;
	}
	// Only a layout the driver said it imports (supported_import_modifiers()).
	// Anything else -- DRM_FORMAT_MOD_INVALID above all, an implicit layout
	// Vulkan can't describe -- would be read with a made-up layout, and a
	// copy from that hangs the GPU.
	// And with exactly the planes the driver says that layout has: the
	// import describes each one, and a wrong count is undefined.
	auto offered = std::find(import_modifiers_.begin(), import_modifiers_.end(), frame.modifier);
	if (offered == import_modifiers_.end() ||
		import_plane_counts_[offered - import_modifiers_.begin()] != (uint32_t)frame.n_planes) {
		WLOG_ERROR(
			"pyrowave: the capture handed over a dmabuf with modifier 0x%016llx and %d plane(s), which "
			"wasn't offered; reading frames back from now on",
			(unsigned long long)frame.modifier, frame.n_planes);
		import_failed_ = true;
		return false;
	}
	// The image is written here, on the caller's thread: only once the
	// worker has nothing in flight, so it is never mid-encode -- and the
	// queue never sees two threads at once.
	if (!wait_worker_idle()) {
		return false;
	}
	int64_t t0 = monotonic_now_us();
	Import *import = nullptr;
	// XRGB8888 and ARGB8888 are both B,G,R,X/A in memory: B8G8R8A8, whose
	// alpha nothing reads.
	bool format_ok = frame.format == DRM_FORMAT_XRGB8888 || frame.format == DRM_FORMAT_ARGB8888;
	if (!format_ok || !(import = find_import(frame)) || !copy_imported(frame, *import)) {
		// The caller asks wants_cpu_frame() per frame and reads the next one
		// back; this one is dropped like any failed push.
		WLOG_ERROR("pyrowave: can't take a %dx%d dmabuf (format 0x%08x, modifier 0x%016llx); reading frames "
				   "back from now on",
			frame.width, frame.height, frame.format, (unsigned long long)frame.modifier);
		import_failed_ = true;
		return false;
	}
	return submit_job(kSlotImported, pts_us, monotonic_now_us() - t0);
}

PyrowaveEncoder::Import *PyrowaveEncoder::find_import(const DmabufFrame &frame) {
	// Planes in more than one buffer would need a disjoint import; a
	// compositor's XRGB8888 never has them.
	struct stat buffer;
	if (fstat(frame.fd[0], &buffer) != 0) {
		return nullptr;
	}
	for (int i = 1; i < frame.n_planes && i < DmabufFrame::kMaxPlanes; i++) {
		struct stat plane;
		if (fstat(frame.fd[i], &plane) != 0 || plane.st_ino != buffer.st_ino) {
			return nullptr;
		}
	}
	for (auto it = imports_.begin(); it != imports_.end(); ++it) {
		if (it->inode != buffer.st_ino) {
			continue;
		}
		bool same = it->width == frame.width && it->height == frame.height &&
			it->modifier == frame.modifier && it->n_planes == frame.n_planes;
		for (int i = 0; same && i < frame.n_planes && i < DmabufFrame::kMaxPlanes; i++) {
			same = it->offset[i] == frame.offset[i] && it->stride[i] == frame.stride[i];
		}
		if (same) {
			it->last_used = ++import_clock_;
			return &*it;
		}
		release_import(*it); // the same buffer, laid out anew
		imports_.erase(it);
		break;
	}
	if (imports_.size() >= kMaxImports) {
		auto oldest = std::min_element(imports_.begin(), imports_.end(),
			[](const Import &a, const Import &b) { return a.last_used < b.last_used; });
		release_import(*oldest);
		imports_.erase(oldest);
	}
	Import import;
	if (!import_dmabuf(frame, buffer.st_ino, &import)) {
		release_import(import);
		return nullptr;
	}
	import.last_used = ++import_clock_;
	imports_.push_back(import);
	return &imports_.back();
}

bool PyrowaveEncoder::import_dmabuf(const DmabufFrame &frame, ino_t inode, Import *out) {
	out->inode = inode;
	out->width = frame.width;
	out->height = frame.height;
	out->modifier = frame.modifier;
	out->n_planes = std::min(frame.n_planes, DmabufFrame::kMaxPlanes);
	VkSubresourceLayout layouts[DmabufFrame::kMaxPlanes] = {};
	for (int i = 0; i < out->n_planes; i++) {
		out->offset[i] = frame.offset[i];
		out->stride[i] = frame.stride[i];
		layouts[i].offset = frame.offset[i];
		layouts[i].rowPitch = frame.stride[i];
	}

	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {};
	modifier_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
	modifier_info.drmFormatModifier = frame.modifier;
	modifier_info.drmFormatModifierPlaneCount = (uint32_t)out->n_planes;
	modifier_info.pPlaneLayouts = layouts;
	VkExternalMemoryImageCreateInfo external = {};
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.pNext = &modifier_info;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkImageCreateInfo image_info = {};
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext = &external;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_B8G8R8A8_UNORM;
	image_info.extent = {(uint32_t)frame.width, (uint32_t)frame.height, 1};
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!vk_ok(vkCreateImage(device_, &image_info, nullptr, &out->image), "vkCreateImage(dmabuf)")) {
		out->image = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryFdPropertiesKHR fd_props = {};
	fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
	if (!vk_ok(get_memory_fd_properties_(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, frame.fd[0],
				   &fd_props),
			"vkGetMemoryFdPropertiesKHR")) {
		return false;
	}
	VkMemoryRequirements req;
	vkGetImageMemoryRequirements(device_, out->image, &req);
	uint32_t type = find_memory_type(physical_, req.memoryTypeBits & fd_props.memoryTypeBits, 0);
	if (type == UINT32_MAX) {
		WLOG_ERROR("pyrowave: no memory type fits the dmabuf");
		return false;
	}
	// The import takes the fd it is given; the caller keeps its own.
	int fd = fcntl(frame.fd[0], F_DUPFD_CLOEXEC, 0);
	if (fd < 0) {
		return false;
	}
	VkMemoryDedicatedAllocateInfo dedicated = {};
	dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated.image = out->image;
	VkImportMemoryFdInfoKHR import_info = {};
	import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import_info.pNext = &dedicated;
	import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	import_info.fd = fd;
	VkMemoryAllocateInfo alloc = {};
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.pNext = &import_info;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = type;
	if (!vk_ok(vkAllocateMemory(device_, &alloc, nullptr, &out->memory), "vkAllocateMemory(dmabuf)")) {
		::close(fd);
		out->memory = VK_NULL_HANDLE;
		return false;
	}
	return vk_ok(vkBindImageMemory(device_, out->image, out->memory, 0), "vkBindImageMemory(dmabuf)");
}

void PyrowaveEncoder::release_import(Import &import) {
	if (import.image != VK_NULL_HANDLE) {
		vkDestroyImage(device_, import.image, nullptr);
	}
	if (import.memory != VK_NULL_HANDLE) {
		vkFreeMemory(device_, import.memory, nullptr);
	}
	import = Import{};
}

bool PyrowaveEncoder::copy_imported(const DmabufFrame &frame, const Import &import) {
	// The producer's writes still pending on the buffer (the compositor may
	// hand a frame over before its GPU has finished drawing it), as a
	// semaphore the copy waits on. amdgpu happens to order work on a shared
	// buffer by itself, but that is the kernel driver's choice, not
	// Vulkan's promise. A
	// kernel without the ioctl (before 6.0) gets no wait, which is what the
	// read-back path relies on anyway.
	bool wait_frame = false;
	dma_buf_export_sync_file exported = {};
	exported.flags = DMA_BUF_SYNC_READ;
	exported.fd = -1;
	if (ioctl(frame.fd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &exported) == 0 && exported.fd >= 0) {
		VkImportSemaphoreFdInfoKHR sync = {};
		sync.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
		sync.semaphore = frame_ready_;
		sync.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
		sync.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		sync.fd = exported.fd;
		if (import_semaphore_fd_(device_, &sync) == VK_SUCCESS) {
			wait_frame = true; // the semaphore owns the fd now
		} else {
			::close(exported.fd);
		}
	}

	vkResetCommandBuffer(copy_command_buffer_, 0);
	VkCommandBufferBeginInfo begin = {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(copy_command_buffer_, &begin);

	// The dmabuf, acquired from its producer (foreign queue family) with
	// its contents kept (GENERAL, never UNDEFINED); the input image, whose
	// last contents nothing needs.
	VkImageMemoryBarrier acquire[2] = {};
	acquire[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	acquire[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	acquire[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	acquire[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	acquire[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	acquire[0].dstQueueFamilyIndex = queue_family_;
	acquire[0].image = import.image;
	acquire[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	acquire[1] = acquire[0];
	acquire[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	acquire[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	acquire[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	acquire[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	acquire[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	acquire[1].image = image_;
	vkCmdPipelineBarrier(copy_command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, acquire);

	VkImageCopy region = {};
	region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.extent = {config_.width, config_.height, 1};
	vkCmdCopyImage(copy_command_buffer_, import.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image_,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	// The dmabuf back to its producer; the input image to the encode's
	// sampled reads, as the upload leaves it.
	VkImageMemoryBarrier release[2] = {};
	release[0] = acquire[0];
	release[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	release[0].dstAccessMask = 0;
	release[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	release[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	release[0].srcQueueFamilyIndex = queue_family_;
	release[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	release[1] = acquire[1];
	release[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	release[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	release[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	release[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(copy_command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
		nullptr, 2, release);
	vkEndCommandBuffer(copy_command_buffer_);

	// Signals the timeline the encode acquires, like the upload does.
	uint64_t wait_value = 0;
	uint64_t signal_value = ++upload_value_;
	VkTimelineSemaphoreSubmitInfo timeline = {};
	timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	timeline.waitSemaphoreValueCount = wait_frame ? 1 : 0;
	timeline.pWaitSemaphoreValues = &wait_value; // ignored: frame_ready_ is binary
	timeline.signalSemaphoreValueCount = 1;
	timeline.pSignalSemaphoreValues = &signal_value;
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.pNext = &timeline;
	submit.waitSemaphoreCount = wait_frame ? 1 : 0;
	submit.pWaitSemaphores = &frame_ready_;
	submit.pWaitDstStageMask = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &copy_command_buffer_;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &upload_done_;
	if (!vk_ok(vkQueueSubmit(queue_, 1, &submit, copy_done_), "vkQueueSubmit(copy)")) {
		return false;
	}
	// Done before the caller can hand the buffer back to the compositor.
	VkResult waited = vkWaitForFences(device_, 1, &copy_done_, VK_TRUE, kCopyTimeoutNs);
	vkResetFences(device_, 1, &copy_done_);
	return vk_ok(waited, "vkWaitForFences(copy)");
}

bool PyrowaveEncoder::push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride,
	int64_t pts_us, const DamageRegion *) {
	if (!pyro_encoder_ || width != config_.width || height != config_.height) {
		return false;
	}

	// The slot the worker isn't uploading: it only ever holds the one
	// submitted last, and slots alternate.
	int slot = next_slot_;
	int64_t t0 = monotonic_now_us();
	uint8_t *dst = staging_[slot].mapped;
	if (stride == pitch_) {
		memcpy(dst, data, (size_t)pitch_ * height);
	} else {
		for (uint32_t y = 0; y < height; y++) {
			memcpy(dst + (size_t)y * pitch_, data + (size_t)y * stride, pitch_);
		}
	}
	int64_t copy_us = monotonic_now_us() - t0;
	return submit_job(slot, pts_us, copy_us);
}

bool PyrowaveEncoder::wait_worker_idle() {
	if (!asynchronous_) {
		return true;
	}
	std::unique_lock<std::mutex> lock(mutex_);
	cv_.wait(lock, [this] { return job_slot_ < 0 || stop_; });
	return !stop_;
}

bool PyrowaveEncoder::submit_job(int slot, int64_t pts_us, int64_t copy_us) {
	if (!asynchronous_) {
		int64_t t1 = monotonic_now_us();
		std::vector<EncodedPacket> out;
		bool ok = encode_slot(slot, pts_us, &out);
		if (ok) {
			note_timing(copy_us, monotonic_now_us() - t1, out.empty() ? 0 : out.front().data.size());
			for (EncodedPacket &packet : out) {
				ready_.push_back(std::move(packet));
			}
		}
		return ok;
	}

	{
		std::unique_lock<std::mutex> lock(mutex_);
		cv_.wait(lock, [this] { return job_slot_ < 0 || stop_; });
		if (stop_) {
			return false;
		}
		job_slot_ = slot;
		job_pts_us_ = pts_us;
		job_copy_us_ = copy_us;
	}
	if (slot != kSlotImported) {
		next_slot_ ^= 1;
	}
	cv_.notify_all();
	return true;
}

bool PyrowaveEncoder::encode_slot(int slot, int64_t pts_us, std::vector<EncodedPacket> *out) {
	uint64_t signal_value = upload_value_; // push()'s copy, if it filled the image
	if (slot != kSlotImported && !upload_slot(slot, &signal_value)) {
		return false;
	}
	return encode_image(signal_value, pts_us, out);
}

bool PyrowaveEncoder::upload_slot(int slot, uint64_t *signal_value_out) {
	// The upload: staging buffer -> image, then hand the image to the
	// encode's sampled reads. The image's last contents are never needed,
	// and the previous encode finished before its packet was read back,
	// so it starts from UNDEFINED every frame.
	vkResetCommandBuffer(command_buffer_, 0);
	VkCommandBufferBeginInfo begin = {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(command_buffer_, &begin);

	VkImageMemoryBarrier to_transfer = {};
	to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	to_transfer.srcAccessMask = 0;
	to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_transfer.image = image_;
	to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_transfer);

	VkBufferImageCopy region = {};
	region.bufferRowLength = config_.width;
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {config_.width, config_.height, 1};
	vkCmdCopyBufferToImage(command_buffer_, staging_[slot].buffer, image_,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	VkImageMemoryBarrier to_sampled = to_transfer;
	to_sampled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_sampled.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	to_sampled.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_sampled.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_sampled);
	vkEndCommandBuffer(command_buffer_);

	uint64_t signal_value = ++upload_value_;
	VkTimelineSemaphoreSubmitInfo timeline = {};
	timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	timeline.signalSemaphoreValueCount = 1;
	timeline.pSignalSemaphoreValues = &signal_value;
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.pNext = &timeline;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command_buffer_;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &upload_done_;
	if (!vk_ok(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit(upload)")) {
		return false;
	}
	*signal_value_out = signal_value;
	return true;
}

bool PyrowaveEncoder::encode_image(uint64_t signal_value, int64_t pts_us, std::vector<EncodedPacket> *out) {
	pyrowave_gpu_sync_operation acquire = {};
	acquire.sync.semaphore = upload_done_;
	acquire.sync.value = signal_value;

	pyrowave_scaled_encode_info scaled = {};
	scaled.view.image = image_;
	scaled.view.width = config_.width;
	scaled.view.height = config_.height;
	scaled.view.image_format = VK_FORMAT_B8G8R8A8_UNORM;
	scaled.view.view_format = VK_FORMAT_B8G8R8A8_UNORM;
	scaled.view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	scaled.view.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
	scaled.view.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	// SDR in and out: full-range BT.709, centre-sited chroma -- what
	// spectre's sampler for this codec expects.
	scaled.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	scaled.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	scaled.intermediate_plane_format = VK_FORMAT_R8_UNORM;
	scaled.ycbcr_chroma_midpoint = 0.5f;

	size_t budget = bitrate_bps_.load() * (uint64_t)config_.framerate_den /
		std::max<uint32_t>(1, config_.framerate_num) / 8;
	if (bitstream_.size() < budget + kBitstreamSlack) {
		bitstream_.resize(budget + kBitstreamSlack);
	}
	pyrowave_rate_control rate = {budget};
	if (!pyro_ok(
			pyrowave_encoder_encode_gpu_scaled_synchronous(pyro_encoder_, &acquire, nullptr, &scaled, &rate),
			"encode")) {
		return false;
	}
	// One packet holding the whole frame: the boundary is the buffer. GDP
	// slices it into datagrams like any codec's frame (gdp-spec.md §9.2).
	size_t packets = 0;
	if (!pyro_ok(pyrowave_encoder_compute_num_packets(pyro_encoder_, bitstream_.size(), &packets),
			"compute_num_packets")) {
		return false;
	}
	pyrowave_packet packet = {};
	size_t written = 0;
	if (packets != 1 ||
		!pyro_ok(pyrowave_encoder_packetize(pyro_encoder_, &packet, bitstream_.size(), &written,
					 bitstream_.data(), bitstream_.size()),
			"packetize") ||
		written != 1) {
		WLOG_ERROR("pyrowave: frame didn't pack into one packet (%zu)", packets);
		return false;
	}

	EncodedPacket out_packet;
	out_packet.pts_us = pts_us;
	out_packet.keyframe = true;
	out_packet.data.assign(bitstream_.data() + packet.offset,
		bitstream_.data() + packet.offset + packet.size);
	out->push_back(std::move(out_packet));
	return true;
}

void PyrowaveEncoder::worker_loop() {
	for (;;) {
		int slot;
		int64_t pts_us, copy_us;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return job_slot_ >= 0 || stop_; });
			if (stop_) {
				return;
			}
			slot = job_slot_;
			pts_us = job_pts_us_;
			copy_us = job_copy_us_;
		}
		int64_t t0 = monotonic_now_us();
		std::vector<EncodedPacket> out;
		if (encode_slot(slot, pts_us, &out)) {
			note_timing(copy_us, monotonic_now_us() - t0, out.empty() ? 0 : out.front().data.size());
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (EncodedPacket &packet : out) {
				ready_.push_back(std::move(packet));
			}
			job_slot_ = -1;
		}
		cv_.notify_all();
		if (!out.empty()) {
			uint64_t one = 1;
			(void)!write(wake_fd_, &one, sizeof(one));
		}
	}
}

bool PyrowaveEncoder::read_back_input(std::vector<uint8_t> *out) {
	if (!pyro_encoder_ || copy_done_ == VK_NULL_HANDLE || !wait_worker_idle()) {
		return false;
	}
	// Into staging slot 0, which nothing is using with the worker idle.
	vkResetCommandBuffer(copy_command_buffer_, 0);
	VkCommandBufferBeginInfo begin = {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(copy_command_buffer_, &begin);
	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image_;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(copy_command_buffer_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	VkBufferImageCopy region = {};
	region.bufferRowLength = config_.width;
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {config_.width, config_.height, 1};
	vkCmdCopyImageToBuffer(copy_command_buffer_, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		staging_[0].buffer, 1, &region);
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(copy_command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	vkEndCommandBuffer(copy_command_buffer_);
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &copy_command_buffer_;
	if (!vk_ok(vkQueueSubmit(queue_, 1, &submit, copy_done_), "vkQueueSubmit(read back)")) {
		return false;
	}
	VkResult waited = vkWaitForFences(device_, 1, &copy_done_, VK_TRUE, kCopyTimeoutNs);
	vkResetFences(device_, 1, &copy_done_);
	if (!vk_ok(waited, "vkWaitForFences(read back)")) {
		return false;
	}
	out->assign(staging_[0].mapped, staging_[0].mapped + (size_t)pitch_ * config_.height);
	return true;
}

void PyrowaveEncoder::note_timing(int64_t copy_us, int64_t encode_us, size_t bytes) {
	constexpr int64_t kTimingWindowUs = 5'000'000;
	int64_t now = monotonic_now_us();
	if (timing_.window_start_us == 0) {
		timing_.window_start_us = now;
	}
	timing_.frames++;
	timing_.copy_sum_us += copy_us;
	timing_.copy_max_us = std::max(timing_.copy_max_us, copy_us);
	timing_.encode_sum_us += encode_us;
	timing_.encode_max_us = std::max(timing_.encode_max_us, encode_us);
	timing_.bytes += bytes;
	int64_t elapsed = now - timing_.window_start_us;
	if (elapsed < kTimingWindowUs) {
		return;
	}
	double n = (double)timing_.frames;
	WLOG_INFO("pyrowave: last %.1fs: %u frames (%.1f fps), %.0f KB avg, copy %.1f ms avg / %.1f max, "
			  "upload+encode %.1f ms avg / %.1f max",
		elapsed / 1e6, timing_.frames, n * 1e6 / (double)elapsed, timing_.bytes / n / 1000.0,
		timing_.copy_sum_us / n / 1000.0, timing_.copy_max_us / 1000.0, timing_.encode_sum_us / n / 1000.0,
		timing_.encode_max_us / 1000.0);
	timing_ = Timing{};
	timing_.window_start_us = now;
}

void PyrowaveEncoder::set_bitrate(uint32_t bitrate_bps) {
	// Read by the next encode, whichever thread runs it.
	bitrate_bps_ = bitrate_bps;
}

std::vector<EncodedPacket> PyrowaveEncoder::poll() {
	std::vector<EncodedPacket> out;
	if (asynchronous_) {
		uint64_t count;
		(void)!read(wake_fd_, &count, sizeof(count));
		std::lock_guard<std::mutex> lock(mutex_);
		out.swap(ready_);
		return out;
	}
	out.swap(ready_);
	return out;
}

void PyrowaveEncoder::close() {
	if (worker_.joinable()) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
		}
		cv_.notify_all();
		worker_.join();
		job_slot_ = -1;
	}
	if (wake_fd_ >= 0) {
		::close(wake_fd_);
		wake_fd_ = -1;
	}
	destroy();
}

void PyrowaveEncoder::destroy() {
	if (device_ != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(device_);
	}
	if (pyro_encoder_) {
		pyrowave_encoder_destroy(pyro_encoder_);
		pyro_encoder_ = nullptr;
	}
	if (pyro_device_) {
		pyrowave_device_destroy(pyro_device_);
		pyro_device_ = nullptr;
	}
	if (device_ != VK_NULL_HANDLE) {
		for (Import &import : imports_) {
			release_import(import);
		}
		imports_.clear();
		if (copy_done_ != VK_NULL_HANDLE) {
			vkDestroyFence(device_, copy_done_, nullptr);
		}
		if (frame_ready_ != VK_NULL_HANDLE) {
			vkDestroySemaphore(device_, frame_ready_, nullptr);
		}
		for (Staging &staging : staging_) {
			if (staging.buffer != VK_NULL_HANDLE) {
				vkDestroyBuffer(device_, staging.buffer, nullptr);
			}
			if (staging.memory != VK_NULL_HANDLE) {
				vkFreeMemory(device_, staging.memory, nullptr);
			}
			staging = Staging{};
		}
		if (image_ != VK_NULL_HANDLE) {
			vkDestroyImage(device_, image_, nullptr);
		}
		if (image_memory_ != VK_NULL_HANDLE) {
			vkFreeMemory(device_, image_memory_, nullptr);
		}
		if (upload_done_ != VK_NULL_HANDLE) {
			vkDestroySemaphore(device_, upload_done_, nullptr);
		}
		if (command_pool_ != VK_NULL_HANDLE) {
			vkDestroyCommandPool(device_, command_pool_, nullptr);
		}
		vkDestroyDevice(device_, nullptr);
	}
	if (instance_ != VK_NULL_HANDLE) {
		vkDestroyInstance(instance_, nullptr);
	}
	image_ = VK_NULL_HANDLE;
	image_memory_ = VK_NULL_HANDLE;
	upload_done_ = VK_NULL_HANDLE;
	command_pool_ = VK_NULL_HANDLE;
	command_buffer_ = VK_NULL_HANDLE;
	copy_command_buffer_ = VK_NULL_HANDLE;
	copy_done_ = VK_NULL_HANDLE;
	frame_ready_ = VK_NULL_HANDLE;
	dmabuf_import_ = false;
	import_failed_ = false;
	device_extensions_.clear();
	import_modifiers_.clear();
	import_plane_counts_.clear();
	get_memory_fd_properties_ = nullptr;
	import_semaphore_fd_ = nullptr;
	queue_ = VK_NULL_HANDLE;
	device_ = VK_NULL_HANDLE;
	physical_ = VK_NULL_HANDLE;
	instance_ = VK_NULL_HANDLE;
}

} // namespace wraith
