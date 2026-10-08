// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/vaapi/vaapi_encoder_base.hpp"

#include "util/log.hpp"

#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

#include <libdrm/drm_fourcc.h>

#include <algorithm>
#include <cstring>

#include <iterator>

#include <sys/timerfd.h>
#include <unistd.h>

namespace wraith {

namespace {

// VADRMPRIMESurfaceDescriptor::fourcc wants a VA_FOURCC_* value, which is
// *not* interchangeable with the DRM_FORMAT_* value in desc.layers[0]
// .drm_format below despite both being 4CC-style codes: e.g.
// DRM_FORMAT_XRGB8888 (bytes X:R:G:B from the *bit* layout name, stored
// B,G,R,X in little-endian memory) is 0x34325258, while VA names the same
// B,G,R,X memory layout VA_FOURCC_BGRX (0x58524742). Every capture path
// wraith has delivers XRGB8888 (what the screencast sources negotiate); the ARGB and BGR-ordered variants are
// mapped too so any other 8-bit RGB dmabuf imports correctly rather than
// being rejected.
uint32_t va_fourcc_from_drm(uint32_t drm_fourcc) {
	switch (drm_fourcc) {
	case DRM_FORMAT_XRGB8888: return VA_FOURCC_BGRX;
	case DRM_FORMAT_ARGB8888: return VA_FOURCC_BGRA;
	case DRM_FORMAT_XBGR8888: return VA_FOURCC_RGBX;
	case DRM_FORMAT_ABGR8888: return VA_FOURCC_RGBA;
	default: return 0;
	}
}

} // namespace

VaapiEncoderBase::~VaapiEncoderBase() {
	close();
}

bool VaapiEncoderBase::open(const EncoderConfig &config) {
	config_ = config;
	if (config_.drm_fd < 0) {
		WLOG_ERROR("vaapi_encoder: no drm_fd given");
		return false;
	}
	if (config_.width == 0 || config_.height == 0) {
		WLOG_ERROR("vaapi_encoder: zero-sized frame");
		return false;
	}
	if ((config_.width % 2) != 0 || (config_.height % 2) != 0) {
		// 4:2:0 chroma cropping units are 2px; odd dimensions would need
		// extra handling this encoder doesn't do.
		WLOG_ERROR("vaapi_encoder: width/height must be even (got %ux%u)", config_.width, config_.height);
		return false;
	}

	uint32_t align = surface_alignment();
	padded_width_ = (config_.width + align - 1) / align * align;
	padded_height_ = (config_.height + align - 1) / align * align;

	if (!init_display() || !init_vpp_pipeline() || !init_encode_pipeline()) {
		close();
		return false;
	}

	frames_since_idr_ = 0;
	force_idr_next_ = true;
	rc_dirty_ = true;
	fr_sent_ = false;
	last_ref_surface_ = VA_INVALID_ID;
	next_nv12_ = 0;
	next_dpb_ = 0;

	if (!init_codec()) {
		close();
		return false;
	}
	if (asynchronous_) {
		timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
		if (timer_fd_ < 0) {
			WLOG_ERROR("vaapi_encoder: timerfd_create failed; waiting for each encode instead");
			asynchronous_ = false;
		}
	}
	return true;
}

bool VaapiEncoderBase::init_display() {
	display_ = vaGetDisplayDRM(config_.drm_fd);
	if (!display_) {
		WLOG_ERROR("vaapi_encoder: vaGetDisplayDRM failed");
		return false;
	}
	int major = 0, minor = 0;
	VAStatus st = vaInitialize(display_, &major, &minor);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaInitialize failed: %s", vaErrorStr(st));
		// vaGetDisplayDRM() only wraps the fd; the driver never actually
		// opened, so there's nothing for vaTerminate/vaDestroy* to clean up
		// and calling them on this handle segfaults inside libva. Drop it
		// so close() skips the VA-API teardown calls entirely.
		display_ = nullptr;
		return false;
	}
	return true;
}

bool VaapiEncoderBase::init_vpp_pipeline() {
	VAStatus st = vaCreateConfig(display_, VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &vpp_config_);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateConfig(VPP) failed: %s", vaErrorStr(st));
		return false;
	}
	st = vaCreateContext(display_, vpp_config_, (int)padded_width_, (int)padded_height_, VA_PROGRESSIVE,
		nullptr, 0, &vpp_context_);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateContext(VPP) failed: %s", vaErrorStr(st));
		return false;
	}
	return true;
}

bool VaapiEncoderBase::init_encode_pipeline() {
	VAConfigAttrib attribs[3];
	attribs[0].type = VAConfigAttribRTFormat;
	attribs[0].value = VA_RT_FORMAT_YUV420;
	attribs[1].type = VAConfigAttribRateControl;
	// VBR, not CBR, for the same reason x264_encoder.cpp uses CRF under a
	// VBV cap: a desktop is static most of the time and should cost nothing
	// while it is. Under VA_RC_CBR radeonsi pads *every* frame to exactly
	// bits_per_second / fps -- measured at 20 Mbps/60: 41667 bytes per
	// P-frame with nothing on screen changing. VBR with the same bits_per_second lets
	// those frames drop to ~300 bytes and still spends up to the ceiling
	// when there is motion, so the congestion controller's set_bitrate()
	// keeps meaning what it did: a cap, not a quota.
	attribs[1].value = VA_RC_VBR;
	attribs[2].type = VAConfigAttribEncPackedHeaders;
	attribs[2].value = packed_header_flags();

	VAStatus st = vaCreateConfig(display_, enc_profile(), VAEntrypointEncSlice, attribs, 3, &enc_config_);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateConfig(%s/EncSlice) failed: %s", enc_profile_name(),
			vaErrorStr(st));
		return false;
	}

	VASurfaceAttrib fmt_attrib = {};
	fmt_attrib.type = VASurfaceAttribPixelFormat;
	fmt_attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
	fmt_attrib.value.type = VAGenericValueTypeInteger;
	fmt_attrib.value.value.i = VA_FOURCC_NV12;

	st = vaCreateSurfaces(display_, VA_RT_FORMAT_YUV420, padded_width_, padded_height_, nv12_surfaces_,
		kNumNv12Surfaces, &fmt_attrib, 1);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateSurfaces(NV12 pool) failed: %s", vaErrorStr(st));
		return false;
	}
	st = vaCreateSurfaces(display_, VA_RT_FORMAT_YUV420, padded_width_, padded_height_, dpb_surfaces_,
		kNumDpbSurfaces, &fmt_attrib, 1);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateSurfaces(DPB pool) failed: %s", vaErrorStr(st));
		return false;
	}

	// Every surface the context will touch, inputs and DPB alike: some
	// drivers (not Mesa) validate render targets against this list.
	VASurfaceID render_targets[kNumNv12Surfaces + kNumDpbSurfaces];
	std::copy(std::begin(nv12_surfaces_), std::end(nv12_surfaces_), render_targets);
	std::copy(std::begin(dpb_surfaces_), std::end(dpb_surfaces_), render_targets + kNumNv12Surfaces);
	st = vaCreateContext(display_, enc_config_, (int)padded_width_, (int)padded_height_, VA_PROGRESSIVE,
		render_targets, (int)std::size(render_targets), &enc_context_);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateContext(encode) failed: %s", vaErrorStr(st));
		return false;
	}
	return true;
}

// A capture source that gets to
// choose a modifier (PipeWire's dmabuf-with-modifiers negotiation) needs to
// know what this encoder can actually import.
//
// VASurfaceAttribDRMFormatModifiers is documented as "(pointer, write)".
// Mesa's radeonsi driver returns it from vaQuerySurfaceAttributes with only
// VA_SURFACE_ATTRIB_SETTABLE set, not VA_SURFACE_ATTRIB_GETTABLE, so it
// gives no modifier list to read. A driver that does mark it gettable has
// its list used; otherwise the answer is LINEAR + INVALID, which
// vaCreateSurfaces's import path accepts unconditionally (frame.modifier
// is passed straight through with no driver-side validation at
// surface-creation time) -- safe, if not zero-copy for every tiling
// layout, for a foreign producer's dmabuf. There is no driver-specific
// tiled-modifier allowlist.
std::vector<uint64_t> VaapiEncoderBase::supported_import_modifiers(uint32_t drm_format) const {
	uint32_t va_fourcc = va_fourcc_from_drm(drm_format);
	if (va_fourcc == 0 || !display_) {
		return {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID};
	}

	unsigned num_attribs = 0;
	VAStatus st = vaQuerySurfaceAttributes(display_, vpp_config_, nullptr, &num_attribs);
	if (st != VA_STATUS_SUCCESS || num_attribs == 0) {
		return {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID};
	}

	std::vector<VASurfaceAttrib> attribs(num_attribs);
	st = vaQuerySurfaceAttributes(display_, vpp_config_, attribs.data(), &num_attribs);
	if (st != VA_STATUS_SUCCESS) {
		return {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID};
	}

	for (const auto &attrib : attribs) {
		if (attrib.type != VASurfaceAttribDRMFormatModifiers) {
			continue;
		}
		if (!(attrib.flags & VA_SURFACE_ATTRIB_GETTABLE) || attrib.value.type != VAGenericValueTypePointer) {
			break; // Settable-only, as documented above -- nothing to read.
		}
		auto *list = static_cast<VADRMFormatModifierList *>(attrib.value.value.p);
		if (list && list->num_modifiers > 0 && list->modifiers) {
			return std::vector<uint64_t>(list->modifiers, list->modifiers + list->num_modifiers);
		}
	}
	return {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID};
}

VASurfaceID VaapiEncoderBase::import_rgb_surface(const DmabufFrame &frame) {
	// Assumes a single DRM object backs every plane (true of the buffers
	// the screencast sources deliver: one linear/tiled GBM buffer, one fd). A frame with
	// planes spread across multiple dmabuf fds would need num_objects > 1.
	uint32_t va_fourcc = va_fourcc_from_drm(frame.format);
	if (va_fourcc == 0) {
		WLOG_ERROR("vaapi_encoder: unsupported dmabuf format 0x%08x", frame.format);
		return VA_INVALID_ID;
	}

	VADRMPRIMESurfaceDescriptor desc = {};
	desc.fourcc = va_fourcc;
	desc.width = (uint32_t)frame.width;
	desc.height = (uint32_t)frame.height;
	desc.num_objects = 1;
	desc.objects[0].fd = frame.fd[0];
	desc.objects[0].drm_format_modifier = frame.modifier;
	uint32_t max_extent = 0;
	for (int i = 0; i < frame.n_planes; i++) {
		uint32_t extent = frame.offset[i] + frame.stride[i] * (uint32_t)frame.height;
		max_extent = extent > max_extent ? extent : max_extent;
	}
	desc.objects[0].size = max_extent;
	desc.num_layers = 1;
	desc.layers[0].drm_format = frame.format;
	desc.layers[0].num_planes = (uint32_t)frame.n_planes;
	for (int i = 0; i < frame.n_planes; i++) {
		desc.layers[0].object_index[i] = 0;
		desc.layers[0].offset[i] = frame.offset[i];
		desc.layers[0].pitch[i] = frame.stride[i];
	}

	VASurfaceAttrib attribs[2] = {};
	attribs[0].type = VASurfaceAttribMemoryType;
	attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[0].value.type = VAGenericValueTypeInteger;
	attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
	attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
	attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
	attribs[1].value.type = VAGenericValueTypePointer;
	attribs[1].value.value.p = &desc;

	VASurfaceID surface = VA_INVALID_ID;
	VAStatus st = vaCreateSurfaces(display_, VA_RT_FORMAT_RGB32, (uint32_t)frame.width,
		(uint32_t)frame.height, &surface, 1, attribs, 2);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateSurfaces(RGB import) failed: %s", vaErrorStr(st));
		return VA_INVALID_ID;
	}
	return surface;
}

VASurfaceID VaapiEncoderBase::convert_to_nv12(VASurfaceID rgb_surface) {
	VASurfaceID target = nv12_surfaces_[next_nv12_];
	next_nv12_ = (next_nv12_ + 1) % kNumNv12Surfaces;

	// submit_frame() doesn't wait for the encode reading this same surface
	// (from up to kNumNv12Surfaces pushes ago) to finish before returning --
	// only push() as a whole is synchronous, via the vaSyncSurface below.
	// Without this wait, a VPP write here can race a still-in-flight encode
	// read of the same surface once the pool wraps around.
	vaSyncSurface(display_, target);

	// Explicit 1:1 rectangles (set per pass below): with both regions left
	// null the VPP scales the whole source onto the whole
	// (alignment-padded) target, i.e. a 1080-row frame gets stretched over
	// 1088 rows and the sequence header's bottom crop then discards real
	// desktop rows instead of padding. Pin the picture to the top-left so
	// the padding rows stay padding.
	VAProcPipelineParameterBuffer pipeline = {};
	pipeline.surface = rgb_surface;

	// The YCbCr matrix. The H.264 SPS (h264_bitstream.cpp) and the AV1
	// sequence header (av1_obu.cpp) state BT.601 studio range; the H.265
	// encoder writes no colour description, so there host and client agree
	// by convention. spectre picks BT.601
	// (VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601 in
	// present/video_image_source.cpp) and the software path's
	// xrgb_to_i420() uses BT.601 coefficients, so BT.601 is that convention
	// and this conversion has to match it.
	//
	// Left unset, radeonsi defaults to BT.709 for both ends, which decoded
	// as BT.601 shifts every saturated colour (pure red's luma is 63
	// instead of 82). Both ends must say BT.601, which matches the scalar
	// reference to within 1 LSB of rounding. Setting only
	// output_color_standard leaves the input taken as BT.709, and the
	// driver then converts primaries as well as the matrix -- a gamut
	// change these plain desktop RGB pixels don't want; the only transform
	// wanted here is RGB -> YCbCr.
	pipeline.surface_color_standard = VAProcColorStandardBT601;
	pipeline.output_color_standard = VAProcColorStandardBT601;
	// Already the driver's default (a 0..255 grey ramp converts to
	// 16..235 either way), so these are pinned rather than corrective --
	// full-range RGB in, studio-range YCbCr out is what spectre's
	// VK_SAMPLER_YCBCR_RANGE_ITU_NARROW expects, and it shouldn't depend on
	// a default staying put.
	pipeline.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
	pipeline.output_color_properties.color_range = VA_SOURCE_RANGE_REDUCED;

	// One VPP submission per (source rect -> target rect) pair. The first
	// is the picture itself; the rest, only when the surface is padded,
	// replicate the picture's last column/row/corner pixel across the
	// padding. Fresh surfaces are zeroed, and zero NV12 is green, so
	// without this the padding is a green strip that the encoder's in-loop
	// filters (deblocking, CDEF for AV1) smear a pixel or two into the
	// real picture along the right/bottom edge -- visible on av1, whose
	// padding is part of the decoded frame, at any width that isn't a
	// multiple of 8 (1366x768). H.264/H.265 crop the padding itself away
	// but their deblocking reads across the same boundary, so they get
	// the fill too. Edge replication is what any encoder does internally
	// for the same reason: the padding then predicts from the picture
	// instead of fighting it.
	struct Pass {
		VARectangle src;
		VARectangle dst;
	};
	Pass passes[4];
	int num_passes = 0;
	uint16_t w = (uint16_t)config_.width, h = (uint16_t)config_.height;
	uint16_t pad_w = (uint16_t)(padded_width_ - config_.width);
	uint16_t pad_h = (uint16_t)(padded_height_ - config_.height);
	passes[num_passes++] = {{0, 0, w, h}, {0, 0, w, h}};
	if (pad_w > 0) {
		passes[num_passes++] = {{(int16_t)(w - 1), 0, 1, h}, {(int16_t)w, 0, pad_w, h}};
	}
	if (pad_h > 0) {
		passes[num_passes++] = {{0, (int16_t)(h - 1), w, 1}, {0, (int16_t)h, w, pad_h}};
	}
	if (pad_w > 0 && pad_h > 0) {
		passes[num_passes++] = {{(int16_t)(w - 1), (int16_t)(h - 1), 1, 1},
			{(int16_t)w, (int16_t)h, pad_w, pad_h}};
	}

	for (int i = 0; i < num_passes; i++) {
		pipeline.surface_region = &passes[i].src;
		pipeline.output_region = &passes[i].dst;

		VABufferID pipeline_buf = VA_INVALID_ID;
		VAStatus st = vaCreateBuffer(display_, vpp_context_, VAProcPipelineParameterBufferType,
			sizeof(pipeline), 1, &pipeline, &pipeline_buf);
		if (st != VA_STATUS_SUCCESS) {
			WLOG_ERROR("vaapi_encoder: vaCreateBuffer(VPP pipeline) failed: %s", vaErrorStr(st));
			return VA_INVALID_ID;
		}

		st = vaBeginPicture(display_, vpp_context_, target);
		if (st == VA_STATUS_SUCCESS) {
			st = vaRenderPicture(display_, vpp_context_, &pipeline_buf, 1);
		}
		if (st == VA_STATUS_SUCCESS) {
			st = vaEndPicture(display_, vpp_context_);
		}
		// The parameter buffer is ours to free; nothing destroys it on our
		// behalf and one per frame at 60fps is a leak worth not having.
		vaDestroyBuffer(display_, pipeline_buf);
		if (st != VA_STATUS_SUCCESS) {
			WLOG_ERROR("vaapi_encoder: VPP submit failed: %s", vaErrorStr(st));
			return VA_INVALID_ID;
		}
	}

	// Blocks until the conversion -- and thus the read of rgb_surface, the
	// caller's live dmabuf -- has finished, so push() can hand the fd back
	// safely as soon as this returns.
	vaSyncSurface(display_, target);
	return target;
}

bool VaapiEncoderBase::push(const DmabufFrame &frame, int64_t pts_us) {
	if ((uint32_t)frame.width != config_.width || (uint32_t)frame.height != config_.height) {
		WLOG_ERROR("vaapi_encoder: push() frame %dx%d doesn't match the %ux%u this encoder was opened with; "
				   "re-open() on resolution change",
			frame.width, frame.height, config_.width, config_.height);
		return false;
	}

	VASurfaceID rgb = import_rgb_surface(frame);
	if (rgb == VA_INVALID_ID) {
		return false;
	}

	VASurfaceID nv12 = convert_to_nv12(rgb);
	// The imported surface is synced before it is destroyed, not only the
	// NV12 output: a synced output is not proof the input read has
	// finished on every driver.
	vaSyncSurface(display_, rgb);
	vaDestroySurfaces(display_, &rgb, 1);
	if (nv12 == VA_INVALID_ID) {
		return false;
	}

	return submit_frame(nv12, pts_us);
}

bool VaapiEncoderBase::ensure_cpu_upload_surface() {
	if (cpu_upload_surface_ != VA_INVALID_ID) {
		return true;
	}

	// The surface's pixel format is pinned to the exact format of the VAImage
	// below, and that is load-bearing. Mesa's vlVaPutImage only does a plain
	// upload when the image format equals the surface's; otherwise it
	// renders through a temporary surface, and on radeonsi that blit smears
	// every frame after the first by a fraction of a pixel (left to itself,
	// radeonsi picks ARGB for RGB32, and our image is BGRX). Same format on
	// both sides means no blit at all, and one copy fewer per frame.
	VASurfaceAttrib fmt_attrib = {};
	fmt_attrib.type = VASurfaceAttribPixelFormat;
	fmt_attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
	fmt_attrib.value.type = VAGenericValueTypeInteger;
	fmt_attrib.value.value.i = VA_FOURCC_BGRX;
	VAStatus st = vaCreateSurfaces(display_, VA_RT_FORMAT_RGB32, config_.width, config_.height,
		&cpu_upload_surface_, 1, &fmt_attrib, 1);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateSurfaces(CPU upload) failed: %s", vaErrorStr(st));
		cpu_upload_surface_ = VA_INVALID_ID;
		return false;
	}

	// vaCreateImage + vaPutImage rather than vaDeriveImage: a derived image
	// exposes the surface's real (possibly tiled) layout, which the driver
	// is free to make something memcpy can't fill row by row. vaPutImage
	// costs one driver-side copy and always works -- provided the formats
	// match, see above.
	VAImageFormat format = {};
	format.fourcc = VA_FOURCC_BGRX; // DRM_FORMAT_XRGB8888's byte order -- see va_fourcc_from_drm()
	format.byte_order = VA_LSB_FIRST;
	format.bits_per_pixel = 32;
	format.depth = 24;
	format.red_mask = 0x00ff0000;
	format.green_mask = 0x0000ff00;
	format.blue_mask = 0x000000ff;
	st = vaCreateImage(display_, &format, config_.width, config_.height, &cpu_upload_image_);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateImage(BGRX) failed: %s", vaErrorStr(st));
		destroy_cpu_upload_surface();
		return false;
	}
	cpu_upload_image_valid_ = true;
	return true;
}

void VaapiEncoderBase::destroy_cpu_upload_surface() {
	if (cpu_upload_image_valid_) {
		vaDestroyImage(display_, cpu_upload_image_.image_id);
		cpu_upload_image_ = {};
		cpu_upload_image_valid_ = false;
	}
	if (cpu_upload_surface_ != VA_INVALID_ID) {
		vaDestroySurfaces(display_, &cpu_upload_surface_, 1);
		cpu_upload_surface_ = VA_INVALID_ID;
	}
}

bool VaapiEncoderBase::push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride,
	int64_t pts_us, const DamageRegion *) {
	if (!display_) {
		return false;
	}
	if (width != config_.width || height != config_.height) {
		WLOG_ERROR(
			"vaapi_encoder: push_cpu() frame %ux%u doesn't match the %ux%u this encoder was opened with; "
			"re-open() on resolution change",
			width, height, config_.width, config_.height);
		return false;
	}
	if (!ensure_cpu_upload_surface()) {
		return false;
	}

	// The surface is read by the VPP in convert_to_nv12() below, which
	// syncs before returning, so by the time we're back here to overwrite
	// it nothing is still reading it.
	uint8_t *mapped = nullptr;
	VAStatus st = vaMapBuffer(display_, cpu_upload_image_.buf, (void **)&mapped);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaMapBuffer(CPU upload) failed: %s", vaErrorStr(st));
		return false;
	}
	uint32_t dst_stride = cpu_upload_image_.pitches[0];
	uint32_t copy_bytes = width * 4 < dst_stride ? width * 4 : dst_stride;
	for (uint32_t y = 0; y < height; y++) {
		memcpy(mapped + cpu_upload_image_.offsets[0] + (size_t)y * dst_stride, data + (size_t)y * stride,
			copy_bytes);
	}
	vaUnmapBuffer(display_, cpu_upload_image_.buf);

	st = vaPutImage(display_, cpu_upload_surface_, cpu_upload_image_.image_id, 0, 0, width, height, 0, 0,
		width, height);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaPutImage failed: %s", vaErrorStr(st));
		return false;
	}

	VASurfaceID nv12 = convert_to_nv12(cpu_upload_surface_);
	if (nv12 == VA_INVALID_ID) {
		return false;
	}
	return submit_frame(nv12, pts_us);
}

bool VaapiEncoderBase::take_idr_decision() {
	bool is_idr = force_idr_next_ || (config_.gop_size != 0 && frames_since_idr_ >= config_.gop_size);
	if (is_idr) {
		frames_since_idr_ = 0;
		force_idr_next_ = false;
		// Every IDR carries a new sequence parameter buffer, and Mesa
		// (radeonsi) takes the frame rate from it -- which ours leave unset
		// -- over the one the frame-rate misc buffer gave: from the first
		// IDR on it budgets each frame for its default rate instead, and
		// VBR on hard content runs at ~1.85x bits_per_second. Re-sending
		// both misc buffers after every sequence puts the real rate back.
		rc_dirty_ = true;
		fr_sent_ = false;
	}
	return is_idr;
}

bool VaapiEncoderBase::add_buffer(ParamBuffers &buffers, VABufferType type, unsigned size, const void *data) {
	VABufferID id = VA_INVALID_ID;
	VAStatus st = vaCreateBuffer(display_, enc_context_, type, size, 1, const_cast<void *>(data), &id);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateBuffer(type=%d) failed: %s", (int)type, vaErrorStr(st));
		return false;
	}
	buffers.ids.push_back(id);
	return true;
}

bool VaapiEncoderBase::add_packed_header(ParamBuffers &buffers, VAEncPackedHeaderType type,
	const std::vector<uint8_t> &nal) {
	VAEncPackedHeaderParameterBuffer param = {};
	param.type = type;
	param.bit_length = (uint32_t)nal.size() * 8;
	param.has_emulation_bytes = 1;
	return add_buffer(buffers, VAEncPackedHeaderParameterBufferType, sizeof(param), &param) &&
		add_buffer(buffers, VAEncPackedHeaderDataBufferType, (unsigned)nal.size(), nal.data());
}

bool VaapiEncoderBase::add_rate_control_buffers(ParamBuffers &buffers) {
	// VAEncMiscParameterBuffer ends in a C `data[]` flexible array member,
	// which C++ won't allow as a non-final struct member -- build the
	// header-plus-payload layout in a raw byte buffer instead.
	if (rc_dirty_) {
		std::vector<uint8_t> misc(sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl));
		auto *hdr = reinterpret_cast<VAEncMiscParameterBuffer *>(misc.data());
		auto *rc = reinterpret_cast<VAEncMiscParameterRateControl *>(misc.data() + sizeof(*hdr));
		hdr->type = VAEncMiscParameterTypeRateControl;
		*rc = {};
		// VBR: bits_per_second is the ceiling, target_percentage the share
		// of it the encoder aims for. 100 keeps the target at the ceiling
		// itself -- lower values (ffmpeg's h264_vaapi asks for 50 against a
		// doubled ceiling) shave a little more off idle frames but also cap
		// the typical rate under the -b/congestion-controller figure, which
		// is the number the rest of wraith reasons about.
		rc->bits_per_second = config_.bitrate_bps;
		rc->target_percentage = 100;
		rc->window_size = 1000;
		if (!add_buffer(buffers, VAEncMiscParameterBufferType, (unsigned)misc.size(), misc.data())) {
			return false;
		}
		rc_dirty_ = false;
	}
	if (!fr_sent_) {
		std::vector<uint8_t> misc(sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterFrameRate));
		auto *hdr = reinterpret_cast<VAEncMiscParameterBuffer *>(misc.data());
		auto *fr = reinterpret_cast<VAEncMiscParameterFrameRate *>(misc.data() + sizeof(*hdr));
		hdr->type = VAEncMiscParameterTypeFrameRate;
		*fr = {};
		fr->framerate = (config_.framerate_den << 16) | (config_.framerate_num & 0xffff);
		if (!add_buffer(buffers, VAEncMiscParameterBufferType, (unsigned)misc.size(), misc.data())) {
			return false;
		}
		fr_sent_ = true;
	}
	return true;
}

VABufferID VaapiEncoderBase::create_coded_buffer() {
	unsigned coded_buf_size = padded_width_ * padded_height_ * 3 / 2 + 0x10000;
	VABufferID coded_buf = VA_INVALID_ID;
	VAStatus st =
		vaCreateBuffer(display_, enc_context_, VAEncCodedBufferType, coded_buf_size, 1, nullptr, &coded_buf);
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: vaCreateBuffer(coded) failed: %s", vaErrorStr(st));
		return VA_INVALID_ID;
	}
	return coded_buf;
}

bool VaapiEncoderBase::submit_picture(VASurfaceID surface, ParamBuffers &buffers, VABufferID coded_buf,
	bool keyframe, int64_t pts_us) {
	VAStatus st = vaBeginPicture(display_, enc_context_, surface);
	if (st == VA_STATUS_SUCCESS) {
		st = vaRenderPicture(display_, enc_context_, buffers.ids.data(), (int)buffers.ids.size());
	}
	if (st == VA_STATUS_SUCCESS) {
		st = vaEndPicture(display_, enc_context_);
	}
	if (st != VA_STATUS_SUCCESS) {
		WLOG_ERROR("vaapi_encoder: encode submit failed: %s", vaErrorStr(st));
		vaDestroyBuffer(display_, coded_buf);
		return false;
	}

	frames_since_idr_++;
	pending_.push_back({coded_buf, pts_us, keyframe});
	if (asynchronous_) {
		arm_poll_timer(true);
	}
	return true;
}

void VaapiEncoderBase::arm_poll_timer(bool on) {
	constexpr long kAsyncPollMs = 2;
	struct itimerspec spec = {};
	if (on) {
		spec.it_value.tv_nsec = kAsyncPollMs * 1'000'000;
		spec.it_interval = spec.it_value;
	}
	timerfd_settime(timer_fd_, 0, &spec, nullptr);
}

void VaapiEncoderBase::request_keyframe() {
	force_idr_next_ = true;
}

void VaapiEncoderBase::set_bitrate(uint32_t bitrate_bps) {
	config_.bitrate_bps = bitrate_bps;
	rc_dirty_ = true;
}

EncodedPacket VaapiEncoderBase::drain_coded_buffer(VABufferID coded_buf, bool keyframe, int64_t pts_us) {
	EncodedPacket pkt;
	pkt.pts_us = pts_us;
	pkt.keyframe = keyframe;

	vaSyncBuffer(display_, coded_buf, VA_TIMEOUT_INFINITE);

	void *mapped = nullptr;
	VAStatus st = vaMapBuffer(display_, coded_buf, &mapped);
	if (st == VA_STATUS_SUCCESS && mapped) {
		for (auto *seg = static_cast<VACodedBufferSegment *>(mapped); seg != nullptr;
			seg = static_cast<VACodedBufferSegment *>(seg->next)) {
			const auto *p = static_cast<const uint8_t *>(seg->buf);
			pkt.data.insert(pkt.data.end(), p, p + seg->size);
		}
		vaUnmapBuffer(display_, coded_buf);
	} else {
		WLOG_ERROR("vaapi_encoder: vaMapBuffer(coded) failed: %s", vaErrorStr(st));
	}
	vaDestroyBuffer(display_, coded_buf);
	return pkt;
}

std::vector<EncodedPacket> VaapiEncoderBase::poll() {
	std::vector<EncodedPacket> out;
	if (asynchronous_) {
		uint64_t expirations;
		(void)!read(timer_fd_, &expirations, sizeof(expirations));
		// Oldest first, and only what has finished: frames complete in
		// submission order.
		size_t done = 0;
		for (; done < pending_.size(); done++) {
			VAStatus st = vaSyncBuffer(display_, pending_[done].coded_buf, 0);
			if (st == VA_STATUS_ERROR_TIMEDOUT) {
				break;
			}
			if (st == VA_STATUS_ERROR_UNIMPLEMENTED) {
				// A driver without timed waits: back to waiting for each one.
				WLOG_INFO("vaapi_encoder: the driver has no vaSyncBuffer timeout; waiting for each encode");
				asynchronous_ = false;
				arm_poll_timer(false);
				break;
			}
			const PendingFrame &pf = pending_[done];
			out.push_back(drain_coded_buffer(pf.coded_buf, pf.keyframe, pf.pts_us));
		}
		pending_.erase(pending_.begin(), pending_.begin() + (ptrdiff_t)done);
		if (asynchronous_) {
			if (pending_.empty()) {
				arm_poll_timer(false);
			}
			return out;
		}
	}
	out.reserve(out.size() + pending_.size());
	for (auto &pf : pending_) {
		out.push_back(drain_coded_buffer(pf.coded_buf, pf.keyframe, pf.pts_us));
	}
	pending_.clear();
	return out;
}

void VaapiEncoderBase::destroy_surface_pools() {
	auto destroy = [&](VASurfaceID *pool, int count) {
		bool any = false;
		for (int i = 0; i < count; i++) {
			any = any || pool[i] != VA_INVALID_ID;
		}
		if (any) {
			vaDestroySurfaces(display_, pool, count);
			std::fill(pool, pool + count, VA_INVALID_ID);
		}
	};
	destroy(nv12_surfaces_, kNumNv12Surfaces);
	destroy(dpb_surfaces_, kNumDpbSurfaces);
}

void VaapiEncoderBase::close() {
	if (timer_fd_ >= 0) {
		::close(timer_fd_);
		timer_fd_ = -1;
	}
	if (display_) {
		for (auto &pf : pending_) {
			vaSyncBuffer(display_, pf.coded_buf, VA_TIMEOUT_INFINITE);
			vaDestroyBuffer(display_, pf.coded_buf);
		}
	}
	pending_.clear();

	if (display_ && enc_context_ != VA_INVALID_ID) {
		vaDestroyContext(display_, enc_context_);
	}
	enc_context_ = VA_INVALID_ID;
	if (display_ && enc_config_ != VA_INVALID_ID) {
		vaDestroyConfig(display_, enc_config_);
	}
	enc_config_ = VA_INVALID_ID;
	if (display_ && vpp_context_ != VA_INVALID_ID) {
		vaDestroyContext(display_, vpp_context_);
	}
	vpp_context_ = VA_INVALID_ID;
	if (display_ && vpp_config_ != VA_INVALID_ID) {
		vaDestroyConfig(display_, vpp_config_);
	}
	vpp_config_ = VA_INVALID_ID;
	if (display_) {
		destroy_cpu_upload_surface();
		destroy_surface_pools();
		vaTerminate(display_);
	}
	display_ = nullptr;
}

} // namespace wraith
