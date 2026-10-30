// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/fec_controller.hpp"

#include <algorithm>
#include <cmath>

namespace wraith {

namespace {

// Fewer slices than this in a report say too little about the loss rate.
constexpr uint64_t kMinSlices = 32;
// Loss below this is noise: one slice in a few hundred, briefly.
constexpr double kNegligibleLoss = 0.002;
// Parity per unit of loss, plus a margin: losses bunch, so a block often
// loses more than its share.
constexpr double kLossGain = 3.0;
constexpr double kLossMargin = 0.02;
// Per report (250 ms): how much of a lower reading is taken, and how fast
// the margin for unsaved frames fades.
constexpr double kLossRelease = 0.1;
constexpr double kBoostStep = 0.05;
constexpr double kBoostMax = 0.3;
constexpr double kBoostDecay = 0.9;
// The encoder's share is taken from measured overhead only once a window
// carried at least this fraction of the target: an idle desktop's few tiny
// frames, each with a shard of parity, say nothing about a busy one's.
constexpr double kMinMeasuredShare = 0.25;
constexpr double kOverheadSmoothing = 0.3;
// Beyond this overhead the encoder's share stops shrinking.
constexpr double kMaxOverhead = 0.6;

double profile_floor(gdp::session::NetworkProfile profile) {
	switch (profile) {
	case gdp::session::NETWORK_PROFILE_LAN: return 0.0;
	case gdp::session::NETWORK_PROFILE_MOBILE: return 0.15;
	default: return 0.05;
	}
}

} // namespace

FecController::FecController(Mode mode, double fixed_ratio)
	: mode_(mode), fixed_ratio_(std::clamp(fixed_ratio, 0.0, 1.0)) {
	overhead_ = ratio();
}

void FecController::on_report(gdp::session::NetworkProfile profile, uint64_t slices_expected,
	uint64_t slices_lost, bool frame_lost) {
	floor_ = profile_floor(profile);
	if (slices_expected >= kMinSlices) {
		double loss = (double)std::min(slices_lost, slices_expected) / (double)slices_expected;
		loss_ = loss > loss_ ? loss : loss_ + (loss - loss_) * kLossRelease;
	}
	if (frame_lost && ratio() > 0.0) {
		boost_ = std::min(kBoostMax, boost_ + kBoostStep);
	} else {
		boost_ *= kBoostDecay;
		if (boost_ < 0.005) {
			boost_ = 0.0;
		}
	}
}

double FecController::ratio() const {
	switch (mode_) {
	case Mode::kOff: return 0.0;
	case Mode::kFixed: return fixed_ratio_;
	case Mode::kAuto: break;
	}
	double from_loss = loss_ >= kNegligibleLoss ? loss_ * kLossGain + kLossMargin : 0.0;
	return std::clamp(std::max(floor_, from_loss) + boost_, 0.0, kMaxRatio);
}

size_t FecController::parity_for(size_t k, bool keyframe) const {
	double r = ratio();
	if (r <= 0.0 || k == 0) {
		return 0;
	}
	if (keyframe && mode_ == Mode::kAuto) {
		r = std::min(r * kKeyframeFactor, std::max(r, kMaxRatio));
	}
	return std::max<size_t>(1, (size_t)std::ceil(r * (double)k - 1e-9));
}

void FecController::on_sent(size_t data_bytes, size_t parity_bytes) {
	data_bytes_ += data_bytes;
	parity_bytes_ += parity_bytes;
}

uint32_t FecController::encoder_bps(uint32_t target_bps, uint64_t interval_us) {
	double r = ratio();
	if (r <= 0.0) {
		overhead_ = 0.0;
	} else if (interval_us > 0 &&
		(double)data_bytes_ * 8e6 / (double)interval_us >= kMinMeasuredShare * (double)target_bps) {
		double measured = (double)parity_bytes_ / (double)data_bytes_;
		overhead_ += (measured - overhead_) * kOverheadSmoothing;
	}
	data_bytes_ = 0;
	parity_bytes_ = 0;
	double overhead = std::clamp(std::max(overhead_, r), 0.0, kMaxOverhead);
	return (uint32_t)((double)target_bps / (1.0 + overhead));
}

} // namespace wraith
