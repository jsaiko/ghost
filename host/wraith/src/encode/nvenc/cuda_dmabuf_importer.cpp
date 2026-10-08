// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/nvenc/cuda_dmabuf_importer.hpp"

#include "util/log.hpp"

#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace wraith {

namespace {

// cuImportExternalMemory's "this is a dedicated allocation" flag; the
// vendored dynlink_cuda.h doesn't define it.
constexpr unsigned kCudaExternalMemoryDedicated = 0x1;

// How long copy() waits for its own GPU copy before giving up on the frame.
constexpr uint64_t kCopyTimeoutNs = 1'000'000'000;

// The packed RGB formats wraith's capture paths hand over (the same four
// VA-API's import takes, see va_fourcc_from_drm()). The Vulkan format is
// only a label for a byte-preserving copy -- any 32-bit format with the
// same memory layout would copy identically.
VkFormat vk_format_from_drm(uint32_t drm_format) {
	switch (drm_format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
	default: return VK_FORMAT_UNDEFINED;
	}
}

bool vk_ok(VkResult result, const char *what) {
	if (result != VK_SUCCESS) {
		WLOG_ERROR("nvenc: %s failed (VkResult %d)", what, (int)result);
		return false;
	}
	return true;
}

bool has_extension(const std::vector<VkExtensionProperties> &available, const char *name) {
	for (const VkExtensionProperties &ext : available) {
		if (strcmp(ext.extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

// Every plane of an imported frame must live in the one dmabuf copy()
// imports (fd[0]); the fds themselves can differ and still name it.
bool same_dmabuf(int a, int b) {
	if (a == b) {
		return true;
	}
	struct stat sa, sb;
	return fstat(a, &sa) == 0 && fstat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

} // namespace

CudaDmabufImporter::~CudaDmabufImporter() {
	destroy();
}

bool CudaDmabufImporter::init(CUdevice device, uint32_t width, uint32_t height, int buffers) {
	width_ = width;
	height_ = height;
	// NVENC is happiest with a 256-byte-aligned pitch, and it keeps every
	// row of the copy's destination aligned for the transfer engine.
	pitch_ = (width * 4 + 255) / 256 * 256;

	if (!create_instance_and_device(device)) {
		destroy();
		return false;
	}
	shared_.resize((size_t)std::max(buffers, 1));
	for (Shared &shared : shared_) {
		if (!create_shared_buffer(&shared)) {
			destroy();
			return false;
		}
	}
	return true;
}

bool CudaDmabufImporter::create_instance_and_device(CUdevice device) {
	const NvencRuntime *runtime = nvenc_runtime();
	CUuuid cuda_uuid = {};
	auto get_uuid =
		runtime->cu->cuDeviceGetUuid_v2 ? runtime->cu->cuDeviceGetUuid_v2 : runtime->cu->cuDeviceGetUuid;
	if (!get_uuid || get_uuid(&cuda_uuid, device) != CUDA_SUCCESS) {
		WLOG_INFO("nvenc: can't read the CUDA device's UUID, no zero-copy import");
		return false;
	}

	VkApplicationInfo app = {};
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "wraith";
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo instance_info = {};
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &app;
	if (!vk_ok(vkCreateInstance(&instance_info, nullptr, &instance_), "vkCreateInstance")) {
		instance_ = VK_NULL_HANDLE;
		return false;
	}

	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance_, &count, nullptr);
	std::vector<VkPhysicalDevice> physicals(count);
	vkEnumeratePhysicalDevices(instance_, &count, physicals.data());
	for (VkPhysicalDevice candidate : physicals) {
		VkPhysicalDeviceIDProperties id = {};
		id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
		VkPhysicalDeviceProperties2 props = {};
		props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		props.pNext = &id;
		vkGetPhysicalDeviceProperties2(candidate, &props);
		static_assert(sizeof(cuda_uuid.bytes) == VK_UUID_SIZE, "CUDA and Vulkan device UUIDs differ in size");
		if (memcmp(id.deviceUUID, cuda_uuid.bytes, VK_UUID_SIZE) == 0 &&
			props.properties.apiVersion >= VK_API_VERSION_1_2) {
			physical_ = candidate;
			break;
		}
	}
	if (physical_ == VK_NULL_HANDLE) {
		WLOG_INFO("nvenc: no Vulkan 1.2 device matches the CUDA device, no zero-copy import");
		return false;
	}

	vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> available(count);
	vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, available.data());
	std::vector<const char *> extensions = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	};
	for (const char *name : extensions) {
		if (!has_extension(available, name)) {
			WLOG_INFO("nvenc: Vulkan device lacks %s, no zero-copy import", name);
			return false;
		}
	}
	have_foreign_queue_ = has_extension(available, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
	if (have_foreign_queue_) {
		extensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
	}
	bool have_semaphore_fd = has_extension(available, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
	if (have_semaphore_fd) {
		extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
	}

	// Any family that can transfer: graphics and compute imply it.
	vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(physical_, &count, families.data());
	bool found_family = false;
	for (uint32_t i = 0; i < count; i++) {
		if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) {
			queue_family_ = i;
			found_family = true;
			break;
		}
	}
	if (!found_family) {
		WLOG_INFO("nvenc: Vulkan device has no transfer queue, no zero-copy import");
		return false;
	}

	float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info = {};
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = queue_family_;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	VkDeviceCreateInfo device_info = {};
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = (uint32_t)extensions.size();
	device_info.ppEnabledExtensionNames = extensions.data();
	if (!vk_ok(vkCreateDevice(physical_, &device_info, nullptr, &device_), "vkCreateDevice")) {
		device_ = VK_NULL_HANDLE;
		return false;
	}
	vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

	get_memory_fd_ = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device_, "vkGetMemoryFdKHR");
	get_memory_fd_properties_ =
		(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(device_, "vkGetMemoryFdPropertiesKHR");
	if (!get_memory_fd_ || !get_memory_fd_properties_) {
		WLOG_INFO("nvenc: Vulkan external-memory entry points missing, no zero-copy import");
		return false;
	}

	if (have_semaphore_fd) {
		VkPhysicalDeviceExternalSemaphoreInfo sem_query = {};
		sem_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
		sem_query.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		VkExternalSemaphoreProperties sem_props = {};
		sem_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
		vkGetPhysicalDeviceExternalSemaphoreProperties(physical_, &sem_query, &sem_props);
		import_semaphore_fd_ =
			(PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(device_, "vkImportSemaphoreFdKHR");
		if ((sem_props.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) &&
			import_semaphore_fd_) {
			VkSemaphoreCreateInfo sem_info = {};
			sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			if (vkCreateSemaphore(device_, &sem_info, nullptr, &implicit_fence_) != VK_SUCCESS) {
				implicit_fence_ = VK_NULL_HANDLE;
			}
		}
	}
	if (implicit_fence_ == VK_NULL_HANDLE) {
		WLOG_INFO(
			"nvenc: Vulkan device can't import sync files; dmabuf copies won't wait on implicit fences");
	}

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
	VkFenceCreateInfo fence_info = {};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (!vk_ok(vkCreateFence(device_, &fence_info, nullptr, &fence_), "vkCreateFence")) {
		fence_ = VK_NULL_HANDLE;
		return false;
	}
	return true;
}

bool CudaDmabufImporter::create_shared_buffer(Shared *shared) {
	VkDeviceSize size = (VkDeviceSize)pitch_ * height_;

	VkExternalMemoryBufferCreateInfo external_info = {};
	external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
	external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkBufferCreateInfo buffer_info = {};
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.pNext = &external_info;
	buffer_info.size = size;
	buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (!vk_ok(vkCreateBuffer(device_, &buffer_info, nullptr, &shared->buffer), "vkCreateBuffer(shared)")) {
		shared->buffer = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device_, shared->buffer, &requirements);
	VkPhysicalDeviceMemoryProperties memory_props;
	vkGetPhysicalDeviceMemoryProperties(physical_, &memory_props);
	uint32_t type_index = UINT32_MAX;
	for (uint32_t i = 0; i < memory_props.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) &&
			(memory_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			type_index = i;
			break;
		}
	}
	if (type_index == UINT32_MAX) {
		WLOG_ERROR("nvenc: no device-local memory type for the shared buffer");
		return false;
	}

	VkMemoryDedicatedAllocateInfo dedicated = {};
	dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated.buffer = shared->buffer;
	VkExportMemoryAllocateInfo export_info = {};
	export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
	export_info.pNext = &dedicated;
	export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkMemoryAllocateInfo alloc_info = {};
	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = &export_info;
	alloc_info.allocationSize = requirements.size;
	alloc_info.memoryTypeIndex = type_index;
	if (!vk_ok(vkAllocateMemory(device_, &alloc_info, nullptr, &shared->memory),
			"vkAllocateMemory(shared)")) {
		shared->memory = VK_NULL_HANDLE;
		return false;
	}
	if (!vk_ok(vkBindBufferMemory(device_, shared->buffer, shared->memory, 0),
			"vkBindBufferMemory(shared)")) {
		return false;
	}

	VkMemoryGetFdInfoKHR fd_info = {};
	fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
	fd_info.memory = shared->memory;
	fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	int fd = -1;
	if (!vk_ok(get_memory_fd_(device_, &fd_info, &fd), "vkGetMemoryFdKHR")) {
		return false;
	}

	const NvencRuntime *runtime = nvenc_runtime();
	if (!runtime->cu->cuImportExternalMemory || !runtime->cu->cuExternalMemoryGetMappedBuffer ||
		!runtime->cu->cuDestroyExternalMemory) {
		WLOG_INFO("nvenc: driver lacks CUDA external-memory import, no zero-copy import");
		::close(fd);
		return false;
	}
	CUDA_EXTERNAL_MEMORY_HANDLE_DESC handle_desc = {};
	handle_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
	handle_desc.handle.fd = fd;
	handle_desc.size = requirements.size;
	handle_desc.flags = kCudaExternalMemoryDedicated;
	CUresult cr = runtime->cu->cuImportExternalMemory(&shared->cuda_memory, &handle_desc);
	if (cr != CUDA_SUCCESS) {
		// Only a successful import takes ownership of the fd.
		::close(fd);
		shared->cuda_memory = nullptr;
		WLOG_ERROR("nvenc: cuImportExternalMemory failed: %s", cuda_error_string(cr).c_str());
		return false;
	}
	CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer_desc = {};
	buffer_desc.offset = 0;
	buffer_desc.size = size;
	cr = runtime->cu->cuExternalMemoryGetMappedBuffer(&shared->cuda_ptr, shared->cuda_memory, &buffer_desc);
	if (cr != CUDA_SUCCESS) {
		shared->cuda_ptr = 0;
		WLOG_ERROR("nvenc: cuExternalMemoryGetMappedBuffer failed: %s", cuda_error_string(cr).c_str());
		return false;
	}
	return true;
}

std::vector<uint64_t> CudaDmabufImporter::import_modifiers(uint32_t drm_format) const {
	std::vector<uint64_t> result;
	VkFormat format = vk_format_from_drm(drm_format);
	if (format == VK_FORMAT_UNDEFINED || physical_ == VK_NULL_HANDLE) {
		return result;
	}

	VkDrmFormatModifierPropertiesListEXT list = {};
	list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
	VkFormatProperties2 props = {};
	props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
	props.pNext = &list;
	vkGetPhysicalDeviceFormatProperties2(physical_, format, &props);
	std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
	list.pDrmFormatModifierProperties = modifiers.data();
	vkGetPhysicalDeviceFormatProperties2(physical_, format, &props);

	for (const VkDrmFormatModifierPropertiesEXT &mod : modifiers) {
		if (!(mod.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT)) {
			continue;
		}
		// Listed as a tiling the format supports is not the same as
		// importable from a dmabuf at this size; ask about exactly the
		// image copy() will create.
		VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod_info = {};
		mod_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
		mod_info.drmFormatModifier = mod.drmFormatModifier;
		mod_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkPhysicalDeviceExternalImageFormatInfo external_info = {};
		external_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
		external_info.pNext = &mod_info;
		external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkPhysicalDeviceImageFormatInfo2 format_info = {};
		format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
		format_info.pNext = &external_info;
		format_info.format = format;
		format_info.type = VK_IMAGE_TYPE_2D;
		format_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		format_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

		VkExternalImageFormatProperties external_props = {};
		external_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
		VkImageFormatProperties2 image_props = {};
		image_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
		image_props.pNext = &external_props;
		if (vkGetPhysicalDeviceImageFormatProperties2(physical_, &format_info, &image_props) != VK_SUCCESS) {
			continue;
		}
		if (!(external_props.externalMemoryProperties.externalMemoryFeatures &
				VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
			continue;
		}
		if (image_props.imageFormatProperties.maxExtent.width < width_ ||
			image_props.imageFormatProperties.maxExtent.height < height_) {
			continue;
		}
		result.push_back(mod.drmFormatModifier);
	}
	return result;
}

bool CudaDmabufImporter::import_implicit_fence(int dmabuf_fd) {
#ifdef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
	if (implicit_fence_ == VK_NULL_HANDLE) {
		return false;
	}
	struct dma_buf_export_sync_file request = {};
	request.flags = DMA_BUF_SYNC_READ;
	request.fd = -1;
	if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request) != 0) {
		return false; // pre-6.0 kernel
	}
	VkImportSemaphoreFdInfoKHR import_info = {};
	import_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
	import_info.semaphore = implicit_fence_;
	import_info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
	import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	import_info.fd = request.fd;
	if (import_semaphore_fd_(device_, &import_info) != VK_SUCCESS) {
		::close(request.fd); // only a successful import takes ownership
		return false;
	}
	return true;
#else
	(void)dmabuf_fd;
	return false;
#endif
}

// The cached import of `frame`'s buffer, importing it on first sight. A
// capture hands the same few buffers round and round, and importing one
// (create an image, import and bind its memory) costs more than the copy;
// the cache keeps each for as long as it is in use. A buffer is known by
// its dmabuf inode -- an import holds the buffer, so the inode can't be
// reused under it -- and re-imported if its layout changes.
CudaDmabufImporter::Import *CudaDmabufImporter::find_import(const DmabufFrame &frame, const struct stat &st) {
	VkFormat format = vk_format_from_drm(frame.format);
	use_counter_++;
	for (Import &cached : imports_) {
		if (cached.dev == st.st_dev && cached.ino == st.st_ino) {
			if (cached.format == frame.format && cached.modifier == frame.modifier &&
				cached.offset == frame.offset[0] && cached.stride == frame.stride[0]) {
				cached.last_used = use_counter_;
				return &cached;
			}
			release_import(cached);
			cached = imports_.back();
			imports_.pop_back();
			break;
		}
	}
	if (imports_.size() >= kMaxImports) {
		// Buffers a renegotiation left behind: the least recently used.
		auto oldest = std::min_element(imports_.begin(), imports_.end(),
			[](const Import &a, const Import &b) { return a.last_used < b.last_used; });
		release_import(*oldest);
		*oldest = imports_.back();
		imports_.pop_back();
	}

	VkSubresourceLayout plane_layouts[DmabufFrame::kMaxPlanes] = {};
	for (int i = 0; i < frame.n_planes; i++) {
		plane_layouts[i].offset = frame.offset[i];
		plane_layouts[i].rowPitch = frame.stride[i];
	}
	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {};
	modifier_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
	modifier_info.drmFormatModifier = frame.modifier;
	modifier_info.drmFormatModifierPlaneCount = (uint32_t)frame.n_planes;
	modifier_info.pPlaneLayouts = plane_layouts;
	VkExternalMemoryImageCreateInfo external_info = {};
	external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external_info.pNext = &modifier_info;
	external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkImageCreateInfo image_info = {};
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext = &external_info;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = format;
	image_info.extent = {width_, height_, 1};
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	auto release = [&] {
		if (image != VK_NULL_HANDLE) {
			vkDestroyImage(device_, image, nullptr);
		}
		if (memory != VK_NULL_HANDLE) {
			vkFreeMemory(device_, memory, nullptr);
		}
	};
	if (!vk_ok(vkCreateImage(device_, &image_info, nullptr, &image), "vkCreateImage(dmabuf)")) {
		image = VK_NULL_HANDLE;
		return nullptr;
	}

	// The import consumes an fd of its own; the caller's stays open.
	int import_fd = fcntl(frame.fd[0], F_DUPFD_CLOEXEC, 0);
	if (import_fd < 0) {
		WLOG_ERROR("nvenc: dup of the dmabuf fd failed");
		release();
		return nullptr;
	}
	VkMemoryFdPropertiesKHR fd_props = {};
	fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
	if (!vk_ok(get_memory_fd_properties_(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, import_fd,
				   &fd_props),
			"vkGetMemoryFdPropertiesKHR")) {
		::close(import_fd);
		release();
		return nullptr;
	}
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(device_, image, &requirements);
	uint32_t type_bits = requirements.memoryTypeBits & fd_props.memoryTypeBits;
	if (type_bits == 0) {
		WLOG_ERROR("nvenc: no memory type can hold the imported dmabuf");
		::close(import_fd);
		release();
		return nullptr;
	}
	VkImportMemoryFdInfoKHR import_info = {};
	import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	import_info.fd = import_fd;
	VkMemoryDedicatedAllocateInfo dedicated = {};
	dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated.pNext = &import_info;
	dedicated.image = image;
	VkMemoryAllocateInfo alloc_info = {};
	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = &dedicated;
	alloc_info.allocationSize = requirements.size;
	alloc_info.memoryTypeIndex = (uint32_t)__builtin_ctz(type_bits);
	if (!vk_ok(vkAllocateMemory(device_, &alloc_info, nullptr, &memory), "vkAllocateMemory(dmabuf)")) {
		// Only a successful import takes ownership of the fd.
		::close(import_fd);
		memory = VK_NULL_HANDLE;
		release();
		return nullptr;
	}
	if (!vk_ok(vkBindImageMemory(device_, image, memory, 0), "vkBindImageMemory(dmabuf)")) {
		release();
		return nullptr;
	}

	Import import;
	import.dev = st.st_dev;
	import.ino = st.st_ino;
	import.format = frame.format;
	import.modifier = frame.modifier;
	import.offset = frame.offset[0];
	import.stride = frame.stride[0];
	import.image = image;
	import.memory = memory;
	import.last_used = use_counter_;
	imports_.push_back(import);
	return &imports_.back();
}

void CudaDmabufImporter::release_import(Import &import) {
	vkDestroyImage(device_, import.image, nullptr);
	vkFreeMemory(device_, import.memory, nullptr);
	import.image = VK_NULL_HANDLE;
	import.memory = VK_NULL_HANDLE;
}

bool CudaDmabufImporter::copy(const DmabufFrame &frame, int index) {
	VkFormat format = vk_format_from_drm(frame.format);
	if (format == VK_FORMAT_UNDEFINED) {
		WLOG_ERROR("nvenc: unsupported dmabuf format 0x%08x", frame.format);
		return false;
	}
	if ((uint32_t)frame.width != width_ || (uint32_t)frame.height != height_) {
		WLOG_ERROR("nvenc: dmabuf is %dx%d, the encoder was opened at %ux%u", frame.width, frame.height,
			width_, height_);
		return false;
	}
	if (frame.modifier == DRM_FORMAT_MOD_INVALID) {
		// An implicit modifier means "the layout the producing driver
		// assumes", which Vulkan has no way to be told.
		WLOG_ERROR("nvenc: dmabuf has no explicit modifier; can't import it");
		return false;
	}
	if (frame.n_planes < 1 || frame.n_planes > DmabufFrame::kMaxPlanes) {
		WLOG_ERROR("nvenc: dmabuf with %d planes", frame.n_planes);
		return false;
	}
	for (int i = 1; i < frame.n_planes; i++) {
		if (!same_dmabuf(frame.fd[0], frame.fd[i])) {
			WLOG_ERROR("nvenc: dmabuf planes spread over several buffers aren't supported");
			return false;
		}
	}

	struct stat st;
	if (fstat(frame.fd[0], &st) != 0) {
		WLOG_ERROR("nvenc: fstat of the dmabuf fd failed");
		return false;
	}
	Import *import = find_import(frame, st);
	if (!import) {
		return false;
	}
	VkImage image = import->image;

	bool wait_implicit = import_implicit_fence(frame.fd[0]);

	vkResetCommandBuffer(command_buffer_, 0);
	VkCommandBufferBeginInfo begin = {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(command_buffer_, &begin);

	// Acquire from the dmabuf's producer. A foreign image's contents are
	// defined in GENERAL layout (the same convention wlroots' Vulkan
	// renderer uses for its imports); UNDEFINED would let the driver
	// discard them.
	VkImageMemoryBarrier acquire = {};
	acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	acquire.srcAccessMask = 0;
	acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	acquire.srcQueueFamilyIndex =
		have_foreign_queue_ ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
	acquire.dstQueueFamilyIndex = queue_family_;
	acquire.image = image;
	acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &acquire);

	VkBufferImageCopy region = {};
	region.bufferOffset = 0;
	region.bufferRowLength = pitch_ / 4; // in texels
	region.bufferImageHeight = 0;
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {width_, height_, 1};
	vkCmdCopyImageToBuffer(command_buffer_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		shared_[(size_t)index].buffer, 1, &region);

	// Hand the image back to its producer in GENERAL, where the next
	// frame's acquire expects it: the import outlives this copy now.
	VkImageMemoryBarrier hand_back = acquire;
	hand_back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	hand_back.dstAccessMask = 0;
	hand_back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	hand_back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	hand_back.srcQueueFamilyIndex = queue_family_;
	hand_back.dstQueueFamilyIndex = acquire.srcQueueFamilyIndex;
	vkCmdPipelineBarrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &hand_back);
	vkEndCommandBuffer(command_buffer_);

	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit = {};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	if (wait_implicit) {
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &implicit_fence_;
		submit.pWaitDstStageMask = &wait_stage;
	}
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command_buffer_;
	vkResetFences(device_, 1, &fence_);
	bool ok = vk_ok(vkQueueSubmit(queue_, 1, &submit, fence_), "vkQueueSubmit(dmabuf copy)");
	if (ok) {
		// Host-side wait: CUDA work submitted after this sees the copy,
		// and the dmabuf can go back to its producer right away.
		ok = vk_ok(vkWaitForFences(device_, 1, &fence_, VK_TRUE, kCopyTimeoutNs),
			"vkWaitForFences(dmabuf copy)");
		if (!ok) {
			// The command buffer may still be executing; don't free what
			// it references until it isn't.
			vkQueueWaitIdle(queue_);
		}
	}
	return ok;
}

void CudaDmabufImporter::destroy() {
	const NvencRuntime *runtime = nvenc_runtime();
	for (Shared &shared : shared_) {
		if (shared.cuda_ptr) {
			runtime->cu->cuMemFree(shared.cuda_ptr);
			shared.cuda_ptr = 0;
		}
		if (shared.cuda_memory) {
			runtime->cu->cuDestroyExternalMemory(shared.cuda_memory);
			shared.cuda_memory = nullptr;
		}
	}
	if (device_ != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(device_);
		for (Import &import : imports_) {
			release_import(import);
		}
		imports_.clear();
		for (Shared &shared : shared_) {
			if (shared.buffer != VK_NULL_HANDLE) {
				vkDestroyBuffer(device_, shared.buffer, nullptr);
			}
			if (shared.memory != VK_NULL_HANDLE) {
				vkFreeMemory(device_, shared.memory, nullptr);
			}
		}
		if (implicit_fence_ != VK_NULL_HANDLE) {
			vkDestroySemaphore(device_, implicit_fence_, nullptr);
		}
		if (fence_ != VK_NULL_HANDLE) {
			vkDestroyFence(device_, fence_, nullptr);
		}
		if (command_pool_ != VK_NULL_HANDLE) {
			vkDestroyCommandPool(device_, command_pool_, nullptr);
		}
		vkDestroyDevice(device_, nullptr);
	}
	if (instance_ != VK_NULL_HANDLE) {
		vkDestroyInstance(instance_, nullptr);
	}
	shared_.clear();
	implicit_fence_ = VK_NULL_HANDLE;
	fence_ = VK_NULL_HANDLE;
	command_pool_ = VK_NULL_HANDLE;
	command_buffer_ = VK_NULL_HANDLE;
	queue_ = VK_NULL_HANDLE;
	device_ = VK_NULL_HANDLE;
	physical_ = VK_NULL_HANDLE;
	instance_ = VK_NULL_HANDLE;
}

} // namespace wraith
