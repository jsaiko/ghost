// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// See video_image_source.hpp for the overview. Implementation notes:
//
// - The imported VASurface arrives as a VK_EXT_queue_family_foreign
//   producer: the very first layout transition on it uses
//   srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT rather than treating
//   it as freshly-allocated Vulkan memory. That barrier handles ownership
//   and caches only -- it cannot wait for the decode job itself, so
//   import_frame() vaSyncSurface()s before exporting (see the comment
//   there).
// - The shared D3D11 texture is the mirror image of that: it stays in
//   VK_IMAGE_LAYOUT_GENERAL for its whole life (the single transition out
//   of UNDEFINED happens at import, while it's still empty), and each
//   frame's barrier is a VK_QUEUE_FAMILY_EXTERNAL ownership acquire. The
//   copy into it is likewise waited on before the barrier, on the CPU,
//   since Vulkan can't see a D3D11 submission either.
// - A Vulkan-decoded image is FFmpeg's, shared rather than handed over:
//   it may still be a reference picture for frames not yet decoded, so its
//   contents are never discarded (the barrier transitions from the layout
//   FFmpeg left it in, not UNDEFINED) and FFmpeg is told what layout it was
//   left in afterwards. FFmpeg makes its images VK_SHARING_MODE_CONCURRENT
//   across the graphics and decode families, so no ownership transfer.
// - Software-decode fallback (and V4L2): the staging image is
//   re-uploaded via mapped memcpy every acquire() call, with the
//   barrier's oldLayout always VK_IMAGE_LAYOUT_UNDEFINED -- legal
//   regardless of the image's actual prior layout, and simpler than
//   tracking "is this the first frame" state. macOS' VideoToolbox frames
//   take the same route (a copy into the same image), so they need no
//   barrier of their own either.
#include "present/video_image_source.hpp"

#include "log.hpp"

#ifdef _WIN32
// Ahead of the extern "C" block below, deliberately: hwcontext_d3d11va.h
// includes d3d11.h itself, and d3d11.h's C++ operator overloads don't
// compile with C linkage forced on them.
#include <d3d11.h>
#include <dxgi1_2.h>
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/frame.h>
#if defined(__linux__)
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#elif defined(_WIN32)
#include <libavutil/hwcontext_d3d11va.h>
#endif
#ifdef SPECTRE_VULKAN_DECODE
#include <libavutil/hwcontext_vulkan.h>
#endif
}

#ifdef __APPLE__
#include <CoreVideo/CoreVideo.h>
#endif

#include <algorithm>
#include <cstring>
#include <iterator>
#ifdef __linux__
#include <unistd.h>
#endif

namespace spectre {

namespace {

constexpr VkFormat kNv12Format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;

// The image format this platform's native hardware path and the software
// fallback land on. See the header for why Windows can't use the planar
// one; Vulkan Video decode is NV12 everywhere.
#ifdef _WIN32
constexpr VkFormat kVideoFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VideoFormat kNativeFormat = VideoFormat::Bgra;
#else
constexpr VkFormat kVideoFormat = kNv12Format;
constexpr VideoFormat kNativeFormat = VideoFormat::Nv12;
#endif

} // namespace

VideoImageSource::~VideoImageSource() {
	if (!dev_) {
		return;
	}
	release_vulkan_frame();
#if defined(_WIN32)
	destroy_shared_texture();
#elif defined(__linux__)
	destroy_imported(&imported_);
#endif
	destroy_software_image();
	for (VkSampler sampler : sampler_) {
		if (sampler) vkDestroySampler(dev_->device(), sampler, nullptr);
	}
	if (ycbcr_conversion_) vkDestroySamplerYcbcrConversion(dev_->device(), ycbcr_conversion_, nullptr);
	if (i420_conversion_) vkDestroySamplerYcbcrConversion(dev_->device(), i420_conversion_, nullptr);
}

bool VideoImageSource::init(VulkanDevice &device) {
	dev_ = &device;
	// Only PyroWave decodes to I420; without it the format just isn't
	// offered (VulkanPresenter::compute_device() then says no).
	if (dev_->compute_device() && !create_i420_sampler()) {
		SLOG_INFO("video_image_source: no I420 sampler -- PyroWave decode unavailable");
	}

#ifdef _WIN32
	// Plain BGRA sampler: both native Windows paths hand over RGB already
	// (the hardware one converted by ID3D11VideoProcessor, the software one
	// on the CPU), so there is nothing for a ycbcr conversion to do, and a
	// single-plane format never needs more than one descriptor slot.
	if (!dev_->create_sampler(VK_FILTER_LINEAR, nullptr, &sampler_[(int)VideoFormat::Bgra],
			"video sampler (BGRA)")) {
		return false;
	}
	// NV12 is only for Vulkan Video decode here, so a device that can't do
	// the conversion still presents everything else -- VulkanPresenter just
	// doesn't offer Decoder that backend.
	if (!create_nv12_sampler()) {
		SLOG_INFO("video_image_source: no NV12 sampler -- Vulkan Video decode unavailable");
	}
	return true;
#else
	return create_nv12_sampler();
#endif
}

bool VideoImageSource::create_nv12_sampler() {
	VkSamplerYcbcrConversionCreateInfo conv_info{};
	conv_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
	conv_info.format = kNv12Format;
	// BT.601, studio range (16-235): what every wraith encoder but PyroWave
	// (VA-API, NVENC, x264) produces -- none of them signal full range.
	// PyroWave's I420 has a sampler of its own (create_i420_sampler()). The
	// Windows path's VideoProcessorSetStreamColorSpace matches this.
	conv_info.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
	conv_info.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
	conv_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
	// Approximation: the codecs' default chroma sample location doesn't map
	// exactly onto Vulkan's two-value-per-axis siting model; cosited-even on
	// both axes is a common close-enough default (sub-pixel chroma shift at
	// worst, not a correctness bug).
	conv_info.xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
	conv_info.yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
	conv_info.chromaFilter = VK_FILTER_LINEAR;
	conv_info.forceExplicitReconstruction = VK_FALSE;

	if (!vk_check(vkCreateSamplerYcbcrConversion(dev_->device(), &conv_info, nullptr, &ycbcr_conversion_),
			"vkCreateSamplerYcbcrConversion")) {
		ycbcr_conversion_ = VK_NULL_HANDLE;
		return false;
	}

	VkSamplerYcbcrConversionInfo conv_ref{};
	conv_ref.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
	conv_ref.conversion = ycbcr_conversion_;

	if (!dev_->create_sampler(VK_FILTER_LINEAR, &conv_ref, &sampler_[(int)VideoFormat::Nv12],
			"video sampler (NV12)")) {
		sampler_[(int)VideoFormat::Nv12] = VK_NULL_HANDLE;
		return false;
	}

	// Query how many descriptor pool slots this binding actually needs: it
	// may be >1 on hardware that stores multi-planar sampler state as
	// several descriptors internally (RADV does), and an undersized pool
	// fails allocation with VK_ERROR_OUT_OF_POOL_MEMORY.
	VkPhysicalDeviceImageFormatInfo2 format_info{};
	format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	format_info.format = kNv12Format;
	format_info.type = VK_IMAGE_TYPE_2D;
	format_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	format_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;

	VkSamplerYcbcrConversionImageFormatProperties ycbcr_format_props{};
	ycbcr_format_props.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES;

	VkImageFormatProperties2 format_props{};
	format_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	format_props.pNext = &ycbcr_format_props;

	if (vkGetPhysicalDeviceImageFormatProperties2(dev_->physical_device(), &format_info, &format_props) ==
		VK_SUCCESS) {
		descriptor_count_[(int)VideoFormat::Nv12] =
			std::max(1u, ycbcr_format_props.combinedImageSamplerDescriptorCount);
	}
	return true;
}

bool VideoImageSource::create_i420_sampler() {
	VkSamplerYcbcrConversionCreateInfo conv_info{};
	conv_info.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
	conv_info.format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
	// What wraith's PyroWave encode produces: full-range BT.709 with
	// centre-sited chroma (pyrowave_encoder.hpp).
	conv_info.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709;
	conv_info.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
	conv_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
	conv_info.xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
	conv_info.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
	conv_info.chromaFilter = VK_FILTER_LINEAR;
	if (!vk_check(vkCreateSamplerYcbcrConversion(dev_->device(), &conv_info, nullptr, &i420_conversion_),
			"vkCreateSamplerYcbcrConversion(I420)")) {
		i420_conversion_ = VK_NULL_HANDLE;
		return false;
	}
	VkSamplerYcbcrConversionInfo conv_ref{};
	conv_ref.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
	conv_ref.conversion = i420_conversion_;
	if (!dev_->create_sampler(VK_FILTER_LINEAR, &conv_ref, &sampler_[(int)VideoFormat::I420],
			"video sampler (I420)")) {
		sampler_[(int)VideoFormat::I420] = VK_NULL_HANDLE;
		return false;
	}

	VkPhysicalDeviceImageFormatInfo2 format_info{};
	format_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
	format_info.format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
	format_info.type = VK_IMAGE_TYPE_2D;
	format_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	format_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	VkSamplerYcbcrConversionImageFormatProperties ycbcr_format_props{};
	ycbcr_format_props.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES;
	VkImageFormatProperties2 format_props{};
	format_props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
	format_props.pNext = &ycbcr_format_props;
	if (vkGetPhysicalDeviceImageFormatProperties2(dev_->physical_device(), &format_info, &format_props) ==
		VK_SUCCESS) {
		descriptor_count_[(int)VideoFormat::I420] =
			std::max(1u, ycbcr_format_props.combinedImageSamplerDescriptorCount);
	}
	return true;
}

bool VideoImageSource::acquire(AVFrame *frame) {
	current_view_ = VK_NULL_HANDLE;
	current_source_ = Source::None;
	current_width_ = (uint32_t)frame->width;
	current_height_ = (uint32_t)frame->height;

	if (frame->format == AV_PIX_FMT_VULKAN) {
		if (!acquire_vulkan_frame(frame)) {
			return false;
		}
		current_source_ = Source::Vulkan;
		current_format_ = vk_format_;
		current_view_ = vk_view_;
		return true;
	}

	current_format_ = kNativeFormat;
#ifdef __APPLE__
	if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
		if (!ensure_software_image((uint32_t)frame->width, (uint32_t)frame->height) ||
			!upload_videotoolbox_frame(frame)) {
			return false;
		}
		current_source_ = Source::Software;
		current_view_ = sw_image_.view;
		return true;
	}
#endif
	if (frame->format != AV_PIX_FMT_VAAPI && frame->format != AV_PIX_FMT_D3D11) {
		if (!ensure_software_image((uint32_t)frame->width, (uint32_t)frame->height) ||
			!upload_software_frame(frame)) {
			return false;
		}
		current_source_ = Source::Software;
		current_view_ = sw_image_.view;
		return true;
	}
#ifdef _WIN32
	if (!convert_shared_frame(frame)) {
		return false;
	}
	current_source_ = Source::D3d11;
	current_view_ = hw_image_.view;
	// Rounded up to even (convert_shared_frame()); the presenter crops the
	// extra column/row back off.
	current_width_ = hw_width_;
	current_height_ = hw_height_;
#elif defined(__linux__)
	if (!import_frame(frame, &imported_)) {
		destroy_imported(&imported_);
		return false;
	}
	current_source_ = Source::Dmabuf;
	current_view_ = imported_.view;
#else
	SLOG_ERROR("video_image_source: pix_fmt %d can't arrive on this platform", frame->format);
	return false;
#endif
	return true;
}

VkImageLayout VideoImageSource::layout() const {
	return current_source_ == Source::D3d11 ? VK_IMAGE_LAYOUT_GENERAL
											: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VideoImageSource::record_acquire_barrier(VkCommandBuffer cmd, uint32_t queue_family_index) {
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	switch (current_source_) {
	case Source::None: return;
	case Source::Software:
		// Host-write -> shader-read barrier for the staging image
		// upload_software_frame() just memcpy'd into. oldLayout=UNDEFINED
		// is legal regardless of the image's actual prior layout (a prior
		// frame may have left it in SHADER_READ_ONLY_OPTIMAL) -- see the
		// file header comment.
		barrier.image = sw_image_.image;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
			nullptr, 0, nullptr, 1, &barrier);
		return;
	case Source::Vulkan:
		// The present submit waits on the frame's timeline semaphore at
		// ALL_COMMANDS (timeline_sync()), which this barrier's source stage
		// chains onto: by the time the layout changes, FFmpeg's decode (and
		// any layout change of its own) has finished. Transitioning from
		// the layout it was really in keeps the picture -- it may still be
		// a reference frame.
		barrier.oldLayout = vk_old_layout_;
		barrier.image = vk_image_;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &barrier);
		return;
	case Source::Dmabuf:
	case Source::D3d11: break;
	}
	// Ownership-transfer barrier: the image was last written by the
	// decoding API, never before touched by this Vulkan queue.
#ifdef _WIN32
	// The layout stays GENERAL on both sides -- the one-time transition
	// out of UNDEFINED happened at import, and re-declaring UNDEFINED
	// here would license the driver to throw the copy away (layout()).
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	barrier.image = hw_image_.image;
#elif defined(__linux__)
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
	barrier.image = imported_.image;
#else
	return; // no imported sources on macOS
#endif
	barrier.dstQueueFamilyIndex = queue_family_index;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
		nullptr, 0, nullptr, 1, &barrier);
}

void VideoImageSource::release() {
	// The dmabuf import and the Vulkan view are the only per-frame
	// objects; the shared D3D11 texture and the software staging image
	// both outlive the frame.
	if (current_source_ == Source::Vulkan) {
		release_vulkan_frame();
	}
#ifdef __linux__
	if (current_source_ == Source::Dmabuf) {
		destroy_imported(&imported_);
	}
#endif
	current_view_ = VK_NULL_HANDLE;
	current_source_ = Source::None;
}

#ifdef SPECTRE_VULKAN_DECODE

bool VideoImageSource::acquire_vulkan_frame(AVFrame *frame) {
	auto *frames = (AVHWFramesContext *)frame->hw_frames_ctx->data;
	auto *vk_frames = (AVVulkanFramesContext *)frames->hwctx;
	auto *vkf = (AVVkFrame *)frame->data[0];

	// FFmpeg's decode output for the 8-bit 4:2:0 streams wraith sends: one
	// multi-planar image, not one image per plane -- NV12 from Vulkan
	// Video, 3-plane I420 from PyroWave.
	VkFormat format = vk_frames->format[0];
	if ((format != kNv12Format && format != VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM) ||
		vkf->img[1] != VK_NULL_HANDLE) {
		SLOG_ERROR("video_image_source: Vulkan frame isn't a single NV12 or I420 image (VkFormat %d)",
			(int)format);
		return false;
	}
	vk_format_ = format == kNv12Format ? VideoFormat::Nv12 : VideoFormat::I420;
	if (!supports(vk_format_)) {
		SLOG_ERROR("video_image_source: Vulkan frame but no sampler for its format");
		return false;
	}

	// FFmpeg creates the image for decode, storage and transfer as well as
	// sampling, and a ycbcr view can't carry usages the format doesn't
	// support as a whole -- so narrow this view to sampling.
	VkSamplerYcbcrConversionInfo conv_ref{};
	conv_ref.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
	conv_ref.conversion = vk_format_ == VideoFormat::Nv12 ? ycbcr_conversion_ : i420_conversion_;
	VkImageViewUsageCreateInfo usage_info{};
	usage_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
	usage_info.pNext = &conv_ref;
	usage_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	if (!dev_->create_image_view(vkf->img[0], format, &usage_info, &vk_view_,
			"vkCreateImageView(Vulkan decode)")) {
		vk_view_ = VK_NULL_HANDLE;
		return false;
	}

	// Held until the present submit is recorded and queued (mark_submitted())
	// so the layout and semaphore value read here are still true then.
	vk_frames->lock_frame(frames, vkf);
	if (vkf->queue_family[0] != VK_QUEUE_FAMILY_IGNORED &&
		vkf->queue_family[0] != dev_->queue_family_index()) {
		// An EXCLUSIVE image owned by another family would need a release
		// barrier on that family's queue first. FFmpeg only makes those
		// when the device has a single queue family, which is then ours.
		SLOG_ERROR("video_image_source: Vulkan frame owned by queue family %u", vkf->queue_family[0]);
		vk_frames->unlock_frame(frames, vkf);
		vkDestroyImageView(dev_->device(), vk_view_, nullptr);
		vk_view_ = VK_NULL_HANDLE;
		return false;
	}
	vk_frames_ = frames;
	vk_frame_ = vkf;
	vk_image_ = vkf->img[0];
	vk_old_layout_ = vkf->layout[0];
	// The pool's images are the coded size; frame->width/height is the
	// picture within it.
	current_width_ = (uint32_t)frames->width;
	current_height_ = (uint32_t)frames->height;
	return true;
}

bool VideoImageSource::timeline_sync(VkSemaphore *semaphore, uint64_t *wait_value,
	uint64_t *signal_value) const {
	if (current_source_ != Source::Vulkan || !vk_frame_) {
		return false;
	}
	*semaphore = vk_frame_->sem[0];
	*wait_value = vk_frame_->sem_value[0];
	*signal_value = vk_frame_->sem_value[0] + 1;
	return true;
}

void VideoImageSource::mark_submitted() {
	if (!vk_frame_) {
		return;
	}
	vk_frame_->layout[0] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vk_frame_->access[0] = VK_ACCESS_SHADER_READ_BIT;
	vk_frame_->sem_value[0]++;
	auto *vk_frames = (AVVulkanFramesContext *)vk_frames_->hwctx;
	vk_frames->unlock_frame(vk_frames_, vk_frame_);
	vk_frame_ = nullptr;
	vk_frames_ = nullptr;
}

void VideoImageSource::release_vulkan_frame() {
	// Still locked means it was never submitted: nothing changed, so hand
	// it back exactly as it was.
	if (vk_frame_) {
		auto *vk_frames = (AVVulkanFramesContext *)vk_frames_->hwctx;
		vk_frames->unlock_frame(vk_frames_, vk_frame_);
		vk_frame_ = nullptr;
		vk_frames_ = nullptr;
	}
	if (vk_view_) {
		vkDestroyImageView(dev_->device(), vk_view_, nullptr);
		vk_view_ = VK_NULL_HANDLE;
	}
	vk_image_ = VK_NULL_HANDLE;
}

#else // !SPECTRE_VULKAN_DECODE -- Decoder never produces AV_PIX_FMT_VULKAN frames.

bool VideoImageSource::acquire_vulkan_frame(AVFrame *) {
	SLOG_ERROR("video_image_source: built without FFmpeg's Vulkan hwcontext");
	return false;
}

bool VideoImageSource::timeline_sync(VkSemaphore *, uint64_t *, uint64_t *) const {
	return false;
}

void VideoImageSource::mark_submitted() {}

void VideoImageSource::release_vulkan_frame() {}

#endif // SPECTRE_VULKAN_DECODE

#ifdef __linux__

bool VideoImageSource::import_frame(AVFrame *frame, Imported *out) {
	VkDevice device = dev_->device();
	auto *frames_ctx = (AVHWFramesContext *)frame->hw_frames_ctx->data;
	auto *device_ctx = (AVHWDeviceContext *)frames_ctx->device_ctx;
	auto *vaapi_ctx = (AVVAAPIDeviceContext *)device_ctx->hwctx;
	VADisplay va_display = vaapi_ctx->display;
	auto surface_id = (VASurfaceID)(uintptr_t)frame->data[3];

	// avcodec_receive_frame() returns a VAAPI frame as soon as the decode
	// job is *submitted* (vaEndPicture), not when the video engine has
	// finished writing the surface. Exporting and sampling it straight away
	// races that write: nothing ties the Vulkan queue to the VCN job (no
	// implicit dma-buf sync between radeonsi/VA and RADV in one process),
	// so the fragment shader can read whatever the surface held last time
	// round the pool. FFmpeg's own vaapi_map_to_drm_esh() and mpv's
	// hwdec_vaapi.c both sync here for the same reason.
	VAStatus sync_status = vaSyncSurface(va_display, surface_id);
	if (sync_status != VA_STATUS_SUCCESS) {
		SLOG_ERROR("video_image_source: vaSyncSurface failed (%d)", sync_status);
		return false;
	}

	VADRMPRIMESurfaceDescriptor desc{};
	VAStatus va_status = vaExportSurfaceHandle(va_display, surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
		VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_READ_ONLY, &desc);
	if (va_status != VA_STATUS_SUCCESS) {
		SLOG_ERROR("video_image_source: vaExportSurfaceHandle failed (%d)", va_status);
		return false;
	}
	// Always owned by us on success (see va_drmcommon.h's comment on
	// VADRMPRIMESurfaceDescriptor) -- close every object fd before
	// returning, regardless of which path returns.
	auto close_desc_fds = [&desc] {
		for (uint32_t i = 0; i < desc.num_objects; i++) {
			close(desc.objects[i].fd);
		}
	};

	if (desc.num_layers != 2) {
		SLOG_ERROR("video_image_source: expected NV12 (2 layers), got %u", desc.num_layers);
		close_desc_fds();
		return false;
	}

	// Both planes in one dmabuf (Intel's iHD, usually AMD's too): import it
	// once as an ordinary image and let the plane layouts carry the offsets.
	// Binding it twice, disjoint, at memory offset 0 leaves finding the
	// second plane to the driver, and ANV doesn't -- the chroma comes out
	// as zeros, a green picture.
	const bool shared_object = desc.layers[0].object_index[0] == desc.layers[1].object_index[0];

	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info{};
	modifier_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
	modifier_info.drmFormatModifier = desc.objects[desc.layers[0].object_index[0]].drm_format_modifier;
	modifier_info.drmFormatModifierPlaneCount = 2;
	VkSubresourceLayout plane_layouts[2] = {};
	plane_layouts[0].offset = desc.layers[0].offset[0];
	plane_layouts[0].rowPitch = desc.layers[0].pitch[0];
	plane_layouts[1].offset = desc.layers[1].offset[0];
	plane_layouts[1].rowPitch = desc.layers[1].pitch[0];
	modifier_info.pPlaneLayouts = plane_layouts;

	VkExternalMemoryImageCreateInfo ext_image_info{};
	ext_image_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	ext_image_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	ext_image_info.pNext = &modifier_info;

	VkImageCreateInfo image_info{};
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext = &ext_image_info;
	image_info.flags = shared_object ? 0 : VK_IMAGE_CREATE_DISJOINT_BIT;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	// frame->width/height, not desc.width/height: the VASurface is padded
	// to the codec's block size, and the rows/columns past the picture
	// must not be sampled. H.264/H.265 signal the real crop and FFmpeg
	// applies it to frame->width/height; AV1 cannot crop, so its padding
	// is in frame->width/height too and VulkanPresenter crops it with
	// video.vert's uvScale instead.
	image_info.extent = {(uint32_t)frame->width, (uint32_t)frame->height, 1};
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	if (!vk_check(vkCreateImage(device, &image_info, nullptr, &out->image), "vkCreateImage(imported)")) {
		close_desc_fds();
		return false;
	}

	VkImageAspectFlagBits plane_aspects[2] = {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT};
	for (int plane = 0; plane < (shared_object ? 1 : 2); plane++) {
		VkImagePlaneMemoryRequirementsInfo plane_req_info{};
		plane_req_info.sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO;
		plane_req_info.planeAspect = plane_aspects[plane];

		VkImageMemoryRequirementsInfo2 req_info{};
		req_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
		req_info.pNext = shared_object ? nullptr : &plane_req_info;
		req_info.image = out->image;

		VkMemoryRequirements2 requirements{};
		requirements.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
		vkGetImageMemoryRequirements2(device, &req_info, &requirements);

		int dup_fd = dup(desc.objects[desc.layers[plane].object_index[0]].fd);
		if (dup_fd < 0) {
			SLOG_ERROR("video_image_source: dup() failed for plane %d", plane);
			close_desc_fds();
			return false;
		}

		VkMemoryFdPropertiesKHR fd_props{};
		fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
		if (dev_->get_memory_fd_properties()(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dup_fd,
				&fd_props) != VK_SUCCESS) {
			close(dup_fd);
			close_desc_fds();
			return false;
		}

		uint32_t memory_type_index = dev_->find_memory_type(
			requirements.memoryRequirements.memoryTypeBits & fd_props.memoryTypeBits, 0);
		if (memory_type_index == UINT32_MAX) {
			SLOG_ERROR("video_image_source: no compatible memory type for imported fd");
			close(dup_fd);
			close_desc_fds();
			return false;
		}

		VkImportMemoryFdInfoKHR import_info{};
		import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
		import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		import_info.fd = dup_fd;

		VkMemoryDedicatedAllocateInfo dedicated_info{};
		dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
		dedicated_info.image = out->image;
		dedicated_info.pNext = &import_info;

		VkMemoryAllocateInfo alloc_info{};
		alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc_info.pNext = &dedicated_info;
		alloc_info.allocationSize = requirements.memoryRequirements.size;
		alloc_info.memoryTypeIndex = memory_type_index;

		// On success, ownership of dup_fd passes to the driver (Vulkan spec,
		// "Importing Memory" -- the application must not use the fd again).
		if (vkAllocateMemory(device, &alloc_info, nullptr, &out->memory[plane]) != VK_SUCCESS) {
			SLOG_ERROR("video_image_source: vkAllocateMemory (import) failed for plane %d", plane);
			close(dup_fd);
			close_desc_fds();
			return false;
		}

		VkBindImagePlaneMemoryInfo bind_plane_info{};
		bind_plane_info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO;
		bind_plane_info.planeAspect = plane_aspects[plane];

		VkBindImageMemoryInfo bind_info{};
		bind_info.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO;
		bind_info.pNext = shared_object ? nullptr : &bind_plane_info;
		bind_info.image = out->image;
		bind_info.memory = out->memory[plane];
		bind_info.memoryOffset = 0;

		if (!vk_check(vkBindImageMemory2(device, 1, &bind_info), "vkBindImageMemory2")) {
			close_desc_fds();
			return false;
		}
	}
	close_desc_fds(); // originals no longer needed; imports used dup'd copies

	VkSamplerYcbcrConversionInfo conv_ref{};
	conv_ref.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
	conv_ref.conversion = ycbcr_conversion_;
	return dev_->create_image_view(out->image, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &conv_ref, &out->view,
		"vkCreateImageView(imported)");
}

void VideoImageSource::destroy_imported(Imported *imported) {
	VkDevice device = dev_->device();
	if (imported->view) vkDestroyImageView(device, imported->view, nullptr);
	if (imported->image) vkDestroyImage(device, imported->image, nullptr);
	if (imported->memory[0]) vkFreeMemory(device, imported->memory[0], nullptr);
	if (imported->memory[1]) vkFreeMemory(device, imported->memory[1], nullptr);
	*imported = Imported{};
}

#endif // __linux__

#ifdef _WIN32

bool VideoImageSource::convert_shared_frame(AVFrame *frame) {
	auto *frames_ctx = (AVHWFramesContext *)frame->hw_frames_ctx->data;
	auto *device_ctx = (AVHWDeviceContext *)frames_ctx->device_ctx;
	auto *d3d11 = (AVD3D11VADeviceContext *)device_ctx->hwctx;

	// An AV_PIX_FMT_D3D11 frame points into a texture array the decoder owns:
	// data[0] is the array, data[1] the slice this frame decoded into.
	auto *source = (ID3D11Texture2D *)frame->data[0];
	UINT source_slice = (UINT)(uintptr_t)frame->data[1];

	// The decoder's NV12 surface subsamples chroma 2:1 on both axes, so the
	// region the video processor reads has to be even; a coded display size
	// need not be. Rounding up takes in one row/column of the decoder's own
	// padding rather than asking for a blit D3D11 rejects -- a sub-pixel
	// stretch in the video quad at worst, and wraith's encoders only ever
	// produce even sizes anyway.
	uint32_t width = (uint32_t)frame->width + ((uint32_t)frame->width & 1u);
	uint32_t height = (uint32_t)frame->height + ((uint32_t)frame->height & 1u);
	if (!ensure_shared_texture(d3d11, width, height)) {
		return false;
	}

	// One input view per frame, because the array slice moves; the output
	// view is the shared texture's and outlives them all.
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
	input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	input_desc.Texture2D.ArraySlice = source_slice;

	// The decoder and this conversion drive the same immediate context;
	// FFmpeg's lock/unlock pair is what serializes them (hwcontext_d3d11va.h).
	d3d11->lock(d3d11->lock_ctx);
	ID3D11VideoProcessorInputView *input_view = nullptr;
	HRESULT hr = d3d11->video_device->CreateVideoProcessorInputView(source, hw_processor_enum_, &input_desc,
		&input_view);
	if (SUCCEEDED(hr)) {
		D3D11_VIDEO_PROCESSOR_STREAM stream{};
		stream.Enable = TRUE;
		stream.pInputSurface = input_view;
		hr = d3d11->video_context->VideoProcessorBlt(hw_processor_, hw_processor_output_, 0, 1, &stream);
		input_view->Release();
	}
	d3d11->device_context->End(hw_convert_done_);
	d3d11->device_context->Flush();
	d3d11->unlock(d3d11->lock_ctx);
	if (FAILED(hr)) {
		SLOG_ERROR("video_image_source: NV12 -> BGRA VideoProcessorBlt failed (0x%08lx)", (unsigned long)hr);
		return false;
	}

	// Then wait for that conversion to land before anything Vulkan samples
	// it. The two APIs feed the same GPU with no shared timeline between
	// them, so nothing else orders the fragment shader after this blit (the
	// same reason the VA-API path vaSyncSurface()s). Cheap today because the
	// present loop keeps one frame in flight and blocks on the GPU
	// regardless.
	for (;;) {
		d3d11->lock(d3d11->lock_ctx);
		HRESULT wait = d3d11->device_context->GetData(hw_convert_done_, nullptr, 0, 0);
		d3d11->unlock(d3d11->lock_ctx);
		if (wait == S_OK) {
			return true;
		}
		if (FAILED(wait)) {
			SLOG_ERROR("video_image_source: waiting on the D3D11 conversion failed (0x%08lx)",
				(unsigned long)wait);
			return false;
		}
		SwitchToThread(); // S_FALSE: still running
	}
}

bool VideoImageSource::ensure_shared_texture(AVD3D11VADeviceContext *d3d11, uint32_t width, uint32_t height) {
	if (hw_texture_ != nullptr && hw_width_ == width && hw_height_ == height) {
		return true;
	}
	destroy_shared_texture();
	ID3D11Device *device = d3d11->device;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	// Single-plane, not the decoder's NV12: see the header for the plane-
	// offset mismatch that makes a shared planar texture unusable here.
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	// RENDER_TARGET is what lets the video processor write into it.
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	// Vulkan imports an NT handle, and D3D11 only issues one for a resource
	// also marked SHARED (or SHARED_KEYEDMUTEX, which would buy a GPU-side
	// lock this design doesn't use -- convert_shared_frame() waits on the CPU).
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &hw_texture_))) {
		SLOG_ERROR("video_image_source: CreateTexture2D(shared BGRA %ux%u) failed", width, height);
		destroy_shared_texture();
		return false;
	}

	// The NV12 -> BGRA converter. Fixed-function video hardware, so this
	// costs far less than the shader pass it replaces would.
	D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
	content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	content.InputWidth = width;
	content.InputHeight = height;
	content.OutputWidth = width;
	content.OutputHeight = height;
	content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
	if (FAILED(d3d11->video_device->CreateVideoProcessorEnumerator(&content, &hw_processor_enum_)) ||
		FAILED(d3d11->video_device->CreateVideoProcessor(hw_processor_enum_, 0, &hw_processor_))) {
		SLOG_ERROR("video_image_source: CreateVideoProcessor failed");
		destroy_shared_texture();
		return false;
	}

	// Match what the Linux ycbcr sampler is configured for, so both platforms
	// put the same colours on screen: studio-range (16-235) BT.601 in --
	// wraith's encoders don't signal full range -- full-range RGB out.
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_space{};
	input_space.YCbCr_Matrix = 0; // BT.601
	input_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
	d3d11->video_context->VideoProcessorSetStreamColorSpace(hw_processor_, 0, &input_space);
	D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_space{};
	output_space.RGB_Range = 0; // full
	output_space.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
	d3d11->video_context->VideoProcessorSetOutputColorSpace(hw_processor_, &output_space);

	// Clip the blit to the frame's real size. The decoder's surface is
	// aligned up to the codec's block size (16 for H.264, 64 for HEVC's
	// CTBs) and the rows past the picture are never written; left to
	// default, the video processor takes the *whole* input surface as its
	// source rect and scales that padding into the output.
	RECT frame_rect{0, 0, (LONG)width, (LONG)height};
	d3d11->video_context->VideoProcessorSetStreamSourceRect(hw_processor_, 0, TRUE, &frame_rect);
	d3d11->video_context->VideoProcessorSetStreamDestRect(hw_processor_, 0, TRUE, &frame_rect);
	d3d11->video_context->VideoProcessorSetOutputTargetRect(hw_processor_, TRUE, &frame_rect);

	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
	output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	if (FAILED(d3d11->video_device->CreateVideoProcessorOutputView(hw_texture_, hw_processor_enum_,
			&output_desc, &hw_processor_output_))) {
		SLOG_ERROR("video_image_source: CreateVideoProcessorOutputView failed");
		destroy_shared_texture();
		return false;
	}

	IDXGIResource1 *resource = nullptr;
	if (FAILED(hw_texture_->QueryInterface(__uuidof(IDXGIResource1), (void **)&resource))) {
		SLOG_ERROR("video_image_source: shared texture has no IDXGIResource1");
		destroy_shared_texture();
		return false;
	}
	HRESULT share_result = resource->CreateSharedHandle(nullptr,
		DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, (HANDLE *)&hw_shared_handle_);
	resource->Release();
	if (FAILED(share_result)) {
		SLOG_ERROR("video_image_source: CreateSharedHandle failed (0x%08lx)", (unsigned long)share_result);
		destroy_shared_texture();
		return false;
	}

	D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
	if (FAILED(device->CreateQuery(&query_desc, &hw_convert_done_))) {
		SLOG_ERROR("video_image_source: CreateQuery(D3D11_QUERY_EVENT) failed");
		destroy_shared_texture();
		return false;
	}

	VkDevice vk_device = dev_->device();

	VkExternalMemoryImageCreateInfo external_info{};
	external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

	// One plane, one allocation, one VkDeviceMemory -- none of the dmabuf
	// import's disjoint per-plane binding, and (see the header) no planar
	// layout for the two APIs to disagree about.
	VkImageCreateInfo image_info{};
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.pNext = &external_info;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = kVideoFormat;
	image_info.extent = {width, height, 1};
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!vk_check(vkCreateImage(vk_device, &image_info, nullptr, &hw_image_.image),
			"vkCreateImage(shared)")) {
		destroy_shared_texture();
		return false;
	}

	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(vk_device, hw_image_.image, &requirements);

	VkMemoryWin32HandlePropertiesKHR handle_props{};
	handle_props.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
	if (!vk_check(dev_->get_memory_win32_handle_properties()(vk_device,
					  VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, (HANDLE)hw_shared_handle_,
					  &handle_props),
			"vkGetMemoryWin32HandlePropertiesKHR")) {
		destroy_shared_texture();
		return false;
	}

	uint32_t memory_type_index =
		dev_->find_memory_type(requirements.memoryTypeBits & handle_props.memoryTypeBits, 0);
	if (memory_type_index == UINT32_MAX) {
		SLOG_ERROR("video_image_source: no compatible memory type for the shared D3D11 texture");
		destroy_shared_texture();
		return false;
	}

	// Importing a Win32 handle *copies* it -- unlike the dmabuf path's fds,
	// this side keeps ownership and destroy_shared_texture() closes it.
	VkImportMemoryWin32HandleInfoKHR import_info{};
	import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
	import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
	import_info.handle = (HANDLE)hw_shared_handle_;

	// Required for a D3D11 texture handle: the allocation *is* the texture,
	// it can't back anything else.
	VkMemoryDedicatedAllocateInfo dedicated_info{};
	dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated_info.pNext = &import_info;
	dedicated_info.image = hw_image_.image;

	VkMemoryAllocateInfo alloc_info{};
	alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc_info.pNext = &dedicated_info;
	alloc_info.allocationSize = requirements.size;
	alloc_info.memoryTypeIndex = memory_type_index;
	if (!vk_check(vkAllocateMemory(vk_device, &alloc_info, nullptr, &hw_image_.memory),
			"vkAllocateMemory(shared)") ||
		!vk_check(vkBindImageMemory(vk_device, hw_image_.image, hw_image_.memory, 0),
			"vkBindImageMemory(shared)")) {
		destroy_shared_texture();
		return false;
	}

	// No ycbcr conversion in the pNext chain: the video processor already
	// produced RGB, and init()'s sampler is a plain one to match.
	if (!dev_->create_image_view(hw_image_.image, kVideoFormat, nullptr, &hw_image_.view,
			"vkCreateImageView(shared)")) {
		destroy_shared_texture();
		return false;
	}

	// The one and only transition out of UNDEFINED, done here -- while the
	// texture is still empty -- rather than per frame, because leaving
	// UNDEFINED behind is what lets a driver discard the image's contents.
	// From here on it stays in GENERAL and every frame's barrier is an
	// ownership acquire (record_acquire_barrier()). Safe to submit from
	// acquire(): the presenter hasn't begun this frame's command buffer yet.
	bool transitioned = dev_->submit_one_time([this](VkCommandBuffer cmd) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = hw_image_.image;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
			0, nullptr, 0, nullptr, 1, &barrier);
	});
	if (!transitioned) {
		destroy_shared_texture();
		return false;
	}

	hw_width_ = width;
	hw_height_ = height;
	SLOG_DEBUG("video_image_source: sharing a %ux%u BGRA D3D11 texture into Vulkan", width, height);
	return true;
}

void VideoImageSource::destroy_shared_texture() {
	hw_image_.destroy(dev_->device());
	if (hw_convert_done_) {
		hw_convert_done_->Release();
		hw_convert_done_ = nullptr;
	}
	if (hw_processor_output_) {
		hw_processor_output_->Release();
		hw_processor_output_ = nullptr;
	}
	if (hw_processor_) {
		hw_processor_->Release();
		hw_processor_ = nullptr;
	}
	if (hw_processor_enum_) {
		hw_processor_enum_->Release();
		hw_processor_enum_ = nullptr;
	}
	if (hw_shared_handle_) {
		CloseHandle((HANDLE)hw_shared_handle_);
		hw_shared_handle_ = nullptr;
	}
	if (hw_texture_) {
		hw_texture_->Release();
		hw_texture_ = nullptr;
	}
	hw_width_ = 0;
	hw_height_ = 0;
}

#endif // _WIN32

void VideoImageSource::destroy_software_image() {
	if (sw_mapped_) {
		vkUnmapMemory(dev_->device(), sw_image_.memory);
		sw_mapped_ = nullptr;
	}
	sw_image_.destroy(dev_->device());
	sw_image_width_ = 0;
	sw_image_height_ = 0;
}

bool VideoImageSource::ensure_software_image(uint32_t width, uint32_t height) {
	if (sw_image_.image != VK_NULL_HANDLE && sw_image_width_ == width && sw_image_height_ == height) {
		return true;
	}
	destroy_software_image();

	// Same format the sampler/video pipeline were built for -- no disjoint
	// flag (a single VkDeviceMemory backs it; unlike the dmabuf import path,
	// this memory isn't coming from separate driver-allocated objects),
	// linear tiling so vkGetImageSubresourceLayout gives usable
	// offsets/pitches for the host-mapped writes in upload_software_frame().
	if (!dev_->create_host_image(kVideoFormat, width, height, VK_IMAGE_LAYOUT_UNDEFINED, &sw_image_.image,
			&sw_image_.memory, "software staging image")) {
		destroy_software_image();
		return false;
	}

#ifdef _WIN32
	const void *view_pnext = nullptr; // BGRA: nothing for a ycbcr conversion to do
#else
	VkSamplerYcbcrConversionInfo conv_ref{};
	conv_ref.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
	conv_ref.conversion = ycbcr_conversion_;
	const void *view_pnext = &conv_ref;
#endif
	if (!dev_->create_image_view(sw_image_.image, kVideoFormat, view_pnext, &sw_image_.view,
			"vkCreateImageView(sw)")) {
		destroy_software_image();
		return false;
	}

#ifdef _WIN32
	VkImageSubresource planes[1] = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0}};
#else
	VkImageSubresource planes[2] = {{VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0}, {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0}};
#endif
	VkDevice device = dev_->device();
	for (size_t i = 0; i < std::size(planes); i++) {
		vkGetImageSubresourceLayout(device, sw_image_.image, &planes[i], &sw_planes_[i]);
	}
	void *mapped = nullptr;
	if (!vk_check(vkMapMemory(device, sw_image_.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(sw)")) {
		destroy_software_image();
		return false;
	}
	sw_mapped_ = (uint8_t *)mapped;

	sw_image_width_ = width;
	sw_image_height_ = height;
	return true;
}

bool VideoImageSource::upload_software_frame(AVFrame *frame) {
	// FFmpeg's software decoders yield this for the 8-bit 4:2:0 streams
	// every wraith encoder produces when no hwaccel is in play -- anything
	// else would need a different staging format entirely.
	if (frame->format != AV_PIX_FMT_YUV420P) {
		SLOG_ERROR("video_image_source: software path expected YUV420P, got pix_fmt %d", frame->format);
		return false;
	}

#ifdef _WIN32
	// Windows samples BGRA (see the header), so the conversion the ycbcr
	// sampler does for free on Linux has to happen here instead: BT.601,
	// studio range, the same coefficients VideoProcessorSetStreamColorSpace
	// is given on the hardware path so both look alike. A plain scalar loop,
	// like the NV12 interleave below -- this is the fallback path, and a
	// client with no GPU decoder has worse problems than its speed.
	const VkSubresourceLayout &layout = sw_planes_[0];
	auto clamp8 = [](int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); };

	for (uint32_t y = 0; y < sw_image_height_; y++) {
		uint8_t *dst = sw_mapped_ + layout.offset + (size_t)y * layout.rowPitch;
		const uint8_t *luma = frame->data[0] + (size_t)y * frame->linesize[0];
		const uint8_t *cb = frame->data[1] + (size_t)(y / 2) * frame->linesize[1];
		const uint8_t *cr = frame->data[2] + (size_t)(y / 2) * frame->linesize[2];
		for (uint32_t x = 0; x < sw_image_width_; x++) {
			int c = luma[x] - 16;
			int d = cb[x / 2] - 128;
			int e = cr[x / 2] - 128;
			dst[x * 4 + 0] = clamp8((298 * c + 516 * d + 128) >> 8);           // B
			dst[x * 4 + 1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8); // G
			dst[x * 4 + 2] = clamp8((298 * c + 409 * e + 128) >> 8);           // R
			dst[x * 4 + 3] = 255;
		}
	}
	return true;
#else
	const VkSubresourceLayout &y_layout = sw_planes_[0];
	const VkSubresourceLayout &uv_layout = sw_planes_[1];

	for (uint32_t y = 0; y < sw_image_height_; y++) {
		memcpy(sw_mapped_ + y_layout.offset + (size_t)y * y_layout.rowPitch,
			frame->data[0] + (size_t)y * frame->linesize[0], sw_image_width_);
	}

	// NV12's chroma plane interleaves U/V per sample; YUV420P keeps them in
	// separate planes at the same 4:2:0 subsampling, so this is a pure
	// interleave, no resampling/color-space math involved.
	uint32_t chroma_width = (sw_image_width_ + 1) / 2;
	uint32_t chroma_height = (sw_image_height_ + 1) / 2;
	for (uint32_t y = 0; y < chroma_height; y++) {
		uint8_t *dst = sw_mapped_ + uv_layout.offset + (size_t)y * uv_layout.rowPitch;
		const uint8_t *u = frame->data[1] + (size_t)y * frame->linesize[1];
		const uint8_t *v = frame->data[2] + (size_t)y * frame->linesize[2];
		for (uint32_t x = 0; x < chroma_width; x++) {
			dst[x * 2 + 0] = u[x];
			dst[x * 2 + 1] = v[x];
		}
	}
	return true;
#endif // _WIN32
}

#ifdef __APPLE__

bool VideoImageSource::upload_videotoolbox_frame(AVFrame *frame) {
	// FFmpeg's VideoToolbox hwaccel hands out 8-bit 4:2:0 as NV12 in video
	// range -- the same layout and range the staging image and the ycbcr
	// sampler are set up for, so this is two plane copies and nothing else.
	auto pixel_buffer = (CVPixelBufferRef)frame->data[3];
	OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
	if (pixel_format != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
		CVPixelBufferGetPlaneCount(pixel_buffer) != 2) {
		SLOG_ERROR("video_image_source: VideoToolbox frame isn't video-range NV12 (format '%c%c%c%c')",
			(char)(pixel_format >> 24), (char)(pixel_format >> 16), (char)(pixel_format >> 8),
			(char)pixel_format);
		return false;
	}
	// VideoToolbox's decode for this frame has finished by the time FFmpeg
	// returns it (its hwaccel waits on each one), so the lock only maps it.
	if (CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
		SLOG_ERROR("video_image_source: CVPixelBufferLockBaseAddress failed");
		return false;
	}

	// Plane sizes clamp to the pixel buffer's own, should FFmpeg's idea of
	// the frame size ever run past them.
	uint32_t chroma_width = (sw_image_width_ + 1) / 2;
	uint32_t chroma_height = (sw_image_height_ + 1) / 2;
	struct Plane {
		uint32_t row_bytes;
		uint32_t rows;
	};
	const Plane planes[2] = {
		{sw_image_width_,
			std::min(sw_image_height_, (uint32_t)CVPixelBufferGetHeightOfPlane(pixel_buffer, 0))},
		{chroma_width * 2, std::min(chroma_height, (uint32_t)CVPixelBufferGetHeightOfPlane(pixel_buffer, 1))},
	};
	for (size_t i = 0; i < 2; i++) {
		const auto *src = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, i);
		size_t src_pitch = CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, i);
		uint32_t row_bytes = std::min(planes[i].row_bytes, (uint32_t)src_pitch);
		const VkSubresourceLayout &layout = sw_planes_[i];
		for (uint32_t y = 0; y < planes[i].rows; y++) {
			memcpy(sw_mapped_ + layout.offset + (size_t)y * layout.rowPitch, src + (size_t)y * src_pitch,
				row_bytes);
		}
	}
	CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
	return true;
}

#endif // __APPLE__

} // namespace spectre
