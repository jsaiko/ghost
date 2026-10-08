// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// See vulkan_presenter.hpp for the overview. One frame in flight: present()
// acquires the frame image, draws, submits, and vkQueueWaitIdle()s before
// returning, then releases the frame image. Simple and correct; not
// pipelined -- see docs/design/spectre-client.md#limitations.
#include "present/vulkan_presenter.hpp"

#include "present/lossless_plane.hpp"
#include "present/overlay_renderer.hpp"
#include "present/video_image_source.hpp"
#include "present/vulkan_device.hpp"
#include "log.hpp"

#include "video_vert_spv.h"
#include "video_frag_spv.h"
#include "splash_bitmap.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace spectre {

namespace {
// Matches assets/app-icon-splash-256.bgra's fixed dimensions -- the raw
// bitmap embedded as kSplashBitmapBgra carries no header of its own (see
// client/spectre/CMakeLists.txt's "Splash bitmap" comment).
constexpr uint32_t kSplashWidth = 256;
constexpr uint32_t kSplashHeight = 256;
} // namespace

struct VulkanPresenter::Impl {
	// Declaration order matters: the two components destroy their Vulkan
	// objects through `device`, so it must be destroyed last.
	VulkanDevice device;
	VideoImageSource video_source;
	// Drawn between the video and the UI: the lossless refinement layer
	// (present/lossless_plane.hpp). Inert -- no image, no draws -- on a
	// session that didn't negotiate refinement.
	LosslessPlane lossless;
	OverlayRenderer overlay;

	// Video pipelines: fullscreen triangle + the video source's sampler,
	// one per VideoFormat the source supports (the sampler is immutable, so
	// it's baked into the set layout and hence the pipeline).
	struct VideoPipeline {
		VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
		VkDescriptorPool pool = VK_NULL_HANDLE;
		VkDescriptorSet set = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
	};
	VideoPipeline video[kVideoFormatCount];

	bool resize_pending = false;
	// set_display_size(): the session's negotiated picture size, which the
	// video quad crops the decoded frame to (video.vert's uvScale). 0x0
	// until known, meaning "show the whole texture".
	uint32_t display_width = 0;
	uint32_t display_height = 0;
	// set_video_placement(): where the picture goes, in window pixels
	// (w == 0: the whole window), and the rows at the top it must leave to
	// spectre's docked toolbar.
	float video_x = 0, video_y = 0, video_w = 0, video_h = 0;
	uint32_t video_clip_top = 0;
	// Set once a queue/swapchain call has returned an error that isn't a
	// resize (VK_ERROR_DEVICE_LOST and friends). Nothing here recovers
	// from that; the stream session checks it and shuts down instead of
	// retrying every frame.
	bool failed = false;

	// Cached for redraw() -- see vulkan_presenter.hpp's comment on it. Owned
	// via av_frame_clone(); for the hw path this holds a ref on the
	// underlying VASurface's buffer pool slot, which is what keeps it valid
	// to re-import after present() returns and the decoder moves on.
	AVFrame *last_frame = nullptr;

	~Impl();
	bool create_video_pipelines();
	bool create_video_pipeline(VideoFormat format, VideoPipeline *out);
	bool do_present(AVFrame *frame);
};

VulkanPresenter::Impl::~Impl() {
	if (last_frame) {
		av_frame_free(&last_frame);
	}
	device.wait_idle();
	VkDevice dev = device.device();
	for (VideoPipeline &v : video) {
		if (v.pipeline) vkDestroyPipeline(dev, v.pipeline, nullptr);
		if (v.layout) vkDestroyPipelineLayout(dev, v.layout, nullptr);
		if (v.pool) vkDestroyDescriptorPool(dev, v.pool, nullptr);
		if (v.set_layout) vkDestroyDescriptorSetLayout(dev, v.set_layout, nullptr);
	}
	// overlay, lossless, video_source, then device are destroyed by the
	// compiler in that (reverse-declaration) order.
}

bool VulkanPresenter::Impl::create_video_pipelines() {
	for (int i = 0; i < kVideoFormatCount; i++) {
		if (video_source.supports((VideoFormat)i) && !create_video_pipeline((VideoFormat)i, &video[i])) {
			return false;
		}
	}
	return true;
}

bool VulkanPresenter::Impl::create_video_pipeline(VideoFormat format, VideoPipeline *out) {
	VkDevice dev = device.device();

	VkDescriptorSetLayoutBinding binding{};
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	// Immutable because for NV12 and I420 this is a ycbcr-conversion
	// sampler, which cannot be bound any other way (video_image_source.hpp);
	// for BGRA it is a plain one and immutability costs nothing.
	VkSampler video_sampler = video_source.sampler(format);
	binding.pImmutableSamplers = &video_sampler;

	VkDescriptorSetLayoutCreateInfo set_layout_info{};
	set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout_info.bindingCount = 1;
	set_layout_info.pBindings = &binding;
	if (!vk_check(vkCreateDescriptorSetLayout(dev, &set_layout_info, nullptr, &out->set_layout),
			"vkCreateDescriptorSetLayout")) {
		return false;
	}

	VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		video_source.descriptor_count(format)};
	VkDescriptorPoolCreateInfo pool_info{};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	if (!vk_check(vkCreateDescriptorPool(dev, &pool_info, nullptr, &out->pool), "vkCreateDescriptorPool")) {
		return false;
	}

	VkDescriptorSetAllocateInfo set_alloc{};
	set_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_alloc.descriptorPool = out->pool;
	set_alloc.descriptorSetCount = 1;
	set_alloc.pSetLayouts = &out->set_layout;
	if (!vk_check(vkAllocateDescriptorSets(dev, &set_alloc, &out->set), "vkAllocateDescriptorSets")) {
		return false;
	}

	// video.vert's push_constant block: vec2 uvScale.
	VkPushConstantRange push_range{VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float)};
	VkPipelineLayoutCreateInfo layout_info{};
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &out->set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push_range;
	if (!vk_check(vkCreatePipelineLayout(dev, &layout_info, nullptr, &out->layout),
			"vkCreatePipelineLayout(video)")) {
		return false;
	}

	VulkanDevice::PipelineDesc desc;
	desc.vert_spv = video_vert_spv;
	desc.vert_words = sizeof(video_vert_spv) / sizeof(uint32_t);
	desc.frag_spv = video_frag_spv;
	desc.frag_words = sizeof(video_frag_spv) / sizeof(uint32_t);
	desc.layout = out->layout;
	desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	desc.what = "vkCreateGraphicsPipelines(video)";
	return device.create_pipeline(desc, &out->pipeline);
}

bool VulkanPresenter::Impl::do_present(AVFrame *frame) {
	// nullptr means "splash only" (present_splash(), before any real frame
	// has decoded) -- everything video_source-related below is skipped in
	// that case, and overlay.record_splash() draws the app icon instead of
	// the video quad.
	bool has_frame = frame != nullptr;

	if (resize_pending) {
		if (!device.recreate_swapchain()) {
			return false;
		}
		resize_pending = false;
	}

	if (has_frame && !video_source.acquire(frame)) {
		return false;
	}
	VkDevice dev = device.device();
	const VideoPipeline &vp = video[(int)(has_frame ? video_source.format() : VideoFormat::Nv12)];
	if (has_frame && vp.pipeline == VK_NULL_HANDLE) {
		SLOG_ERROR("vulkan_presenter: no pipeline for the frame's format");
		video_source.release();
		return false;
	}

	if (has_frame) {
		VkDescriptorImageInfo image_info{};
		image_info.imageView = video_source.view();
		image_info.imageLayout = video_source.layout();

		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = vp.set;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &image_info;
		vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
	}

	uint32_t image_index = 0;
	VkResult acquire_result = vkAcquireNextImageKHR(dev, device.swapchain(), UINT64_MAX,
		device.image_available(), VK_NULL_HANDLE, &image_index);
	if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
		// The window changed size since the last present: rebuild and try
		// once more, so this frame still reaches the screen.
		if (device.recreate_swapchain()) {
			acquire_result = vkAcquireNextImageKHR(dev, device.swapchain(), UINT64_MAX,
				device.image_available(), VK_NULL_HANDLE, &image_index);
		}
	}
	if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR || device.swapchain() == VK_NULL_HANDLE) {
		// Still no usable swapchain (e.g. a minimized window with a 0x0
		// extent): not drawn, but not fatal. resize_pending retries the
		// rebuild at the top of the next call instead of acquiring from a
		// swapchain that doesn't exist.
		resize_pending = true;
		if (has_frame) video_source.release();
		return false;
	}
	// VK_SUBOPTIMAL_KHR still hands back a usable image; draw it and let
	// the present result trigger the resize.
	if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
		vk_check(acquire_result, "vkAcquireNextImageKHR");
		if (has_frame) video_source.release();
		failed = true;
		return false;
	}

	VkCommandBuffer cmd = device.command_buffer();
	VkCommandBufferBeginInfo begin_info{};
	begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	if (!vk_check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer") ||
		!vk_check(vkBeginCommandBuffer(cmd, &begin_info), "vkBeginCommandBuffer")) {
		if (has_frame) video_source.release();
		failed = true;
		return false;
	}

	if (has_frame) {
		video_source.record_acquire_barrier(cmd, device.queue_family_index());
	}
	lossless.record_upload_barrier(cmd);

	VkImageMemoryBarrier to_attachment{};
	to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	to_attachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_attachment.image = device.swapchain_image(image_index);
	to_attachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_attachment);

	VkExtent2D extent = device.swapchain_extent();

	VkRenderingAttachmentInfo color_attachment{};
	color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	color_attachment.imageView = device.swapchain_view(image_index);
	color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	color_attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};

	VkRenderingInfo rendering_info{};
	rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering_info.renderArea = {{0, 0}, extent};
	rendering_info.layerCount = 1;
	rendering_info.colorAttachmentCount = 1;
	rendering_info.pColorAttachments = &color_attachment;
	vkCmdBeginRendering(cmd, &rendering_info);

	// The video and the lossless plane both draw the whole clip volume, so
	// the viewport alone places them: stretched over the area below the
	// docked toolbar, or at 1:1 somewhere that may hang off the window's
	// edges (StreamSession's actual-size view), with the scissor keeping
	// them out of the toolbar's band. The UI and cursor then go back to the
	// full window.
	uint32_t clip_top = video_clip_top < extent.height ? video_clip_top : 0;
	VkViewport video_viewport{0, (float)clip_top, (float)extent.width, (float)(extent.height - clip_top), 0,
		1};
	if (video_w > 0 && video_h > 0) {
		video_viewport = VkViewport{video_x, video_y, video_w, video_h, 0, 1};
	}
	int32_t sy_top = std::max((int32_t)clip_top, (int32_t)std::floor(video_viewport.y));
	int32_t sx_left = std::max(0, (int32_t)std::floor(video_viewport.x));
	int32_t sx_right =
		std::min((int32_t)extent.width, (int32_t)std::ceil(video_viewport.x + video_viewport.width));
	int32_t sy_bottom =
		std::min((int32_t)extent.height, (int32_t)std::ceil(video_viewport.y + video_viewport.height));
	VkRect2D video_scissor{{sx_left, sy_top},
		{(uint32_t)std::max(0, sx_right - sx_left), (uint32_t)std::max(0, sy_bottom - sy_top)}};
	VkViewport viewport{0, 0, (float)extent.width, (float)extent.height, 0, 1};
	VkRect2D scissor{{0, 0}, extent};
	vkCmdSetViewport(cmd, 0, 1, &video_viewport);
	vkCmdSetScissor(cmd, 0, 1, &video_scissor);

	if (has_frame) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vp.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vp.layout, 0, 1, &vp.set, 0, nullptr);
		// Crop the image to the picture: first to the frame's own size
		// (a Vulkan-decoded image is the codec's coded size, padding
		// included -- every other path's image already is the frame),
		// then to the display size where the codec couldn't crop (AV1
		// pads to 8; H.264/H.265 crop in the decoder, so frame->width
		// already matches). Never scale up: a frame smaller than the
		// display is a resize in flight, and stretching it is the
		// least-wrong thing until the next one.
		uint32_t visible_w = (uint32_t)frame->width;
		uint32_t visible_h = (uint32_t)frame->height;
		if (display_width > 0 && visible_w > display_width) {
			visible_w = display_width;
		}
		if (display_height > 0 && visible_h > display_height) {
			visible_h = display_height;
		}
		float uv_scale[2] = {
			(float)visible_w / (float)video_source.image_width(),
			(float)visible_h / (float)video_source.image_height(),
		};
		vkCmdPushConstants(cmd, vp.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uv_scale), uv_scale);
		vkCmdDraw(cmd, 3, 1, 0, 0);
		// Over the video, under the UI and cursor: these are the host's
		// own pixels for regions it has refreshed losslessly, not
		// spectre's chrome.
		lossless.record_draw(cmd);
	}
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	if (!has_frame) {
		overlay.record_splash(cmd, extent);
	}

	overlay.record_draw(cmd, extent);

	vkCmdEndRendering(cmd);

	VkImageMemoryBarrier to_present{};
	to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_present.image = device.swapchain_image(image_index);
	to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_present);

	if (!vk_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer")) {
		if (has_frame) video_source.release();
		failed = true;
		return false;
	}

	VkSemaphore render_finished = device.render_finished();
	VkSemaphore wait_semaphores[2] = {device.image_available(), VK_NULL_HANDLE};
	VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
	VkSemaphore signal_semaphores[2] = {render_finished, VK_NULL_HANDLE};
	// Binary semaphores ignore their slot's value.
	uint64_t wait_values[2] = {0, 0};
	uint64_t signal_values[2] = {0, 0};
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = wait_semaphores;
	submit.pWaitDstStageMask = wait_stages;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = signal_semaphores;
	// A Vulkan-decoded frame: wait for FFmpeg's decode to land, and signal
	// its timeline one higher once the draw has sampled it
	// (VideoImageSource::timeline_sync()).
	VkTimelineSemaphoreSubmitInfo timeline_info{};
	if (has_frame && video_source.timeline_sync(&wait_semaphores[1], &wait_values[1], &signal_values[1])) {
		signal_semaphores[1] = wait_semaphores[1];
		submit.waitSemaphoreCount = 2;
		submit.signalSemaphoreCount = 2;
		timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
		timeline_info.waitSemaphoreValueCount = 2;
		timeline_info.pWaitSemaphoreValues = wait_values;
		timeline_info.signalSemaphoreValueCount = 2;
		timeline_info.pSignalSemaphoreValues = signal_values;
		submit.pNext = &timeline_info;
	}
	if (!vk_check(vkQueueSubmit(device.queue(), 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit")) {
		// The image_available wait never got queued, so nothing is in
		// flight; the semaphore is left signalled, which the next acquire
		// would trip over -- another reason this is terminal.
		if (has_frame) video_source.release();
		failed = true;
		return false;
	}
	if (has_frame) video_source.mark_submitted();

	VkSwapchainKHR swapchain = device.swapchain();
	VkPresentInfoKHR present_info{};
	present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present_info.waitSemaphoreCount = 1;
	present_info.pWaitSemaphores = &render_finished;
	present_info.swapchainCount = 1;
	present_info.pSwapchains = &swapchain;
	present_info.pImageIndices = &image_index;
	VkResult present_result = vkQueuePresentKHR(device.queue(), &present_info);

	// One frame in flight (see the file header comment): block until the
	// GPU is done before tearing down the frame image (the hw import --
	// the software staging image is persistent) and returning control to
	// the caller, which frees `frame` right after this call
	// (StreamSession::present_pending_frame()).
	VkResult wait_result = vkQueueWaitIdle(device.queue());
	if (has_frame) video_source.release();

	if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
		resize_pending = true;
	} else if (!vk_check(present_result, "vkQueuePresentKHR")) {
		failed = true;
		return false;
	}
	if (!vk_check(wait_result, "vkQueueWaitIdle")) {
		failed = true;
		return false;
	}
	return true;
}

VulkanPresenter::VulkanPresenter() : impl_(std::make_unique<Impl>()) {}
VulkanPresenter::~VulkanPresenter() = default;

bool VulkanPresenter::init(SDL_Window *window, const char *drm_render_node) {
	return impl_->device.init(window, drm_render_node) && impl_->video_source.init(impl_->device) &&
		impl_->create_video_pipelines() && impl_->lossless.init(impl_->device) &&
		impl_->overlay.init(impl_->device) &&
		impl_->overlay.set_splash_bitmap(kSplashWidth, kSplashHeight, kSplashBitmapBgra);
}

const VulkanDecodeDevice *VulkanPresenter::compute_device() const {
	// No I420 pipeline means nothing PyroWave decoded could be drawn.
	return impl_->video_source.supports(VideoFormat::I420) ? impl_->device.compute_device() : nullptr;
}

const VulkanDecodeDevice *VulkanPresenter::decode_device() const {
	// No NV12 pipeline (Windows, on a device without the ycbcr conversion)
	// means nothing Vulkan decode produced could be drawn.
	return impl_->video_source.supports(VideoFormat::Nv12) ? impl_->device.decode_device() : nullptr;
}

uint32_t VulkanPresenter::vendor_id() const {
	return impl_->device.vendor_id();
}

#if defined(__linux__)
bool VulkanPresenter::supports_dmabuf_import() const {
	return impl_->device.supports_dmabuf_import();
}
#elif defined(_WIN32)
const uint8_t *VulkanPresenter::adapter_luid() const {
	return impl_->device.supports_d3d11_import() ? impl_->device.device_luid() : nullptr;
}
#endif

void VulkanPresenter::apply_lossless_update(const gdp::RefineLayer &layer, uint32_t width, uint32_t height) {
	impl_->lossless.apply(layer, width, height);
}

void VulkanPresenter::clear_lossless_plane() {
	impl_->lossless.clear();
}

void VulkanPresenter::set_display_size(uint32_t width, uint32_t height) {
	impl_->display_width = width;
	impl_->display_height = height;
}

void VulkanPresenter::set_video_placement(float x, float y, float width, float height, uint32_t clip_top) {
	impl_->video_x = x;
	impl_->video_y = y;
	impl_->video_w = width;
	impl_->video_h = height;
	impl_->video_clip_top = clip_top;
}

void VulkanPresenter::notify_resized() {
	impl_->resize_pending = true;
}

void VulkanPresenter::set_cursor_shape(uint32_t width, uint32_t height, int32_t hotspot_x, int32_t hotspot_y,
	const uint8_t *argb8888) {
	impl_->overlay.set_cursor_shape(width, height, hotspot_x, hotspot_y, argb8888);
}

void VulkanPresenter::set_cursor_position(float x_px, float y_px, bool visible) {
	impl_->overlay.set_cursor_position(x_px, y_px, visible);
}

void VulkanPresenter::set_cursor_scale(float scale) {
	impl_->overlay.set_cursor_scale(scale);
}

void VulkanPresenter::set_ui(const UiDrawList &list) {
	impl_->overlay.set_ui(list);
}

bool VulkanPresenter::set_font(const UiFont &font) {
	return impl_->overlay.set_font(font);
}

bool VulkanPresenter::present(AVFrame *frame) {
	if (impl_->failed || !impl_->do_present(frame)) {
		return false;
	}
	// av_frame_clone() rather than a fresh alloc()+ref(): ref()ing the
	// caller's `frame` directly would work too, but clone() also copies its
	// side-data pointers, and it's what the cached copy conceptually is --
	// an independent handle on the same underlying buffers.
	AVFrame *cloned = av_frame_clone(frame);
	if (cloned) {
		if (impl_->last_frame) {
			av_frame_free(&impl_->last_frame);
		}
		impl_->last_frame = cloned;
	}
	return true;
}

void VulkanPresenter::present_splash() {
	if (!impl_->failed) {
		impl_->do_present(nullptr);
	}
}

void VulkanPresenter::redraw() {
	if (impl_->failed) {
		return;
	}
	// Before the first present(), there's no last_frame to re-draw yet --
	// fall back to the splash so a window exposed (dragged over/uncovered)
	// during that gap doesn't show a stale hole instead.
	impl_->do_present(impl_->last_frame);
}

bool VulkanPresenter::failed() const {
	return impl_->failed;
}

void VulkanPresenter::shutdown() {
	impl_.reset();
}

} // namespace spectre
