// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/path_rate_estimator.hpp"

#include <algorithm>

namespace wraith {

void PathRateEstimator::add(uint32_t bytes, uint32_t span_us, uint64_t now_us) {
	if (bytes == 0 || span_us == 0) {
		return;
	}
	if (count_ == kWindow) {
		std::copy(rates_ + 1, rates_ + kWindow, rates_);
		std::copy(added_us_ + 1, added_us_ + kWindow, added_us_);
		count_--;
	}
	rates_[count_] = (uint64_t)bytes * 8 * 1'000'000 / span_us;
	added_us_[count_] = now_us;
	count_++;
	update();
}

bool PathRateEstimator::expire(uint64_t now_us) {
	size_t old = 0;
	while (old < count_ && now_us - added_us_[old] > kMaxAgeUs) {
		old++;
	}
	if (old == 0) {
		return false;
	}
	std::copy(rates_ + old, rates_ + count_, rates_);
	std::copy(added_us_ + old, added_us_ + count_, added_us_);
	count_ -= old;
	uint64_t before = estimate_bps_;
	update();
	return estimate_bps_ != before;
}

void PathRateEstimator::update() {
	if (count_ == 0) {
		estimate_bps_ = 0;
		return;
	}
	uint64_t sorted[kWindow];
	std::copy(rates_, rates_ + count_, sorted);
	std::nth_element(sorted, sorted + count_ / 2, sorted + count_);
	estimate_bps_ = sorted[count_ / 2];
}

} // namespace wraith
