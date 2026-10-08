// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// session/rate_controller.hpp: the bitrate decisions GdpSession applies,
// driven by synthetic reports on a synthetic clock.
#include "session/rate_controller.hpp"

// Plain assert()s: make sure an optimized build (-DNDEBUG) can't compile
// them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

using namespace wraith;
using gdp::session::NetworkProfile;

namespace {

constexpr uint64_t kReportUs = 250'000;
constexpr uint32_t kCeiling = 50'000'000;

// A clean report: the encoder sent `send_bps` for the window, all of it
// arrived and was acked, and the one-way delay sat at `delay_us`.
RateSample clean(uint64_t now_us, double send_bps, int32_t delay_us = 1'000) {
	RateSample s;
	s.now_us = now_us;
	s.rtt_us = 2'000;
	s.sent_bytes = (uint64_t)(send_bps / 8 * kReportUs / 1e6);
	s.acked_datagrams = s.sent_bytes / 1400 + 1;
	s.has_receiver_stats = true;
	s.interval_us = kReportUs;
	s.bytes_received = s.sent_bytes;
	s.delay_samples = 15;
	s.delay_min_us = delay_us;
	s.delay_avg_us = delay_us;
	return s;
}

void test_lan_starts_at_ceiling_and_stays() {
	RateController rc(kCeiling, gdp::session::NETWORK_PROFILE_LAN, 500, 0);
	assert(rc.target_bps() == kCeiling);
	uint64_t now = 0;
	for (int i = 0; i < 20; i++) {
		now += kReportUs;
		rc.on_sample(clean(now, kCeiling));
	}
	assert(rc.target_bps() == kCeiling);
}

void test_delay_cuts_to_what_got_through_then_recovers() {
	RateController rc(kCeiling, gdp::session::NETWORK_PROFILE_LAN, 500, 0);
	uint64_t now = 0;
	// Establish the base delay.
	for (int i = 0; i < 4; i++) {
		now += kReportUs;
		rc.on_sample(clean(now, kCeiling));
	}
	// The link only carries 20 Mbps: a standing queue puts 30 ms on every
	// packet, and only 20 Mbps' worth arrives.
	now += kReportUs;
	RateSample s = clean(now, kCeiling, 31'000);
	s.bytes_received = (uint64_t)(20e6 / 8 * kReportUs / 1e6);
	assert(rc.on_sample(s));
	assert(std::string(rc.last_reason()) == "delay");
	// 0.9 of what got through, not merely 0.85 of the old target.
	assert(rc.target_bps() <= 18'000'001 && rc.target_bps() >= 17'900'000);
	uint32_t after_cut = rc.target_bps();

	// Still congested on the very next report (the queue is draining), but
	// that's inside the hold: no second cut.
	now += kReportUs;
	s.now_us = now;
	s.sent_bytes = (uint64_t)(after_cut / 8.0 * kReportUs / 1e6);
	assert(!rc.on_sample(s));
	assert(rc.target_bps() == after_cut);

	// Clean from here on: it climbs back, but slowly near the rate that
	// caused trouble.
	for (int i = 0; i < 8; i++) {
		now += kReportUs;
		rc.on_sample(clean(now, rc.target_bps()));
	}
	assert(rc.target_bps() > after_cut);
	assert(rc.target_bps() < kCeiling);
}

void test_internet_app_limited_holds() {
	RateController rc(kCeiling, gdp::session::NETWORK_PROFILE_INTERNET, 30'000, 0);
	assert(rc.target_bps() == 8'000'000);
	uint64_t now = 0;
	// An idle desktop sending 1 Mbps proves nothing about headroom.
	for (int i = 0; i < 40; i++) {
		now += kReportUs;
		rc.on_sample(clean(now, 1'000'000));
	}
	assert(rc.target_bps() == 8'000'000);
	// Sending at the target, it climbs.
	for (int i = 0; i < 8; i++) {
		now += kReportUs;
		rc.on_sample(clean(now, rc.target_bps()));
	}
	assert(rc.target_bps() > 8'000'000);
}

void test_loss_tolerance() {
	RateController rc(kCeiling, gdp::session::NETWORK_PROFILE_INTERNET, 30'000, 0);
	uint64_t now = kReportUs;
	// 1% loss on the internet profile is written off.
	RateSample s = clean(now, rc.target_bps());
	s.acked_datagrams = 990;
	s.lost_datagrams = 10;
	rc.on_sample(s);
	assert(std::string(rc.last_reason()) != "loss");
	// 5% with no queue behind it is a link that drops at random: not a
	// reason to send less.
	now += kReportUs;
	s.now_us = now;
	s.acked_datagrams = 950;
	s.lost_datagrams = 50;
	rc.on_sample(s);
	assert(std::string(rc.last_reason()) != "loss");
	// 5% with the delay creeping up is congestion.
	now += kReportUs;
	s.now_us = now;
	s.delay_min_us = s.delay_avg_us = 1'000 + 15'000;
	uint32_t before = rc.target_bps();
	assert(rc.on_sample(s));
	assert(std::string(rc.last_reason()) == "loss");
	assert(rc.target_bps() < before);
	// And 15% needs no corroboration.
	RateController severe(kCeiling, gdp::session::NETWORK_PROFILE_INTERNET, 30'000, 0);
	s = clean(kReportUs, severe.target_bps());
	s.acked_datagrams = 850;
	s.lost_datagrams = 150;
	assert(severe.on_sample(s));
	assert(std::string(severe.last_reason()) == "loss");
	// A single lost datagram at a low packet rate isn't enough, however
	// large a fraction it is.
	RateController rc2(kCeiling, gdp::session::NETWORK_PROFILE_INTERNET, 30'000, 0);
	s = clean(kReportUs, rc2.target_bps());
	s.acked_datagrams = 3;
	s.lost_datagrams = 1;
	rc2.on_sample(s);
	assert(std::string(rc2.last_reason()) != "loss");
}

void test_backlog() {
	RateController rc(kCeiling, gdp::session::NETWORK_PROFILE_LAN, 500, 0);
	assert(rc.should_send_frame(0));
	assert(rc.should_send_frame(25'000));
	assert(!rc.should_send_frame(25'001));
	RateSample s = clean(kReportUs, kCeiling);
	s.queue_age_us = 40'000;
	assert(rc.on_sample(s));
	assert(std::string(rc.last_reason()) == "backlog");
}

// Reports a stall held back arrive together, each seeing the same
// backlog: one cut, not one per report. A backlog still growing a report
// interval later cuts again.
void test_backlog_burst_cuts_once() {
	RateController rc(kCeiling, gdp::session::NETWORK_PROFILE_LAN, 500, 0);
	uint64_t now = 0;
	for (int i = 0; i < 4; i++) {
		now += kReportUs;
		rc.on_sample(clean(now, kCeiling));
	}
	// A queue in the network behind the backlog, so each cut is a full one
	// rather than a match to what QUIC sent.
	now += kReportUs;
	RateSample s = clean(now, kCeiling, 60'000);
	s.queue_age_us = 600'000;
	assert(rc.on_sample(s));
	uint32_t after_one = rc.target_bps();
	for (int i = 0; i < 6; i++) {
		s.now_us = ++now;
		rc.on_sample(s);
	}
	assert(rc.target_bps() == after_one);
	s.now_us = now + kReportUs;
	assert(rc.on_sample(s));
	assert(std::string(rc.last_reason()) == "backlog" && rc.target_bps() < after_one);
}

void test_rtt_fallback_for_old_spectre() {
	RateController rc(kCeiling, gdp::session::NETWORK_PROFILE_LAN, 500, 0);
	uint64_t now = 0;
	for (int i = 0; i < 4; i++) {
		now += kReportUs;
		RateSample s = clean(now, kCeiling);
		s.has_receiver_stats = false;
		rc.on_sample(s);
	}
	assert(rc.target_bps() == kCeiling);
	now += kReportUs;
	RateSample s = clean(now, kCeiling);
	s.has_receiver_stats = false;
	s.rtt_us = 2'000 + 15'000; // 15 ms of queue on a 10 ms threshold
	assert(rc.on_sample(s));
	assert(std::string(rc.last_reason()) == "delay");
	assert(rc.target_bps() < kCeiling);
}

void test_auto_reclassifies_after_sustained_loss() {
	RateController rc(kCeiling, gdp::session::NETWORK_PROFILE_AUTO, 800, 0);
	assert(rc.auto_profile());
	assert(rc.params().kind == gdp::session::NETWORK_PROFILE_LAN);
	uint64_t now = 0;
	for (int i = 0; i < 80 && rc.params().kind != gdp::session::NETWORK_PROFILE_MOBILE; i++) {
		now += kReportUs;
		RateSample s = clean(now, rc.target_bps());
		s.acked_datagrams = 900;
		s.lost_datagrams = 100; // 10%: a lossy radio link
		rc.on_sample(s);
	}
	assert(rc.params().kind == gdp::session::NETWORK_PROFILE_MOBILE);
	// And never below the new profile's floor.
	assert(rc.target_bps() >= rc.params().floor_bps);

	// Jitter alone -- a radio link's -- also reads as mobile, and isn't
	// mistaken for queuing: the average wanders, the minimum doesn't.
	RateController jittery(kCeiling, gdp::session::NETWORK_PROFILE_AUTO, 20'000, 0);
	assert(jittery.params().kind == gdp::session::NETWORK_PROFILE_INTERNET);
	now = 0;
	uint32_t start = jittery.target_bps();
	for (int i = 0; i < 40; i++) {
		now += kReportUs;
		RateSample s = clean(now, jittery.target_bps());
		s.delay_min_us = 1'000;
		s.delay_avg_us = 1'000 + 18'000;
		jittery.on_sample(s);
		assert(std::string(jittery.last_reason()) != "delay");
	}
	assert(jittery.params().kind == gdp::session::NETWORK_PROFILE_MOBILE);
	assert(jittery.target_bps() >= start);

	// A far-away host starts as internet.
	RateController far(kCeiling, gdp::session::NETWORK_PROFILE_AUTO, 40'000, 0);
	assert(far.params().kind == gdp::session::NETWORK_PROFILE_INTERNET);
	// An explicit profile never moves.
	RateController fixed(kCeiling, gdp::session::NETWORK_PROFILE_LAN, 40'000, 0);
	assert(!fixed.auto_profile());
	now = 0;
	for (int i = 0; i < 40; i++) {
		now += kReportUs;
		RateSample s = clean(now, fixed.target_bps());
		s.acked_datagrams = 900;
		s.lost_datagrams = 100;
		fixed.on_sample(s);
	}
	assert(fixed.params().kind == gdp::session::NETWORK_PROFILE_LAN);
}

void test_base_delay_forgets_a_route_change() {
	RateController rc(kCeiling, gdp::session::NETWORK_PROFILE_LAN, 500, 0);
	uint64_t now = 0;
	for (int i = 0; i < 4; i++) {
		now += kReportUs;
		rc.on_sample(clean(now, kCeiling, 1'000));
	}
	// The path's base delay steps up 20 ms for good. At first that reads as
	// queuing; once the old minimum ages out of the window it doesn't.
	int cuts = 0;
	for (int i = 0; i < 80; i++) {
		now += kReportUs;
		RateSample s = clean(now, rc.target_bps(), 21'000);
		if (rc.on_sample(s) && std::string(rc.last_reason()) == "delay") {
			cuts++;
		}
	}
	assert(cuts > 0);
	assert(rc.queuing_delay_us() == 0);
	assert(rc.target_bps() == kCeiling); // and it climbed all the way back
}

void test_ceiling_below_floor() {
	// A -b under the profile's floor wins.
	RateController rc(1'500'000, gdp::session::NETWORK_PROFILE_LAN, 500, 0);
	assert(rc.target_bps() == 1'500'000);
	RateSample s = clean(kReportUs, 1'500'000, 50'000);
	rc.on_sample(s);
	assert(rc.target_bps() == 1'500'000);
}

} // namespace

int main() {
	test_lan_starts_at_ceiling_and_stays();
	test_delay_cuts_to_what_got_through_then_recovers();
	test_internet_app_limited_holds();
	test_loss_tolerance();
	test_backlog();
	test_backlog_burst_cuts_once();
	test_rtt_fallback_for_old_spectre();
	test_auto_reclassifies_after_sustained_loss();
	test_base_delay_forgets_a_route_change();
	test_ceiling_below_floor();
	printf("rate_controller_test: all passed\n");
	return 0;
}
