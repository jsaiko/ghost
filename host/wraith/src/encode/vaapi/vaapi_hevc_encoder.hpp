// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// VA-API H.265 backend for the Encoder interface.
//
// Structurally the H.264 backend's twin: the whole pipeline -- dmabuf
// import, VPP colour conversion, DPB handling, coded buffer drain -- comes
// from VaapiEncoderBase (vaapi_encoder_base.hpp), and what's here is HEVC's
// parameter buffer triplet plus the VPS/SPS/PPS writer in
// hevc_bitstream.hpp.
//
// Coding structure matches H.264's deliberately, so a session behaves the
// same whichever codec it negotiated: one slice per picture, no B-frames,
// a single reference, IDR every EncoderConfig::gop_size frames or on
// request. Only the block sizes are queried from the driver
// (VAConfigAttribEncHEVCBlockSizes), because HEVC hardware genuinely
// differs there -- radeonsi, for instance, supports exactly one CTB size.
#pragma once

#include "encode/vaapi/vaapi_encoder_base.hpp"

#include <cstdint>
#include <vector>

namespace wraith {

class VaapiHevcEncoder : public VaapiEncoderBase {
public:
	VAProfile enc_profile() const override;

protected:
	const char *enc_profile_name() const override { return "HEVCMain"; }
	// 64 is the largest coding tree block H.265 allows, so a 64-aligned
	// frame is a whole number of CTBs on any hardware. Padding only to
	// the 8-px minimum coding block instead would be legal, but radeonsi
	// then pads the picture to its own CTB size behind our back and
	// (on Mesa 26.0) emits no conformance window for the difference, so
	// the client sees 1920x1088 for a 1080p desktop. Doing the alignment
	// here keeps the cropping ours to state.
	uint32_t surface_alignment() const override { return 64; }
	bool init_codec() override;
	bool submit_frame(VASurfaceID surface, int64_t pts_us) override;

private:
	// Reads VAConfigAttribEncHEVCBlockSizes and picks coding/transform
	// block sizes inside what it allows, falling back to the spec's
	// smallest-legal set if the driver doesn't answer.
	void query_block_sizes();

	uint8_t level_idc_ = 120;
	uint8_t log2_min_cb_minus3_ = 0;
	uint8_t log2_diff_max_min_cb_ = 3;
	uint8_t log2_min_tb_minus2_ = 0;
	uint8_t log2_diff_max_min_tb_ = 3;
	uint8_t max_transform_hierarchy_depth_ = 3;
	// MaxPicOrderCntLsb = 2^(this + 4) = 256. poc_ wraps at it inside a
	// longer GOP, which is legal: the slice header carries only the LSBs
	// and a relative reference (delta -1), and decoders rebuild the full
	// POC themselves. radeonsi encodes across the wrap correctly.
	uint8_t log2_max_poc_lsb_minus4_ = 4;
	uint32_t ctb_size_ = 64;

	std::vector<uint8_t> vps_nal_;
	std::vector<uint8_t> sps_nal_;
	std::vector<uint8_t> pps_nal_;

	uint32_t poc_ = 0;
	uint32_t max_poc_lsb_ = 256;
	int32_t last_ref_poc_ = 0;
};

} // namespace wraith
