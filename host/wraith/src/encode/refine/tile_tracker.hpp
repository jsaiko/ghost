// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Decides, frame by frame, which parts of the screen lossless refinement
// should refresh losslessly and which parts the client must stop showing
// losslessly (gdp/refine.hpp has the container and the client-side model).
//
// The policy, in one paragraph: the frame is cut into a fixed grid of
// tiles. Each real frame, every tile is hashed and compared with its
// previous hash (about 1 ms at 4K); the idle pump's re-fed frames, whose
// pixels are identical by construction, hash nothing. A
// tile whose content just changed is *moving*: its lossless copy on the
// client is now stale, so it gets a clear rect and the video codec alone
// carries it. A tile that has been identical for settle_us of wall-clock
// time has *settled*: it gets sent once, exactly, as a lossless tile, and
// then costs nothing again until it changes. So dragging a window, playing
// a video or scrolling stays on the lossy path at full speed, and the
// moment the screen stops moving it converges to a bit-exact image --
// which is the point, for text and UI that the codec renders mushily.
//
// Settling is measured in time, not in calls, because the calls are not
// evenly spaced: real frames arrive at the content's rate and the idle
// pump adds ticks between them, so counting calls would let a 24 fps
// video (each frame held ~41 ms) look "still for three calls" and go out
// as a megabyte of poorly compressing lossless pixels, cleared again a
// frame later.
//
// "Still" means more than "this tile's hash is unchanged": a tile the
// caller's damage region touched is treated as active even when its bytes
// came out identical. Decoded video is full of skip macroblocks, so sky
// and blurred background are byte-identical for a few hundred ms at a
// time and would settle on their own hash, then pop back to lossy a
// moment later -- chatter. But the browser damages the whole video quad
// every frame it gets, so every tile under a playing video is damaged at
// least every ~41 ms and never settles. Text is the opposite: once a
// keystroke lands nothing damages anything, and the settle time runs.
// When damage is unknown (null) only the hash counts, so a source
// without damage metadata still converges.
//
// Damage decides activity only, never what changed: every real frame
// hashes every tile, whatever the damage says (about 1 ms at 4K), and the
// hash alone decides clears. Headless GNOME's damage misses real changes
// (the area a fast-dragged window just left, a caret's tile ~24 px outside
// its rect), which would leave stale lossless pixels if damage gated the
// hashing; used as activity, a miss only lets a tile settle a little
// early. The cost of trusting damage this far: mutter and KWin report a
// surface's damage without subtracting what covers it, so a terminal on
// top of a playing video stays lossy where the two overlap. Pixels alone
// can't do better: a motion rule built on them (neighbourhood and region
// holds) can't tell a video's still patches from still content, and
// chatters at 4K.
//
// There is deliberately no per-tile backoff for a tile that gets sent
// and then changed again soon after: a 16 px tile holds about two
// characters, so ordinary typing would trip it and hold the line just
// typed lossy for 240 ms or more. Damage is what tells a video's still
// patch from a line of text.
//
// The frame itself is read only through a TileSource (tile_source.hpp):
// its tile hashes, and the pixels of the rects that go out -- so it may
// be in host memory or still on the GPU. The policy here is
// content-agnostic; it never looks at the
// codec bitstream, and it does not need to know when the base encoder
// emits an IDR -- the caller asks for a reset (kFlagReset, "drop the whole
// overlay plane") explicitly, at the same moments it asks the base encoder
// for a keyframe. See RefineEncoder for why the two are driven together
// rather than derived from each other.
//
// What the tracker believes the client is showing (`Tile::sent`) is only
// as good as the caller's delivery of the layers it hands out. process()
// marks tiles sent optimistically; a layer that never reached the client
// -- dropped before the wire, or reported lost by the client -- is given
// back through repair(), which re-arms the tiles it carried and re-issues
// the clears it carried, so only what that one layer did is redone. There
// is deliberately no periodic "re-send everything" sweep: a 64-bit hash
// collision is not a realistic event, and the divergences that are not
// are exactly the ones repair() (fed by the host's own failures and by
// spectre's loss reports, discarded frames included) closes.
#pragma once

#include "encode/encoder.hpp" // DamageRegion
#include "encode/refine/tile_source.hpp"
#include "gdp/refine.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace wraith {

struct TileTrackerConfig {
	// Grid cell size. 16 matches H.264's macroblock, the base codec's own
	// unit of change, so a tile drops back to lossy only where the video
	// itself is being re-coded: a caret, a typed character or a window
	// edge holds at most a 15px band lossy, and re-sending a tile costs
	// 768 bytes raw. The bookkeeping this implies (a
	// 4K frame is 32,400 tiles) is kept cheap by coalescing emitted tiles
	// into rects the same way clears are, so the wire never sees one rect
	// per tile, and by counting unsent tiles rather than scanning for
	// them. 8 would shrink the band a little more for four times the
	// bookkeeping and no codec alignment to show for it.
	uint32_t tile_size = 16;
	// How long a tile must stay inactive before it counts as settled and
	// is sent losslessly. A playing video's own tiles stay lossy by
	// changing faster than this, but only while the gap between two frame
	// deliveries stays under this, and PipeWire delivery from a busy compositor
	// jitters: at 50 ms, a 24 fps video (41 ms holds) settles its whole
	// quad whenever a frame arrives 10 ms late. 80 ms is about twice the
	// hold, the usual jitter margin, and with the 16 ms idle pump still
	// puts the text just typed exact 80-96 ms after the last keystroke.
	int64_t settle_us = 80000;
	// Upper bound on raw (pre-Zstd) tile bytes emitted in one frame, so
	// that a whole screen settling at once is spread over several frames
	// instead of producing one enormous datagram burst. Counted in
	// tile_pixels bytes (gdp::kRefineBytesPerPixel), so 768 KiB is 1024
	// tiles of 16px. Tiles that don't fit this frame stay settled and are
	// picked up by the next one. The tile count also keeps the rect count
	// per frame well inside the container's u16 even before coalescing.
	size_t max_tile_bytes_per_frame = 1024 * 16 * 16 * gdp::kRefineBytesPerPixel;
};

// What one process() call committed to: the grid indices (and the content
// hash at the time) of the tiles it marked sent, the clear rects the layer
// carried, and whether it reset the plane. Handed back to repair() if the
// layer never reached the client. A few KB at most (the per-frame cap
// bounds the tiles, the container's u8 count bounds the clears), so the
// caller can afford to keep one per frame until the client reports on it.
struct TileEmission {
	std::vector<std::pair<uint32_t, uint64_t>> tiles;
	std::vector<gdp::RefineRect> clears;
	bool reset = false;

	bool empty() const { return tiles.empty() && clears.empty() && !reset; }
};

class TileTracker {
public:
	// `width`/`height` are the frame size in pixels; changing either means
	// a new TileTracker (the encoder is re-opened on resize anyway).
	void reset(uint32_t width, uint32_t height, const TileTrackerConfig &config);

	// Feeds one XRGB8888 source frame and produces the lossless layer to
	// send with it. `now_us` is a monotonic clock the caller reads for
	// every call (real frame or idle-pump tick alike); the tracker never
	// reads a clock itself, and does not use frame pts, which may be a
	// presentation clock. `force_reset` sets RefineLayer::reset and re-arms every
	// tile, putting a client that joined late -- or that lost a reset --
	// back into a known state; it also drops any clears repair() still
	// owes, which the reset covers. The first frame after reset() always
	// resets regardless.
	//
	// `damage` is which part of the frame the source says was repainted
	// since the previous call (null: unknown). Tiles it touches count as
	// active -- they don't settle -- whether or not their hash moved; see
	// "Still" above.
	//
	// `emission`, if given, receives what this call committed to, for
	// repair(). `data`/`stride` are only read during the call. The
	// returned layer owns its pixels.
	gdp::RefineLayer process(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride,
		int64_t now_us, bool force_reset, const DamageRegion *damage = nullptr,
		TileEmission *emission = nullptr);

	// The idle pump's call: `data` is, by the caller's guarantee, exactly
	// the pixels of the last process() call, re-fed only to advance the
	// settle clock and copy out the tiles that are now due. That guarantee
	// is what lets it hash nothing -- the only difference from process().
	gdp::RefineLayer pump(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride,
		int64_t now_us, TileEmission *emission = nullptr);

	// The same two, reading the frame through `source` (tile_source.hpp)
	// rather than from host memory -- the frame may still be on the GPU.
	// The frame size is the source's. If the source fails to hash, the
	// call changes nothing and returns an empty layer; if it fails to read
	// the tiles it chose, the layer's pixels are unusable and its
	// emission must go back through repair(). source.failed() says which
	// calls those were.
	gdp::RefineLayer process(TileSource &source, int64_t now_us, bool force_reset,
		const DamageRegion *damage = nullptr, TileEmission *emission = nullptr);
	gdp::RefineLayer pump(TileSource &source, int64_t now_us, TileEmission *emission = nullptr);

	// The layer a previous process() or pump() produced never reached the
	// client: undo what it did, so the next call redoes exactly that.
	// - Its tiles are re-armed, if their content is still what was
	//   emitted (one that changed since was re-armed by that change). A
	//   tile re-sent since with the same content gets sent once more:
	//   wasted bytes, never wrong pixels, since only current content goes out.
	// - Its clears go out again with the next call's layer, and every tile
	//   under them is re-armed. The client may still be showing lossless
	//   pixels there from before the lost clear, whatever has happened to
	//   the tile since -- a later clear only went out if the tile was sent
	//   again in between -- so they are re-issued unconditionally. A tile a
	//   later layer did re-send correctly is wiped with the rest and
	//   re-sent: again bytes, not pixels.
	// A reset can't be repaired here -- it covers the tiles the tracker
	// knows nothing about any more; the caller answers a lost reset with
	// another one (RefineEncoder does).
	void repair(const TileEmission &emission);

	// True if there is a tile that hasn't been sent yet -- still waiting
	// out its settle time, or due but waiting on the byte budget -- or a
	// clear repair() still owes. Frames only arrive when something
	// changes, so on a still desktop nothing would reach settle_us without
	// SessionServices' idle pump, which keeps calling pump() while this is
	// true.
	bool has_pending_work() const;

	// Replaces the settle time (settle_us) with `config`'s, from the next
	// call on, keeping every tile's state: a session whose network profile
	// changed carries on where it was. The grid and the per-frame cap stay
	// as reset() set them.
	void set_policy(const TileTrackerConfig &config);

	// Caps the raw tile bytes the next process() or pump() call may emit,
	// below max_tile_bytes_per_frame: the caller's bandwidth budget
	// (RefineEncoder's, a share of the video's target bitrate). Settled
	// tiles that don't fit stay due and go out on a later call; unlike the
	// per-frame cap, which always lets one tile through, a budget smaller
	// than a tile admits none. Applies to that one call only -- one that
	// isn't preceded by this has no budget but the per-frame cap.
	void set_byte_budget(size_t raw_bytes) { byte_budget_ = raw_bytes; }

	// Running counters, for the caller's periodic log line: how many
	// tile-frames were held back from settling by damage while their own
	// hash was unchanged (the video case) versus because their hash
	// actually moved, how many settled tiles had to wait for the bandwidth
	// budget (set_byte_budget), and how many were cleared within
	// kChurnWindowUs of being sent -- lossless bytes that bought nothing,
	// and on screen, chatter. Reset by the caller.
	struct Stats {
		uint64_t held_by_damage = 0;
		uint64_t held_by_change = 0;
		uint64_t held_by_budget = 0;
		uint64_t tiles_sent = 0;
		uint64_t churned = 0;
	};

	Stats take_stats() {
		Stats out = stats_;
		stats_ = Stats{};
		return out;
	}

private:
	// The budget's sweep walks settled tiles in squares of this many
	// tiles a side (128 px at 16 px tiles), see process_frame().
	static constexpr uint32_t kSweepBlock = 8;
	// Stats::churned: sent, then cleared within this.
	static constexpr int64_t kChurnWindowUs = 1000000;
	// Far enough in the past that `now - kLongAgo` can't overflow.
	static constexpr int64_t kLongAgoUs = -(int64_t{1} << 60);

	struct Tile {
		uint64_t hash = 0;
		// When the tile was last active: its hash changed, or the damage
		// touched it. The first frame counts as a change.
		int64_t changed_us = 0;
		// When it was last sent (Stats::churned).
		int64_t sent_us = kLongAgoUs;
		// True once the client has been sent this tile's current content
		// losslessly, so it isn't sent again until the content changes.
		// Also means "the client has lossless pixels here", which is what
		// decides whether a change needs a clear.
		bool sent = false;
	};

	// process() and pump(): `same_pixels` is pump()'s guarantee, which
	// skips hashing.
	gdp::RefineLayer process_frame(TileSource &source, int64_t now_us, bool force_reset,
		const DamageRegion *damage, bool same_pixels, TileEmission *emission);

	// Grid indices, ascending, merged into as few rects as possible:
	// horizontally adjacent tiles become a run, and runs in consecutive
	// rows with the same column span stack into one taller rect. Both the
	// clear list and the tile list go through this, so a repainting or a
	// settling window is a handful of rects rather than one per tile.
	void coalesce(const std::vector<uint32_t> &indices, std::vector<gdp::RefineRect> *out) const;

	// Converts clear_tiles_ into layer->clears, coalesced, with the
	// bounding-box fallback for the u8 clear_count (see the .cpp).
	void build_clears(gdp::RefineLayer *layer);

	// Marks in dirty_ every tile that `damage` touches (none for null:
	// unknown damage marks no activity, and the hash alone decides).
	void mark_dirty(const DamageRegion *damage);

	// The one place Tile::sent changes, so unsent_ stays exact.
	void set_sent(Tile &tile, bool sent);

	TileTrackerConfig config_;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint32_t cols_ = 0;
	uint32_t rows_ = 0;
	std::vector<Tile> tiles_;
	// Per tile, whether this call's damage touched it. Per-call scratch.
	std::vector<uint8_t> dirty_;
	// Per tile, whether it is settled and due to be sent. Per-call scratch.
	std::vector<uint8_t> due_;
	// This frame's hash of every tile, computed up front by the source
	// (TileSource::hash_tiles()) and read by process_frame()'s sequential
	// pass.
	std::vector<uint64_t> new_hash_;
	// Per tile, whether repair() owes the client a clear there; the next
	// call's layer carries it. pending_clears_ counts them.
	std::vector<uint8_t> repair_clear_;
	size_t pending_clears_ = 0;
	// Grid indices of the tiles that changed this frame while the client
	// held them losslessly, in grid order; build_clears() turns them into
	// rects. Per-call scratch, kept to avoid reallocating.
	std::vector<uint32_t> clear_tiles_;
	// Grid indices of the tiles this frame's byte budget admitted, before
	// coalescing into layer->tiles. Per-call scratch.
	std::vector<uint32_t> emit_tiles_;
	// How many tiles have sent == false: has_pending_work() in O(1).
	size_t unsent_ = 0;
	Stats stats_;
	// Where the next frame's byte budget resumes, as a block-order key
	// (one past the last tile emitted; see process_frame()). A screen with
	// more settled tiles than one frame's budget therefore fills in order
	// across frames rather than always restarting at the top-left corner.
	// It has to be a position on the grid rather than an index into the
	// per-frame candidate list: that list is rebuilt every frame and
	// shrinks as tiles are sent, so the same offset into it walks a
	// different screen position each time, and the fill comes out striped.
	uint32_t sweep_start_ = 0;
	bool first_frame_ = true;
	// set_byte_budget()'s cap for the next call; SIZE_MAX is none.
	size_t byte_budget_ = SIZE_MAX;
};

} // namespace wraith
