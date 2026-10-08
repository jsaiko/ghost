// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Session clock (gdp-spec.md §11): CLOCK_MONOTONIC, microseconds,
// zeroed at session start, truncated to the wire format's 32 bits. Plus
// the raw monotonic clock it's built on, for the places that need a
// process-local timestamp with no session epoch.
#pragma once

#include <cstdint>

namespace gdp {

// Raw monotonic time in microseconds (std::steady_clock; CLOCK_MONOTONIC
// on Linux). Arbitrary epoch -- only ever diff two of these against each
// other. This is what InputEnvelope.client_time_us carries (session.proto:
// "spectre's monotonic clock, microseconds"; wraith only diffs them,
// gdp-spec.md §8) and what spectre paces its own timers/timing stats
// with; SessionClock below is the session-relative pts clock for media.
uint64_t monotonic_us();

class SessionClock {
public:
	SessionClock() { reset(); }

	// Marks "now" as time zero. Call when SessionAccept is sent (wraith)
	// or received (spectre) -- see gdp-spec.md §5.
	void reset();

	// Microseconds since reset(), truncated to 32 bits per the wire
	// format's pts fields (wraps every ~71.6 minutes).
	uint32_t now_us() const;

private:
	uint64_t start_us_ = 0;
};

// Signed difference a - b in the 32-bit wire pts domain, correctly handling
// wraparound (gdp-spec.md §11): compare only nearby timestamps with this,
// never `a > b` directly.
inline int32_t pts_diff(uint32_t a, uint32_t b) {
	return static_cast<int32_t>(a - b);
}

} // namespace gdp
