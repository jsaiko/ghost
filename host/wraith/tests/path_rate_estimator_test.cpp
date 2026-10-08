// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// session/path_rate_estimator.hpp: frame trains in, the path's rate out.
// Plain assert()s: a Release build (-DNDEBUG) must not compile them out.
#undef NDEBUG
#include "session/path_rate_estimator.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>

using namespace wraith;

namespace {

// A train of `bytes` arriving at `mbps`.
void add_at(PathRateEstimator &est, double mbps, uint64_t now_us = 0, uint32_t bytes = 100'000) {
	est.add(bytes, (uint32_t)(bytes * 8 / mbps), now_us);
}

bool near(uint64_t bps, double mbps) {
	return bps > mbps * 0.97e6 && bps < mbps * 1.03e6;
}

void test_nothing_until_a_sample() {
	PathRateEstimator est;
	assert(est.estimate_bps() == 0);
	est.add(0, 100, 0);
	est.add(1000, 0, 0);
	assert(est.estimate_bps() == 0 && est.samples() == 0);
}

void test_steady_path() {
	PathRateEstimator est;
	for (int i = 0; i < 20; i++) {
		add_at(est, 20);
	}
	assert(near(est.estimate_bps(), 20));
}

// A stray squeezed span (receive batching) or stretched one (cross
// traffic) doesn't move the median.
void test_outliers_are_outvoted() {
	PathRateEstimator est;
	for (int i = 0; i < 7; i++) {
		add_at(est, 50);
	}
	add_at(est, 5000);
	assert(near(est.estimate_bps(), 50));
	add_at(est, 3);
	assert(near(est.estimate_bps(), 50));
}

// A path that slows is followed within a few frames.
void test_follows_a_drop() {
	PathRateEstimator est;
	for (int i = 0; i < 7; i++) {
		add_at(est, 500);
	}
	for (int i = 0; i < 4; i++) {
		add_at(est, 20);
	}
	assert(near(est.estimate_bps(), 20));
}

// Paced at the rate controller's target once that climbs past the
// estimate (GdpSession::apply_pacing()), frames arrive as fast as they were
// paced; the estimate follows the climb until it finds the path.
void test_follows_a_climbing_target() {
	PathRateEstimator est;
	for (int i = 0; i < 7; i++) {
		add_at(est, 20);
	}
	double path = 100;
	double target = 20;
	for (int i = 0; i < 100; i++) {
		double paced = std::max(est.estimate_bps() / 1e6, target);
		add_at(est, paced < path ? paced : path);
		target *= 1.05; // clean reports: the rate controller climbs
	}
	assert(near(est.estimate_bps(), 100));
}

// Samples from before a stall stop pacing once they're old: with no
// large frame since, the estimate lapses rather than holding pacing at
// the stall's crawl.
void test_old_samples_expire() {
	PathRateEstimator est;
	for (int i = 0; i < 7; i++) {
		add_at(est, 1, 1'000'000 + i * 100'000);
	}
	assert(!est.expire(1'000'000 + PathRateEstimator::kMaxAgeUs));
	assert(near(est.estimate_bps(), 1));
	// The oldest go first; the rest still stand.
	assert(!est.expire(1'250'001 + PathRateEstimator::kMaxAgeUs));
	assert(est.samples() == 4 && near(est.estimate_bps(), 1));
	assert(est.expire(2'000'000 + PathRateEstimator::kMaxAgeUs));
	assert(est.samples() == 0 && est.estimate_bps() == 0);

	// Fresh samples after a gap stand on their own.
	uint64_t later = 10'000'000;
	add_at(est, 1, later);
	for (int i = 0; i < 3; i++) {
		add_at(est, 300, later + 3'000'000 + i);
	}
	est.expire(later + 3'500'000);
	assert(est.samples() == 3 && near(est.estimate_bps(), 300));
}

} // namespace

int main() {
	test_nothing_until_a_sample();
	test_steady_path();
	test_outliers_are_outvoted();
	test_follows_a_drop();
	test_follows_a_climbing_target();
	test_old_samples_expire();
	printf("path_rate_estimator_test: all checks passed\n");
	return 0;
}
