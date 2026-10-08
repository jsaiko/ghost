// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// VA-API H.264 backend for the Encoder interface.
//
// The pipeline -- dmabuf import, VPP colour conversion, DPB handling, coded
// buffer drain -- lives in VaapiEncoderBase (vaapi_encoder_base.hpp) and is
// shared with the H.265 and AV1 backends. What's left here is the H.264
// parameter buffer triplet and the SPS/PPS writer.
#pragma once

#include "encode/vaapi/vaapi_encoder_base.hpp"

#include <cstdint>
#include <vector>

namespace wraith {

class VaapiH264Encoder : public VaapiEncoderBase {
public:
	VAProfile enc_profile() const override;

protected:
	const char *enc_profile_name() const override { return "H264Main"; }
	bool init_codec() override;
	bool submit_frame(VASurfaceID surface, int64_t pts_us) override;

private:
	uint32_t width_mb_ = 0, height_mb_ = 0;
	uint8_t level_idc_ = 51;
	uint8_t log2_max_frame_num_minus4_ = 4;

	std::vector<uint8_t> sps_nal_;
	std::vector<uint8_t> pps_nal_;

	uint32_t frame_num_ = 0;
	uint32_t max_frame_num_ = 0;
	uint16_t idr_pic_id_ = 0;
};

} // namespace wraith
