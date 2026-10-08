// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The one monotonic clock wraith stamps things with: encoder pts, the
// refine tracker's settle clock, capture timestamps when the producer sent
// none. It is libgdp's
// (gdp::monotonic_us(), CLOCK_MONOTONIC on Linux), signed for arithmetic.
#pragma once

#include "gdp/clock.hpp"

#include <cstdint>

namespace wraith {

inline int64_t monotonic_now_us() {
	return (int64_t)gdp::monotonic_us();
}

inline uint32_t monotonic_now_msec() {
	return (uint32_t)(monotonic_now_us() / 1000);
}

} // namespace wraith
