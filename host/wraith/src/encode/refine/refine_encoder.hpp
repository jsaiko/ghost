// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Lossless refinement (gdp/refine.hpp for the wire container,
// tile_tracker.hpp for the policy that decides what goes in it): an
// Encoder that wraps another one.
//
// This is a *decorator*, not a backend -- it doesn't encode anything
// itself and doesn't care which codec it is wrapping. The factory picks
// the session's base encoder exactly as it would without refinement (the
// same table walk, hardware rows before software, the same -F
// handling), then hands it to this class, which feeds it every frame and
// wraps each packet that comes back in the refine container together with
// that frame's lossless tiles. H.264, H.265 and AV1 all work underneath
// because nothing here looks at the bytes.
//
// The tile layer is computed by comparing source pixels frame to frame,
// and the tracker reads them through a TileSource (tile_source.hpp): a
// hash per tile, and the pixels of the few tiles that go out. Over a
// hardware base on a host that can hash on the GPU (the screencast hosts,
// screencast/dmabuf_tile_source.hpp) the frame stays a dmabuf:
// takes_tiled_dmabuf() is true and push_tiled() hands the base the dmabuf
// and the tracker the GPU source, so a refined session costs a 0.3 ms
// compute pass instead of a full read-back. Everywhere else
// wants_cpu_frame() is true -- including over a hardware base, which is
// why VaapiEncoderBase and NvencEncoder have a push_cpu() -- and the frame
// comes in read back, through push_cpu(). A session that never negotiated
// the capability never instantiates this class, so it pays nothing.
//
// Paused (set_refine_paused(), the client's Lossless switch) there is no layer
// to build: the next packet carries a reset, wiping the client's plane,
// and every one after it an empty layer. The tracker is left alone and
// wants_cpu_frame() becomes the base encoder's own answer, which the host
// asks per frame, so a hardware base goes back to importing dmabufs --
// no readback -- from the next frame. The
// base encoder's stream is untouched: both of its input paths feed the
// same encode session, so the switch needs no IDR either way. Resuming
// starts the tracker over from a reset, as at the start of a session.
//
// Idle desktops go through pump() rather than push_cpu(): the tracker
// advances its settle clock and emits the tiles that are due as a
// *layer-only* packet (the container with no base bytes, which the client
// applies to the picture it already has), and the base encoder is not
// touched at all. So a screen that has stopped changing costs a few small
// tile packets until it converges, and then nothing.
#pragma once

#include "encode/encoder.hpp"
#include "encode/refine/tile_tracker.hpp"

#include "gdp/refine.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace wraith {

class RefineEncoder : public Encoder {
public:
	// `base` is an *unopened* encoder for the session's codec; open()
	// opens it with the same config it receives.
	explicit RefineEncoder(std::unique_ptr<Encoder> base) : base_(std::move(base)) {}
	~RefineEncoder() override;

	bool open(const EncoderConfig &config) override;
	bool wants_cpu_frame() const override;                        // see the file comment
	bool push(const DmabufFrame &frame, int64_t pts_us) override; // only while paused
	bool push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride, int64_t pts_us,
		const DamageRegion *damage) override;
	bool takes_tiled_dmabuf() const override;
	bool push_tiled(const DmabufFrame &frame, TileSource &tiles, int64_t pts_us,
		const DamageRegion *damage) override;
	void request_keyframe() override;
	void request_repair_keyframe() override;
	void frames_lost(const std::vector<int64_t> &pts_us) override;
	void set_bitrate(uint32_t bitrate_bps) override;
	void set_link_profile(LinkProfile profile) override;
	void set_refine_paused(bool paused) override;
	std::vector<EncodedPacket> poll() override;
	void close() override;
	// Forwarded to the base: the tile work stays on the caller's thread,
	// only the base encode moves (see in_flight_).
	void set_asynchronous(bool on) override { base_->set_asynchronous(on); }
	int completion_fd() const override { return base_open_ ? base_->completion_fd() : -1; }
	bool ready_for_frame() const override { return !base_open_ || base_->ready_for_frame(); }
	bool wants_idle_pump() const override { return true; } // see tile_tracker.hpp's has_pending_work()
	bool has_pending_work() const override { return !paused_ && tracker_.has_pending_work(); }
	bool pump(TileSource &tiles, int64_t pts_us) override;

private:
	// The tracker's layer for one real frame, staged under `pts_us` before
	// the base encoder sees the frame. False if `tiles` failed: nothing is
	// staged, and whatever the tracker had committed to is repaired.
	bool stage_layer(TileSource &tiles, int64_t pts_us, const DamageRegion *damage);

	// Periodic DEBUG line from the tracker's counters; see log_stats().
	void log_stats(int64_t now_us);
	// Counts one layer that made it into a packet: `raw` tile pixel bytes,
	// `wire` what the layer added to the packet (header, rects and the
	// Zstd frame).
	void count_layer(size_t raw, size_t wire);
	int64_t stats_logged_us_ = 0;
	uint64_t layer_raw_bytes_ = 0;
	uint64_t layer_wire_bytes_ = 0;

	// The bandwidth budget (wraith.toml's refine.bandwidth_percent): a
	// token bucket in wire bytes, refilled at that share of the base
	// layer's target bitrate and drained by every layer that goes out.
	// Before each tracker call, what's in it is turned into a raw byte
	// cap through the running compression ratio -- the tracker counts
	// raw bytes, the wire carries Zstd's output. A layer may overdraw the
	// bucket (the ratio is only an estimate); the next ones then wait.
	// The layer is outside the rate controller's view otherwise, so this
	// is what keeps a slow or congested link from carrying a screenful of
	// lossless tiles at once.
	double budget_bytes_per_s() const;
	// wraith.toml's [refine] for the session's current profile, and the
	// tracker settings they make.
	static TileTrackerConfig tracker_config_for(const RefineSettings &settings);
	void log_policy(const char *what) const;
	LinkProfile link_profile_ = LinkProfile::kLan;
	RefineSettings settings_ = wraith::config().refine_for(LinkProfile::kLan);
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	gdp::VideoCodec codec_ = gdp::VideoCodec::Unknown;
	void apply_byte_budget(int64_t now_us);
	uint32_t target_bps_ = 0;
	double tokens_ = 0; // wire bytes; negative after an overdraw
	int64_t tokens_us_ = 0;
	// Wire bytes per raw byte, smoothed over recent layers. Starts
	// pessimistic -- desktop text gets ~0.08, video-like tiles ~0.3-0.5 --
	// so the first layers undershoot rather than overdraw.
	double wire_ratio_ = 0.25;

	// A layer the tracker produced together with what it committed to in
	// producing it, so the commitment can be undone if the layer is lost
	// before it reaches the wire (see lose_layer()).
	struct StagedLayer {
		gdp::RefineLayer layer;
		TileEmission emission;
		int64_t staged_us = 0; // wraith's clock, for expiry
	};

	// The staged layer is not going to be sent; repair() its emission.
	void lose_layer(const StagedLayer &staged) { repair(staged.emission); }
	// A layer that never reached the client, whether it was dropped here
	// or reported lost by the client: its tiles and clears are redone by
	// the tracker (TileTracker::repair()), so only that layer's work is
	// repeated. A lost reset can only be answered with another reset --
	// the pause's, while paused.
	void repair(const TileEmission &emission);

	// Every packet's layer as it went out, keyed by pts, so a frame the
	// client reports lost (frames_lost()) can be repaired from exactly
	// what it carried. Packets with no layer are recorded too, empty: a
	// pts missing from here is one the history has already forgotten,
	// which is answered with a reset. `epoch` counts the resets sent
	// before the entry; a reset wipes the whole plane, so repairing an
	// entry from before the latest one would only redo work the reset
	// already undid.
	struct SentLayer {
		TileEmission emission;
		uint64_t epoch = 0;
	};
	// About 8 s at 60 fps. spectre reports every 250 ms on the last 64
	// frames, so an entry is only ever looked up within a second or so;
	// the margin covers a report delayed behind a congested link.
	static constexpr size_t kSentHistory = 512;
	void record_sent(int64_t pts_us, TileEmission emission);
	std::map<int64_t, SentLayer> sent_history_;
	uint64_t reset_epoch_ = 0;

	std::unique_ptr<Encoder> base_;
	bool base_open_ = false;
	TileTracker tracker_;

	// Lossless layers waiting for the base encoder to hand back the packet
	// they belong to, keyed by pts. An asynchronous base returns its packet
	// on a later poll(), so the layer computed in push_cpu() can't just be
	// attached to whatever poll() returns next -- it's matched by pts
	// instead. Layers for packets that never arrive (a dropped frame) are
	// discarded by age (staged_us), not by pts: pts may come from the
	// compositor's clock while a re-delivered frame's comes from wraith's,
	// so they needn't increase from one push to the next.
	std::map<int64_t, StagedLayer> pending_layers_;
	void stage(int64_t pts_us, StagedLayer staged);

	// Layer-only packets built by pump(), handed out by poll(), each with
	// the emission record_sent() keeps for it and `after_seq`, how many
	// frames had been pushed when it was built: it goes out behind those
	// and ahead of every later one (see in_flight_).
	struct LayerOnlyPacket {
		EncodedPacket packet;
		TileEmission emission;
		uint64_t after_seq = 0;
	};
	std::vector<LayerOnlyPacket> layer_only_packets_;

	// Frames handed to an asynchronous base encoder whose packets haven't
	// come out of its poll() yet, numbered in push order. The client
	// applies layers in arrival order, so a layer-only packet goes out
	// exactly where it was built: after every frame pushed before it (one
	// sent ahead of a frame still being encoded would land before that
	// frame's own layer), and before every frame pushed after it (whose
	// layer may take those same tiles back as changed -- arriving after
	// it, they would sit stale over the newer picture). Always empty after
	// poll() with a synchronous base. An entry that never comes back (an
	// encode error) expires after kInFlightExpiryUs rather than holding
	// layers back for good.
	struct InFlight {
		int64_t pts_us;
		int64_t pushed_us;
		uint64_t seq;
	};
	std::vector<InFlight> in_flight_;
	uint64_t push_seq_ = 0;
	void note_pushed(int64_t pts_us);
	// Hands out the layer-only packets built before frame `seq` was pushed.
	void release_layer_only(uint64_t seq, std::vector<EncodedPacket> *out);

	// Send kFlagReset with the next frame -- wipe the client's overlay
	// plane. Set by request_keyframe(), i.e. when the host has no idea
	// what the client's plane holds: session start (a new client), or a
	// loss the sent history can no longer account for; by repair() for a
	// reset that was itself lost; and by resuming from a pause. Only
	// request_keyframe() pairs it with an IDR -- the plane is independent
	// of the video's references, so the others need none. Ordinary loss
	// does not come here: frames_lost() repairs just the lost layers and
	// request_repair_keyframe() asks for the video's IDR alone. A wipe
	// drops the whole plane back to the lossy base and repaints it over
	// however many frames the tracker's byte budget needs, which is
	// visible, so nothing else should reach for it.
	//
	// Deliberately *not* derived from the base encoder's own keyframe
	// flag: the base decides IDRs internally and asynchronously, so by the
	// time a packet comes back marked keyframe the source pixels that
	// produced it are long gone and the tracker can no longer build a
	// layer for them. Driving both from the same request instead keeps the
	// reset and the IDR within a frame of each other, which is all the
	// client needs -- it applies a reset wherever it lands. pump() never
	// consumes it: a reset rides only on a real frame, next to its IDR.
	bool send_reset_next_ = true;

	// set_refine_paused(): no tracker, and the reset that wipes the
	// client's plane is still owed until a packet carries it
	// (stage_pause_reset()).
	bool paused_ = false;
	bool send_pause_reset_ = false;
	// Paused: stages the owed reset, if any, for the frame at `pts_us`.
	void stage_pause_reset(int64_t pts_us);
	// The frame at `pts_us` never reached the base encoder: gives back
	// whatever layer was staged for it (lose_layer()).
	void unstage(int64_t pts_us);
};

} // namespace wraith
