// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// TileTracker's policy, which is the whole of what makes lossless
// refinement "settle to lossless" (encode/refine/tile_tracker.hpp): moving
// regions are cleared and left to the codec, still regions are refreshed
// losslessly exactly once, a reset re-arms everything, every real frame
// is hashed (the idle pump's re-fed ones aren't), and a lost layer is
// repaired by redoing exactly what it did.
//
// Pure CPU logic over a synthetic framebuffer -- no encoder, no GPU, no
// session. The far end of the same behaviour (applying the layer to the
// overlay plane) is spectre's LosslessPlane, and the container between
// them is covered by libgdp's refine_test.
#include "encode/refine/tile_tracker.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <vector>

namespace {

// Not <cassert>: the tree's default build type is RelWithDebInfo, which
// defines NDEBUG and would compile every check -- calls included -- away.
int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 128;
constexpr uint32_t kTile = 64;
constexpr uint32_t kStride = kWidth * 4;
// 4 columns x 2 rows of tiles.
constexpr size_t kTileCount = (kWidth / kTile) * (kHeight / kTile);

struct Frame {
	std::vector<uint8_t> pixels = std::vector<uint8_t>((size_t)kStride * kHeight, 0);

	// The whole frame as the layer carries it: XRGB with the X byte
	// dropped (gdp::RefineLayer::tile_pixels).
	std::vector<uint8_t> wire_pixels() const {
		std::vector<uint8_t> out;
		for (size_t i = 0; i < pixels.size(); i += 4) {
			out.insert(out.end(), pixels.begin() + i, pixels.begin() + i + 3);
		}
		return out;
	}

	// Paints one tile of the grid a flat value.
	void paint_tile(uint32_t col, uint32_t row, uint8_t value) {
		for (uint32_t y = 0; y < kTile; y++) {
			uint8_t *p = pixels.data() + (size_t)(row * kTile + y) * kStride + (size_t)col * kTile * 4;
			for (uint32_t x = 0; x < kTile * 4; x++) {
				p[x] = value;
			}
		}
	}
};

// Rects are coalesced (tiles as well as clears), so "is this tile in the
// layer" is containment, not an origin match.
bool covers(const std::vector<gdp::RefineRect> &rects, uint32_t col, uint32_t row) {
	uint32_t x = col * kTile, y = row * kTile;
	for (const gdp::RefineRect &rect : rects) {
		if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height) {
			return true;
		}
	}
	return false;
}

// Grid indices of every `tile`-sized cell the rects cover, ascending.
std::vector<uint32_t> tiles_in(const std::vector<gdp::RefineRect> &rects, uint32_t tile = kTile) {
	std::vector<uint32_t> out;
	uint32_t cols = (kWidth + tile - 1) / tile;
	for (const gdp::RefineRect &rect : rects) {
		for (uint32_t y = rect.y; y < rect.y + rect.height; y += tile) {
			for (uint32_t x = rect.x; x < rect.x + rect.width; x += tile) {
				out.push_back((y / tile) * cols + x / tile);
			}
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

size_t tile_count(const gdp::RefineLayer &layer, uint32_t tile = kTile) {
	return tiles_in(layer.tiles, tile).size();
}

// The tracker settles on wall-clock time; the tests drive a synthetic
// clock that advances one 60 Hz frame per call, with the settle time set
// to exactly three frames so "three quiet calls, then sent" holds.
constexpr int64_t kFrameUs = 16667;
constexpr int64_t kSettleUs = 3 * kFrameUs;
int64_t g_now_us = 0;

gdp::RefineLayer step(wraith::TileTracker &tracker, const Frame &frame, bool force_reset,
	wraith::TileEmission *emission);

void settle_all(wraith::TileTracker &tracker, const Frame &frame) {
	for (int i = 0; i < 8; i++) {
		step(tracker, frame, false, nullptr);
	}
}

wraith::TileTrackerConfig config() {
	wraith::TileTrackerConfig c;
	c.tile_size = kTile;
	c.settle_us = kSettleUs;
	return c;
}

// Settles everything: eight frames covers the reset frame, the settle
// time and the send.
void settle_all(wraith::TileTracker &tracker, const Frame &frame);

// One process() call, a frame later than the last, with unknown damage.
gdp::RefineLayer step(wraith::TileTracker &tracker, const Frame &frame, bool force_reset = false,
	wraith::TileEmission *emission = nullptr) {
	g_now_us += kFrameUs;
	return tracker.process(frame.pixels.data(), kWidth, kHeight, kStride, g_now_us, force_reset, nullptr,
		emission);
}

// One process() call whose damage is exactly the tiles listed, as
// (col, row) pairs.
gdp::RefineLayer damaged_step(wraith::TileTracker &tracker, const Frame &frame,
	std::initializer_list<std::pair<uint32_t, uint32_t>> tiles) {
	wraith::DamageRegion damage;
	for (auto [col, row] : tiles) {
		damage.rects.push_back(wraith::DamageRect{(int32_t)(col * kTile), (int32_t)(row * kTile),
			(int32_t)kTile, (int32_t)kTile});
	}
	g_now_us += kFrameUs;
	return tracker.process(frame.pixels.data(), kWidth, kHeight, kStride, g_now_us, false, &damage);
}

// One pump() call (the idle pump: same pixels as the last frame), a frame
// later than the last.
gdp::RefineLayer pump_step(wraith::TileTracker &tracker, const Frame &frame) {
	g_now_us += kFrameUs;
	return tracker.pump(frame.pixels.data(), kWidth, kHeight, kStride, g_now_us);
}

// A still screen is sent losslessly exactly once, then costs nothing.
void test_settles_then_goes_quiet() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	frame.paint_tile(0, 0, 0x40);

	// Frame 1: first frame, so a reset -- and nothing has been still for
	// the settle time yet.
	gdp::RefineLayer layer = step(tracker, frame);
	CHECK(layer.reset);
	CHECK(layer.tiles.empty());

	// Frames 2 and 3 are still, but haven't reached the settle time.
	for (int i = 0; i < 2; i++) {
		layer = step(tracker, frame);
		CHECK(layer.tiles.empty());
	}

	// Frame 4: every tile has now been unchanged for 3 frames, so the
	// whole screen goes out losslessly in one go.
	layer = step(tracker, frame);
	CHECK(tile_count(layer) == kTileCount);
	CHECK(!layer.reset);
	CHECK(layer.clears.empty());
	CHECK(layer.tile_pixels.size() == gdp::refine_layer_pixel_bytes(layer.tiles));

	// And then nothing, forever, while the screen stays still -- this is
	// what makes the layer nearly free on an idle desktop.
	for (int i = 0; i < 10; i++) {
		layer = step(tracker, frame);
		CHECK(layer.empty());
	}
}

// A region that changes is cleared, stays on the H.264 path while it keeps
// changing, and is refreshed once it stops.
void test_moving_region_clears_then_resettles() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;

	// Settle the whole screen first.
	gdp::RefineLayer layer;
	for (int i = 0; i < 4; i++) {
		layer = step(tracker, frame);
	}
	CHECK(tile_count(layer) == kTileCount);
	settle_all(tracker, frame);

	// Now one tile starts changing every frame.
	for (uint8_t value = 1; value <= 5; value++) {
		frame.paint_tile(1, 0, value);
		layer = step(tracker, frame);

		if (value == 1) {
			// First change: the client is showing a stale lossless copy,
			// so it must be told to drop it.
			CHECK(layer.clears.size() == 1);
			CHECK(covers(layer.clears, 1, 0));
		} else {
			// Still moving: nothing more to say, it was already cleared.
			CHECK(layer.clears.empty());
		}
		// A moving tile is never sent losslessly.
		CHECK(!covers(layer.tiles, 1, 0));
		// ...and its still neighbours aren't re-sent either.
		CHECK(layer.tiles.empty());
	}

	// It stops. Three still frames later it comes back losslessly, and
	// only it.
	for (int i = 0; i < 2; i++) {
		layer = step(tracker, frame);
		CHECK(layer.tiles.empty());
	}
	layer = step(tracker, frame);
	CHECK(tile_count(layer) == 1);
	CHECK(covers(layer.tiles, 1, 0));
}

// A reset re-sends everything: this is how a client that lost a frame (or
// joined mid-stream) is put back into a known state.
void test_reset_resends_everything() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);

	gdp::RefineLayer layer = step(tracker, frame, true);
	CHECK(layer.reset);
	// A reset drops the whole plane, so clears alongside it are redundant.
	CHECK(layer.clears.empty());
	// Nothing has moved, so everything is still settled and goes straight
	// back out in the same frame.
	CHECK(tile_count(layer) == kTileCount);
}

// The per-frame byte budget spreads a whole screen settling at once over
// several frames rather than emitting one huge datagram burst.
void test_byte_budget_spreads_tiles() {
	wraith::TileTrackerConfig c = config();
	c.max_tile_bytes_per_frame = kTile * kTile * gdp::kRefineBytesPerPixel * 3; // three tiles' worth
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, c);
	Frame frame;

	size_t total = 0;
	int frames = 0;
	for (; frames < 20 && total < kTileCount; frames++) {
		gdp::RefineLayer layer = step(tracker, frame);
		CHECK(tile_count(layer) <= 3);
		total += tile_count(layer);
	}
	// All 8 tiles arrive, and not in one frame.
	CHECK(total == kTileCount);
	CHECK(frames > 4);
}

// The per-frame budget resumes where the previous frame stopped, in grid
// order, so a screen too big to send in one frame fills in a single pass
// rather than striping. The resume point is a grid index, not an index
// into the per-frame candidate list: that list shrinks as tiles are sent,
// so the same offset would land on a different tile each frame and skip
// regions until a later wrap.
void test_budget_sweeps_in_grid_order() {
	wraith::TileTrackerConfig c = config();
	c.max_tile_bytes_per_frame = kTile * kTile * gdp::kRefineBytesPerPixel * 2; // two tiles' worth
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, c);
	Frame frame;

	// Settle everything, then collect the order the 8 tiles arrive in.
	std::vector<uint32_t> order;
	for (int i = 0; i < 20 && order.size() < kTileCount; i++) {
		gdp::RefineLayer layer = step(tracker, frame);
		CHECK(tile_count(layer) <= 2);
		for (uint32_t index : tiles_in(layer.tiles)) {
			order.push_back(index);
		}
	}
	CHECK(order.size() == kTileCount);

	// Strictly ascending grid indices: every tile once, no skipping ahead
	// and no coming back.
	for (size_t i = 1; i < order.size(); i++) {
		CHECK(order[i] > order[i - 1]);
	}
}

// Every real frame rehashes every tile, so any change is caught on the
// frame it happens -- there is no damage region to miss it -- while
// pump(), guaranteed the last frame's pixels, hashes nothing.
void test_every_real_frame_is_hashed() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);
	CHECK(!tracker.has_pending_work());

	frame.paint_tile(1, 0, 0x55);
	frame.paint_tile(3, 1, 0x66);
	gdp::RefineLayer layer = step(tracker, frame);
	CHECK(covers(layer.clears, 1, 0));
	CHECK(covers(layer.clears, 3, 1));
	CHECK(tiles_in(layer.clears).size() == 2);
	settle_all(tracker, frame);

	// pump() doesn't look...
	frame.paint_tile(0, 0, 0x88);
	layer = pump_step(tracker, frame);
	CHECK(layer.clears.empty());

	// ...the next real frame does.
	layer = step(tracker, frame);
	CHECK(covers(layer.clears, 0, 0));
	CHECK(tiles_in(layer.clears).size() == 1);
}

// set_byte_budget(): the caller's bandwidth budget caps what one call
// may emit, below the per-frame cap. Too small for a tile admits none
// (the settled tiles stay due, counted as held by budget); room for two
// admits exactly two; and it applies to one call only.
void test_byte_budget_caps_one_call() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	const size_t tile_bytes = (size_t)kTile * kTile * gdp::kRefineBytesPerPixel;

	// The reset frame, then the settle time: everything is due on step 4.
	for (int i = 0; i < 3; i++) {
		step(tracker, frame);
	}
	(void)tracker.take_stats();

	tracker.set_byte_budget(tile_bytes - 1);
	gdp::RefineLayer layer = step(tracker, frame);
	CHECK(layer.tiles.empty());
	CHECK(tracker.has_pending_work());
	CHECK(tracker.take_stats().held_by_budget == kTileCount);

	tracker.set_byte_budget(2 * tile_bytes);
	layer = step(tracker, frame);
	CHECK(tile_count(layer) == 2);
	CHECK(tracker.take_stats().held_by_budget == kTileCount - 2);

	// No budget set: the rest go out (the per-frame cap fits them all).
	layer = step(tracker, frame);
	CHECK(tile_count(layer) == kTileCount - 2);
	CHECK(!tracker.has_pending_work());
}

// The idle pump's pump(): nothing is hashed, the settle count still
// advances, and settled tiles still go out. The pixels are still read for
// the tiles themselves.
void test_pump_advances_settle() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	frame.paint_tile(2, 0, 0x33);
	step(tracker, frame); // reset frame, hashes all

	gdp::RefineLayer layer;
	for (int i = 0; i < 3; i++) {
		layer = pump_step(tracker, frame);
	}
	CHECK(tile_count(layer) == kTileCount);
	CHECK(!tracker.has_pending_work());
	// The painted tile's pixels came through, not stale ones.
	bool found = false;
	size_t offset = 0;
	for (const gdp::RefineRect &rect : layer.tiles) {
		if (rect.x <= 2 * kTile && 2 * kTile < rect.x + rect.width && rect.y == 0) {
			found =
				layer.tile_pixels[offset + (size_t)(2 * kTile - rect.x) * gdp::kRefineBytesPerPixel] == 0x33;
		}
		offset += (size_t)rect.width * rect.height * gdp::kRefineBytesPerPixel;
	}
	CHECK(found);
}

// A lost layer is given back with repair(): its tiles are re-armed and go
// out again on the next frame -- but a tile whose content changed in
// between is left to the change's own bookkeeping.
void test_repair_rearms_lost_tiles() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	wraith::TileEmission emission;
	gdp::RefineLayer layer;
	for (int i = 0; i < 4; i++) {
		layer = step(tracker, frame, false, &emission);
	}
	CHECK(tile_count(layer) == kTileCount);
	CHECK(emission.tiles.size() == kTileCount);
	CHECK(emission.clears.empty() && !emission.reset);
	CHECK(!tracker.has_pending_work());
	settle_all(tracker, frame);

	// One tile changes before the loss is discovered.
	frame.paint_tile(0, 1, 0x22);
	gdp::RefineLayer changed = step(tracker, frame);
	CHECK(covers(changed.clears, 0, 1));

	tracker.repair(emission);
	CHECK(tracker.has_pending_work());
	// The 7 unchanged tiles are settled already and go straight out; the
	// changed one is still counting and stays out of this frame.
	gdp::RefineLayer again = step(tracker, frame);
	CHECK(tile_count(again) == kTileCount - 1);
	CHECK(!covers(again.tiles, 0, 1));
	CHECK(again.clears.empty());
}

// The emission records the layer's reset and its clear rects, which is
// what repair() needs to redo them.
void test_emission_records_clears_and_reset() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	wraith::TileEmission emission;
	step(tracker, frame, false, &emission);
	CHECK(emission.reset); // the first frame is a reset
	for (int i = 0; i < 3; i++) {
		step(tracker, frame, false, &emission);
	}
	CHECK(!emission.reset && emission.clears.empty());
	settle_all(tracker, frame);
	frame.paint_tile(1, 1, 0x11);
	gdp::RefineLayer layer = step(tracker, frame, false, &emission);
	CHECK(!emission.reset);
	CHECK(emission.clears.size() == layer.clears.size());
	CHECK(covers(emission.clears, 1, 1));
	CHECK(emission.tiles.empty());
}

// A lost clear is sent again, by itself: the next layer clears exactly
// the tile the lost one did, and nothing is reset or re-sent elsewhere.
void test_repair_reissues_lost_clear() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);
	frame.paint_tile(1, 1, 0x11);
	wraith::TileEmission lost;
	step(tracker, frame, false, &lost);
	CHECK(covers(lost.clears, 1, 1));

	tracker.repair(lost);
	CHECK(tracker.has_pending_work());
	gdp::RefineLayer layer = step(tracker, frame);
	CHECK(!layer.reset);
	CHECK(tiles_in(layer.clears) == std::vector<uint32_t>{5}); // (1,1) alone
	CHECK(!covers(layer.tiles, 0, 0));
	// Owed once: the layer after that carries no clear.
	layer = step(tracker, frame);
	CHECK(layer.clears.empty());
}

// The client may still show pre-clear lossless pixels whatever happened
// to the tile after the lost clear, so the clear is re-issued even when
// the tile was re-sent since -- and the tile goes out again in the same
// layer, after the clear (the client applies clears first).
void test_repair_reclears_a_tile_resent_since() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);
	frame.paint_tile(2, 0, 0x33);
	wraith::TileEmission lost;
	step(tracker, frame, false, &lost);
	settle_all(tracker, frame); // (2,0) re-settles and is sent again
	CHECK(!tracker.has_pending_work());

	tracker.repair(lost);
	gdp::RefineLayer layer = step(tracker, frame);
	CHECK(covers(layer.clears, 2, 0));
	CHECK(covers(layer.tiles, 2, 0));
	CHECK(tile_count(layer) == 1);
}

// A tile that changed again after the lost clear, while unsent, gets no
// clear from the change (nothing lossless of its own to drop) -- so the
// repair's clear is the only thing that removes the stale pixels.
void test_repair_reclears_a_tile_changed_since() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);
	frame.paint_tile(3, 1, 0x44);
	wraith::TileEmission lost;
	step(tracker, frame, false, &lost);
	frame.paint_tile(3, 1, 0x45);
	gdp::RefineLayer changed = step(tracker, frame);
	CHECK(changed.clears.empty());

	tracker.repair(lost);
	gdp::RefineLayer layer = step(tracker, frame);
	CHECK(covers(layer.clears, 3, 1));
}

// A repair on a still screen goes out through the idle pump, with no real
// frame to carry it.
void test_repair_rides_the_pump() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);
	frame.paint_tile(0, 0, 0x55);
	wraith::TileEmission lost;
	step(tracker, frame, false, &lost);
	settle_all(tracker, frame);

	tracker.repair(lost);
	CHECK(tracker.has_pending_work());
	gdp::RefineLayer layer = pump_step(tracker, frame);
	CHECK(covers(layer.clears, 0, 0));
	CHECK(covers(layer.tiles, 0, 0));
	CHECK(!tracker.has_pending_work());
}

// A reset covers everything a repair owes: the owed clears are dropped
// rather than sent alongside it.
void test_reset_drops_owed_clears() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);
	frame.paint_tile(1, 0, 0x66);
	wraith::TileEmission lost;
	step(tracker, frame, false, &lost);
	tracker.repair(lost);
	gdp::RefineLayer layer = step(tracker, frame, true);
	CHECK(layer.reset);
	CHECK(layer.clears.empty());
	layer = step(tracker, frame);
	CHECK(layer.clears.empty());
}

// Clears are coalesced: a block of changed tiles comes out as one rect,
// not one per tile, so a repainting window stays far inside the
// container's u8 clear_count.
void test_clears_coalesce() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);
	CHECK(!tracker.has_pending_work());

	// Columns 1-2 of both rows: a 2x2 block -> a single 128x128 clear.
	frame.paint_tile(1, 0, 0x10);
	frame.paint_tile(2, 0, 0x10);
	frame.paint_tile(1, 1, 0x10);
	frame.paint_tile(2, 1, 0x10);
	gdp::RefineLayer layer = step(tracker, frame);
	CHECK(!layer.reset);
	CHECK(layer.clears.size() == 1);
	if (layer.clears.size() == 1) {
		CHECK(layer.clears[0].x == kTile && layer.clears[0].y == 0);
		CHECK(layer.clears[0].width == 2 * kTile && layer.clears[0].height == 2 * kTile);
	}

	// Two separate tiles that don't touch stay two rects.
	frame.paint_tile(0, 0, 0x20);
	frame.paint_tile(3, 1, 0x20);
	settle_all(tracker, frame);
	settle_all(tracker, frame);
	frame.paint_tile(0, 0, 0x21);
	frame.paint_tile(3, 1, 0x21);
	layer = step(tracker, frame);
	CHECK(layer.clears.size() == 2);
}

// Change scattered so finely that coalescing can't get under 255 rects
// produces one bounding-box clear -- never a reset, which would repaint
// the whole screen. Lossless tiles inside the box that did not change are
// re-armed, since the client lost them too.
void test_many_scattered_clears_become_a_bounding_box() {
	wraith::TileTrackerConfig c = config();
	c.tile_size = 8; // 32 x 16 = 512 tiles on the test frame
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, c);
	Frame frame;
	settle_all(tracker, frame);
	CHECK(!tracker.has_pending_work());

	// A full-grid checkerboard: 256 isolated tiles, which cannot merge.
	uint32_t changed = 0;
	for (uint32_t row = 0; row < kHeight / 8; row++) {
		for (uint32_t col = 0; col < kWidth / 8; col++) {
			if ((row + col) % 2 == 0) {
				for (uint32_t y = 0; y < 8; y++) {
					uint8_t *p = frame.pixels.data() + (size_t)(row * 8 + y) * kStride + (size_t)col * 8 * 4;
					for (uint32_t x = 0; x < 8 * 4; x++) {
						p[x] = 0x5a;
					}
				}
				changed++;
			}
		}
	}
	CHECK(changed == 256);
	gdp::RefineLayer layer = step(tracker, frame);
	CHECK(!layer.reset);
	CHECK(layer.clears.size() == 1);
	if (layer.clears.size() == 1) {
		CHECK(layer.clears[0].x == 0 && layer.clears[0].y == 0);
		CHECK(layer.clears[0].width == kWidth && layer.clears[0].height == kHeight);
	}
	// The unchanged half of the board was inside the box: it is settled
	// already, so it goes straight back out on the next frame.
	layer = step(tracker, frame);
	CHECK(tile_count(layer, 8) == 256);
}

// Emitted tiles are coalesced like clears: a whole still screen goes out
// as one rect whose pixels are the frame itself, row-major, and a settled
// region with a hole in it comes out as a few rects, never one per tile.
// With the 16px default grid this is what keeps the per-rect overhead
// off the wire (a 4K screen is 32,400 tiles).
void test_tiles_coalesce() {
	wraith::TileTrackerConfig defaults;
	CHECK(defaults.tile_size == 16);

	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	for (uint32_t col = 0; col < kWidth / kTile; col++) {
		frame.paint_tile(col, 0, (uint8_t)(0x10 * (col + 1)));
	}
	// An X byte unlike its pixel's colour, so the layer shows it dropped.
	for (size_t i = 3; i < frame.pixels.size(); i += 4) {
		frame.pixels[i] = 0x77;
	}
	wraith::TileEmission emission;
	gdp::RefineLayer layer;
	for (int i = 0; i < 4; i++) {
		layer = step(tracker, frame, false, &emission);
	}
	CHECK(tile_count(layer) == kTileCount);
	CHECK(layer.tiles.size() == 1);
	if (layer.tiles.size() == 1) {
		CHECK(layer.tiles[0].x == 0 && layer.tiles[0].y == 0);
		CHECK(layer.tiles[0].width == kWidth && layer.tiles[0].height == kHeight);
		CHECK(layer.tile_pixels == frame.wire_pixels());
	}
	settle_all(tracker, frame);

	// Tile (1,0) changes, then the full emission is repaired: the seven
	// unchanged tiles come straight back as rects around the hole --
	// (0,0), (2..3,0) and the whole bottom row -- three rects, not seven.
	frame.paint_tile(1, 0, 0x01);
	step(tracker, frame);
	tracker.repair(emission);
	layer = step(tracker, frame);
	CHECK(tile_count(layer) == kTileCount - 1);
	CHECK(!covers(layer.tiles, 1, 0));
	CHECK(layer.tiles.size() == 3);
	CHECK(layer.tile_pixels.size() == gdp::refine_layer_pixel_bytes(layer.tiles));
}

// Settling is time, not calls. A fullscreen 24 fps video holds each frame
// for ~41 ms, and with the idle pump ticking between real frames the
// tracker sees several identical calls inside one hold; counting calls
// would send a megabyte of video pixels losslessly and clear them a frame
// later.
// Here: 3:2 pulldown, a tile changing after holds of 2 and 3 calls
// alternately while being polled every frame, and it must never be sent.
void test_video_holds_never_settle() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);

	uint8_t value = 1;
	size_t sent = 0;
	for (int call = 0; call < 200; call++) {
		// The longest hold is 3 calls, i.e. 2 quiet frames after the
		// change -- under the 3-frame settle time.
		if (call % 5 == 0 || call % 5 == 2) {
			frame.paint_tile(2, 0, value++);
		}
		gdp::RefineLayer layer = step(tracker, frame);
		if (covers(layer.tiles, 2, 0)) {
			sent++;
		}
	}
	CHECK(sent == 0);
	CHECK(tracker.has_pending_work());
}

// A still tile settles beside a playing video that the damage doesn't
// cover. The video here is column 0, changing every frame; the window
// next to it is column 3. (A window *over* the video stays lossy: mutter
// and KWin damage the video's whole rect, the covered part included --
// the accepted cost of damage as activity.)
void test_still_tile_beside_video_settles() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);
	(void)tracker.take_stats(); // the first frame counts every tile as changed

	frame.paint_tile(3, 0, 0x42); // the terminal appears
	uint8_t value = 1;
	int sent_on = -1;
	for (int i = 1; i <= 30; i++) {
		frame.paint_tile(0, 0, value++);
		frame.paint_tile(0, 1, value++);
		gdp::RefineLayer layer = step(tracker, frame);
		CHECK(!covers(layer.tiles, 0, 0));
		if (covers(layer.tiles, 3, 0) && sent_on < 0) {
			sent_on = i;
		}
	}
	CHECK(sent_on == 4); // the change, then the 3-frame settle time

	// Now sent: the video carrying on neither clears nor re-sends it.
	for (int i = 0; i < 10; i++) {
		frame.paint_tile(0, 0, value++);
		gdp::RefineLayer layer = step(tracker, frame);
		CHECK(!covers(layer.clears, 3, 0));
		CHECK(!covers(layer.tiles, 3, 0));
	}
}

// Under a budget, settled tiles go out in 8x8-tile squares, not strips:
// on 16 px tiles this frame is 16 x 8 tiles, two blocks side by side, and
// a budget of 16 tiles a call sends the left block's top two rows first
// -- one 128 x 32 rect -- then its next two, never a strip reaching into
// the right block.
void test_budget_fills_in_blocks() {
	constexpr uint32_t kSmall = 16;
	wraith::TileTrackerConfig c = config();
	c.tile_size = kSmall;
	c.max_tile_bytes_per_frame = kSmall * kSmall * gdp::kRefineBytesPerPixel * 16;
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, c);
	Frame frame;

	gdp::RefineLayer first, second;
	for (int i = 0; i < 10 && first.tiles.empty(); i++) {
		first = step(tracker, frame);
	}
	second = step(tracker, frame);
	CHECK(first.tiles.size() == 1);
	CHECK(second.tiles.size() == 1);
	if (first.tiles.size() == 1 && second.tiles.size() == 1) {
		const gdp::RefineRect &a = first.tiles[0], &b = second.tiles[0];
		CHECK(a.x == 0 && a.y == 0 && a.width == 128 && a.height == 32);
		CHECK(b.x == 0 && b.y == 32 && b.width == 128 && b.height == 32);
	}
}

// Damage holds a tile whose bytes don't change: a video's skip-macroblock
// patch (column 2, changed once, then byte-identical) doesn't settle
// while the browser keeps repainting the video quad over it, however long
// its bytes hold still -- and settles settle_us (3 frames) after the
// repainting stops.
void test_damage_holds_still_patch() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);
	(void)tracker.take_stats();

	frame.paint_tile(2, 0, 0x42);
	uint8_t value = 1;
	for (int i = 0; i < 60; i++) {
		frame.paint_tile(1, 0, value++);
		gdp::RefineLayer layer = damaged_step(tracker, frame, {{1, 0}, {2, 0}});
		CHECK(!covers(layer.tiles, 2, 0));
	}
	CHECK(tracker.take_stats().held_by_damage > 0);

	int sent_on = -1;
	for (int i = 1; i <= 10; i++) {
		gdp::RefineLayer layer = pump_step(tracker, frame);
		if (covers(layer.tiles, 2, 0)) {
			sent_on = i;
			break;
		}
	}
	CHECK(sent_on == 3);
}

// Damage decides activity, never what changed: a sent tile that changes
// outside the damage (headless mutter misses some) is still cleared, and
// damage alone over an unchanged sent tile clears nothing.
void test_damage_never_decides_clears() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);

	frame.paint_tile(3, 1, 0x55); // changed, but the damage says (0, 0)
	gdp::RefineLayer layer = damaged_step(tracker, frame, {{0, 0}});
	CHECK(covers(layer.clears, 3, 1));
	CHECK(!covers(layer.clears, 0, 0));
}

// set_policy() takes effect on the next call without touching tile
// state: a tile still waiting on a long settle time goes out as soon as
// a shorter one applies (a session reclassified onto a faster profile).
void test_set_policy_applies_mid_stream() {
	wraith::TileTracker tracker;
	wraith::TileTrackerConfig slow = config();
	slow.settle_us = 100 * kFrameUs;
	tracker.reset(kWidth, kHeight, slow);
	Frame frame;
	settle_all(tracker, frame);
	frame.paint_tile(2, 0, 0x42);
	for (int i = 0; i < 10; i++) {
		CHECK(!covers(step(tracker, frame).tiles, 2, 0));
	}
	tracker.set_policy(config());
	CHECK(covers(step(tracker, frame).tiles, 2, 0));
}

// Stats::churned counts a tile cleared within a second of being sent,
// and not one that had been on screen longer.
void test_churn_is_counted() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame); // sent within the last ~130 ms
	(void)tracker.take_stats();

	frame.paint_tile(1, 0, 0x11);
	step(tracker, frame);
	CHECK(tracker.take_stats().churned == 1);

	settle_all(tracker, frame);
	g_now_us += 2000000;
	frame.paint_tile(1, 0, 0x22);
	step(tracker, frame);
	CHECK(tracker.take_stats().churned == 0);
}

// No backoff: a tile that is sent and then changed again right away
// settles at the base rate every time. A 16 px tile holds two
// characters, so this is ordinary typing, which a per-tile backoff would
// slow down.
void test_churn_does_not_slow_settling() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	settle_all(tracker, frame);
	uint8_t value = 1;
	for (int cycle = 0; cycle < 6; cycle++) {
		// Change (seen on step 1), wait for the send, change again on the
		// very next call.
		frame.paint_tile(3, 0, value++);
		int when = -1;
		for (int i = 1; i <= 8; i++) {
			gdp::RefineLayer layer = step(tracker, frame);
			if (covers(layer.tiles, 3, 0)) {
				when = i;
				break;
			}
		}
		CHECK(when == 4);
	}
}

// has_pending_work() is what tells SessionServices' idle pump whether to
// keep re-poking the tracker with the same frame or go quiet -- it must
// be true while anything is still converging and false once nothing is.
void test_has_pending_work_tracks_convergence() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;

	// Frames 1-3 (the reset, then two quiet frames): the settle time (3
	// frames) hasn't elapsed yet, so nothing has been sent.
	for (int i = 0; i < 3; i++) {
		step(tracker, frame);
		CHECK(tracker.has_pending_work());
	}

	// Frame 4: the settle time elapses and everything goes out
	// in one batch (kTileCount is small enough to fit the default budget)
	// -- nothing left pending.
	step(tracker, frame);
	CHECK(!tracker.has_pending_work());
	settle_all(tracker, frame);

	// A region changes: it is unsent again, so there is pending work,
	// right up until it settles once more (same 3-quiet-then-settle
	// cadence, starting from the frame that changed it).
	frame.paint_tile(2, 1, 0x77);
	for (int i = 0; i < 3; i++) {
		step(tracker, frame);
		CHECK(tracker.has_pending_work());
	}
	step(tracker, frame);
	CHECK(!tracker.has_pending_work());
}

// The scenario this test suite exists to catch: a tracker fed the *same*
// unchanged frame repeatedly (what SessionServices' idle pump does when
// the compositor itself produces no more real frames -- see
// session_services.cpp's service_idle_pump()) must still converge and
// then stop reporting pending work, exactly as if those pushes had been
// organic.
void test_settles_under_repeated_identical_pushes() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	frame.paint_tile(3, 1, 0x99);

	for (int i = 0; i < 10 && tracker.has_pending_work(); i++) {
		step(tracker, frame);
	}
	CHECK(!tracker.has_pending_work());
}

void test_mismatched_size_is_inert() {
	wraith::TileTracker tracker;
	tracker.reset(kWidth, kHeight, config());
	Frame frame;
	gdp::RefineLayer layer =
		tracker.process(frame.pixels.data(), kWidth / 2, kHeight, kStride, kFrameUs, false);
	CHECK(layer.empty());
}

} // namespace

// A frame tall enough that hashing runs in bands on BandPool's threads
// (1080 px of 16 px tiles: 68 rows). One tile changed in the top, middle
// and bottom of the frame -- so in different bands -- must be held back,
// and only those: everything else settled and goes out.
void test_banded_hashing_matches_every_tile() {
	constexpr uint32_t kW = 1920, kH = 1080, kT = 16, kS = kW * 4;
	wraith::TileTracker tracker;
	wraith::TileTrackerConfig c;
	c.tile_size = kT;
	c.settle_us = kSettleUs;
	tracker.reset(kW, kH, c);
	std::vector<uint8_t> pixels((size_t)kS * kH, 0x30);
	int64_t now = 1'000'000;
	CHECK(tracker.process(pixels.data(), kW, kH, kS, now, false, nullptr).reset);

	const uint32_t cols = kW / kT, rows = (kH + kT - 1) / kT;
	const std::pair<uint32_t, uint32_t> changed[] = {{5, 0}, {60, rows / 2}, {100, rows - 1}};
	now += kSettleUs / 2;
	for (auto [col, row] : changed) {
		uint8_t *p = pixels.data() + (size_t)row * kT * kS + (size_t)col * kT * 4;
		p[0] ^= 0xff; // one byte of the tile's first pixel
	}
	tracker.process(pixels.data(), kW, kH, kS, now, false, nullptr);

	// Past the settle time of the untouched tiles, not of the changed ones;
	// a frame carries at most 1024 tiles, so drain over several calls.
	now += kSettleUs / 2 + kSettleUs / 4;
	std::vector<uint8_t> sent((size_t)cols * rows, 0);
	for (int call = 0; call < 16; call++) {
		gdp::RefineLayer layer = tracker.process(pixels.data(), kW, kH, kS, now + call, false, nullptr);
		for (const gdp::RefineRect &r : layer.tiles) {
			for (uint32_t y = r.y; y < r.y + r.height; y += kT) {
				for (uint32_t x = r.x; x < r.x + r.width; x += kT) {
					sent[(y / kT) * cols + x / kT] = 1;
				}
			}
		}
	}
	size_t total = 0;
	for (uint8_t v : sent) {
		total += v;
	}
	CHECK(total == (size_t)cols * rows - 3);
	for (auto [col, row] : changed) {
		CHECK(!sent[row * cols + col]);
	}
}

int main() {
	test_banded_hashing_matches_every_tile();
	test_settles_then_goes_quiet();
	test_moving_region_clears_then_resettles();
	test_reset_resends_everything();
	test_byte_budget_spreads_tiles();
	test_budget_sweeps_in_grid_order();
	test_budget_fills_in_blocks();
	test_every_real_frame_is_hashed();
	test_byte_budget_caps_one_call();
	test_pump_advances_settle();
	test_repair_rearms_lost_tiles();
	test_emission_records_clears_and_reset();
	test_repair_reissues_lost_clear();
	test_repair_reclears_a_tile_resent_since();
	test_repair_reclears_a_tile_changed_since();
	test_repair_rides_the_pump();
	test_reset_drops_owed_clears();
	test_clears_coalesce();
	test_many_scattered_clears_become_a_bounding_box();
	test_tiles_coalesce();
	test_video_holds_never_settle();
	test_still_tile_beside_video_settles();
	test_damage_holds_still_patch();
	test_damage_never_decides_clears();
	test_set_policy_applies_mid_stream();
	test_churn_is_counted();
	test_churn_does_not_slow_settling();
	test_has_pending_work_tracks_convergence();
	test_settles_under_repeated_identical_pushes();
	test_mismatched_size_is_inert();
	if (g_failures > 0) {
		fprintf(stderr, "tile_tracker_test: %d check(s) failed\n", g_failures);
		return 1;
	}
	printf("tile_tracker_test: ok\n");
	return 0;
}
