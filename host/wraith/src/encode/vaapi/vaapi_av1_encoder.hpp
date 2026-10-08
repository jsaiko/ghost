// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// VA-API AV1 backend for the Encoder interface.
//
// The third subclass of VaapiEncoderBase (vaapi_encoder_base.hpp), which
// still owns the whole pipeline: dmabuf import, VPP colour conversion, the
// DPB pool, the coded-buffer drain. What is here is AV1's parameter-buffer
// pair plus the OBU writer in av1_obu.hpp.
//
// Coding structure matches the other two deliberately, so a session
// behaves the same whichever codec it negotiated: one tile per picture, no
// reordering (every frame is shown as it is coded), a single reference, a
// key frame every EncoderConfig::gop_size frames or on request.
//
// Where AV1 differs from H.264/H.265 here, it is because Mesa's radeonsi
// driver differs:
//
//   - There is no packed slice header, because AV1 has no slices. The
//     frame header plays that role -- the driver's "does this picture have
//     a slice?" test counts it -- and a temporal delimiter OBU rides ahead
//     of it as the one header copied through verbatim.
//
//   - Far more of the bitstream comes from the packed headers than from
//     the structured buffers; see av1_obu.hpp.
//
//   - The driver substitutes its own aligned picture size into the
//     sequence header it emits, as it does for H.265 -- but AV1, unlike
//     H.264 and H.265, has no crop to state the difference with, so this
//     backend pads to the driver's own alignment instead of to a
//     superblock. See surface_alignment().
#pragma once

#include "encode/vaapi/vaapi_encoder_base.hpp"

#include <cstdint>
#include <vector>

namespace wraith {

class VaapiAv1Encoder : public VaapiEncoderBase {
public:
	VAProfile enc_profile() const override;

protected:
	const char *enc_profile_name() const override { return "AV1Profile0"; }
	// 8, not the 64 the H.265 backend pads to, and for the opposite
	// reason. AV1 has no cropping syntax: render_size() states a display
	// size but decoders treat it as an aspect-ratio hint, so whatever is
	// coded is what the client sees. The coded size is not ours to pick
	// either -- radeonsi rewrites the sequence header with its own
	// 8-aligned size (radeon_enc_write_sequence_header) -- so the only way
	// to have the two agree is to pad to exactly that 8, which leaves the
	// coded picture equal to the desktop on any dimension that is a
	// multiple of 8. On one that isn't, the stream carries up to 7 padding
	// rows or columns, which spectre crops to SessionAccept's display size
	// (VulkanPresenter::set_display_size).
	uint32_t surface_alignment() const override { return 8; }
	// Sequence header and frame header, plus raw data for the temporal
	// delimiter. No slice headers: AV1 has none.
	uint32_t packed_header_flags() const override {
		return VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE | VA_ENC_PACKED_HEADER_RAW_DATA;
	}
	bool init_codec() override;
	bool submit_frame(VASurfaceID surface, int64_t pts_us) override;

private:
	uint8_t seq_level_idx_ = 8; // 4.0
	// 8 order-hint bits give a 256-frame wrap, comfortably longer than any
	// GOP this encoder produces.
	static constexpr uint8_t kOrderHintBits = 8;
	static constexpr uint32_t kOrderHintWrap = 1u << kOrderHintBits;

	// The single reference slot inter frames read and refresh. Which slot
	// it is does not matter (the hardware only ever holds one reference);
	// slot 0 keeps refresh_frame_flags to a single bit.
	static constexpr uint8_t kRefSlot = 0;

	uint32_t sb_cols_ = 0, sb_rows_ = 0;

	std::vector<uint8_t> sequence_obu_;
	std::vector<uint8_t> temporal_delimiter_obu_;

	uint32_t order_hint_ = 0;
};

} // namespace wraith
