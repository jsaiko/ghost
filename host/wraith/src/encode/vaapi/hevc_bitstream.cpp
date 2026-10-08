// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/vaapi/hevc_bitstream.hpp"

namespace wraith::hevc {

namespace {

// profile_tier_level(profilePresentFlag=1, maxNumSubLayersMinus1=0),
// H.265 §7.3.3. Fixed to Main profile, main tier, no sub-layers.
void put_profile_tier_level(BitWriter &bw, uint8_t level_idc) {
	bw.put_bits(0, 2); // general_profile_space
	bw.put_bit(0);     // general_tier_flag: main tier
	bw.put_bits(1, 5); // general_profile_idc = 1 (Main)
	// general_profile_compatibility_flag[32]: bit i says "conforms to
	// profile i". Main (1) is set; a decoder that only checks the
	// compatibility bits rather than profile_idc needs it.
	for (int i = 0; i < 32; i++) {
		bw.put_bit(i == 1 ? 1 : 0);
	}
	bw.put_bit(1);      // general_progressive_source_flag
	bw.put_bit(0);      // general_interlaced_source_flag
	bw.put_bit(1);      // general_non_packed_constraint_flag: no frame-packing SEI
	bw.put_bit(1);      // general_frame_only_constraint_flag
	bw.put_bits(0, 32); // general_reserved_zero_43bits, first 32 ...
	bw.put_bits(0, 11); // ... and the remaining 11
	bw.put_bit(0);      // general_inbld_flag (reserved zero for Main)
	bw.put_bits(level_idc, 8);
}

} // namespace

std::vector<uint8_t> wrap_nal(uint8_t nal_unit_type, const std::vector<uint8_t> &escaped_rbsp) {
	std::vector<uint8_t> out;
	out.reserve(5 + escaped_rbsp.size());
	out.push_back(0x00);
	out.push_back(0x00);
	out.push_back(0x01);
	// forbidden_zero_bit(1)=0, nal_unit_type(6), nuh_layer_id(6)=0,
	// nuh_temporal_id_plus1(3)=1.
	out.push_back(static_cast<uint8_t>((nal_unit_type & 0x3f) << 1));
	out.push_back(0x01);
	out.insert(out.end(), escaped_rbsp.begin(), escaped_rbsp.end());
	return out;
}

// video_parameter_set_rbsp(), §7.3.2.1. A single layer, a single temporal
// sub-layer, no timing info: everything here is the degenerate case, and
// the VPS exists at all only because HEVC requires one ahead of the SPS.
std::vector<uint8_t> build_vps(uint8_t level_idc) {
	BitWriter bw;
	bw.put_bits(0, 4);       // vps_video_parameter_set_id
	bw.put_bit(1);           // vps_base_layer_internal_flag
	bw.put_bit(1);           // vps_base_layer_available_flag
	bw.put_bits(0, 6);       // vps_max_layers_minus1
	bw.put_bits(0, 3);       // vps_max_sub_layers_minus1
	bw.put_bit(1);           // vps_temporal_id_nesting_flag
	bw.put_bits(0xffff, 16); // vps_reserved_0xffff_16bits
	put_profile_tier_level(bw, level_idc);
	bw.put_bit(1);     // vps_sub_layer_ordering_info_present_flag
	bw.put_ue(1);      // vps_max_dec_pic_buffering_minus1: current + one reference
	bw.put_ue(0);      // vps_max_num_reorder_pics: no B-frames, nothing reorders
	bw.put_ue(0);      // vps_max_latency_increase_plus1: unconstrained
	bw.put_bits(0, 6); // vps_max_layer_id
	bw.put_ue(0);      // vps_num_layer_sets_minus1
	bw.put_bit(0);     // vps_timing_info_present_flag
	bw.put_bit(0);     // vps_extension_flag
	bw.rbsp_trailing_bits();
	return bw.bytes();
}

// seq_parameter_set_rbsp(), §7.3.2.2.
std::vector<uint8_t> build_sps(const SpsParams &p) {
	BitWriter bw;
	bw.put_bits(0, 4); // sps_video_parameter_set_id: the VPS above
	bw.put_bits(0, 3); // sps_max_sub_layers_minus1
	bw.put_bit(1);     // sps_temporal_id_nesting_flag
	put_profile_tier_level(bw, p.level_idc);
	bw.put_ue(0); // sps_seq_parameter_set_id
	bw.put_ue(1); // chroma_format_idc = 4:2:0
	bw.put_ue(p.coded_width);
	bw.put_ue(p.coded_height);

	bool crop = p.coded_width != p.width_px || p.coded_height != p.height_px;
	bw.put_bit(crop ? 1 : 0); // conformance_window_flag
	if (crop) {
		// Offsets are in units of SubWidthC/SubHeightC, both 2 for 4:2:0.
		// Even padding deltas are guaranteed: open() rejects odd frame
		// sizes and the alignment is a power of two >= 2.
		bw.put_ue(0);
		bw.put_ue((p.coded_width - p.width_px) / 2);
		bw.put_ue(0);
		bw.put_ue((p.coded_height - p.height_px) / 2);
	}

	bw.put_ue(0); // bit_depth_luma_minus8
	bw.put_ue(0); // bit_depth_chroma_minus8
	bw.put_ue(p.log2_max_pic_order_cnt_lsb_minus4);
	bw.put_bit(1); // sps_sub_layer_ordering_info_present_flag
	bw.put_ue(1);  // sps_max_dec_pic_buffering_minus1 -- as in the VPS
	bw.put_ue(0);  // sps_max_num_reorder_pics
	bw.put_ue(0);  // sps_max_latency_increase_plus1

	bw.put_ue(p.log2_min_luma_coding_block_size_minus3);
	bw.put_ue(p.log2_diff_max_min_luma_coding_block_size);
	bw.put_ue(p.log2_min_transform_block_size_minus2);
	bw.put_ue(p.log2_diff_max_min_transform_block_size);
	bw.put_ue(p.max_transform_hierarchy_depth_inter);
	bw.put_ue(p.max_transform_hierarchy_depth_intra);
	bw.put_bit(0); // scaling_list_enabled_flag: flat lists
	bw.put_bit(p.amp_enabled ? 1 : 0);
	bw.put_bit(p.sao_enabled ? 1 : 0);
	bw.put_bit(0); // pcm_enabled_flag
	// num_short_term_ref_pic_sets = 0: each slice carries its own
	// st_ref_pic_set, which is what the driver generates from the
	// reference lists in the picture/slice parameter buffers.
	bw.put_ue(0);
	bw.put_bit(0); // long_term_ref_pics_present_flag
	bw.put_bit(p.temporal_mvp_enabled ? 1 : 0);
	bw.put_bit(p.strong_intra_smoothing_enabled ? 1 : 0);
	bw.put_bit(0); // vui_parameters_present_flag: see the colour-matrix
				   // comment in vaapi_encoder_base.cpp -- the convention is
				   // BT.601 by agreement, not by signalling
	bw.put_bit(0); // sps_extension_present_flag
	bw.rbsp_trailing_bits();
	return bw.bytes();
}

// pic_parameter_set_rbsp(), §7.3.2.3.
std::vector<uint8_t> build_pps() {
	BitWriter bw;
	bw.put_ue(0);      // pps_pic_parameter_set_id
	bw.put_ue(0);      // pps_seq_parameter_set_id
	bw.put_bit(0);     // dependent_slice_segments_enabled_flag
	bw.put_bit(0);     // output_flag_present_flag
	bw.put_bits(0, 3); // num_extra_slice_header_bits
	bw.put_bit(0);     // sign_data_hiding_enabled_flag
	bw.put_bit(0);     // cabac_init_present_flag
	bw.put_ue(0);      // num_ref_idx_l0_default_active_minus1: one reference
	bw.put_ue(0);      // num_ref_idx_l1_default_active_minus1
	bw.put_se(0);      // init_qp_minus26: actual QP is rate-control driven
	bw.put_bit(0);     // constrained_intra_pred_flag
	bw.put_bit(0);     // transform_skip_enabled_flag
	bw.put_bit(1);     // cu_qp_delta_enabled_flag -- see the header comment
	bw.put_ue(0);      // diff_cu_qp_delta_depth: QP groups are whole CTBs
	bw.put_se(0);      // pps_cb_qp_offset
	bw.put_se(0);      // pps_cr_qp_offset
	bw.put_bit(0);     // pps_slice_chroma_qp_offsets_present_flag
	bw.put_bit(0);     // weighted_pred_flag
	bw.put_bit(0);     // weighted_bipred_flag
	bw.put_bit(0);     // transquant_bypass_enabled_flag
	bw.put_bit(0);     // tiles_enabled_flag: one slice covers the picture
	bw.put_bit(0);     // entropy_coding_sync_enabled_flag
	bw.put_bit(1);     // pps_loop_filter_across_slices_enabled_flag
	bw.put_bit(1);     // deblocking_filter_control_present_flag
	bw.put_bit(0);     // deblocking_filter_override_enabled_flag
	bw.put_bit(0);     // pps_deblocking_filter_disabled_flag
	bw.put_se(0);      // pps_beta_offset_div2
	bw.put_se(0);      // pps_tc_offset_div2
	bw.put_bit(0);     // pps_scaling_list_data_present_flag
	bw.put_bit(0);     // lists_modification_present_flag
	bw.put_ue(0);      // log2_parallel_merge_level_minus2
	bw.put_bit(0);     // slice_segment_header_extension_present_flag
	bw.put_bit(0);     // pps_extension_present_flag
	bw.rbsp_trailing_bits();
	return bw.bytes();
}

// slice_segment_header(), §7.3.6.1.
std::vector<uint8_t> build_slice_header_prefix(const SliceHeaderParams &p) {
	BitWriter bw;
	bw.put_bit(1); // first_slice_segment_in_pic_flag: one slice per picture
	if (p.is_idr) {
		// nal_unit_type is in the IRAP range, which carries this flag.
		bw.put_bit(0); // no_output_of_prior_pics_flag
	}
	bw.put_ue(0); // slice_pic_parameter_set_id
	// num_extra_slice_header_bits is 0 and output_flag_present_flag is 0,
	// so slice_type comes next.
	bw.put_ue(p.slice_type);

	if (!p.is_idr) {
		bw.put_bits(p.poc_lsb, (int)(p.log2_max_pic_order_cnt_lsb_minus4 + 4));
		bw.put_bit(0); // short_term_ref_pic_set_sps_flag: written inline below
		// st_ref_pic_set(0), §7.3.7. stRpsIdx is 0, so no
		// inter_ref_pic_set_prediction_flag. One negative (earlier in
		// output order) reference, the picture immediately before this
		// one, and it is used by the current picture.
		bw.put_ue(1);  // num_negative_pics
		bw.put_ue(0);  // num_positive_pics
		bw.put_ue(0);  // delta_poc_s0_minus1[0]: the previous picture
		bw.put_bit(1); // used_by_curr_pic_s0_flag[0]
					   // long_term_ref_pics_present_flag and sps_temporal_mvp_enabled_flag
					   // are both 0, so nothing follows.
	}

	if (p.sao_enabled) {
		bw.put_bit(1); // slice_sao_luma_flag
		bw.put_bit(1); // slice_sao_chroma_flag
	}

	if (p.slice_type != 2) { // P (or B, which this encoder never emits)
		bw.put_bit(1);       // num_ref_idx_active_override_flag
		bw.put_ue(0);        // num_ref_idx_l0_active_minus1: the one reference
		// lists_modification_present_flag and cabac_init_present_flag are
		// both 0 in the PPS, and mvd_l1_zero_flag is B-only.
		bw.put_ue(0); // five_minus_max_num_merge_cand: 5 candidates
	}

	bw.put_se(0); // slice_qp_delta: QP is rate-control driven per picture
	// pps_slice_chroma_qp_offsets_present_flag and
	// deblocking_filter_override_enabled_flag are both 0.
	// pps_loop_filter_across_slices_enabled_flag is 1 and SAO is on, so:
	bw.put_bit(1); // slice_loop_filter_across_slices_enabled_flag
	// tiles_enabled_flag and entropy_coding_sync_enabled_flag are 0, so no
	// entry point offsets; slice_segment_header_extension_present_flag is
	// 0. byte_alignment() is rbsp_trailing_bits() by another name.
	bw.rbsp_trailing_bits();
	return bw.bytes();
}

} // namespace wraith::hevc
