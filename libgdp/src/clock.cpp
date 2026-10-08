// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/clock.hpp"

#include <chrono>

namespace gdp {

uint64_t monotonic_us() {
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

void SessionClock::reset() {
	start_us_ = monotonic_us();
}

uint32_t SessionClock::now_us() const {
	return static_cast<uint32_t>(monotonic_us() - start_us_);
}

} // namespace gdp
