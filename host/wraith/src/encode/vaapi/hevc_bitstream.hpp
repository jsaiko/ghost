// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Hand-rolled H.265 VPS/SPS/PPS bitstream construction, the HEVC
// counterpart to h264_bitstream.hpp.
//
// As with H.264, VA-API's HEVC encode entrypoint generates slice data from
// structured parameter buffers but expects the application to supply the
// sequence-level NALs verbatim. HEVC needs three of them rather than two:
// a VPS ahead of the SPS, and the SPS itself is a different syntax rather
// than a variant of H.264's. The values written here must stay in lockstep
// with the VAEncSequenceParameterBufferHEVC / VAEncPictureParameterBufferHEVC
// fields set in vaapi_hevc_encoder.cpp -- see the comments there.
//
// A packed *slice* header is needed too, for two reasons. Mesa's radeonsi
// frontend reads the per-slice syntax out of it
// (parseEncSliceParamsH265) rather than only out of the structured
// VAEncSliceParameterBufferHEVC -- in particular the short-term reference
// picture set, which the structured buffer has no field for. And its
// driver half only emits *any* application-supplied header for a picture
// that also has a slice header among them (radeon_vcn_enc_encode_headers
// returns early when num_slices == 0), so without one the VPS/SPS/PPS are
// silently dropped and the stream is undecodable. As with H.264 the driver
// regenerates the headers it actually emits from the state these parse
// into, so what matters is that the syntax here is well-formed and says
// what the parameter buffers say.
#pragma once

#include "encode/vaapi/nal_bitstream.hpp"

#include <cstdint>
#include <vector>

namespace wraith::hevc {

using nal::BitWriter;
using nal::escape_emulation;

// nal_unit_type values (H.265 Table 7-1) this encoder emits.
inline constexpr uint8_t kNalTrailR = 1;    // non-IDR reference picture
inline constexpr uint8_t kNalIdrWRadl = 19; // IDR
inline constexpr uint8_t kNalVps = 32;
inline constexpr uint8_t kNalSps = 33;
inline constexpr uint8_t kNalPps = 34;

struct SpsParams {
	uint32_t width_px;     // display width (post-cropping)
	uint32_t height_px;    // display height (post-cropping)
	uint32_t coded_width;  // pic_width_in_luma_samples (min-CB aligned)
	uint32_t coded_height; // pic_height_in_luma_samples
	uint8_t level_idc;
	// Coding/transform block sizes, as the matching VAEncSequence
	// ParameterBufferHEVC fields name them; both come from what the
	// driver advertises in VAConfigAttribEncHEVCBlockSizes.
	uint8_t log2_min_luma_coding_block_size_minus3;
	uint8_t log2_diff_max_min_luma_coding_block_size;
	uint8_t log2_min_transform_block_size_minus2;
	uint8_t log2_diff_max_min_transform_block_size;
	uint8_t max_transform_hierarchy_depth_inter;
	uint8_t max_transform_hierarchy_depth_intra;
	uint8_t log2_max_pic_order_cnt_lsb_minus4;
	bool amp_enabled;
	bool sao_enabled;
	bool temporal_mvp_enabled;
	bool strong_intra_smoothing_enabled;
};

// profile_idc is fixed at 1 (Main): 8-bit 4:2:0, which is exactly what the
// VPP conversion in vaapi_encoder_base.cpp produces, and the only HEVC
// profile every hardware decoder implements.
std::vector<uint8_t> build_vps(uint8_t level_idc);
std::vector<uint8_t> build_sps(const SpsParams &params);

// cu_qp_delta_enabled_flag is fixed at 1 (with diff_cu_qp_delta_depth 0):
// the driver's rate control varies QP within a picture, and without it the
// emitted CU-level QP deltas would have nowhere to go. Matches the
// VAEncPictureParameterBufferHEVC fields in vaapi_hevc_encoder.cpp.
std::vector<uint8_t> build_pps();

struct SliceHeaderParams {
	bool is_idr;
	uint32_t slice_type; // H.265 §7.4.7.1: 2 = I, 1 = P
	uint32_t poc_lsb;    // slice_pic_order_cnt_lsb, ignored for an IDR
	uint8_t log2_max_pic_order_cnt_lsb_minus4;
	// The reference is always the immediately preceding picture, so the
	// short-term RPS this writes is the single entry (poc - 1, used).
	bool sao_enabled;
};

// slice_segment_header() for the picture's one slice, as far as the
// deblocking/loop-filter fields -- everything Mesa's parser reads before
// it returns. Assumes what build_sps()/build_pps() wrote:
// num_extra_slice_header_bits=0, output_flag_present_flag=0,
// num_short_term_ref_pic_sets=0 (so the RPS is written out in full here),
// no long-term references, sps_temporal_mvp_enabled_flag=0,
// lists_modification_present_flag=0, cabac_init_present_flag=0,
// deblocking_filter_override_enabled_flag=0.
std::vector<uint8_t> build_slice_header_prefix(const SliceHeaderParams &params);

// Wraps `rbsp` (already escaped) with the Annex-B start code (00 00 01)
// and HEVC's *two*-byte NAL header (forbidden_zero_bit=0, nal_unit_type,
// nuh_layer_id=0, nuh_temporal_id_plus1=1). As with H.264, the start code
// has to be part of what's handed to the driver, not just of the final
// output -- Mesa's packed-header handler scans the buffer it's given for
// one and ignores the whole thing if it finds none.
std::vector<uint8_t> wrap_nal(uint8_t nal_unit_type, const std::vector<uint8_t> &escaped_rbsp);

} // namespace wraith::hevc
