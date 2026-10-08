// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/vaapi/h264_bitstream.hpp"

namespace wraith::h264 {

std::vector<uint8_t> wrap_nal(uint8_t nal_ref_idc, uint8_t nal_unit_type,
	const std::vector<uint8_t> &escaped_rbsp) {
	std::vector<uint8_t> out;
	out.reserve(4 + escaped_rbsp.size());
	out.push_back(0x00);
	out.push_back(0x00);
	out.push_back(0x01);
	uint8_t header = static_cast<uint8_t>(((nal_ref_idc & 0x3) << 5) | (nal_unit_type & 0x1f));
	out.push_back(header);
	out.insert(out.end(), escaped_rbsp.begin(), escaped_rbsp.end());
	return out;
}

// profile_idc 77 (Main) deliberately skips the high-profile chroma_format_idc
// / bit_depth / seq_scaling_matrix block (§7.3.2.1.1's `if (profile_idc ==
// 100 || ...)`) -- Main implies 4:2:0, 8-bit, no scaling lists, which is
// exactly what the VPP conversion in vaapi_encoder_base.cpp produces.
std::vector<uint8_t> build_sps(const SpsParams &p) {
	BitWriter bw;
	bw.put_bits(77, 8); // profile_idc = Main
	bw.put_bits(0, 8);  // 6 constraint_set flags + 2 reserved bits, all zero
	bw.put_bits(p.level_idc, 8);
	bw.put_ue(0); // seq_parameter_set_id
	bw.put_ue(p.log2_max_frame_num_minus4);
	bw.put_ue(2);               // pic_order_cnt_type = 2: POC = 2 * frame_num, no B-frames
	bw.put_ue(1);               // max_num_ref_frames (matches VAConfigAttribEncMaxRefFrames)
	bw.put_bit(0);              // gaps_in_frame_num_value_allowed_flag
	bw.put_ue(p.width_mb - 1);  // pic_width_in_mbs_minus1
	bw.put_ue(p.height_mb - 1); // pic_height_in_map_units_minus1
	bw.put_bit(1);              // frame_mbs_only_flag: progressive, no interlace
	bw.put_bit(1);              // direct_8x8_inference_flag

	uint32_t padded_w = p.width_mb * 16;
	uint32_t padded_h = p.height_mb * 16;
	bool crop = padded_w != p.width_px || padded_h != p.height_px;
	bw.put_bit(crop ? 1 : 0);
	if (crop) {
		// 4:2:0 crop units are 2 luma samples in both directions here
		// (CropUnitX = SubWidthC = 2; CropUnitY = SubHeightC * (2 -
		// frame_mbs_only_flag) = 2 * 1 = 2). Requires even padding deltas,
		// which 16-px macroblock alignment always gives for even width/height.
		bw.put_ue(0);
		bw.put_ue((padded_w - p.width_px) / 2);
		bw.put_ue(0);
		bw.put_ue((padded_h - p.height_px) / 2);
	}
	// A VUI with the colour description and bitstream_restriction.
	// Without bitstream_restriction a decoder has to assume pictures may be
	// reordered and is allowed to hold up to a full DPB before output --
	// Chrome's hardware decoders do (4 frames at 1080p level 4.2), so an
	// idle desktop showed a keypress only after four more frames.
	// max_num_reorder_frames = 0 says each frame can be output as soon as
	// it is decoded.
	bw.put_bit(1); // vui_parameters_present_flag
	bw.put_bit(0); // aspect_ratio_info_present_flag
	bw.put_bit(0); // overscan_info_present_flag
	// BT.601 studio range, what VPP converts to (vaapi_encoder_base.cpp)
	// and spectre decodes as. Unsignalled, each decoder guesses: Firefox
	// takes HD video as BT.709 and every colour shifts.
	bw.put_bit(1);     // video_signal_type_present_flag
	bw.put_bits(5, 3); // video_format: unspecified
	bw.put_bit(0);     // video_full_range_flag: studio range
	bw.put_bit(1);     // colour_description_present_flag
	bw.put_bits(6, 8); // colour_primaries: SMPTE 170M
	bw.put_bits(6, 8); // transfer_characteristics: SMPTE 170M
	bw.put_bits(6, 8); // matrix_coefficients: SMPTE 170M (BT.601)
	bw.put_bit(0);     // chroma_loc_info_present_flag
	bw.put_bit(0);     // timing_info_present_flag
	bw.put_bit(0);     // nal_hrd_parameters_present_flag
	bw.put_bit(0);     // vcl_hrd_parameters_present_flag
	bw.put_bit(0);     // pic_struct_present_flag
	bw.put_bit(1);     // bitstream_restriction_flag
	bw.put_bit(1);     // motion_vectors_over_pic_boundaries_flag
	bw.put_ue(2);      // max_bytes_per_pic_denom (the inferred default)
	bw.put_ue(1);      // max_bits_per_mb_denom (the inferred default)
	bw.put_ue(15);     // log2_max_mv_length_horizontal (the inferred default)
	bw.put_ue(15);     // log2_max_mv_length_vertical (the inferred default)
	bw.put_ue(0);      // max_num_reorder_frames: no B-frames, nothing reorders
	bw.put_ue(1);      // max_dec_frame_buffering: max_num_ref_frames above
	bw.rbsp_trailing_bits();
	return bw.bytes();
}

std::vector<uint8_t> build_pps() {
	BitWriter bw;
	bw.put_ue(0);      // pic_parameter_set_id
	bw.put_ue(0);      // seq_parameter_set_id
	bw.put_bit(0);     // entropy_coding_mode_flag = CAVLC
	bw.put_bit(0);     // bottom_field_pic_order_in_frame_present_flag
	bw.put_ue(0);      // num_slice_groups_minus1
	bw.put_ue(0);      // num_ref_idx_l0_default_active_minus1
	bw.put_ue(0);      // num_ref_idx_l1_default_active_minus1
	bw.put_bit(0);     // weighted_pred_flag
	bw.put_bits(0, 2); // weighted_bipred_idc
	bw.put_se(0);      // pic_init_qp_minus26: actual QP is rate-control driven per frame
	bw.put_se(0);      // pic_init_qs_minus26
	bw.put_se(0);      // chroma_qp_index_offset
	bw.put_bit(1);     // deblocking_filter_control_present_flag
	bw.put_bit(0);     // constrained_intra_pred_flag
	bw.put_bit(0);     // redundant_pic_cnt_present_flag
	bw.rbsp_trailing_bits();
	return bw.bytes();
}

// Mirrors slice_header() as far as parseEncSliceParamsH264() in Mesa's
// radeonsi VA-API frontend reads it (see wrap_nal()'s comment for why this
// NAL is sent at all): that function's sole purpose here is to capture
// nal_ref_idc/nal_unit_type into the driver's internal picture-control
// state before it returns, so everything after slice_type just needs to be
// well-formed enough for the parser to walk past it consistently with the
// PPS flags in build_pps() (deblocking_filter_control_present_flag=1,
// redundant_pic_cnt_present_flag=0, entropy_coding_mode_flag=0/CAVLC). The
// driver regenerates the *actual* emitted slice header from the structured
// VAEncSliceParameterBufferH264 buffer, not from this.
std::vector<uint8_t> build_slice_header_prefix(const SliceHeaderParams &p) {
	BitWriter bw;
	bw.put_ue(0); // first_mb_in_slice
	bw.put_ue(p.slice_type);
	bw.put_ue(0); // pic_parameter_set_id
	bw.put_bits(p.frame_num, (int)(p.log2_max_frame_num_minus4 + 4));
	if (p.is_idr) {
		bw.put_ue(p.idr_pic_id);
	}
	// pic_order_cnt_type == 2: no pic_order_cnt_lsb field.
	bool is_p = p.slice_type != 2 && p.slice_type != 7; // VA-API numbering: 2/7 = I
	if (is_p) {
		bw.put_bit(0); // num_ref_idx_active_override_flag
		bw.put_bit(0); // ref_pic_list_modification_flag_l0
	}
	if (p.is_idr) {
		bw.put_bit(0); // no_output_of_prior_pics_flag
		bw.put_bit(0); // long_term_reference_flag
	} else {
		bw.put_bit(0); // adaptive_ref_pic_marking_mode_flag (nal_ref_idc != 0 always here)
	}
	bw.put_se(0); // slice_qp_delta
	bw.put_ue(0); // disable_deblocking_filter_idc
	bw.put_se(0); // slice_alpha_c0_offset_div2
	bw.put_se(0); // slice_beta_offset_div2
	bw.rbsp_trailing_bits();
	return bw.bytes();
}

} // namespace wraith::h264
