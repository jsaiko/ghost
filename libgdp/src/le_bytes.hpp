// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Little-endian integer load/store for the hand-written wire formats
// (datagram headers, the §3.1 length prefix, the refinement container).
// Private to libgdp's sources.
#pragma once

#include <cstdint>

namespace gdp::le {

inline void put_u16(uint8_t *out, uint16_t v) {
	out[0] = static_cast<uint8_t>(v);
	out[1] = static_cast<uint8_t>(v >> 8);
}

inline void put_u32(uint8_t *out, uint32_t v) {
	out[0] = static_cast<uint8_t>(v);
	out[1] = static_cast<uint8_t>(v >> 8);
	out[2] = static_cast<uint8_t>(v >> 16);
	out[3] = static_cast<uint8_t>(v >> 24);
}

inline uint16_t get_u16(const uint8_t *p) {
	return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t get_u32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
		(static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace gdp::le
