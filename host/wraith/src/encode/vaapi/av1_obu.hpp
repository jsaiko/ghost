// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Hand-rolled AV1 OBU construction, the AV1 counterpart to
// h264_bitstream.hpp and hevc_bitstream.hpp.
//
// AV1's packets are OBUs rather than Annex-B NAL units: a 1-byte header, a
// LEB128 size, and a payload that is not emulation-escaped (nothing in an
// AV1 stream is scanned for start codes). That is the only structural
// difference; the bit-level syntax is written with the same MSB-first
// writer the other two codecs use, minus the Exp-Golomb helpers, which AV1
// has no use for.
//
// What the application has to supply is more than for H.264/H.265, because
// AV1 has no sequence *parameter* buffer worth the name: Mesa's radeonsi
// frontend takes almost the whole sequence header, and a good half of the
// frame header, by *parsing the OBUs written here*
// (av1_sequence_header() / av1_frame_header() in picture_av1_enc.c) rather
// than from VAEncSequenceParameterBufferAV1. The structured buffers carry
// the encode-side decisions (which surfaces, which rate control); the
// bitstream-side decisions -- picture size, colour description, order-hint
// bits, refresh flags, render size -- are only in here.
//
// As with the other two codecs the bytes themselves are not what ends up
// in the stream:
//   - the sequence header is *regenerated* by the driver from the parsed
//     state (radeon_enc_write_sequence_header), with the picture size
//     replaced by its own aligned size -- which is why the encoder pads to
//     exactly that alignment, see vaapi_av1_encoder.hpp;
//   - the frame header is marked as this picture's "slice" and its bytes
//     are dropped entirely; the hardware writes the real one from the
//     parsed state. build_frame_header_prefix() therefore stops where
//     Mesa's parser stops, exactly as the H.264/H.265 slice-header
//     builders do.
// The temporal delimiter is the one thing here copied out verbatim.
#pragma once

#include "encode/vaapi/nal_bitstream.hpp"

#include <cstdint>
#include <vector>

namespace wraith::av1 {

// AV1's trailing_bits() (a one bit, then zeroes to the byte) is bit for
// bit what the shared writer calls rbsp_trailing_bits(), so the Annex-B
// name is reused here rather than duplicated under an AV1 one.
using nal::BitWriter;

// obu_type values (AV1 §6.2.2) this encoder emits.
inline constexpr uint8_t kObuSequenceHeader = 1;
inline constexpr uint8_t kObuTemporalDelimiter = 2;
inline constexpr uint8_t kObuFrameHeader = 3;

// frame_type values (AV1 §6.8.2), which are also what
// VAEncPictureParameterBufferAV1::picture_flags.bits.frame_type takes.
inline constexpr uint8_t kFrameTypeKey = 0;
inline constexpr uint8_t kFrameTypeInter = 1;

// "no primary reference frame" (AV1 §6.8.2, PRIMARY_REF_NONE).
inline constexpr uint8_t kPrimaryRefNone = 7;

// REFS_PER_FRAME (AV1 §3): how many reference slots an inter frame names,
// LAST through ALTREF.
inline constexpr int kRefsPerFrame = 7;

struct SeqParams {
	// The *coded* picture size, which is what max_frame_width/height carry.
	// The displayable size is stated per frame as a render size instead --
	// see FrameHeaderParams.
	uint32_t coded_width;
	uint32_t coded_height;
	uint8_t seq_level_idx;
	uint8_t order_hint_bits; // 1..8
	bool enable_cdef;
	// Written into color_config() as a colour description. Unlike the
	// H.264/H.265 backends -- which write no VUI at all and leave the
	// BT.601 convention implicit -- AV1's sequence header has nowhere
	// cheaper to put this, so the stream says what it is.
	uint8_t color_primaries;
	uint8_t transfer_characteristics;
	uint8_t matrix_coefficients;
	bool full_range;
};

// sequence_header_obu() (AV1 §5.5.1), wrapped as an OBU.
std::vector<uint8_t> build_sequence_header(const SeqParams &params);

struct FrameHeaderParams {
	bool is_key;
	// The size to display. Stated as render_size(), which is a hint and
	// not a crop -- see put_render_size() in the .cpp.
	uint32_t render_width;
	uint32_t render_height;
	uint32_t order_hint;
	uint8_t order_hint_bits;
	// Which of the eight reference slots this picture overwrites. Ignored
	// (inferred as all of them) on a shown key frame.
	uint8_t refresh_frame_flags;
	uint8_t primary_ref_frame;
	// The slot every reference name resolves to, i.e. the slot the
	// previous picture refreshed. Unused on a key frame.
	uint8_t ref_frame_idx;
};

// uncompressed_header() (AV1 §5.9.2) as far as Mesa's packed-header parser
// reads it -- through tile_info()'s uniform_tile_spacing_flag -- wrapped
// as an OBU_FRAME_HEADER. Everything past that point the driver decides
// for itself (tiles, quantizer, loop filter, CDEF), so there is nothing
// this side could usefully say about it; see the file comment.
//
// Assumes what build_sequence_header() writes: no frame ids, no superres,
// no decoder model, order hints on, screen-content tools selectable per
// frame, one tile.
std::vector<uint8_t> build_frame_header_prefix(const FrameHeaderParams &params);

// temporal_delimiter_obu(): an empty payload whose presence marks the
// start of a temporal unit (AV1 §5.6). The driver emits no delimiter of
// its own, so each frame's packet starts with this one.
std::vector<uint8_t> build_temporal_delimiter();

// Prefixes `payload` with the 1-byte OBU header (obu_has_size_field = 1,
// no extension) and the LEB128 payload size.
std::vector<uint8_t> wrap_obu(uint8_t obu_type, const std::vector<uint8_t> &payload);

} // namespace wraith::av1
