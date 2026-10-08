// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/vaapi/vaapi_hevc_encoder.hpp"
#include "encode/vaapi/hevc_bitstream.hpp"

#include "util/log.hpp"

#include <va/va_enc_hevc.h>

#include <iterator>

namespace wraith {

namespace {

// H.265 Annex A Table A.6/A.8 (MaxLumaPs in samples, MaxLumaSr in
// samples/second), enough of the table to cover everything from SD to 8K.
// Picks the lowest level that satisfies both the picture-size and
// sample-rate limits at the configured framerate; falls back to the
// highest listed level. general_level_idc is 30 * the level number, hence
// 120 for level 4.
uint8_t pick_level_idc(uint64_t luma_samples, uint32_t fps_num, uint32_t fps_den) {
	struct LevelLimit {
		uint8_t level_idc;
		uint64_t max_luma_ps;
		uint64_t max_luma_sr;
	};
	static const LevelLimit kLevels[] = {
		{90, 552960, 16588800},      // 3.0
		{93, 983040, 33177600},      // 3.1
		{120, 2228224, 66846720},    // 4.0
		{123, 2228224, 133693440},   // 4.1
		{150, 8912896, 267386880},   // 5.0
		{153, 8912896, 534773760},   // 5.1
		{156, 8912896, 1069547520},  // 5.2
		{180, 35651584, 1069547520}, // 6.0
		{183, 35651584, 2139095040}, // 6.1
		{186, 35651584, 4278190080}, // 6.2
	};
	uint32_t fps = fps_den > 0 ? (fps_num + fps_den - 1) / fps_den : fps_num;
	if (fps == 0) {
		fps = 1;
	}
	uint64_t sample_rate = luma_samples * fps;
	for (const auto &l : kLevels) {
		if (luma_samples <= l.max_luma_ps && sample_rate <= l.max_luma_sr) {
			return l.level_idc;
		}
	}
	return kLevels[std::size(kLevels) - 1].level_idc;
}

} // namespace

VAProfile VaapiHevcEncoder::enc_profile() const {
	return VAProfileHEVCMain;
}

// HEVC hardware is much less uniform than H.264's fixed 16x16 macroblock:
// radeonsi (gfx1201, Mesa 26.0) advertises exactly one legal CTB size --
// min == max == 64 -- so "pick the spec default" is not an option and the
// driver has to be asked. Everything not covered by the attribute keeps
// the spec's smallest legal value, which every decoder handles.
void VaapiHevcEncoder::query_block_sizes() {
	VAConfigAttrib attrib = {};
	attrib.type = VAConfigAttribEncHEVCBlockSizes;
	VAStatus st = vaGetConfigAttributes(display_, enc_profile(), VAEntrypointEncSlice, &attrib, 1);
	if (st != VA_STATUS_SUCCESS || attrib.value == VA_ATTRIB_NOT_SUPPORTED) {
		WLOG_INFO("vaapi_encoder: HEVC block sizes not advertised, using 8..64 CTB / 4..32 TB");
		return;
	}

	VAConfigAttribValEncHEVCBlockSizes sizes;
	sizes.value = attrib.value;
	// The smallest coding block the driver allows, and the largest coding
	// tree block: log2_diff is what the SPS actually codes.
	uint32_t log2_min_cb = sizes.bits.log2_min_luma_coding_block_size_minus3 + 3;
	uint32_t log2_max_ctb = sizes.bits.log2_max_coding_tree_block_size_minus3 + 3;
	uint32_t log2_min_tb = sizes.bits.log2_min_luma_transform_block_size_minus2 + 2;
	uint32_t log2_max_tb = sizes.bits.log2_max_luma_transform_block_size_minus2 + 2;
	if (log2_max_ctb < log2_min_cb || log2_max_tb < log2_min_tb) {
		WLOG_ERROR("vaapi_encoder: nonsensical HEVC block sizes (ctb<=%u, cb>=%u, tb %u..%u), "
				   "using the defaults",
			log2_max_ctb, log2_min_cb, log2_min_tb, log2_max_tb);
		return;
	}

	log2_min_cb_minus3_ = (uint8_t)(log2_min_cb - 3);
	log2_diff_max_min_cb_ = (uint8_t)(log2_max_ctb - log2_min_cb);
	log2_min_tb_minus2_ = (uint8_t)(log2_min_tb - 2);
	log2_diff_max_min_tb_ = (uint8_t)(log2_max_tb - log2_min_tb);
	ctb_size_ = 1u << log2_max_ctb;
	// The transform tree can't be deeper than the CTB-to-smallest-TB gap.
	max_transform_hierarchy_depth_ = (uint8_t)(log2_max_ctb - log2_min_tb);
	if (max_transform_hierarchy_depth_ > 3) {
		max_transform_hierarchy_depth_ = 3;
	}
	WLOG_INFO("vaapi_encoder: HEVC blocks: CTB %u, min CB %u, TB %u..%u, transform depth %u", ctb_size_,
		1u << log2_min_cb, 1u << log2_min_tb, 1u << log2_max_tb, max_transform_hierarchy_depth_);
}

bool VaapiHevcEncoder::init_codec() {
	query_block_sizes();

	// surface_alignment() pads to 64, the largest CTB the spec allows, so
	// this holds for any driver; it is checked rather than assumed because
	// the CTB size is the one thing here that comes from the hardware.
	if (padded_width_ % ctb_size_ != 0 || padded_height_ % ctb_size_ != 0) {
		WLOG_ERROR("vaapi_encoder: %ux%u is not a multiple of this driver's %u-px HEVC "
				   "coding tree block",
			padded_width_, padded_height_, ctb_size_);
		return false;
	}

	level_idc_ = pick_level_idc((uint64_t)padded_width_ * padded_height_, config_.framerate_num,
		config_.framerate_den);
	max_poc_lsb_ = 1u << (log2_max_poc_lsb_minus4_ + 4);

	hevc::SpsParams sps_params{};
	sps_params.width_px = config_.width;
	sps_params.height_px = config_.height;
	sps_params.coded_width = padded_width_;
	sps_params.coded_height = padded_height_;
	sps_params.level_idc = level_idc_;
	sps_params.log2_min_luma_coding_block_size_minus3 = log2_min_cb_minus3_;
	sps_params.log2_diff_max_min_luma_coding_block_size = log2_diff_max_min_cb_;
	sps_params.log2_min_transform_block_size_minus2 = log2_min_tb_minus2_;
	sps_params.log2_diff_max_min_transform_block_size = log2_diff_max_min_tb_;
	sps_params.max_transform_hierarchy_depth_inter = max_transform_hierarchy_depth_;
	sps_params.max_transform_hierarchy_depth_intra = max_transform_hierarchy_depth_;
	sps_params.log2_max_pic_order_cnt_lsb_minus4 = log2_max_poc_lsb_minus4_;
	sps_params.amp_enabled = true;
	sps_params.sao_enabled = true;
	// Off deliberately: temporal MV prediction would make every picture's
	// motion vectors depend on the collocated picture's, which is one more
	// thing to get wrong for a few percent of bitrate, and it needs the
	// collocated reference plumbed through the picture parameters. The
	// H.264 backend makes the same trade (no direct_8x8 temporal mode).
	sps_params.temporal_mvp_enabled = false;
	sps_params.strong_intra_smoothing_enabled = true;

	vps_nal_ = hevc::wrap_nal(hevc::kNalVps, hevc::escape_emulation(hevc::build_vps(level_idc_)));
	sps_nal_ = hevc::wrap_nal(hevc::kNalSps, hevc::escape_emulation(hevc::build_sps(sps_params)));
	pps_nal_ = hevc::wrap_nal(hevc::kNalPps, hevc::escape_emulation(hevc::build_pps()));

	poc_ = 0;
	last_ref_poc_ = 0;
	return true;
}

bool VaapiHevcEncoder::submit_frame(VASurfaceID surface, int64_t pts_us) {
	bool is_idr = take_idr_decision();
	if (is_idr) {
		poc_ = 0;
	}

	ParamBuffers buffers(display_);

	if (is_idr) {
		VAEncSequenceParameterBufferHEVC seq = {};
		seq.general_profile_idc = 1; // Main
		seq.general_level_idc = level_idc_;
		seq.general_tier_flag = 0;
		seq.intra_period = idr_period_hint();
		seq.intra_idr_period = idr_period_hint();
		seq.ip_period = 1;
		seq.bits_per_second = config_.bitrate_bps;
		seq.pic_width_in_luma_samples = (uint16_t)padded_width_;
		seq.pic_height_in_luma_samples = (uint16_t)padded_height_;
		seq.seq_fields.bits.chroma_format_idc = 1; // 4:2:0
		seq.seq_fields.bits.bit_depth_luma_minus8 = 0;
		seq.seq_fields.bits.bit_depth_chroma_minus8 = 0;
		seq.seq_fields.bits.scaling_list_enabled_flag = 0;
		seq.seq_fields.bits.strong_intra_smoothing_enabled_flag = 1;
		seq.seq_fields.bits.amp_enabled_flag = 1;
		seq.seq_fields.bits.sample_adaptive_offset_enabled_flag = 1;
		seq.seq_fields.bits.pcm_enabled_flag = 0;
		seq.seq_fields.bits.sps_temporal_mvp_enabled_flag = 0;
		// No B-frames and one reference: every picture references only the
		// one before it, which is exactly what "low delay" describes.
		seq.seq_fields.bits.low_delay_seq = 1;
		seq.log2_min_luma_coding_block_size_minus3 = log2_min_cb_minus3_;
		seq.log2_diff_max_min_luma_coding_block_size = log2_diff_max_min_cb_;
		seq.log2_min_transform_block_size_minus2 = log2_min_tb_minus2_;
		seq.log2_diff_max_min_transform_block_size = log2_diff_max_min_tb_;
		seq.max_transform_hierarchy_depth_inter = max_transform_hierarchy_depth_;
		seq.max_transform_hierarchy_depth_intra = max_transform_hierarchy_depth_;
		seq.vui_parameters_present_flag = 0;
		if (!add_buffer(buffers, VAEncSequenceParameterBufferType, sizeof(seq), &seq)) {
			return false;
		}
		// HEVC's three sequence-level NALs all go in as VAEncPackedHeader
		// Sequence; there is no separate "packed VPS" type, and the driver
		// concatenates them in the order they're rendered.
		if (!add_packed_header(buffers, VAEncPackedHeaderSequence, vps_nal_) ||
			!add_packed_header(buffers, VAEncPackedHeaderSequence, sps_nal_) ||
			!add_packed_header(buffers, VAEncPackedHeaderSequence, pps_nal_)) {
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
	// name a DPB surface as the current picture (see dpb_surfaces_ in the
	// base class header for why naming the input here is a use-after-free
	// on Mesa).
	VASurfaceID recon = dpb_surfaces_[next_dpb_];
	next_dpb_ = (next_dpb_ + 1) % kNumDpbSurfaces;

	VAEncPictureParameterBufferHEVC pic = {};
	pic.decoded_curr_pic.picture_id = recon;
	pic.decoded_curr_pic.pic_order_cnt = (int32_t)poc_;
	pic.decoded_curr_pic.flags = 0;
	for (auto &r : pic.reference_frames) {
		r.picture_id = VA_INVALID_SURFACE;
		r.flags = VA_PICTURE_HEVC_INVALID;
	}
	if (!is_idr) {
		pic.reference_frames[0].picture_id = last_ref_surface_;
		pic.reference_frames[0].pic_order_cnt = last_ref_poc_;
		pic.reference_frames[0].flags = 0;
	}
	pic.coded_buf = coded_buf;
	// 0xff = "none": sps_temporal_mvp_enabled_flag is 0, so no picture is
	// collocated with this one.
	pic.collocated_ref_pic_index = 0xff;
	pic.last_picture = 0;
	pic.pic_init_qp = 26;
	pic.diff_cu_qp_delta_depth = 0;
	pic.pps_cb_qp_offset = 0;
	pic.pps_cr_qp_offset = 0;
	pic.num_tile_columns_minus1 = 0;
	pic.num_tile_rows_minus1 = 0;
	pic.log2_parallel_merge_level_minus2 = 0;
	pic.num_ref_idx_l0_default_active_minus1 = 0;
	pic.num_ref_idx_l1_default_active_minus1 = 0;
	pic.slice_pic_parameter_set_id = 0;
	pic.nal_unit_type = is_idr ? hevc::kNalIdrWRadl : hevc::kNalTrailR;
	pic.pic_fields.bits.idr_pic_flag = is_idr ? 1 : 0;
	pic.pic_fields.bits.coding_type = is_idr ? 1 : 2; // 1 = I, 2 = P
	pic.pic_fields.bits.reference_pic_flag = 1;
	pic.pic_fields.bits.cu_qp_delta_enabled_flag = 1; // matches the packed PPS
	pic.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
	if (!add_buffer(buffers, VAEncPictureParameterBufferType, sizeof(pic), &pic)) {
		vaDestroyBuffer(display_, coded_buf);
		return false;
	}

	// Mesa's radeonsi frontend reads the short-term reference picture set
	// -- which the structured slice buffer below has no field for -- out
	// of this packed header, and its driver half drops every other
	// application header for a picture that has no slice header among
	// them. See hevc_bitstream.hpp.
	hevc::SliceHeaderParams slice_hdr_params{};
	slice_hdr_params.is_idr = is_idr;
	slice_hdr_params.slice_type = is_idr ? 2u : 1u;
	slice_hdr_params.poc_lsb = poc_ % max_poc_lsb_;
	slice_hdr_params.log2_max_pic_order_cnt_lsb_minus4 = log2_max_poc_lsb_minus4_;
	slice_hdr_params.sao_enabled = true;
	std::vector<uint8_t> slice_nal = hevc::wrap_nal(is_idr ? hevc::kNalIdrWRadl : hevc::kNalTrailR,
		hevc::escape_emulation(hevc::build_slice_header_prefix(slice_hdr_params)));
	if (!add_packed_header(buffers, VAEncPackedHeaderSlice, slice_nal)) {
		vaDestroyBuffer(display_, coded_buf);
		return false;
	}

	uint32_t ctbs =
		((padded_width_ + ctb_size_ - 1) / ctb_size_) * ((padded_height_ + ctb_size_ - 1) / ctb_size_);

	VAEncSliceParameterBufferHEVC slice = {};
	slice.slice_segment_address = 0;
	slice.num_ctu_in_slice = ctbs;
	slice.slice_type = is_idr ? 2 : 1; // H.265 §7.4.7.1: 2 = I, 1 = P, 0 = B
	slice.slice_pic_parameter_set_id = 0;
	slice.num_ref_idx_l0_active_minus1 = 0;
	slice.num_ref_idx_l1_active_minus1 = 0;
	for (auto &r : slice.ref_pic_list0) {
		r.picture_id = VA_INVALID_SURFACE;
		r.flags = VA_PICTURE_HEVC_INVALID;
	}
	for (auto &r : slice.ref_pic_list1) {
		r.picture_id = VA_INVALID_SURFACE;
		r.flags = VA_PICTURE_HEVC_INVALID;
	}
	if (!is_idr) {
		// The one reference is always the previous picture, i.e. earlier in
		// output order: ST_CURR_BEFORE.
		slice.ref_pic_list0[0].picture_id = last_ref_surface_;
		slice.ref_pic_list0[0].pic_order_cnt = last_ref_poc_;
		slice.ref_pic_list0[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
	}
	slice.max_num_merge_cand = 5;
	slice.slice_qp_delta = 0;
	slice.slice_cb_qp_offset = 0;
	slice.slice_cr_qp_offset = 0;
	slice.slice_beta_offset_div2 = 0;
	slice.slice_tc_offset_div2 = 0;
	slice.slice_fields.bits.last_slice_of_pic_flag = 1;
	slice.slice_fields.bits.num_ref_idx_active_override_flag = is_idr ? 0 : 1;
	slice.slice_fields.bits.slice_sao_luma_flag = 1;
	slice.slice_fields.bits.slice_sao_chroma_flag = 1;
	slice.slice_fields.bits.slice_loop_filter_across_slices_enabled_flag = 1;
	slice.slice_fields.bits.collocated_from_l0_flag = 1;
	if (!add_buffer(buffers, VAEncSliceParameterBufferType, sizeof(slice), &slice)) {
		vaDestroyBuffer(display_, coded_buf);
		return false;
	}

	if (!submit_picture(surface, buffers, coded_buf, is_idr, pts_us)) {
		return false;
	}

	last_ref_surface_ = recon;
	last_ref_poc_ = (int32_t)poc_;
	poc_ = (poc_ + 1) % max_poc_lsb_;
	return true;
}

} // namespace wraith
