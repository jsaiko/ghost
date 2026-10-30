// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// session/fec_controller.hpp: reported slice loss in, parity per block out.
// Plain assert()s: a Release build (-DNDEBUG) must not compile them out.
#undef NDEBUG
#include "session/fec_controller.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace wraith;
using gdp::session::NETWORK_PROFILE_INTERNET;
using gdp::session::NETWORK_PROFILE_LAN;
using gdp::session::NETWORK_PROFILE_MOBILE;

namespace {

bool near(double a, double b) {
	return std::abs(a - b) < 1e-6;
}

void test_lan_sends_none_until_loss() {
	FecController fec(FecController::Mode::kAuto, 0.2);
	fec.on_report(NETWORK_PROFILE_LAN, 1000, 0, false);
	assert(fec.ratio() == 0.0);
	assert(fec.parity_for(64, true) == 0);
	// One slice in a thousand is noise...
	fec.on_report(NETWORK_PROFILE_LAN, 1000, 1, false);
	assert(fec.ratio() == 0.0);
	// ...one in fifty isn't: 3 x 2% + 2%.
	fec.on_report(NETWORK_PROFILE_LAN, 1000, 20, false);
	assert(near(fec.ratio(), 0.08));
	assert(fec.parity_for(64, false) == 6);
	assert(fec.parity_for(1, false) == 1); // every protected block gets one
	assert(fec.parity_for(64, true) == 8); // keyframes 1.5x
}

void test_profile_floors() {
	FecController fec(FecController::Mode::kAuto, 0.2);
	fec.on_report(NETWORK_PROFILE_INTERNET, 0, 0, false);
	assert(near(fec.ratio(), 0.05));
	fec.on_report(NETWORK_PROFILE_MOBILE, 0, 0, false);
	assert(near(fec.ratio(), 0.15));
	fec.on_report(NETWORK_PROFILE_LAN, 0, 0, false);
	assert(fec.ratio() == 0.0);
}

void test_fast_up_slow_down() {
	FecController fec(FecController::Mode::kAuto, 0.2);
	fec.on_report(NETWORK_PROFILE_INTERNET, 1000, 50, false); // 5%: 17%
	assert(near(fec.ratio(), 0.17));
	fec.on_report(NETWORK_PROFILE_INTERNET, 1000, 0, false); // a clean report takes only a tenth off the loss
	assert(fec.ratio() > 0.15);
	for (int i = 0; i < 100; i++) {
		fec.on_report(NETWORK_PROFILE_INTERNET, 1000, 0, false);
	}
	assert(near(fec.ratio(), 0.05)); // back to the floor
	// Too few slices to say anything: the estimate holds.
	fec.on_report(NETWORK_PROFILE_INTERNET, 10, 10, false);
	assert(near(fec.ratio(), 0.05));
	// Capped however bad.
	fec.on_report(NETWORK_PROFILE_INTERNET, 100, 90, false);
	assert(near(fec.ratio(), FecController::kMaxRatio));
	assert(fec.parity_for(64, true) == 32); // the cap holds for keyframes too
}

void test_unsaved_frames_add_margin() {
	FecController fec(FecController::Mode::kAuto, 0.2);
	fec.on_report(NETWORK_PROFILE_INTERNET, 1000, 0, false);
	fec.on_report(NETWORK_PROFILE_INTERNET, 1000, 0, true);
	assert(near(fec.ratio(), 0.10));
	fec.on_report(NETWORK_PROFILE_INTERNET, 1000, 0, true);
	assert(near(fec.ratio(), 0.15));
	for (int i = 0; i < 100; i++) {
		fec.on_report(NETWORK_PROFILE_INTERNET, 1000, 0, false);
	}
	assert(near(fec.ratio(), 0.05));
	// Without parity a lost frame says nothing about parity: the loss
	// estimate is what turns it on.
	FecController lan(FecController::Mode::kAuto, 0.2);
	lan.on_report(NETWORK_PROFILE_LAN, 1000, 0, true);
	assert(lan.ratio() == 0.0);
}

void test_fixed_and_off() {
	FecController fixed(FecController::Mode::kFixed, 0.25);
	fixed.on_report(NETWORK_PROFILE_LAN, 1000, 500, true);
	assert(near(fixed.ratio(), 0.25));
	assert(fixed.parity_for(64, true) == 16); // keyframes too
	FecController off(FecController::Mode::kOff, 0.25);
	off.on_report(NETWORK_PROFILE_MOBILE, 1000, 500, true);
	assert(off.ratio() == 0.0 && off.parity_for(64, true) == 0);
}

void test_encoder_share() {
	const uint32_t target = 10'000'000;
	const uint64_t window = 250'000; // 312'500 bytes at the target
	FecController fec(FecController::Mode::kFixed, 0.2);
	// Nothing measured yet: the ratio's share.
	assert(fec.encoder_bps(target, 0) == (uint32_t)(target / 1.2));
	// A busy window measured at 30%: the share moves toward it.
	fec.on_sent(200'000, 60'000);
	uint32_t share = fec.encoder_bps(target, window);
	assert(share < (uint32_t)(target / 1.2) && share > (uint32_t)(target / 1.3));
	// A nearly idle one (tiny frames, a shard each) doesn't count.
	fec.on_sent(1'000, 1'000);
	assert(fec.encoder_bps(target, window) == share);
	// Never below the ratio, however little a window measured.
	for (int i = 0; i < 50; i++) {
		fec.on_sent(200'000, 0);
		fec.encoder_bps(target, window);
	}
	assert(fec.encoder_bps(target, 0) == (uint32_t)(target / 1.2));
	// And all of it without parity.
	FecController off(FecController::Mode::kOff, 0.2);
	off.on_sent(200'000, 0);
	assert(off.encoder_bps(target, window) == target);
}

} // namespace

int main() {
	test_lan_sends_none_until_loss();
	test_profile_floors();
	test_fast_up_slow_down();
	test_unsaved_frames_add_margin();
	test_fixed_and_off();
	test_encoder_share();
	printf("fec_controller_test: all tests passed\n");
	return 0;
}
