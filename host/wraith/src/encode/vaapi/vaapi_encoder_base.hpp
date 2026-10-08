// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The codec-independent half of wraith's VA-API encode path.
//
// Everything below is true of any VA-API encoder wraith might run: open a
// VADisplay on the render node, import the caller's RGB dmabuf as a VA
// surface (zero copy), VPP colour-space-convert it into an NV12 surface
// from a small driver-allocated pool (every encode entrypoint here only
// accepts YUV420 input), hand that to the codec's parameter buffers, and
// drain the coded buffer. Nothing round-trips through system RAM.
//
// What a codec adds on top is its parameter-buffer triplet (sequence /
// picture / slice), its own sequence-header writer, and its level
// selection -- i.e. init_codec() and submit_frame(). VaapiH264Encoder,
// VaapiHevcEncoder and VaapiAv1Encoder are the subclasses; the DPB and
// colour-matrix rules below apply to all three.
#pragma once

#include "encode/encoder.hpp"

#include <va/va.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace wraith {

class VaapiEncoderBase : public Encoder {
public:
	// Surface IDs start at VA_INVALID_ID, not 0 (a valid ID), so close()
	// after a failed open() never destroys a surface it didn't create.
	VaapiEncoderBase() {
		std::fill(std::begin(dpb_surfaces_), std::end(dpb_surfaces_), VA_INVALID_ID);
		std::fill(std::begin(nv12_surfaces_), std::end(nv12_surfaces_), VA_INVALID_ID);
	}
	~VaapiEncoderBase() override;

	bool open(const EncoderConfig &config) override;
	bool push(const DmabufFrame &frame, int64_t pts_us) override;
	// Implemented as well as push(), unusually for a hardware backend:
	// wants_cpu_frame() stays false (dmabuf import is still the path
	// callers should use), but a caller that already holds the pixels in
	// host memory can hand them straight over instead of round-tripping
	// through a dmabuf. RefineEncoder is why -- it needs the source pixels
	// on the CPU for its tile layer anyway, and would otherwise be stuck
	// on the software base encoder even on a machine with VA-API.
	bool push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride, int64_t pts_us,
		const DamageRegion *damage) override;
	std::vector<uint64_t> supported_import_modifiers(uint32_t drm_format) const override;
	void request_keyframe() override;
	void set_bitrate(uint32_t bitrate_bps) override;
	std::vector<EncodedPacket> poll() override;
	void close() override;
	void set_asynchronous(bool on) override { asynchronous_ = on; }
	int completion_fd() const override { return asynchronous_ ? timer_fd_ : -1; }
	bool ready_for_frame() const override { return !asynchronous_ || pending_.empty(); }

	// The VA profile to create the encode config with. Public so the
	// encoder factory can ask a render node whether it has an encode
	// entrypoint for it (hardware_encodable_codecs) without opening one.
	virtual VAProfile enc_profile() const = 0;

protected:
	// --- what a codec subclass supplies -------------------------------

	// enc_profile() above, and a name for it to appear under in log
	// messages ("H264Main", "HEVCMain", "AV1Profile0").
	virtual const char *enc_profile_name() const = 0;

	// Frame dimensions are rounded up to a multiple of this for the NV12
	// and DPB surfaces, and hence for the coded picture size. The default
	// of 16 is H.264's macroblock size, with the difference cropped away
	// in the SPS; VaapiHevcEncoder and VaapiAv1Encoder override it (64 and
	// 8) for the reasons given there.
	virtual uint32_t surface_alignment() const { return 16; }

	// Which packed headers the encode config is created with. The default
	// is the set the two Annex-B codecs send (sequence parameter sets,
	// picture parameter sets, slice headers); AV1 has no slice header but
	// does send a raw OBU, so it asks for a different set.
	virtual uint32_t packed_header_flags() const {
		return VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE | VA_ENC_PACKED_HEADER_SLICE;
	}

	// Called once from open() with the pipelines already up: derive the
	// codec's level, build its parameter-set NALs, reset its own counters.
	virtual bool init_codec() = 0;

	// Encode one frame from `surface` (an NV12 surface out of the input
	// pool). Implementations build their parameter buffers with the
	// helpers below and finish with submit_picture().
	virtual bool submit_frame(VASurfaceID surface, int64_t pts_us) = 0;

	// --- what a codec subclass builds on ------------------------------

	// True if this frame should be coded as an IDR, consuming the pending
	// request/GOP deadline: call exactly once per submit_frame(), and
	// reset the codec's own per-IDR state (frame_num, POC, ...) when it
	// returns true.
	bool take_idr_decision();

	// The sequence parameters' intra_period / intra_idr_period. Only a
	// hint -- take_idr_decision() places every IDR -- but VA-API leaves 0
	// undefined, so an infinite GOP goes to the driver as a long finite
	// one rather than as 0.
	uint32_t idr_period_hint() const { return config_.gop_size != 0 ? config_.gop_size : 1u << 16; }

	// One frame's parameter buffers, in the order submit_picture() hands
	// them to the driver. They are ours to free, and are freed when this goes
	// out of scope: after vaEndPicture, or on any earlier failure.
	struct ParamBuffers {
		explicit ParamBuffers(VADisplay display) : display(display) {}
		~ParamBuffers() {
			for (VABufferID id : ids) {
				vaDestroyBuffer(display, id);
			}
		}
		ParamBuffers(const ParamBuffers &) = delete;
		ParamBuffers &operator=(const ParamBuffers &) = delete;

		VADisplay display;
		std::vector<VABufferID> ids;
	};

	// Appends one parameter buffer to `buffers`.
	bool add_buffer(ParamBuffers &buffers, VABufferType type, unsigned size, const void *data);
	// A packed header (start-code prefixed, emulation-escaped) plus the
	// parameter buffer describing it.
	bool add_packed_header(ParamBuffers &buffers, VAEncPackedHeaderType type,
		const std::vector<uint8_t> &nal);
	// The rate-control and frame-rate misc buffers, each sent only when it
	// has something new to say (set_bitrate() marks the first dirty; the
	// second never changes after the first frame).
	bool add_rate_control_buffers(ParamBuffers &buffers);

	// Creates the coded (output) buffer for one frame. VA_INVALID_ID on
	// failure; the caller owns it until submit_picture() takes it.
	VABufferID create_coded_buffer();

	// vaBeginPicture/vaRenderPicture/vaEndPicture, then queue `coded_buf`
	// for poll(). Takes ownership of `coded_buf` either way: on failure it
	// is destroyed here.
	bool submit_picture(VASurfaceID surface, ParamBuffers &buffers, VABufferID coded_buf, bool keyframe,
		int64_t pts_us);

	EncoderConfig config_;
	VADisplay display_ = nullptr;
	VAContextID enc_context_ = VA_INVALID_ID;

	uint32_t padded_width_ = 0, padded_height_ = 0;

	// Reconstructed-picture (DPB) pool, distinct from the encode inputs:
	// the picture parameters' "current picture" and reference lists only
	// ever name these. Mesa's VA frontend turns any surface named as the
	// current picture into a DPB surface by destroying its buffer and
	// reallocating it in the driver's DPB layout (va/picture_h264_enc.c),
	// without refreshing the encode context's `target` pointer taken at
	// vaBeginPicture -- so naming the input surface there leaves the
	// encoder reading a freed buffer, which crashes in vaEndPicture under
	// concurrent GPU load. 2 = current + one reference, matching
	// max_num_ref_frames=1.
	static constexpr int kNumDpbSurfaces = 2;
	VASurfaceID dpb_surfaces_[kNumDpbSurfaces];
	int next_dpb_ = 0;

	// The DPB surface the last encoded picture was reconstructed into.
	VASurfaceID last_ref_surface_ = VA_INVALID_ID;

private:
	struct PendingFrame {
		VABufferID coded_buf;
		int64_t pts_us;
		bool keyframe;
	};

	bool init_display();
	bool init_encode_pipeline();
	bool init_vpp_pipeline();
	void destroy_surface_pools();

	// Imports `frame` as a short-lived RGB VA surface (VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2).
	VASurfaceID import_rgb_surface(const DmabufFrame &frame);

	// push_cpu()'s upload target: a driver-allocated RGB surface plus the
	// VAImage used to write host pixels into it. Created on first use and
	// kept for the life of the encoder (a per-frame create/destroy pair
	// shows up clearly in frame time at 60fps), so they stay VA_INVALID_ID
	// on a dmabuf-only session.
	bool ensure_cpu_upload_surface();
	void destroy_cpu_upload_surface();

	// VPP-converts `rgb_surface` into the next surface from nv12_surfaces_
	// and returns it. Blocks (vaSyncSurface) until the conversion completes,
	// so it's safe for the caller to release/reuse `rgb_surface`'s backing
	// dmabuf immediately after this returns.
	VASurfaceID convert_to_nv12(VASurfaceID rgb_surface);

	EncodedPacket drain_coded_buffer(VABufferID coded_buf, bool keyframe, int64_t pts_us);

	VAConfigID enc_config_ = VA_INVALID_ID;
	VAConfigID vpp_config_ = VA_INVALID_ID;
	VAContextID vpp_context_ = VA_INVALID_ID;

	// Encode *input* pool: the VPP writes each frame's NV12 into the next
	// one and it is the render target handed to vaBeginPicture. These are
	// never named in the picture parameters -- see dpb_surfaces_.
	static constexpr int kNumNv12Surfaces = 2;
	VASurfaceID nv12_surfaces_[kNumNv12Surfaces];
	int next_nv12_ = 0;

	VASurfaceID cpu_upload_surface_ = VA_INVALID_ID;
	VAImage cpu_upload_image_ = {};
	bool cpu_upload_image_valid_ = false;

	uint32_t frames_since_idr_ = 0;
	bool force_idr_next_ = true;
	// (Re)send the rate-control / frame-rate misc buffers with the next
	// picture: after open(), a set_bitrate(), and every IDR's sequence
	// parameters (take_idr_decision() says why).
	bool rc_dirty_ = true;
	bool fr_sent_ = false;

	std::vector<PendingFrame> pending_;

	// Asynchronous mode (set_asynchronous()): poll() never waits for the
	// GPU. It takes the frames vaSyncBuffer() with a zero timeout says are
	// done, and timer_fd_ ticks every kAsyncPollMs while any is still
	// encoding so the caller's event loop comes back for it. A blocking
	// wait would hold the session's main thread -- also capture and
	// input -- for a whole encode: 40-50 ms a 4K frame on a Polaris VCE. A
	// timer rather than a thread blocked in vaSyncBuffer(): the VA driver
	// isn't known to be safe to call from two threads at once.
	bool asynchronous_ = false;
	int timer_fd_ = -1;
	void arm_poll_timer(bool on);
};

} // namespace wraith
