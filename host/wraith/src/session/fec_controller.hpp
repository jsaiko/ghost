// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// How much parity a session's video carries (gdp-spec.md §9.7,
// docs/design/transport-and-rate-control.md#forward-error-correction).
// Pure logic like RateController: GdpSession feeds it spectre's reports
// and what it sent, and asks it how many parity shards each block gets.
//
// The ratio follows the slice loss spectre reports from before recovery:
// up as soon as a report shows more, back down slowly, never below the
// network profile's floor -- none on a LAN, where loss is rare and parity
// would only take bitrate. A frame parity couldn't save adds a margin of
// its own on top, which also decays. Keyframes get half as much again (up
// to kMaxRatio): they are the most likely to lose a slice and the dearest
// to lose. A fixed ratio is just that, keyframes included.
#pragma once

#include "session.pb.h"

#include <cstddef>
#include <cstdint>

namespace wraith {

class FecController {
public:
	enum class Mode {
		kAuto,  // adapts, as above
		kOff,   // never any parity
		kFixed, // always fixed_ratio
	};

	FecController(Mode mode, double fixed_ratio);

	// One StatsReport's figures (StreamStats.slices_expected and
	// slices_lost), the profile the rate controller runs on now, and
	// whether the report showed a newly lost frame.
	void on_report(gdp::session::NetworkProfile profile, uint64_t slices_expected, uint64_t slices_lost,
		bool frame_lost);

	// Parity shards for a block of k data shards: 0 while the ratio is.
	size_t parity_for(size_t k, bool keyframe) const;
	// Parity per data byte the policy asks for now.
	double ratio() const;

	// A frame went out with `data_bytes` of payload and `parity_bytes` of
	// parity.
	void on_sent(size_t data_bytes, size_t parity_bytes);
	// The encoder's share of the rate controller's `target_bps`: the target
	// covers the parity too, which costs more than the ratio on small
	// frames (every protected block gets at least one shard). Measured
	// over what went out since the last call when there was enough of it,
	// never less than the ratio.
	uint32_t encoder_bps(uint32_t target_bps, uint64_t interval_us);

	static constexpr double kMaxRatio = 0.5;
	static constexpr double kKeyframeFactor = 1.5;

private:
	Mode mode_;
	double fixed_ratio_;
	double floor_ = 0.0;
	double loss_ = 0.0;  // slice loss before recovery, smoothed
	double boost_ = 0.0; // added for frames parity didn't save
	double overhead_ = 0.0;
	uint64_t data_bytes_ = 0;
	uint64_t parity_bytes_ = 0;
};

} // namespace wraith
