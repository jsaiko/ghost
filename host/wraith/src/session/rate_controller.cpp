// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/rate_controller.hpp"

#include <algorithm>

namespace wraith {

namespace {

using gdp::session::NetworkProfile;

// start_bps == UINT32_MAX means "start at the ceiling": on a LAN there's
// nothing to be gained by ramping up to what the link plainly carries.
const NetworkProfileParams kLan = {
	gdp::session::NETWORK_PROFILE_LAN,
	"lan",
	/*floor_bps=*/2'000'000,
	/*start_bps=*/UINT32_MAX,
	/*delay_threshold_us=*/10'000,
	/*loss_tolerance=*/0.01,
	/*increase_per_s=*/0.5,
	/*max_queue_age_us=*/25'000,
	/*keyframe_min_interval_us=*/0,
};
const NetworkProfileParams kInternet = {
	gdp::session::NETWORK_PROFILE_INTERNET,
	"internet",
	/*floor_bps=*/1'000'000,
	/*start_bps=*/8'000'000,
	/*delay_threshold_us=*/25'000,
	/*loss_tolerance=*/0.02,
	/*increase_per_s=*/0.15,
	/*max_queue_age_us=*/50'000,
	/*keyframe_min_interval_us=*/300'000,
};
const NetworkProfileParams kMobile = {
	gdp::session::NETWORK_PROFILE_MOBILE,
	"mobile",
	/*floor_bps=*/500'000,
	/*start_bps=*/4'000'000,
	/*delay_threshold_us=*/50'000,
	/*loss_tolerance=*/0.05,
	/*increase_per_s=*/0.08,
	/*max_queue_age_us=*/80'000,
	/*keyframe_min_interval_us=*/600'000,
};

// A cut leaves the target at this fraction of what it was, unless what
// actually got through says to go lower.
constexpr double kDecrease = 0.85;
// What got through, scaled down a little so the queue that built up can
// drain rather than merely stop growing.
constexpr double kDrainFactor = 0.9;
// A backlog-only cut's share of what QUIC actually sent in the window.
constexpr double kBacklogMatch = 0.95;
// The encoder counts as filling its target at this fraction of it; below
// it the session is app-limited and a clean report proves nothing about
// headroom above what was sent.
constexpr double kFullSend = 0.7;
constexpr double kAppLimited = 0.5;
// Near the rate that last caused trouble, climb at this fraction of the
// profile's pace; the trouble mark is forgotten after kTroubleMemoryUs
// without another cut.
constexpr double kNearTroubleFactor = 0.25;
constexpr uint64_t kTroubleMemoryUs = 15'000'000;
// After a cut, no climbing and no further delay/loss cuts for this long
// (or two RTTs, if longer): the queue the cut is meant to drain needs time
// to, and until it has, the signals still describe the old rate.
constexpr uint64_t kHoldUs = 500'000;
// A backlog that keeps growing may cut again inside the hold, but no
// sooner than this after the last cut. After a stall the StatsReports it
// held back arrive together, each seeing the same backlog, and without the
// spacing each of them cuts again (on Wi-Fi, a handful within a
// millisecond take 8 Mbps to 2). A cut needs about a report interval to
// show in the backlog at all.
constexpr uint64_t kMinBacklogCutSpacingUs = 200'000;
// A loss cut needs at least this many lost datagrams in the window, so one
// stray drop at a low packet rate can't read as a high loss fraction.
constexpr uint64_t kMinLossDatagrams = 2;
// Loss past the profile's tolerance only counts with queuing delay at this
// fraction of its threshold, the report's average delay past the whole
// threshold, or frames skipped for backlog, alongside it --
// or on its own past kSevereLoss, which no random-loss link reaches. Not
// the backlog's age by itself: every keyframe queues for as long as the
// link takes to carry it, congested or not.
constexpr double kLossWithDelay = 0.5;
constexpr double kSevereLoss = 0.10;
// Delay that has survived this many cuts in a row at no less than this
// fraction of its level at the last one is taken as the path's new base.
constexpr int kRebaseAfterCuts = 2;
constexpr double kRebaseUnchanged = 0.75;

// AUTO: a link whose base RTT is at most this is a LAN.
constexpr uint32_t kLanRttUs = 5'000;
// AUTO: smoothed datagram loss past these reads as a lossy (mobile/Wi-Fi)
// link, or as not-a-LAN; smoothed delay jitter past kMobileJitterUs, as a
// radio link's.
constexpr double kMobileLoss = 0.03;
constexpr double kInternetLoss = 0.008;
constexpr double kMobileJitterUs = 10'000;
// ...and, once mobile, it stays so until both fall below these: a
// bufferbloated link's own keyframe bursts spread one-way delay much like
// a radio link's jitter, and without the gap it flips back and forth.
constexpr double kLeaveMobileLoss = 0.015;
constexpr double kLeaveMobileJitterUs = 5'000;
constexpr double kEwmaAlpha = 0.1;
// AUTO: consecutive reports a new classification must win (2 s at the
// 250 ms report cadence) before the profile switches.
constexpr int kReclassifyReports = 8;

enum class Overuse { kNone, kBacklog, kDelay, kLoss };

const char *overuse_name(Overuse overuse) {
	switch (overuse) {
	case Overuse::kBacklog: return "backlog";
	case Overuse::kDelay: return "delay";
	case Overuse::kLoss: return "loss";
	case Overuse::kNone: break;
	}
	return "";
}

} // namespace

const NetworkProfileParams &network_profile_params(NetworkProfile profile) {
	switch (profile) {
	case gdp::session::NETWORK_PROFILE_LAN: return kLan;
	case gdp::session::NETWORK_PROFILE_MOBILE: return kMobile;
	default: return kInternet;
	}
}

void RateController::WindowedMin::add(int64_t value, uint64_t now_us) {
	if (!valid_ || now_us - bucket_start_us_ >= kBuckets * kBucketUs) {
		// First value, or a gap longer than the whole window: start over.
		for (int i = 0; i < kBuckets; i++) {
			filled_[i] = false;
		}
		current_ = 0;
		bucket_start_us_ = now_us;
		valid_ = true;
	}
	while (now_us - bucket_start_us_ >= kBucketUs) {
		current_ = (current_ + 1) % kBuckets;
		bucket_start_us_ += kBucketUs;
		filled_[current_] = false;
	}
	if (!filled_[current_] || value < mins_[current_]) {
		mins_[current_] = value;
		filled_[current_] = true;
	}
}

int64_t RateController::WindowedMin::get() const {
	int64_t min = 0;
	bool any = false;
	for (int i = 0; i < kBuckets; i++) {
		if (filled_[i] && (!any || mins_[i] < min)) {
			min = mins_[i];
			any = true;
		}
	}
	return min;
}

RateController::RateController(uint32_t ceiling_bps, NetworkProfile profile, uint32_t handshake_rtt_us,
	uint64_t now_us)
	: auto_(profile == gdp::session::NETWORK_PROFILE_AUTO), ceiling_bps_(ceiling_bps),
	  last_sample_us_(now_us) {
	if (auto_) {
		profile = handshake_rtt_us != 0 && handshake_rtt_us <= kLanRttUs
			? gdp::session::NETWORK_PROFILE_LAN
			: gdp::session::NETWORK_PROFILE_INTERNET;
	}
	params_ = &network_profile_params(profile);
	candidate_ = params_->kind;
	uint32_t floor = std::min(params_->floor_bps, ceiling_bps_);
	target_bps_ = std::max(floor, std::min(params_->start_bps, ceiling_bps_));
}

void RateController::set_profile(const NetworkProfileParams &params) {
	params_ = &params;
	// Keep the current target -- it reflects what's been measured, which
	// beats any profile's starting guess -- but respect the new floor.
	target_bps_ = std::max(target_bps_, std::min(params_->floor_bps, ceiling_bps_));
}

void RateController::reclassify(double loss_rate, const RateSample &sample) {
	loss_ewma_ += kEwmaAlpha * (loss_rate - loss_ewma_);
	if (sample.has_receiver_stats && sample.delay_samples > 1) {
		jitter_ewma_us_ +=
			kEwmaAlpha * ((double)(sample.delay_avg_us - sample.delay_min_us) - jitter_ewma_us_);
	}

	NetworkProfile verdict;
	bool lan_rtt = base_rtt_.valid() && base_rtt_.get() <= kLanRttUs;
	bool is_mobile = params_->kind == gdp::session::NETWORK_PROFILE_MOBILE;
	if (loss_ewma_ > kMobileLoss || jitter_ewma_us_ > kMobileJitterUs ||
		(is_mobile && (loss_ewma_ > kLeaveMobileLoss || jitter_ewma_us_ > kLeaveMobileJitterUs))) {
		verdict = gdp::session::NETWORK_PROFILE_MOBILE;
	} else if (!lan_rtt || loss_ewma_ > kInternetLoss) {
		verdict = gdp::session::NETWORK_PROFILE_INTERNET;
	} else {
		verdict = gdp::session::NETWORK_PROFILE_LAN;
	}

	if (verdict == params_->kind) {
		candidate_count_ = 0;
		return;
	}
	if (verdict != candidate_) {
		candidate_ = verdict;
		candidate_count_ = 0;
	}
	if (++candidate_count_ >= kReclassifyReports) {
		set_profile(network_profile_params(verdict));
		candidate_count_ = 0;
	}
}

bool RateController::on_sample(const RateSample &sample) {
	uint64_t dt_us = sample.now_us > last_sample_us_ ? sample.now_us - last_sample_us_ : 1;
	last_sample_us_ = sample.now_us;
	last_reason_ = "";

	uint64_t loss_total = sample.acked_datagrams + sample.lost_datagrams;
	double loss_rate = loss_total ? (double)sample.lost_datagrams / (double)loss_total : 0;

	// Queuing delay: spectre's one-way delay when it reports it -- it
	// moves with the forward path alone -- else the QUIC RTT, which also
	// carries the return path and ack delay but is always there. The
	// report's *minimum* against the running one, not its average: a
	// standing queue delays every packet, the fastest included, while
	// jitter only spreads them out, and on a radio link the average sits
	// well above the minimum with no queue at all.
	if (sample.rtt_us) {
		base_rtt_.add(sample.rtt_us, sample.now_us);
	}
	// The average over the same baseline is kept too, only to corroborate
	// loss: a burst that overflows a short queue delays the packets around
	// it, which moves the average but not the minimum.
	int64_t burst_delay_us = 0;
	if (sample.has_receiver_stats && sample.delay_samples > 0) {
		base_delay_.add(sample.delay_min_us, sample.now_us);
		queuing_delay_us_ = sample.delay_min_us - base_delay_.get();
		burst_delay_us = sample.delay_avg_us - base_delay_.get();
	} else if (sample.rtt_us && base_rtt_.valid()) {
		queuing_delay_us_ = (int64_t)sample.rtt_us - base_rtt_.get();
		burst_delay_us = queuing_delay_us_;
	} else {
		queuing_delay_us_ = 0;
	}

	double recv_bps = sample.has_receiver_stats && sample.interval_us
		? (double)sample.bytes_received * 8e6 / sample.interval_us
		: 0;
	double send_bps = (double)sample.sent_bytes * 8e6 / (double)dt_us;

	if (auto_) {
		reclassify(loss_rate, sample);
	}

	uint32_t floor = std::min(params_->floor_bps, ceiling_bps_);
	bool holding = sample.now_us < hold_until_us_;
	Overuse overuse = Overuse::kNone;
	if (sample.queue_age_us > (holding ? 2ull : 1ull) * params_->max_queue_age_us &&
		(!holding || sample.now_us - last_cut_us_ >= kMinBacklogCutSpacingUs)) {
		overuse = Overuse::kBacklog;
	} else if (!holding && queuing_delay_us_ > (int64_t)params_->delay_threshold_us) {
		overuse = Overuse::kDelay;
	} else if (!holding && sample.lost_datagrams >= kMinLossDatagrams &&
		loss_rate > params_->loss_tolerance &&
		(loss_rate > kSevereLoss || sample.frames_skipped > 0 ||
			queuing_delay_us_ > (int64_t)(params_->delay_threshold_us * kLossWithDelay) ||
			burst_delay_us > (int64_t)params_->delay_threshold_us)) {
		overuse = Overuse::kLoss;
	}

	if (overuse == Overuse::kDelay) {
		if (delay_cuts_in_row_ >= kRebaseAfterCuts &&
			queuing_delay_us_ >= (int64_t)(delay_at_last_cut_us_ * kRebaseUnchanged)) {
			base_delay_.reset();
			base_rtt_.reset();
			if (sample.has_receiver_stats && sample.delay_samples > 0) {
				base_delay_.add(sample.delay_min_us, sample.now_us);
			}
			if (sample.rtt_us) {
				base_rtt_.add(sample.rtt_us, sample.now_us);
			}
			delay_cuts_in_row_ = 0;
			last_reason_ = "rebase";
			return false;
		}
		delay_cuts_in_row_++;
		delay_at_last_cut_us_ = queuing_delay_us_;
	} else if (!holding) {
		delay_cuts_in_row_ = 0;
	}

	uint32_t new_target = target_bps_;
	if (overuse != Overuse::kNone) {
		double cut = target_bps_ * kDecrease;
		if (overuse == Overuse::kBacklog &&
			queuing_delay_us_ < (int64_t)(params_->delay_threshold_us * kLossWithDelay)) {
			// A backlog with no queue in the network behind it: QUIC's own
			// congestion control is pacing below the encoder (BBR still
			// learning the path, CUBIC after a loss), not the link
			// refusing. Match what it's actually sending rather than
			// undercut it -- BBR sizes its estimate from what it's given,
			// so a harder cut here would only teach it a lower rate and
			// back up the next frame the same way. Never more than an
			// ordinary cut, though: a window that caught QUIC stalled (BBR's
			// ProbeRTT) sent next to nothing, and matching that would throw
			// away almost the whole rate in one step.
			cut = std::max(target_bps_ * kDecrease, std::min<double>(target_bps_, send_bps * kBacklogMatch));
		} else if (recv_bps > 0 && send_bps >= kFullSend * target_bps_) {
			cut = std::min(cut, recv_bps * kDrainFactor);
		}
		new_target = std::max(floor, (uint32_t)cut);
		trouble_bps_ = target_bps_;
		trouble_at_us_ = sample.now_us;
		last_cut_us_ = sample.now_us;
		hold_until_us_ = sample.now_us + std::max<uint64_t>(kHoldUs, 2ull * sample.rtt_us);
	} else if (!holding) {
		if (trouble_bps_ && sample.now_us - trouble_at_us_ > kTroubleMemoryUs) {
			trouble_bps_ = 0;
		}
		// An app-limited sender proves nothing about headroom, so it holds
		// the target where it is -- except on a LAN, where the headroom is
		// taken as given and the target just returns to the ceiling.
		bool app_limited = send_bps < kAppLimited * target_bps_;
		if (!app_limited || params_->kind == gdp::session::NETWORK_PROFILE_LAN) {
			double rate = params_->increase_per_s;
			if (trouble_bps_ && target_bps_ >= trouble_bps_ * 0.9) {
				rate *= kNearTroubleFactor;
			}
			double raised = target_bps_ * (1.0 + rate * (double)dt_us / 1e6);
			new_target = (uint32_t)std::min<double>(ceiling_bps_, raised);
		}
	}
	new_target = std::max(floor, std::min(new_target, ceiling_bps_));

	if (new_target == target_bps_) {
		return false;
	}
	last_reason_ = overuse != Overuse::kNone ? overuse_name(overuse) : "increase";
	target_bps_ = new_target;
	return true;
}

} // namespace wraith
