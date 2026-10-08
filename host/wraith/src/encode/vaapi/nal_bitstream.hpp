// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The RBSP bitstream primitives H.264 and H.265 share.
//
// Both codecs' parameter sets are written by hand here (VA-API's encode
// entrypoints generate slice *data* from structured parameter buffers but
// expect the application to supply the sequence-level NALs verbatim), and
// both use the same Exp-Golomb syntax elements and the same
// emulation-prevention rule. What differs -- which parameter sets exist,
// their field order, and the NAL header layout -- lives in
// h264_bitstream.hpp and hevc_bitstream.hpp.
#pragma once

#include <cstdint>
#include <vector>

namespace wraith::nal {

// MSB-first bitstream writer implementing the RBSP syntax primitives
// (u(n), ue(v), se(v)) plus rbsp_trailing_bits(). Produces raw RBSP bytes;
// callers run escape_emulation() before handing bytes to VA-API.
class BitWriter {
public:
	void put_bit(unsigned bit);
	void put_bits(uint32_t value, int n); // n <= 32, MSB first
	void put_ue(uint32_t value);          // unsigned Exp-Golomb
	void put_se(int32_t value);           // signed Exp-Golomb
	void rbsp_trailing_bits();            // stop bit + zero pad to byte

	const std::vector<uint8_t> &bytes() const { return bytes_; }

private:
	std::vector<uint8_t> bytes_;
	uint8_t cur_byte_ = 0;
	int bits_in_cur_ = 0;
};

// Inserts 0x03 emulation-prevention bytes so no 3-byte sequence in the
// output can be mistaken for a start code (00 00 00 / 00 00 01 / 00 00 02 /
// 00 00 03), per H.264 §7.4.1.1 / H.265 §7.4.2.1. Operates on RBSP bytes
// only (no NAL header, no start code).
std::vector<uint8_t> escape_emulation(const std::vector<uint8_t> &rbsp);

} // namespace wraith::nal
