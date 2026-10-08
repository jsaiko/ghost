// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Video bitrate control and send-queue gating for one GDP session
// (docs/design/transport-and-rate-control.md; the wire side is gdp-spec.md
// §7.5). Pure logic: GdpSession feeds it what the transport and spectre's
// StatsReports measured and applies what it decides, so it can be
// exercised without a connection.
//
// Three signals, any of which means the link is saturated:
//  - queuing delay: how far the one-way delay spectre measures (or, from a
//    spectre that doesn't report it, the QUIC RTT) has risen above its
//    recent minimum. The early warning -- it rises before anything is lost.
//  - send backlog: how long the oldest datagram the transport still holds
//    unsent has waited. QUIC gates datagrams on congestion control and never
//    drops them, so a link slower than the encoder shows up here first.
//  - loss: the fraction of datagrams QUIC declared lost -- but only with
//    rising delay or skipped frames alongside it, or when severe: a link
//    that drops packets at random (Wi-Fi, radio) does so at any rate, so
//    cutting for it only starves the stream, while a queue that overflows
//    shows its delay first.
// On saturation the target drops toward what actually got through; otherwise
// it climbs, quickly while far below the rate that last caused trouble and
// slowly near it, and only while the encoder is really sending near the
// target (an idle desktop proves nothing about the link).
#pragma once

#include "session.pb.h"

#include <cstdint>

namespace wraith {

// Tuning per kind of link. `kind` is the wire enum it was chosen for.
struct NetworkProfileParams {
	gdp::session::NetworkProfile kind;
	const char *name;
	uint32_t floor_bps;
	// Where a session starts, before anything is measured; capped at the
	// ceiling (encode.max_bitrate_mbps, or -b).
	uint32_t start_bps;
	// Queuing delay above the recent minimum that counts as saturation.
	uint32_t delay_threshold_us;
	// Datagram loss fraction written off as noise.
	double loss_tolerance;
	// Multiplicative climb per second while well below the last trouble.
	double increase_per_s;
	// Send backlog age past which frames are skipped rather than queued.
	uint32_t max_queue_age_us;
	// Least time between keyframes requested to repair a lost frame. Every
	// loss still gets its keyframe, but a lossy link that drops a packet
	// every few hundred ms would otherwise get a keyframe -- many times a
	// P-frame's size -- just as often, each one courting the next loss.
	uint32_t keyframe_min_interval_us;
};

const NetworkProfileParams &network_profile_params(gdp::session::NetworkProfile profile);

// What one StatsReport plus the transport's counters say about the
// window since the previous one.
struct RateSample {
	uint64_t now_us = 0;
	uint32_t rtt_us = 0; // QUIC smoothed RTT, 0 if unknown
	// Datagram counters since the previous sample (DatagramStats deltas).
	uint64_t sent_bytes = 0;
	uint64_t acked_datagrams = 0;
	uint64_t lost_datagrams = 0;
	// The worst send backlog seen over the window (DatagramStats::
	// oldest_queued_age_us, sampled at every frame), and how many frames
	// the gate skipped for it.
	uint64_t queue_age_us = 0;
	uint32_t frames_skipped = 0;
	// From StreamStats; has_receiver_stats is false when the report
	// carries none (interval_us == 0), and the fields below are unset.
	bool has_receiver_stats = false;
	uint32_t interval_us = 0;
	uint64_t bytes_received = 0;
	uint32_t delay_samples = 0;
	int32_t delay_min_us = 0;
	int32_t delay_avg_us = 0;
};

class RateController {
public:
	// `ceiling_bps` is encode.max_bitrate_mbps, or -b; `profile` what
	// SessionHello asked for (AUTO picks one from the handshake RTT and
	// keeps re-deciding);
	// `handshake_rtt_us` the QUIC RTT when the session was accepted.
	RateController(uint32_t ceiling_bps, gdp::session::NetworkProfile profile, uint32_t handshake_rtt_us,
		uint64_t now_us);

	// Feeds one report's worth of measurements. Returns true if the target
	// changed (read it back with target_bps()).
	bool on_sample(const RateSample &sample);

	// Frame gate: false while the send backlog is past the profile's
	// limit, meaning the frame should be skipped rather than encoded
	// and queued behind it.
	bool should_send_frame(uint64_t queue_age_us) const { return queue_age_us <= params_->max_queue_age_us; }

	uint32_t target_bps() const { return target_bps_; }
	const NetworkProfileParams &params() const { return *params_; }
	bool auto_profile() const { return auto_; }

	// Why the last on_sample() moved the target, for logs: "delay",
	// "backlog", "loss", "increase", or "" if it didn't. "rebase" when it
	// instead took a persistent delay rise as the path's new baseline.
	const char *last_reason() const { return last_reason_; }
	// The last queuing-delay estimate, µs, for logs.
	int64_t queuing_delay_us() const { return queuing_delay_us_; }

private:
	// A minimum over a sliding window, kept as the minimum of each of a
	// few fixed-length buckets so it forgets old values: a route change
	// that raises the path's base delay must not read as queuing forever.
	class WindowedMin {
	public:
		void reset() { valid_ = false; }
		void add(int64_t value, uint64_t now_us);
		bool valid() const { return valid_; }
		int64_t get() const;

	private:
		static constexpr int kBuckets = 4;
		static constexpr uint64_t kBucketUs = 2'500'000; // 10 s of history
		int64_t mins_[kBuckets] = {};
		uint64_t bucket_start_us_ = 0;
		int current_ = 0;
		bool valid_ = false;
		bool filled_[kBuckets] = {};
	};

	void set_profile(const NetworkProfileParams &params);
	// AUTO only: re-classifies the link from what's been measured and
	// switches profile once the new answer has held for a while.
	void reclassify(double loss_rate, const RateSample &sample);

	const NetworkProfileParams *params_;
	bool auto_;
	uint32_t ceiling_bps_;
	uint32_t target_bps_;
	// The target in effect when saturation last forced a cut; the climb
	// slows near it. 0 once it has been forgotten.
	uint32_t trouble_bps_ = 0;
	uint64_t trouble_at_us_ = 0;
	// No climbing until this time: after a cut, the queue needs to drain
	// before a clean report means anything.
	uint64_t hold_until_us_ = 0;
	// When the last cut was, for kMinBacklogCutSpacingUs.
	uint64_t last_cut_us_ = 0;
	uint64_t last_sample_us_;

	WindowedMin base_delay_;
	WindowedMin base_rtt_;
	int64_t queuing_delay_us_ = 0;
	// Consecutive delay cuts, and the queuing delay the last one saw: a
	// real queue drains after a cut, so delay that survives two of them
	// unchanged is the path's base moving (a route change), not queuing,
	// and the baseline is reset instead of cutting a third time.
	int delay_cuts_in_row_ = 0;
	int64_t delay_at_last_cut_us_ = 0;
	const char *last_reason_ = "";

	// AUTO's reclassification: smoothed loss and delay jitter (a report's
	// average one-way delay over its minimum), and a candidate profile that
	// must win several consecutive reports before it's adopted.
	double loss_ewma_ = 0;
	double jitter_ewma_us_ = 0;
	gdp::session::NetworkProfile candidate_;
	int candidate_count_ = 0;
};

} // namespace wraith
