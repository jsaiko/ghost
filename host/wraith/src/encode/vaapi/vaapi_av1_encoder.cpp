// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/vaapi/vaapi_av1_encoder.hpp"
#include "encode/vaapi/av1_obu.hpp"

#include "util/log.hpp"

#include <va/va_enc_av1.h>

#include <iterator>

namespace wraith {

namespace {

// AV1 Annex A Table A.1 (MaxPicSize in samples, MaxHSize/MaxVSize in
// samples, MaxDisplayRate in samples/second), enough of the table to cover
// everything from SD to 8K. Picks the lowest level that satisfies all four
// limits at the configured framerate; falls back to the highest listed.
// seq_level_idx counts levels in order, so 2.0 is 0 and 4.0 is 8.
uint8_t pick_seq_level_idx(uint32_t width, uint32_t height, uint32_t fps_num, uint32_t fps_den) {
	struct LevelLimit {
		uint8_t seq_level_idx;
		uint64_t max_pic_size;
		uint32_t max_h_size;
		uint32_t max_v_size;
		uint64_t max_display_rate;
	};
	static const LevelLimit kLevels[] = {
		{0, 147456, 2048, 1152, 4423680},           // 2.0
		{1, 278784, 2816, 1584, 8363520},           // 2.1
		{4, 665856, 4352, 2448, 19975680},          // 3.0
		{5, 1065024, 5504, 3096, 31950720},         // 3.1
		{8, 2359296, 6144, 3456, 70778880},         // 4.0
		{9, 2359296, 6144, 3456, 141557760},        // 4.1
		{12, 8912896, 8192, 4352, 267386880},       // 5.0
		{13, 8912896, 8192, 4352, 534773760},       // 5.1
		{14, 8912896, 8192, 4352, 1069547520},      // 5.2
		{16, 35651584, 16384, 8704, 1069547520},    // 6.0
		{17, 35651584, 16384, 8704, 2139095040},    // 6.1
		{18, 35651584, 16384, 8704, 4278190080ull}, // 6.2
	};
	uint32_t fps = fps_den > 0 ? (fps_num + fps_den - 1) / fps_den : fps_num;
	if (fps == 0) {
		fps = 1;
	}
	uint64_t samples = (uint64_t)width * height;
	uint64_t display_rate = samples * fps;
	for (const auto &l : kLevels) {
		if (samples <= l.max_pic_size && width <= l.max_h_size && height <= l.max_v_size &&
			display_rate <= l.max_display_rate) {
			return l.seq_level_idx;
		}
	}
	return kLevels[std::size(kLevels) - 1].seq_level_idx;
}

} // namespace

VAProfile VaapiAv1Encoder::enc_profile() const {
	// Profile 0: 4:2:0, 8 or 10 bit. The NV12 surfaces the shared VPP step
	// produces are the 8-bit case, and it is the only AV1 profile radeonsi
	// exposes an encode entrypoint for.
	return VAProfileAV1Profile0;
}

bool VaapiAv1Encoder::init_codec() {
	sb_cols_ = (padded_width_ + 63) / 64;
	sb_rows_ = (padded_height_ + 63) / 64;

	// AV1 has no crop (see surface_alignment()), so a desktop whose
	// dimensions are not multiples of 8 is coded with a few columns or
	// rows of padding along the right or bottom edge. spectre crops them
	// to the display size from SessionAccept; any other consumer of the
	// stream (the -e dump, say) sees them.
	if (padded_width_ != config_.width || padded_height_ != config_.height) {
		WLOG_INFO("vaapi_encoder: AV1 codes %ux%u for a %ux%u desktop -- AV1 cannot crop, so "
				  "%u column(s) and %u row(s) of padding are in the coded picture",
			padded_width_, padded_height_, config_.width, config_.height, padded_width_ - config_.width,
			padded_height_ - config_.height);
	}

	seq_level_idx_ =
		pick_seq_level_idx(padded_width_, padded_height_, config_.framerate_num, config_.framerate_den);

	av1::SeqParams seq_params{};
	seq_params.coded_width = padded_width_;
	seq_params.coded_height = padded_height_;
	seq_params.seq_level_idx = seq_level_idx_;
	seq_params.order_hint_bits = kOrderHintBits;
	// CDEF on, with the strengths left to the firmware (radeonsi maps
	// "enabled" to its own default-strength mode). It is the one in-loop
	// filter this hardware will pick parameters for unprompted, and a
	// deblocked desktop reads better at the bitrates this streams at.
	seq_params.enable_cdef = true;
	// BT.601, studio range: what the shared VPP step produces (see
	// convert_to_nv12() in vaapi_encoder_base.cpp) and what spectre
	// converts back with. The H.264 SPS states it in its VUI; the H.265
	// backend leaves it implicit (no VUI); AV1's sequence header has a
	// colour description in it either way, so this one says what it means.
	seq_params.color_primaries = 6;          // BT.601 / SMPTE 170M
	seq_params.transfer_characteristics = 6; // BT.601 / SMPTE 170M
	seq_params.matrix_coefficients = 6;      // BT.601 / SMPTE 170M
	seq_params.full_range = false;
	sequence_obu_ = av1::build_sequence_header(seq_params);
	temporal_delimiter_obu_ = av1::build_temporal_delimiter();

	order_hint_ = 0;
	return true;
}

bool VaapiAv1Encoder::submit_frame(VASurfaceID surface, int64_t pts_us) {
	bool is_key = take_idr_decision();
	if (is_key) {
		order_hint_ = 0;
	}

	ParamBuffers buffers(display_);

	// Every temporal unit starts with a delimiter -- ahead of the sequence
	// header, which is why it is added first -- and the driver emits none
	// of its own. It is also the only header of ours that reaches the
	// stream byte for byte -- the sequence header is regenerated and the
	// frame header is discarded, so this is what keeps the picture's
	// application headers from being dropped wholesale (the driver's
	// "num_headers == num_slices" early-out, see hevc_bitstream.hpp).
	if (!add_packed_header(buffers, VAEncPackedHeaderRawData, temporal_delimiter_obu_)) {
		return false;
	}

	if (is_key) {
		VAEncSequenceParameterBufferAV1 seq = {};
		seq.seq_profile = 0;
		seq.seq_level_idx = seq_level_idx_;
		seq.seq_tier = 0;
		seq.hierarchical_flag = 0;
		seq.intra_period = idr_period_hint();
		seq.ip_period = 1;
		seq.bits_per_second = config_.bitrate_bps;
		seq.seq_fields.bits.bit_depth_minus8 = 0;
		seq.seq_fields.bits.subsampling_x = 1;
		seq.seq_fields.bits.subsampling_y = 1;
		seq.seq_fields.bits.mono_chrome = 0;
		seq.seq_fields.bits.enable_order_hint = 1;
		seq.seq_fields.bits.enable_cdef = 1;
		seq.order_hint_bits_minus_1 = kOrderHintBits - 1;
		if (!add_buffer(buffers, VAEncSequenceParameterBufferType, sizeof(seq), &seq)) {
			return false;
		}
		if (!add_packed_header(buffers, VAEncPackedHeaderSequence, sequence_obu_)) {
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
	// name a DPB surface as the reconstructed picture (see dpb_surfaces_
	// in the base class header for why naming the input here is a
	// use-after-free on Mesa).
	VASurfaceID recon = dpb_surfaces_[next_dpb_];
	next_dpb_ = (next_dpb_ + 1) % kNumDpbSurfaces;

	VAEncPictureParameterBufferAV1 pic = {};
	pic.frame_width_minus_1 = (uint16_t)(padded_width_ - 1);
	pic.frame_height_minus_1 = (uint16_t)(padded_height_ - 1);
	pic.reconstructed_frame = recon;
	pic.coded_buf = coded_buf;
	for (auto &ref : pic.reference_frames) {
		ref = VA_INVALID_SURFACE;
	}
	for (auto &idx : pic.ref_frame_idx) {
		idx = kRefSlot;
	}
	if (!is_key) {
		pic.reference_frames[kRefSlot] = last_ref_surface_;
		// search_idx0 is a 1-based index into ref_frame_idx[], so 1 names
		// ref_frame_idx[0] -- LAST_FRAME, the picture before this one.
		pic.ref_frame_ctrl_l0.fields.search_idx0 = 1;
	}
	pic.primary_ref_frame = is_key ? av1::kPrimaryRefNone : 0;
	pic.order_hint = (uint8_t)order_hint_;
	// A shown key frame refreshes every slot; everything else replaces the
	// one reference with itself.
	pic.refresh_frame_flags = is_key ? 0xff : (uint8_t)(1u << kRefSlot);
	pic.picture_flags.bits.frame_type = is_key ? av1::kFrameTypeKey : av1::kFrameTypeInter;
	pic.picture_flags.bits.error_resilient_mode = is_key ? 1 : 0;
	pic.picture_flags.bits.enable_frame_obu = 1; // one tile group, so header
												 // and tile data share an OBU
	pic.picture_flags.bits.allow_screen_content_tools = 1;
	// The reason screen-content tools are on: palette coding is what makes
	// flat-coloured UI and text cheap, and it is the one screen-content
	// tool this hardware advertises (VAConfigAttribValEncAV1's
	// support_palette_mode; intra block copy it does not).
	pic.picture_flags.bits.palette_mode_enable = 1;
	pic.picture_flags.bits.force_integer_mv = is_key ? 1 : 0;
	pic.num_tile_groups_minus1 = 0;
	pic.superres_scale_denominator = 8; // SUPERRES_NUM: unscaled
	pic.interpolation_filter = 4;       // SWITCHABLE
	// base_qindex 0 hands the quantizer to the driver's rate control
	// (Mesa reads a zero here as "no application-requested QP"), which is
	// the same division of labour as the other two backends.
	pic.base_qindex = 0;
	pic.min_base_qindex = 0;
	pic.max_base_qindex = 0;
	pic.mode_control_flags.bits.tx_mode = 2;        // TX_MODE_SELECT
	pic.mode_control_flags.bits.reference_mode = 0; // SINGLE_REFERENCE
	pic.tile_cols = 1;
	pic.tile_rows = 1;
	pic.width_in_sbs_minus_1[0] = (uint16_t)(sb_cols_ - 1);
	pic.height_in_sbs_minus_1[0] = (uint16_t)(sb_rows_ - 1);
	pic.context_update_tile_id = 0;
	if (!add_buffer(buffers, VAEncPictureParameterBufferType, sizeof(pic), &pic)) {
		vaDestroyBuffer(display_, coded_buf);
		return false;
	}

	// Most of the frame header the driver emits is taken from parsing this
	// one, not from the picture parameters above -- the refresh flags, the
	// reference indices, the render size (see av1_obu.hpp). It also counts
	// as this picture's "slice", which is what makes the delimiter above
	// reach the stream.
	av1::FrameHeaderParams hdr{};
	hdr.is_key = is_key;
	hdr.render_width = config_.width;
	hdr.render_height = config_.height;
	hdr.order_hint = order_hint_;
	hdr.order_hint_bits = kOrderHintBits;
	hdr.refresh_frame_flags = pic.refresh_frame_flags;
	hdr.primary_ref_frame = pic.primary_ref_frame;
	hdr.ref_frame_idx = kRefSlot;
	std::vector<uint8_t> frame_header = av1::build_frame_header_prefix(hdr);
	if (!add_packed_header(buffers, VAEncPackedHeaderPicture, frame_header)) {
		vaDestroyBuffer(display_, coded_buf);
		return false;
	}

	// One tile group covering the picture's one tile. AV1's equivalent of
	// a slice parameter buffer, and the driver needs one to know the tile
	// group exists at all.
	VAEncTileGroupBufferAV1 tile_group = {};
	tile_group.tg_start = 0;
	tile_group.tg_end = 0;
	if (!add_buffer(buffers, VAEncSliceParameterBufferType, sizeof(tile_group), &tile_group)) {
		vaDestroyBuffer(display_, coded_buf);
		return false;
	}

	if (!submit_picture(surface, buffers, coded_buf, is_key, pts_us)) {
		return false;
	}

	last_ref_surface_ = recon;
	order_hint_ = (order_hint_ + 1) % kOrderHintWrap;
	return true;
}

} // namespace wraith
