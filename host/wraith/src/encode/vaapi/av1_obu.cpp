// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/vaapi/av1_obu.hpp"

namespace wraith::av1 {

namespace {

// The number of bits AV1 codes a size-1 value in: f(n) with n one more
// than the position of the value's top bit. Matches what the driver
// computes when it regenerates the sequence header, so the two agree on
// how wide max_frame_width_minus_1 is.
int size_bits(uint32_t value) {
	int bits = 1;
	while ((value >> bits) != 0) {
		bits++;
	}
	return bits;
}

void put_leb128(std::vector<uint8_t> &out, uint64_t value) {
	do {
		uint8_t byte = value & 0x7f;
		value >>= 7;
		if (value != 0) {
			byte |= 0x80;
		}
		out.push_back(byte);
	} while (value != 0);
}

// render_size() (AV1 §5.9.6), always stated. It is not a crop -- AV1 has
// none, and decoders read this as an aspect-ratio hint -- but it is the
// only place the real desktop size appears once the picture has been
// padded up to the driver's 8-px alignment, so it is worth stating
// truthfully rather than leaving the padded size to speak for itself.
void put_render_size(BitWriter &bw, const FrameHeaderParams &p) {
	bw.put_bit(1); // render_and_frame_size_different
	bw.put_bits(p.render_width - 1, 16);
	bw.put_bits(p.render_height - 1, 16);
}

} // namespace

std::vector<uint8_t> wrap_obu(uint8_t obu_type, const std::vector<uint8_t> &payload) {
	std::vector<uint8_t> out;
	out.reserve(payload.size() + 8);
	// obu_forbidden_bit(0) | obu_type(4) | obu_extension_flag(0) |
	// obu_has_size_field(1) | obu_reserved_1bit(0).
	out.push_back(static_cast<uint8_t>(((obu_type & 0x0f) << 3) | 0x02));
	put_leb128(out, payload.size());
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

std::vector<uint8_t> build_temporal_delimiter() {
	return wrap_obu(kObuTemporalDelimiter, {});
}

std::vector<uint8_t> build_sequence_header(const SeqParams &p) {
	BitWriter bw;
	bw.put_bits(0, 3);  // seq_profile = 0: 4:2:0, 8/10-bit
	bw.put_bit(0);      // still_picture
	bw.put_bit(0);      // reduced_still_picture_header
	bw.put_bit(0);      // timing_info_present_flag
	bw.put_bit(0);      // initial_display_delay_present_flag
	bw.put_bits(0, 5);  // operating_points_cnt_minus_1: one operating point
	bw.put_bits(0, 12); // operating_point_idc[0]: all layers
	bw.put_bits(p.seq_level_idx, 5);
	if (p.seq_level_idx > 7) {
		bw.put_bit(0); // seq_tier[0]: Main tier
	}

	int width_bits = size_bits(p.coded_width - 1);
	int height_bits = size_bits(p.coded_height - 1);
	bw.put_bits((uint32_t)(width_bits - 1), 4);   // frame_width_bits_minus_1
	bw.put_bits((uint32_t)(height_bits - 1), 4);  // frame_height_bits_minus_1
	bw.put_bits(p.coded_width - 1, width_bits);   // max_frame_width_minus_1
	bw.put_bits(p.coded_height - 1, height_bits); // max_frame_height_minus_1

	bw.put_bit(0); // frame_id_numbers_present_flag
	// The coding tools below are all off, and all of them have to be: the
	// driver regenerates this header with these exact bits hard-coded to
	// zero (radeon_bs_av1_seq), so anything else here would describe a
	// stream the hardware is not going to produce.
	bw.put_bit(0); // use_128x128_superblock: 64x64 superblocks
	bw.put_bit(0); // enable_filter_intra
	bw.put_bit(0); // enable_intra_edge_filter
	bw.put_bit(0); // enable_interintra_compound
	bw.put_bit(0); // enable_masked_compound
	bw.put_bit(0); // enable_warped_motion
	bw.put_bit(0); // enable_dual_filter
	bw.put_bit(1); // enable_order_hint
	bw.put_bit(0); // enable_jnt_comp
	bw.put_bit(0); // enable_ref_frame_mvs
	// seq_choose_screen_content_tools = 1 leaves allow_screen_content_tools
	// to be chosen per frame, which is what lets palette coding be turned
	// on for a desktop (see vaapi_av1_encoder.cpp).
	bw.put_bit(1); // seq_choose_screen_content_tools -> SELECT_SCREEN_CONTENT_TOOLS
	bw.put_bit(0); // seq_choose_integer_mv = 0 ...
	bw.put_bit(0); // ... so force_integer_mv = 0: normal motion vectors
	bw.put_bits((uint32_t)(p.order_hint_bits - 1), 3); // order_hint_bits_minus_1

	bw.put_bit(0); // enable_superres
	bw.put_bit(p.enable_cdef ? 1 : 0);
	bw.put_bit(0); // enable_restoration

	// color_config()
	bw.put_bit(0); // high_bitdepth: 8-bit
	bw.put_bit(0); // mono_chrome
	bw.put_bit(1); // color_description_present_flag
	bw.put_bits(p.color_primaries, 8);
	bw.put_bits(p.transfer_characteristics, 8);
	bw.put_bits(p.matrix_coefficients, 8);
	bw.put_bit(p.full_range ? 1 : 0); // color_range
	// seq_profile 0 implies subsampling_x = subsampling_y = 1 (4:2:0), so
	// chroma_sample_position is present and nothing else is.
	bw.put_bits(0, 2); // chroma_sample_position = CSP_UNKNOWN
	bw.put_bit(0);     // separate_uv_delta_q

	bw.put_bit(0); // film_grain_params_present
	bw.rbsp_trailing_bits();
	return wrap_obu(kObuSequenceHeader, bw.bytes());
}

std::vector<uint8_t> build_frame_header_prefix(const FrameHeaderParams &p) {
	BitWriter bw;
	bw.put_bit(0); // show_existing_frame
	bw.put_bits(p.is_key ? kFrameTypeKey : kFrameTypeInter, 2);
	bw.put_bit(1); // show_frame: no reordering, every picture is displayed
	if (!p.is_key) {
		// A shown key frame has error_resilient_mode inferred as 1; an
		// inter frame codes it, and 0 is what we want -- a lost reference
		// is answered with a fresh key frame, not with a stream that can
		// be decoded from the middle.
		bw.put_bit(0); // error_resilient_mode
	}
	bw.put_bit(0); // disable_cdf_update
	// force_screen_content_tools was left SELECT in the sequence header,
	// so this is coded per frame. force_integer_mv is not: seq_choose_
	// integer_mv pinned it to 0.
	bw.put_bit(1); // allow_screen_content_tools
	bw.put_bit(0); // frame_size_override_flag: the sequence's max size is
				   // this picture's coded size
	bw.put_bits(p.order_hint, p.order_hint_bits);
	if (!p.is_key) {
		// Coded only when the frame is inter and not error-resilient,
		// which is exactly the case here.
		bw.put_bits(p.primary_ref_frame, 3);
		bw.put_bits(p.refresh_frame_flags, 8);
	}
	// (A shown key frame refreshes every slot, and says so by saying
	// nothing: refresh_frame_flags is inferred as 0xff.)

	if (p.is_key) {
		put_render_size(bw, p);
		bw.put_bit(0); // allow_intrabc: coded because screen content tools
					   // are allowed and there is no superres scaling
	} else {
		bw.put_bit(0); // frame_refs_short_signaling
		for (int i = 0; i < kRefsPerFrame; i++) {
			// Every reference slot this picture may name points at the
			// one slot the previous picture refreshed.
			bw.put_bits(p.ref_frame_idx, 3);
		}
		put_render_size(bw, p);
		bw.put_bit(0); // allow_high_precision_mv
		bw.put_bit(1); // is_filter_switchable: matches the SWITCHABLE
					   // interpolation filter in the picture parameters
		bw.put_bit(0); // is_motion_mode_switchable
					   // use_ref_frame_mvs is not coded: enable_ref_frame_mvs is 0.
	}

	bw.put_bit(0); // disable_frame_end_update_cdf
	// tile_info(). Mesa's parser stops after this bit and the driver
	// decides the tiling itself, so this is where the header ends.
	bw.put_bit(1); // uniform_tile_spacing_flag
	bw.rbsp_trailing_bits();
	return wrap_obu(kObuFrameHeader, bw.bytes());
}

} // namespace wraith::av1
