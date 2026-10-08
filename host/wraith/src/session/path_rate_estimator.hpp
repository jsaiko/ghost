// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <cstdint>

namespace wraith {

// The rate of the path's slowest hop, from how spectre saw large frames
// arrive (StreamStats.trains, gdp-spec.md §7.5). A frame's
// datagrams leave wraith faster than any hop slower than the pacing rate
// can carry, so that hop spreads them out and their arrival span measures
// it. A frame that arrived no slower than it was paced only shows the
// path carries at least that much; the median of the recent samples then
// climbs as those come in, which is how a faster path is found (see
// GdpSession::apply_pacing() for what paces them faster).
class PathRateEstimator {
public:
	// One frame, reported at `now_us`: `bytes` after the first datagram
	// arrived over `span_us`.
	void add(uint32_t bytes, uint32_t span_us, uint64_t now_us);

	// Forgets samples older than kMaxAgeUs. Returns whether the estimate
	// changed.
	bool expire(uint64_t now_us);

	// bits/s; 0 until the first sample, and again once every sample has
	// aged out.
	uint64_t estimate_bps() const { return estimate_bps_; }
	size_t samples() const { return count_; }

	// Large frames only come while the screen is busy, so the last ones
	// can be from before the path changed -- a wifi stall or a drop to a
	// slower band times a handful of frames at a crawl, and with nothing
	// large sent since, pacing would stay down there long after the link
	// came back. An estimate this old says nothing about the path now.
	static constexpr uint64_t kMaxAgeUs = 3'000'000;

private:
	void update();

	// Enough to outvote a stray sample (receive batching squeezing a
	// span, cross traffic stretching one) without lagging a real change
	// by more than a few large frames.
	static constexpr size_t kWindow = 7;
	// Oldest first: rates_[0] and added_us_[0] are the oldest sample.
	uint64_t rates_[kWindow] = {};
	uint64_t added_us_[kWindow] = {};
	size_t count_ = 0;
	uint64_t estimate_bps_ = 0;
};

} // namespace wraith
