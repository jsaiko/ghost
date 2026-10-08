// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/vaapi/nal_bitstream.hpp"

namespace wraith::nal {

void BitWriter::put_bit(unsigned bit) {
	cur_byte_ = static_cast<uint8_t>((cur_byte_ << 1) | (bit & 1));
	bits_in_cur_++;
	if (bits_in_cur_ == 8) {
		bytes_.push_back(cur_byte_);
		cur_byte_ = 0;
		bits_in_cur_ = 0;
	}
}

void BitWriter::put_bits(uint32_t value, int n) {
	for (int i = n - 1; i >= 0; i--) {
		put_bit((value >> i) & 1);
	}
}

void BitWriter::put_ue(uint32_t value) {
	uint32_t code_num_plus1 = value + 1;
	int leading_zero_bits = 0;
	while ((code_num_plus1 >> (leading_zero_bits + 1)) != 0) {
		leading_zero_bits++;
	}
	for (int i = 0; i < leading_zero_bits; i++) {
		put_bit(0);
	}
	for (int i = leading_zero_bits; i >= 0; i--) {
		put_bit((code_num_plus1 >> i) & 1);
	}
}

void BitWriter::put_se(int32_t value) {
	uint32_t code_num = value <= 0 ? static_cast<uint32_t>(-2 * static_cast<int64_t>(value))
								   : static_cast<uint32_t>(2 * static_cast<int64_t>(value) - 1);
	put_ue(code_num);
}

void BitWriter::rbsp_trailing_bits() {
	put_bit(1);
	while (bits_in_cur_ != 0) {
		put_bit(0);
	}
}

std::vector<uint8_t> escape_emulation(const std::vector<uint8_t> &rbsp) {
	std::vector<uint8_t> out;
	out.reserve(rbsp.size() + rbsp.size() / 2 + 1);
	int zero_run = 0;
	for (uint8_t b : rbsp) {
		if (zero_run >= 2 && b <= 3) {
			out.push_back(0x03);
			zero_run = 0;
		}
		out.push_back(b);
		zero_run = (b == 0) ? zero_run + 1 : 0;
	}
	return out;
}

} // namespace wraith::nal
