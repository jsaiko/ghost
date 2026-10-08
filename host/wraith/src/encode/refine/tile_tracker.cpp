// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/refine/tile_tracker.hpp"

#include <algorithm>
#include <cstring>

namespace wraith {

void TileTracker::reset(uint32_t width, uint32_t height, const TileTrackerConfig &config) {
	config_ = config;
	if (config_.tile_size == 0) {
		config_.tile_size = TileTrackerConfig{}.tile_size;
	}
	width_ = width;
	height_ = height;
	cols_ = (width + config_.tile_size - 1) / config_.tile_size;
	rows_ = (height + config_.tile_size - 1) / config_.tile_size;
	tiles_.assign((size_t)cols_ * rows_, Tile{});
	dirty_.assign(tiles_.size(), 0);
	due_.assign(tiles_.size(), 0);
	repair_clear_.assign(tiles_.size(), 0);
	pending_clears_ = 0;
	unsent_ = tiles_.size();
	sweep_start_ = 0;
	first_frame_ = true;
}

void TileTracker::set_policy(const TileTrackerConfig &config) {
	config_.settle_us = config.settle_us;
}

void TileTracker::set_sent(Tile &tile, bool sent) {
	if (tile.sent == sent) {
		return;
	}
	tile.sent = sent;
	if (sent) {
		unsent_--;
	} else {
		unsent_++;
	}
}

void TileTracker::mark_dirty(const DamageRegion *damage) {
	std::fill(dirty_.begin(), dirty_.end(), 0);
	if (!damage) {
		return;
	}
	for (const DamageRect &rect : damage->rects) {
		if (rect.width <= 0 || rect.height <= 0) {
			continue;
		}
		// Clip to the frame; a rect entirely outside it marks nothing.
		int64_t x0 = std::max<int64_t>(rect.x, 0);
		int64_t y0 = std::max<int64_t>(rect.y, 0);
		int64_t x1 = std::min<int64_t>((int64_t)rect.x + rect.width, width_);
		int64_t y1 = std::min<int64_t>((int64_t)rect.y + rect.height, height_);
		if (x1 <= x0 || y1 <= y0) {
			continue;
		}
		uint32_t c0 = (uint32_t)(x0 / config_.tile_size);
		uint32_t c1 = (uint32_t)((x1 - 1) / config_.tile_size);
		uint32_t r0 = (uint32_t)(y0 / config_.tile_size);
		uint32_t r1 = (uint32_t)((y1 - 1) / config_.tile_size);
		for (uint32_t row = r0; row <= r1; row++) {
			memset(dirty_.data() + (size_t)row * cols_ + c0, 1, c1 - c0 + 1);
		}
	}
}

void TileTracker::repair(const TileEmission &emission) {
	for (const auto &[index, hash] : emission.tiles) {
		if (index >= tiles_.size()) {
			continue;
		}
		Tile &tile = tiles_[index];
		if (tile.sent && tile.hash == hash) {
			set_sent(tile, false);
		}
	}
	// Clear rects are tile-aligned (coalesce() builds them from the grid),
	// so each maps back onto whole tiles; the one clipped at the right or
	// bottom edge still starts on a tile boundary.
	const uint32_t ts = config_.tile_size;
	for (const gdp::RefineRect &rect : emission.clears) {
		if (rect.width == 0 || rect.height == 0 || rect.x >= width_ || rect.y >= height_) {
			continue;
		}
		uint32_t c0 = rect.x / ts;
		uint32_t r0 = rect.y / ts;
		uint32_t c1 = std::min<uint32_t>((rect.x + rect.width - 1) / ts, cols_ - 1);
		uint32_t r1 = std::min<uint32_t>((rect.y + rect.height - 1) / ts, rows_ - 1);
		for (uint32_t row = r0; row <= r1; row++) {
			for (uint32_t col = c0; col <= c1; col++) {
				uint32_t index = row * cols_ + col;
				set_sent(tiles_[index], false);
				if (!repair_clear_[index]) {
					repair_clear_[index] = 1;
					pending_clears_++;
				}
			}
		}
	}
}

// Merges ascending grid indices into rects: horizontally adjacent tiles
// in a row become one run, and runs in consecutive rows with the same
// column span are stacked into one taller rect. A terminal repaint or a
// window's worth of change -- thousands of 16px tiles -- comes out as a
// handful of rects.
void TileTracker::coalesce(const std::vector<uint32_t> &indices, std::vector<gdp::RefineRect> *out) const {
	struct Run {
		uint32_t row;
		uint32_t rows; // stacked height in tiles
		uint32_t c0;
		uint32_t c1; // inclusive column span
	};
	// Horizontal runs, in grid order.
	std::vector<Run> runs;
	for (uint32_t index : indices) {
		uint32_t row = index / cols_;
		uint32_t col = index % cols_;
		if (!runs.empty() && runs.back().row == row && runs.back().c1 + 1 == col) {
			runs.back().c1 = col;
		} else {
			runs.push_back(Run{row, 1, col, col});
		}
	}
	// Vertical stacking: a run with the same span as one whose bottom is
	// the row above extends it. `prev` holds the rects whose bottom row
	// is the previous row; `cur` collects this row's.
	std::vector<Run> stacked;
	std::vector<size_t> prev, cur;
	uint32_t cur_row = UINT32_MAX;
	for (const Run &run : runs) {
		if (run.row != cur_row) {
			prev = (run.row == cur_row + 1) ? std::move(cur) : std::vector<size_t>{};
			cur.clear();
			cur_row = run.row;
		}
		bool merged = false;
		for (size_t i : prev) {
			Run &above = stacked[i];
			if (above.c0 == run.c0 && above.c1 == run.c1) {
				above.rows++;
				cur.push_back(i);
				merged = true;
				break;
			}
		}
		if (!merged) {
			stacked.push_back(run);
			cur.push_back(stacked.size() - 1);
		}
	}

	for (const Run &run : stacked) {
		gdp::RefineRect rect;
		uint32_t x = run.c0 * config_.tile_size;
		uint32_t y = run.row * config_.tile_size;
		rect.x = (uint16_t)x;
		rect.y = (uint16_t)y;
		rect.width = (uint16_t)(std::min((run.c1 + 1) * config_.tile_size, width_) - x);
		rect.height = (uint16_t)(std::min((run.row + run.rows) * config_.tile_size, height_) - y);
		out->push_back(rect);
	}
}

// Turns the changed-and-sent tiles of this frame (clear_tiles_, in grid
// order) into as few clear rects as possible, well inside the container's
// u8 clear_count for anything that coalesces.
//
// If that still overflows (change scattered so finely that nothing
// coalesces), the layer carries one clear over the bounding box of all of
// them instead. Unchanged lossless tiles inside that box are wiped on the
// client along with the rest, so they are re-armed here and re-sent once
// the budget allows: bytes for those tiles, far less than a plane reset
// repainting the whole screen.
void TileTracker::build_clears(gdp::RefineLayer *layer) {
	std::vector<gdp::RefineRect> rects;
	coalesce(clear_tiles_, &rects);
	if (rects.size() <= 255) {
		layer->clears = std::move(rects);
		return;
	}

	uint32_t r0 = UINT32_MAX, r1 = 0, c0 = UINT32_MAX, c1 = 0;
	for (uint32_t index : clear_tiles_) {
		uint32_t row = index / cols_;
		uint32_t col = index % cols_;
		r0 = std::min(r0, row);
		r1 = std::max(r1, row);
		c0 = std::min(c0, col);
		c1 = std::max(c1, col);
	}
	std::vector<uint32_t> box;
	box.reserve((size_t)(r1 - r0 + 1) * (c1 - c0 + 1));
	for (uint32_t row = r0; row <= r1; row++) {
		for (uint32_t col = c0; col <= c1; col++) {
			box.push_back(row * cols_ + col);
			set_sent(tiles_[row * cols_ + col], false);
		}
	}
	coalesce(box, &layer->clears); // one rect: the box is a full block
}

bool TileTracker::has_pending_work() const {
	return unsent_ > 0 || pending_clears_ > 0;
}

gdp::RefineLayer TileTracker::process(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride,
	int64_t now_us, bool force_reset, const DamageRegion *damage, TileEmission *emission) {
	CpuTileSource source(data, width, height, stride);
	return process_frame(source, now_us, force_reset, damage, false, emission);
}

gdp::RefineLayer TileTracker::pump(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride,
	int64_t now_us, TileEmission *emission) {
	CpuTileSource source(data, width, height, stride);
	return process_frame(source, now_us, false, nullptr, true, emission);
}

gdp::RefineLayer TileTracker::process(TileSource &source, int64_t now_us, bool force_reset,
	const DamageRegion *damage, TileEmission *emission) {
	return process_frame(source, now_us, force_reset, damage, false, emission);
}

gdp::RefineLayer TileTracker::pump(TileSource &source, int64_t now_us, TileEmission *emission) {
	return process_frame(source, now_us, false, nullptr, true, emission);
}

gdp::RefineLayer TileTracker::process_frame(TileSource &source, int64_t now_us, bool force_reset,
	const DamageRegion *damage, bool same_pixels, TileEmission *emission) {
	gdp::RefineLayer layer;
	if (emission) {
		*emission = TileEmission{};
	}
	if (source.width() != width_ || source.height() != height_ || tiles_.empty()) {
		// A frame that doesn't match what reset() was told about: send no
		// lossless layer at all rather than index out of the grid. The
		// encoder re-opens on resize, so this is a transient at most.
		return layer;
	}

	// The hashing comes first, into new_hash_ -- banded across threads for
	// a frame in host memory, a compute shader for one on the GPU -- before
	// anything here changes: a source that can't read the frame leaves the
	// tracker exactly as it was, and the caller tries the frame another way
	// (TileSource::failed()). The pump's pixels are the last call's, so it
	// hashes nothing.
	if (!same_pixels) {
		new_hash_.resize(tiles_.size());
		if (!source.hash_tiles(config_.tile_size, new_hash_.data())) {
			return layer;
		}
	}

	// A reset wipes the client's overlay plane, so every tile is unsent
	// from here on regardless of what it was before. No clears are needed
	// alongside -- reset covers the whole plane.
	if (force_reset || first_frame_) {
		layer.reset = true;
		for (Tile &tile : tiles_) {
			set_sent(tile, false);
			if (first_frame_) {
				tile.changed_us = now_us;
			}
		}
		first_frame_ = false;
		if (pending_clears_ > 0) {
			std::fill(repair_clear_.begin(), repair_clear_.end(), 0);
			pending_clears_ = 0;
		}
	}
	clear_tiles_.clear();
	// The pump's pixels (and so its damage) are the last call's: nothing
	// new was repainted, and the settle clock runs.
	mark_dirty(same_pixels ? nullptr : damage);

	// Pass 1: compare every tile's new hash (none on a pump call) with its
	// last and classify it. Tiles that just changed produce clears;
	// damaged tiles restart their settle time without one; tiles that have
	// been inactive long enough become candidates for a lossless refresh
	// (due_, then `candidates` below).
	std::fill(due_.begin(), due_.end(), 0);
	size_t due_count = 0;
	for (uint32_t row = 0; row < rows_; row++) {
		for (uint32_t col = 0; col < cols_; col++) {
			uint32_t index = row * cols_ + col;
			Tile &tile = tiles_[index];
			// A clear repair() owes the client, from a lost layer. The
			// tile itself was re-armed then, so it goes on through the
			// classification below like any unsent tile -- and may be
			// sent again in this same layer, which the client applies
			// after the clears.
			bool reclear = false;
			if (pending_clears_ > 0 && repair_clear_[index]) {
				repair_clear_[index] = 0;
				pending_clears_--;
				reclear = true;
			}
			if (!same_pixels) {
				uint64_t hash = new_hash_[index];
				if (hash != tile.hash) {
					tile.hash = hash;
					tile.changed_us = now_us;
					stats_.held_by_change++;
					if (tile.sent && !layer.reset) {
						// The client is showing a lossless copy of what
						// this tile used to be; tell it to drop back to
						// the video until the tile settles again.
						clear_tiles_.push_back(index);
						if (now_us - tile.sent_us < kChurnWindowUs) {
							stats_.churned++;
						}
					} else if (reclear) {
						clear_tiles_.push_back(index);
					}
					set_sent(tile, false);
					continue;
				}
			}
			if (reclear) {
				clear_tiles_.push_back(index);
			}

			if (tile.sent) {
				continue;
			}
			if (dirty_[index]) {
				// Repainted with the same bytes: a video's skip patch.
				// Active all the same.
				tile.changed_us = now_us;
				stats_.held_by_damage++;
				continue;
			}
			if (now_us - tile.changed_us >= config_.settle_us) {
				due_[index] = 1;
				due_count++;
			}
		}
	}

	if (!clear_tiles_.empty()) {
		build_clears(&layer);
	}
	if (emission) {
		emission->reset = layer.reset;
		emission->clears = layer.clears;
	}

	const size_t budget = byte_budget_;
	byte_budget_ = SIZE_MAX; // one call only
	if (due_count == 0) {
		return layer;
	}

	// The due tiles in *block order*: kSweepBlock x kSweepBlock squares
	// of tiles (128 px), left to right and top to bottom, and row-major
	// inside each. A budget that admits a few hundred tiles a call then
	// fills a settling window in square blocks; in plain grid order it
	// filled it a one-tile strip per call, all the way across. Each entry
	// is (block-order key, grid index), keys ascending.
	std::vector<std::pair<uint32_t, uint32_t>> candidates;
	candidates.reserve(due_count);
	const uint32_t block_cols = (cols_ + kSweepBlock - 1) / kSweepBlock;
	for (uint32_t br = 0; br * kSweepBlock < rows_; br++) {
		for (uint32_t bc = 0; bc < block_cols; bc++) {
			uint32_t block = br * block_cols + bc;
			uint32_t r1 = std::min((br + 1) * kSweepBlock, rows_);
			uint32_t c1 = std::min((bc + 1) * kSweepBlock, cols_);
			for (uint32_t row = br * kSweepBlock; row < r1; row++) {
				for (uint32_t col = bc * kSweepBlock; col < c1; col++) {
					uint32_t index = row * cols_ + col;
					if (due_[index]) {
						uint32_t key = block * kSweepBlock * kSweepBlock + (row % kSweepBlock) * kSweepBlock +
							col % kSweepBlock;
						candidates.emplace_back(key, index);
					}
				}
			}
		}
	}

	// Pass 2: admit as many settled tiles as this frame's byte budget
	// allows, resuming at the block-order position the last frame stopped
	// at. lower_bound finds the first tile at or after it -- or the end,
	// meaning everything left is behind it and the sweep wraps to the top.
	size_t tile_bytes = 0;
	emit_tiles_.clear();
	size_t start = (size_t)(std::lower_bound(candidates.begin(), candidates.end(),
								std::make_pair(sweep_start_, uint32_t{0})) -
		candidates.begin());
	if (start >= candidates.size()) {
		start = 0;
	}
	for (size_t n = 0; n < candidates.size(); n++) {
		auto [key, index] = candidates[(start + n) % candidates.size()];
		uint32_t col = index % cols_;
		uint32_t row = index / cols_;
		uint32_t w = std::min(config_.tile_size, width_ - col * config_.tile_size);
		uint32_t h = std::min(config_.tile_size, height_ - row * config_.tile_size);

		size_t bytes = (size_t)w * h * gdp::kRefineBytesPerPixel;
		if (tile_bytes + bytes > budget) {
			// Out of bandwidth: everything still due waits for a later
			// call, which is also what keeps the idle pump ticking.
			stats_.held_by_budget += candidates.size() - n;
			break;
		}
		if (tile_bytes + bytes > config_.max_tile_bytes_per_frame && !emit_tiles_.empty()) {
			break;
		}
		if (emit_tiles_.size() >= UINT16_MAX) {
			break;
		}

		emit_tiles_.push_back(index);
		set_sent(tiles_[index], true);
		tiles_[index].sent_us = now_us;
		stats_.tiles_sent++;
		if (emission) {
			emission->tiles.emplace_back(index, tiles_[index].hash);
		}
		tile_bytes += bytes;
		// One past the tile just sent. Past the last key is fine: next
		// frame's lower_bound then wraps to the start.
		sweep_start_ = key + 1;
	}

	// The admitted tiles are in block order; sorting puts them in grid
	// order for coalesce(), which stacks a block's equal-width rows back
	// into one square rect. The rects then carry the pixels,
	// each rect row-major and tightly packed, which also keeps horizontal
	// neighbours adjacent in the Zstd input rather than a tile apart.
	// A source that fails here has already had its tiles marked sent: the
	// caller sees failed() and gives the emission back to repair().
	std::sort(emit_tiles_.begin(), emit_tiles_.end());
	coalesce(emit_tiles_, &layer.tiles);
	source.append_rects(layer.tiles, &layer.tile_pixels);

	return layer;
}

} // namespace wraith
