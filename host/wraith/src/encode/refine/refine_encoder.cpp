// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/refine/refine_encoder.hpp"

#include "util/clock.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <algorithm>
#include <cstdio>

namespace wraith {

namespace {

// Zstd level for the tile layer. This runs inline on wraith's main thread
// once per frame, so it has to stay cheap: on 1080p text screens level 3
// is 7-25% smaller than level 1 for ~10% more time, 0.3-0.6 ms per MiB of
// tiles. Past 3 the time climbs faster than the size falls -- the wrong
// trade for a latency-first protocol.
constexpr int kZstdLevel = 3;

// Layers older than this many microseconds behind the newest packet the
// base encoder has produced are dropped. A layer whose frame the base
// encoder never emits a packet for (it dropped the frame, or is holding it
// past a reopen) would otherwise sit in the map forever holding its tile
// pixels.
constexpr int64_t kLayerExpiryUs = 2'000'000;
constexpr int64_t kInFlightExpiryUs = 1'000'000;

} // namespace

RefineEncoder::~RefineEncoder() {
	close();
}

bool RefineEncoder::open(const EncoderConfig &config) {
	// The base layer is the session codec's ordinary stream; nothing about
	// it knows it is inside a refined session.
	if (!base_ || !base_->open(config)) {
		WLOG_ERROR("refine: base encoder failed to open");
		return false;
	}
	base_open_ = true;
	target_bps_ = config.bitrate_bps; // until the rate controller's first set_bitrate()
	tokens_us_ = 0;                   // the bucket starts full, at the first frame

	// wraith.toml's [refine] for the profile last set. SessionServices sets
	// the session's real one right after open().
	settings_ = wraith::config().refine_for(link_profile_);
	tracker_.reset(config.width, config.height, tracker_config_for(settings_));
	pending_layers_.clear();
	layer_only_packets_.clear();
	in_flight_.clear();
	sent_history_.clear();
	send_reset_next_ = true;
	width_ = config.width;
	height_ = config.height;
	codec_ = config.codec;
	log_policy("opened");
	return true;
}

TileTrackerConfig RefineEncoder::tracker_config_for(const RefineSettings &settings) {
	TileTrackerConfig tracker_config;
	tracker_config.settle_us = (int64_t)settings.settle_ms * 1000;
	return tracker_config;
}

void RefineEncoder::set_link_profile(LinkProfile profile) {
	if (profile == link_profile_) {
		return;
	}
	link_profile_ = profile;
	settings_ = wraith::config().refine_for(profile);
	if (base_open_) {
		tracker_.set_policy(tracker_config_for(settings_));
		log_policy("now");
	}
}

void RefineEncoder::log_policy(const char *what) const {
	const RefineSettings &s = settings_;
	char budget[48] = "no bandwidth budget";
	if (s.bandwidth_percent > 0) {
		snprintf(budget, sizeof(budget), "budget %u%% of target, %u ms burst", s.bandwidth_percent,
			s.burst_ms);
	}
	WLOG_INFO("refine: %s %ux%u %s base + Zstd lossless tiles, %s profile (%upx grid, settle %u ms, "
			  "damage holds, %s)",
		what, width_, height_, gdp::video_codec_token(codec_), link_profile_name(link_profile_),
		TileTrackerConfig{}.tile_size, s.settle_ms, budget);
}

bool RefineEncoder::wants_cpu_frame() const {
	// Building a layer needs the source pixels in host memory; with none to
	// build, whatever the base encoder prefers.
	return !paused_ || !base_open_ || base_->wants_cpu_frame();
}

// Only while paused, when wants_cpu_frame() is the base encoder's answer:
// unpaused, it is true and callers always route to push_cpu().
bool RefineEncoder::push(const DmabufFrame &frame, int64_t pts_us) {
	if (!base_open_ || !paused_) {
		return false;
	}
	stage_pause_reset(pts_us);
	if (!base_->push(frame, pts_us)) {
		unstage(pts_us);
		return false;
	}
	note_pushed(pts_us);
	return true;
}

void RefineEncoder::note_pushed(int64_t pts_us) {
	if (base_->completion_fd() >= 0) {
		in_flight_.push_back(InFlight{pts_us, monotonic_now_us(), push_seq_});
	}
	push_seq_++;
}

void RefineEncoder::release_layer_only(uint64_t seq, std::vector<EncodedPacket> *out) {
	size_t n = 0;
	while (n < layer_only_packets_.size() && layer_only_packets_[n].after_seq <= seq) {
		LayerOnlyPacket &layer_only = layer_only_packets_[n++];
		record_sent(layer_only.packet.pts_us, std::move(layer_only.emission));
		out->push_back(std::move(layer_only.packet));
	}
	layer_only_packets_.erase(layer_only_packets_.begin(), layer_only_packets_.begin() + (ptrdiff_t)n);
}

void RefineEncoder::set_refine_paused(bool paused) {
	if (paused == paused_) {
		return;
	}
	paused_ = paused;
	if (paused) {
		// The client may be holding lossless tiles the tracker is about to
		// stop looking after: wipe them. Layer-only packets not yet handed
		// out would land after the wipe, so they go -- the tracker starts
		// over on resume anyway.
		send_pause_reset_ = true;
		layer_only_packets_.clear();
		WLOG_INFO("refine: paused by the client");
	} else {
		// Everything the client sees is video now; start over as at the
		// start of a session. No IDR needed: the reset is the layer's own.
		send_pause_reset_ = false;
		send_reset_next_ = true;
		WLOG_INFO("refine: resumed");
	}
}

void RefineEncoder::unstage(int64_t pts_us) {
	auto it = pending_layers_.find(pts_us);
	if (it != pending_layers_.end()) {
		StagedLayer lost = std::move(it->second);
		pending_layers_.erase(it);
		lose_layer(lost);
	}
}

void RefineEncoder::stage_pause_reset(int64_t pts_us) {
	if (!send_pause_reset_) {
		return;
	}
	StagedLayer staged;
	staged.layer.reset = true;
	staged.emission.reset = true;
	stage(pts_us, std::move(staged));
	send_pause_reset_ = false;
}

void RefineEncoder::stage(int64_t pts_us, StagedLayer staged) {
	staged.staged_us = monotonic_now_us();
	auto [it, inserted] = pending_layers_.try_emplace(pts_us);
	if (!inserted) {
		lose_layer(it->second); // two pushes with one pts: the first's layer can't be matched now
	}
	it->second = std::move(staged);
}

// One line every few seconds while the tracker is doing anything, so a
// session log shows what is holding tiles back: `damage` is tile-frames
// whose hash held but which the compositor repainted (a playing video),
// `change` is tile-frames whose hash moved. `churned` is tiles cleared
// within a second of being sent -- wasted bytes, and on screen, chatter.
void RefineEncoder::log_stats(int64_t now_us) {
	constexpr int64_t kIntervalUs = 5000000;
	if (stats_logged_us_ == 0) {
		stats_logged_us_ = now_us; // the first window starts here
		return;
	}
	int64_t elapsed_us = now_us - stats_logged_us_;
	if (elapsed_us < kIntervalUs) {
		return;
	}
	stats_logged_us_ = now_us;
	TileTracker::Stats stats = tracker_.take_stats();
	uint64_t raw = layer_raw_bytes_;
	uint64_t wire = layer_wire_bytes_;
	layer_raw_bytes_ = 0;
	layer_wire_bytes_ = 0;
	if (stats.held_by_damage == 0 && stats.held_by_change == 0 && stats.held_by_budget == 0 &&
		stats.tiles_sent == 0 && wire == 0) {
		return;
	}
	// `wire` is the refinement layer's own share of the stream, on top of
	// the base codec's bitrate; `budget` is what apply_byte_budget() lets
	// it have right now.
	char budget[32] = "none";
	if (settings_.bandwidth_percent > 0) {
		snprintf(budget, sizeof(budget), "%.1f Mbit/s", budget_bytes_per_s() * 8.0 / 1e6);
	}
	WLOG_DEBUG(
		"refine: last %.1fs: %llu tiles sent, %.2f MiB raw -> %.2f MiB on the wire (%.1f Mbit/s, budget %s), "
		"%llu churned, held back by damage %llu, by change %llu, by budget %llu",
		elapsed_us / 1e6, (unsigned long long)stats.tiles_sent, raw / 1048576.0, wire / 1048576.0,
		wire * 8.0 / (double)elapsed_us, budget, (unsigned long long)stats.churned,
		(unsigned long long)stats.held_by_damage, (unsigned long long)stats.held_by_change,
		(unsigned long long)stats.held_by_budget);
}

void RefineEncoder::count_layer(size_t raw, size_t wire) {
	layer_raw_bytes_ += raw;
	layer_wire_bytes_ += wire;
	tokens_ -= (double)wire;
	if (raw > 0) {
		constexpr double kRatioAlpha = 0.2;
		double ratio = std::clamp((double)wire / (double)raw, 0.01, 1.5);
		wire_ratio_ += kRatioAlpha * (ratio - wire_ratio_);
	}
}

double RefineEncoder::budget_bytes_per_s() const {
	return (double)target_bps_ / 8.0 * settings_.bandwidth_percent / 100.0;
}

void RefineEncoder::apply_byte_budget(int64_t now_us) {
	if (settings_.bandwidth_percent == 0) {
		return; // no budget: the tracker's per-frame cap alone
	}
	double rate = budget_bytes_per_s();
	double capacity = rate * settings_.burst_ms / 1000.0;
	if (tokens_us_ == 0) {
		tokens_ = capacity;
	} else {
		tokens_ += rate * (double)(now_us - tokens_us_) / 1e6;
	}
	tokens_ = std::min(tokens_, capacity);
	tokens_us_ = now_us;
	tracker_.set_byte_budget(tokens_ > 0 ? (size_t)(tokens_ / wire_ratio_) : 0);
}

void RefineEncoder::repair(const TileEmission &emission) {
	if (emission.reset) {
		// The wipe is still owed; the next frame carries it.
		if (paused_) {
			send_pause_reset_ = true;
		} else {
			send_reset_next_ = true;
		}
		return;
	}
	if (paused_) {
		return; // the pause's reset wipes the plane, and resuming starts the tracker over
	}
	tracker_.repair(emission);
}

void RefineEncoder::record_sent(int64_t pts_us, TileEmission emission) {
	if (emission.reset) {
		reset_epoch_++;
	}
	SentLayer &entry = sent_history_[pts_us];
	if (entry.emission.empty()) {
		entry.emission = std::move(emission);
	} else {
		// Two packets on one pts (a pump tick landing on a real frame's
		// clock reading): repairing the pair together is what losing
		// either could need.
		entry.emission.tiles.insert(entry.emission.tiles.end(), emission.tiles.begin(), emission.tiles.end());
		entry.emission.clears.insert(entry.emission.clears.end(), emission.clears.begin(),
			emission.clears.end());
		entry.emission.reset = entry.emission.reset || emission.reset;
	}
	entry.epoch = reset_epoch_;
	while (sent_history_.size() > kSentHistory) {
		sent_history_.erase(sent_history_.begin());
	}
}

void RefineEncoder::frames_lost(const std::vector<int64_t> &pts_us) {
	if (!base_open_) {
		return;
	}
	size_t tiles = 0;
	size_t clears = 0;
	for (int64_t pts : pts_us) {
		auto it = sent_history_.find(pts);
		if (it == sent_history_.end()) {
			WLOG_INFO("refine: lost frame at pts %lld is older than the sent history; resetting the plane",
				(long long)pts);
			request_keyframe();
			return;
		}
		if (it->second.epoch < reset_epoch_) {
			continue; // a later reset wiped whatever it did
		}
		tiles += it->second.emission.tiles.size();
		clears += it->second.emission.clears.size();
		repair(it->second.emission);
	}
	WLOG_DEBUG("refine: repairing %zu lost frame(s): %zu tiles re-armed, %zu clear rects re-sent",
		pts_us.size(), tiles, clears);
}

bool RefineEncoder::takes_tiled_dmabuf() const {
	// Only a base that imports dmabufs itself gains anything: one that wants
	// host pixels anyway (x264, PyroWave) reads the whole frame back
	// regardless, and refinement may as well hash that.
	return base_open_ && !paused_ && !base_->wants_cpu_frame();
}

bool RefineEncoder::stage_layer(TileSource &tiles, int64_t pts_us, const DamageRegion *damage) {
	// A reset is already paired with a base IDR by request_keyframe(), which
	// is what sets it (see send_reset_next_'s comment). The tracker's settle
	// clock is wraith's own monotonic clock rather than the frame pts: the
	// screencast-ext path can stamp pts from the compositor's presentation
	// clock, and the tracker only needs consistent spacing between calls.
	bool reset = send_reset_next_;
	StagedLayer staged;
	int64_t now_us = monotonic_now_us();
	apply_byte_budget(now_us);
	staged.layer = tracker_.process(tiles, now_us, reset, damage, &staged.emission);
	if (tiles.failed()) {
		// Whatever the tracker committed to (tiles marked sent, a reset) is
		// given back; a reset stays owed.
		lose_layer(staged);
		return false;
	}
	send_reset_next_ = false;
	log_stats(now_us);

	// Staged before the push so that a synchronous base encoder finds it
	// already there on the poll() that follows.
	if (!staged.layer.empty()) {
		stage(pts_us, std::move(staged));
	}
	return true;
}

bool RefineEncoder::push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride,
	int64_t pts_us, const DamageRegion *damage) {
	if (!base_open_) {
		return false;
	}

	if (paused_) {
		// No layer to build; the base encoder takes the frame as it is.
		stage_pause_reset(pts_us);
		if (!base_->push_cpu(data, width, height, stride, pts_us, damage)) {
			unstage(pts_us);
			return false;
		}
		note_pushed(pts_us);
		return true;
	}

	CpuTileSource tiles(data, width, height, stride);
	stage_layer(tiles, pts_us, damage); // host memory never fails
	if (!base_->push_cpu(data, width, height, stride, pts_us, damage)) {
		unstage(pts_us);
		return false;
	}
	note_pushed(pts_us);
	return true;
}

bool RefineEncoder::push_tiled(const DmabufFrame &frame, TileSource &tiles, int64_t pts_us,
	const DamageRegion *damage) {
	if (!takes_tiled_dmabuf()) {
		return false;
	}
	if (!stage_layer(tiles, pts_us, damage)) {
		return false; // the base hasn't seen it: the caller re-sends it read back
	}
	if (!base_->push(frame, pts_us)) {
		unstage(pts_us);
		return false;
	}
	note_pushed(pts_us);
	return true;
}

bool RefineEncoder::pump(TileSource &tiles, int64_t pts_us) {
	if (!base_open_) {
		return false;
	}
	if (paused_) {
		return true; // nothing to settle (has_pending_work() is false anyway)
	}
	// The frame is, by contract, exactly the last push's, so the tracker
	// hashes nothing and only advances the settle count -- and copies out
	// whatever tiles are now due.
	StagedLayer staged;
	int64_t now_us = monotonic_now_us();
	apply_byte_budget(now_us);
	staged.layer = tracker_.pump(tiles, now_us, &staged.emission);
	if (tiles.failed()) {
		lose_layer(staged);
		return false;
	}
	log_stats(now_us);
	if (staged.layer.empty()) {
		return true;
	}

	// No video this frame: base_len 0, which tells the client to apply the
	// layer to the picture it is already showing (gdp-spec.md §9.6).
	EncodedPacket packet;
	packet.pts_us = pts_us;
	packet.keyframe = false;
	if (!gdp::refine_pack_frame(nullptr, 0, staged.layer, kZstdLevel, &packet.data)) {
		WLOG_ERROR("refine: failed to pack a layer-only frame at pts %lld", (long long)pts_us);
		lose_layer(staged);
		return true;
	}
	count_layer(staged.layer.tile_pixels.size(), packet.data.size());
	layer_only_packets_.push_back(LayerOnlyPacket{std::move(packet), std::move(staged.emission), push_seq_});
	return true;
}

void RefineEncoder::request_keyframe() {
	send_reset_next_ = true;
	if (base_open_) {
		base_->request_keyframe();
	}
}

void RefineEncoder::request_repair_keyframe() {
	// The video's references are broken; the plane isn't -- frames_lost()
	// already put back what the lost layers did.
	if (base_open_) {
		base_->request_keyframe();
	}
}

void RefineEncoder::set_bitrate(uint32_t bitrate_bps) {
	// The base layer is rate-controlled directly; the tile layer follows
	// the same target through its bandwidth budget (apply_byte_budget()).
	target_bps_ = bitrate_bps;
	if (base_open_) {
		base_->set_bitrate(bitrate_bps);
	}
}

std::vector<EncodedPacket> RefineEncoder::poll() {
	if (!base_open_) {
		return {};
	}

	std::vector<EncodedPacket> base_packets = base_->poll();
	std::vector<EncodedPacket> out;
	out.reserve(base_packets.size() + layer_only_packets_.size());

	for (EncodedPacket &packet : base_packets) {
		// Layer-only packets built before this frame was pushed go ahead
		// of it.
		auto flight = std::find_if(in_flight_.begin(), in_flight_.end(),
			[&](const InFlight &f) { return f.pts_us == packet.pts_us; });
		if (flight != in_flight_.end()) {
			release_layer_only(flight->seq, &out);
		}

		StagedLayer staged;
		auto it = pending_layers_.find(packet.pts_us);
		if (it != pending_layers_.end()) {
			staged = std::move(it->second);
			pending_layers_.erase(it);
		}

		EncodedPacket wrapped;
		wrapped.pts_us = packet.pts_us;
		wrapped.keyframe = packet.keyframe;
		if (!gdp::refine_pack_frame(packet.data.data(), packet.data.size(), staged.layer, kZstdLevel,
				&wrapped.data)) {
			// Packing only fails on a malformed layer or a Zstd error.
			// Ship the frame with no lossless layer rather than dropping
			// it: the base stream is what keeps the session alive. The
			// layer it should have carried is given back to the tracker.
			WLOG_ERROR("refine: failed to pack frame at pts %lld, sending base layer only",
				(long long)packet.pts_us);
			lose_layer(staged);
			staged.emission = TileEmission{};
			gdp::RefineLayer empty;
			if (!gdp::refine_pack_frame(packet.data.data(), packet.data.size(), empty, kZstdLevel,
					&wrapped.data)) {
				continue;
			}
		} else if (!staged.layer.empty()) {
			count_layer(staged.layer.tile_pixels.size(), wrapped.data.size() - packet.data.size());
		}
		record_sent(wrapped.pts_us, std::move(staged.emission));
		out.push_back(std::move(wrapped));
	}

	// Layers for frames the base encoder is never going to emit: the
	// tiles in them were never shown, so they are re-armed.
	int64_t now_us = monotonic_now_us();
	for (auto it = pending_layers_.begin(); it != pending_layers_.end();) {
		if (now_us - it->second.staged_us > kLayerExpiryUs) {
			lose_layer(it->second);
			it = pending_layers_.erase(it);
		} else {
			++it;
		}
	}

	// What came back is no longer in flight, nor anything pushed before
	// it: packets come out in push order. By position, not by comparing
	// pts, which needn't increase (see pending_layers_).
	for (const EncodedPacket &packet : base_packets) {
		auto it = std::find_if(in_flight_.begin(), in_flight_.end(),
			[&](const InFlight &f) { return f.pts_us == packet.pts_us; });
		if (it != in_flight_.end()) {
			in_flight_.erase(in_flight_.begin(), it + 1);
		}
	}
	std::erase_if(in_flight_, [&](const InFlight &f) { return now_us - f.pushed_us > kInFlightExpiryUs; });

	// The rest wait behind the oldest frame still encoding, for the poll()
	// its completion triggers.
	release_layer_only(in_flight_.empty() ? push_seq_ : in_flight_.front().seq, &out);

	return out;
}

void RefineEncoder::close() {
	if (base_open_) {
		base_->close();
		base_open_ = false;
	}
	pending_layers_.clear();
	layer_only_packets_.clear();
	in_flight_.clear();
	sent_history_.clear();
	send_reset_next_ = true;
}

} // namespace wraith
