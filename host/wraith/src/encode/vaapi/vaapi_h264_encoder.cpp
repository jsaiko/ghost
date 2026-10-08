// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/vaapi/vaapi_h264_encoder.hpp"
#include "encode/vaapi/h264_bitstream.hpp"

#include "util/log.hpp"

#include <va/va_enc_h264.h>

#include <iterator>

namespace wraith {

namespace {

// H.264 Annex A Table A-1 (MaxMBPS, MaxFS in macroblocks), enough of the
// table to cover everything from CIF to 4K-ish. Picks the lowest level that
// satisfies both the frame-size and macroblock-rate limits at the
// configured framerate; falls back to the highest listed level.
uint8_t pick_level_idc(uint32_t total_mbs, uint32_t fps_num, uint32_t fps_den) {
	struct LevelLimit {
		uint8_t level_idc;
		uint32_t max_mbps;
		uint32_t max_fs;
	};
	static const LevelLimit kLevels[] = {
		{30, 40500, 1620},
		{31, 108000, 3600},
		{32, 216000, 5120},
		{40, 245760, 8192},
		{41, 245760, 8192},
		{42, 522240, 8704},
		{50, 589824, 22080},
		{51, 983040, 36864},
		{52, 2073600, 36864},
		{60, 4177920, 139264},
		{61, 8355840, 139264},
		{62, 16711680, 139264},
	};
	uint32_t fps = fps_den > 0 ? (fps_num + fps_den - 1) / fps_den : fps_num;
	if (fps == 0) {
		fps = 1;
	}
	uint32_t mbps = total_mbs * fps;
	for (const auto &l : kLevels) {
		if (total_mbs <= l.max_fs && mbps <= l.max_mbps) {
			return l.level_idc;
		}
	}
	return kLevels[std::size(kLevels) - 1].level_idc;
}

} // namespace

VAProfile VaapiH264Encoder::enc_profile() const {
	return VAProfileH264Main;
}

bool VaapiH264Encoder::init_codec() {
	width_mb_ = padded_width_ / 16;
	height_mb_ = padded_height_ / 16;
	level_idc_ = pick_level_idc(width_mb_ * height_mb_, config_.framerate_num, config_.framerate_den);
	max_frame_num_ = 1u << (log2_max_frame_num_minus4_ + 4);

	h264::SpsParams sps_params{};
	sps_params.width_px = config_.width;
	sps_params.height_px = config_.height;
	sps_params.width_mb = width_mb_;
	sps_params.height_mb = height_mb_;
	sps_params.level_idc = level_idc_;
	sps_params.log2_max_frame_num_minus4 = log2_max_frame_num_minus4_;
	sps_nal_ = h264::wrap_nal(3, 7, h264::escape_emulation(h264::build_sps(sps_params)));
	pps_nal_ = h264::wrap_nal(3, 8, h264::escape_emulation(h264::build_pps()));

	frame_num_ = 0;
	idr_pic_id_ = 0;
	return true;
}

bool VaapiH264Encoder::submit_frame(VASurfaceID surface, int64_t pts_us) {
	bool is_idr = take_idr_decision();
	if (is_idr) {
		frame_num_ = 0;
		idr_pic_id_++;
	}

	ParamBuffers buffers(display_);

	if (is_idr) {
		VAEncSequenceParameterBufferH264 seq = {};
		seq.seq_parameter_set_id = 0;
		seq.level_idc = level_idc_;
		seq.intra_period = idr_period_hint();
		seq.intra_idr_period = idr_period_hint();
		seq.ip_period = 1;
		seq.bits_per_second = config_.bitrate_bps;
		seq.max_num_ref_frames = 1;
		seq.picture_width_in_mbs = (uint16_t)width_mb_;
		seq.picture_height_in_mbs = (uint16_t)height_mb_;
		seq.seq_fields.bits.chroma_format_idc = 1; // 4:2:0
		seq.seq_fields.bits.frame_mbs_only_flag = 1;
		seq.seq_fields.bits.direct_8x8_inference_flag = 1;
		seq.seq_fields.bits.log2_max_frame_num_minus4 = log2_max_frame_num_minus4_;
		seq.seq_fields.bits.pic_order_cnt_type = 2;
		bool crop = padded_width_ != config_.width || padded_height_ != config_.height;
		seq.frame_cropping_flag = crop;
		if (crop) {
			seq.frame_crop_right_offset = (padded_width_ - config_.width) / 2;
			seq.frame_crop_bottom_offset = (padded_height_ - config_.height) / 2;
		}
		if (!add_buffer(buffers, VAEncSequenceParameterBufferType, sizeof(seq), &seq)) {
			return false;
		}
		if (!add_packed_header(buffers, VAEncPackedHeaderSequence, sps_nal_)) {
			return false;
		}
		if (!add_packed_header(buffers, VAEncPackedHeaderPicture, pps_nal_)) {
			return false;
		}
	}

	if (!add_rate_control_buffers(buffers)) {
		return false;
	}

	VABufferID coded_buf = create_coded_buffer();
	if (coded_buf == VA_INVALID_ID) {
		return false;
	}

	// `surface` is only ever the vaBeginPicture input; the picture params
	// name a DPB surface as CurrPic (see dpb_surfaces_ in the base class
	// header for why naming the input here is a use-after-free on Mesa).
	VASurfaceID recon = dpb_surfaces_[next_dpb_];
	next_dpb_ = (next_dpb_ + 1) % kNumDpbSurfaces;

	VAEncPictureParameterBufferH264 pic = {};
	pic.CurrPic.picture_id = recon;
	pic.CurrPic.frame_idx = frame_num_;
	pic.CurrPic.flags = is_idr ? 0 : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
	pic.CurrPic.TopFieldOrderCnt = (int32_t)(2 * frame_num_);
	for (auto &r : pic.ReferenceFrames) {
		r.picture_id = VA_INVALID_SURFACE;
		r.flags = VA_PICTURE_H264_INVALID;
	}
	if (!is_idr) {
		pic.ReferenceFrames[0].picture_id = last_ref_surface_;
		pic.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
	}
	pic.coded_buf = coded_buf;
	pic.pic_parameter_set_id = 0;
	pic.seq_parameter_set_id = 0;
	pic.last_picture = 0;
	pic.frame_num = (uint16_t)frame_num_;
	pic.pic_init_qp = 26;
	pic.num_ref_idx_l0_active_minus1 = 0;
	pic.num_ref_idx_l1_active_minus1 = 0;
	pic.chroma_qp_index_offset = 0;
	pic.second_chroma_qp_index_offset = 0;
	pic.pic_fields.bits.idr_pic_flag = is_idr ? 1 : 0;
	pic.pic_fields.bits.reference_pic_flag = 1;
	pic.pic_fields.bits.entropy_coding_mode_flag = 0; // CAVLC, matches the packed PPS
	pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;
	if (!add_buffer(buffers, VAEncPictureParameterBufferType, sizeof(pic), &pic)) {
		vaDestroyBuffer(display_, coded_buf);
		return false;
	}

	// Mesa's radeonsi VA-API frontend only learns nal_ref_idc/nal_unit_type
	// for the slice NAL it emits by parsing a packed slice header (see
	// h264_bitstream.hpp's wrap_nal()/build_slice_header_prefix() comments)
	// -- without this every slice NAL gets written with header byte 0x00
	// (nal_unit_type 0, undecodable). The structured slice buffer below,
	// processed after this, still supplies the real per-frame values
	// (frame_num, qp, deblocking, ...); this one only needs to walk the
	// same syntax far enough for the driver to capture those two fields.
	h264::SliceHeaderParams slice_hdr_params{};
	slice_hdr_params.is_idr = is_idr;
	slice_hdr_params.slice_type = is_idr ? 2u : 0u;
	slice_hdr_params.frame_num = frame_num_;
	slice_hdr_params.log2_max_frame_num_minus4 = log2_max_frame_num_minus4_;
	slice_hdr_params.idr_pic_id = idr_pic_id_;
	std::vector<uint8_t> slice_nal = h264::wrap_nal(3, is_idr ? 5 : 1,
		h264::escape_emulation(h264::build_slice_header_prefix(slice_hdr_params)));
	if (!add_packed_header(buffers, VAEncPackedHeaderSlice, slice_nal)) {
		vaDestroyBuffer(display_, coded_buf);
		return false;
	}

	VAEncSliceParameterBufferH264 slice = {};
	slice.macroblock_address = 0;
	slice.num_macroblocks = width_mb_ * height_mb_;
	slice.macroblock_info = VA_INVALID_ID;
	slice.slice_type = is_idr ? 2 : 0; // 2 = I, 0 = P
	slice.pic_parameter_set_id = 0;
	slice.idr_pic_id = idr_pic_id_;
	slice.direct_spatial_mv_pred_flag = 0;
	slice.num_ref_idx_active_override_flag = is_idr ? 0 : 1;
	slice.num_ref_idx_l0_active_minus1 = 0;
	for (auto &r : slice.RefPicList0) {
		r.picture_id = VA_INVALID_SURFACE;
		r.flags = VA_PICTURE_H264_INVALID;
	}
	for (auto &r : slice.RefPicList1) {
		r.picture_id = VA_INVALID_SURFACE;
		r.flags = VA_PICTURE_H264_INVALID;
	}
	if (!is_idr) {
		slice.RefPicList0[0].picture_id = last_ref_surface_;
		slice.RefPicList0[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
	}
	slice.cabac_init_idc = 0;
	slice.slice_qp_delta = 0;
	slice.disable_deblocking_filter_idc = 0;
	slice.slice_alpha_c0_offset_div2 = 0;
	slice.slice_beta_offset_div2 = 0;
	if (!add_buffer(buffers, VAEncSliceParameterBufferType, sizeof(slice), &slice)) {
		vaDestroyBuffer(display_, coded_buf);
		return false;
	}

	if (!submit_picture(surface, buffers, coded_buf, is_idr, pts_us)) {
		return false;
	}

	last_ref_surface_ = recon;
	frame_num_ = (frame_num_ + 1) % max_frame_num_;
	return true;
}

} // namespace wraith
