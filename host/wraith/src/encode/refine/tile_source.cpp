// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/refine/tile_source.hpp"

#include "util/band_pool.hpp"

#include <algorithm>
#include <cstring>

namespace wraith {

// FNV-1a over a tile's rows, four interleaved lanes. Not cryptographic and
// not meant to be: a collision costs one stale lossless tile until that
// region changes again, and at 64 bits over a 16x16 tile that is not a
// failure mode worth paying a real hash function's per-frame cost to
// avoid. This is the hot loop -- it runs over every pixel of every
// real frame -- and a single FNV chain is bound by the latency of its
// multiply (~3.2 ms for a full 4K frame, measured); four independent
// chains over consecutive words let the multiplies overlap and bring that
// to ~1.1 ms. The lanes are folded together at the end. Rows are 4-byte
// aligned by construction (row start is a 4-byte multiple, the stride
// from the compositor is a multiple of 4) and tiles are 16 px wide except
// at the right/bottom edge -- a 64-byte row, two turns of the four-lane
// loop -- so the scalar tail below is rarely taken.
//
// screencast/dmabuf_tile_source.cpp's compute shader is a port of exactly
// this, tail included; change one, change both (tests/gpu_tile_hash_test.cpp
// holds them to it).
uint64_t hash_tile(const uint8_t *data, uint32_t stride, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	constexpr uint64_t kPrime = 1099511628211ull;
	uint64_t h0 = 1469598103934665603ull;
	uint64_t h1 = h0 ^ 0x1111111111111111ull;
	uint64_t h2 = h0 ^ 0x2222222222222222ull;
	uint64_t h3 = h0 ^ 0x3333333333333333ull;
	for (uint32_t row = 0; row < h; row++) {
		const uint8_t *p = data + (size_t)(y + row) * stride + (size_t)x * 4;
		size_t bytes = (size_t)w * 4;
		size_t words = bytes / 8;
		size_t i = 0;
		for (; i + 4 <= words; i += 4) {
			uint64_t c0, c1, c2, c3;
			memcpy(&c0, p + i * 8, 8);
			memcpy(&c1, p + i * 8 + 8, 8);
			memcpy(&c2, p + i * 8 + 16, 8);
			memcpy(&c3, p + i * 8 + 24, 8);
			h0 = (h0 ^ c0) * kPrime;
			h1 = (h1 ^ c1) * kPrime;
			h2 = (h2 ^ c2) * kPrime;
			h3 = (h3 ^ c3) * kPrime;
		}
		for (; i < words; i++) {
			uint64_t c;
			memcpy(&c, p + i * 8, 8);
			h0 = (h0 ^ c) * kPrime;
		}
		for (size_t b = words * 8; b < bytes; b++) {
			h0 = (h0 ^ p[b]) * kPrime;
		}
	}
	uint64_t hash = h0;
	hash = (hash ^ h1) * kPrime;
	hash = (hash ^ h2) * kPrime;
	hash = (hash ^ h3) * kPrime;
	return hash;
}

bool CpuTileSource::hash_tiles(uint32_t tile_size, uint64_t *out) {
	const uint32_t cols = (width_ + tile_size - 1) / tile_size;
	const uint32_t rows = (height_ + tile_size - 1) / tile_size;
	auto hash_rows = [&](uint32_t row0, uint32_t row1) {
		for (uint32_t row = row0; row < row1; row++) {
			for (uint32_t col = 0; col < cols; col++) {
				uint32_t x = col * tile_size;
				uint32_t y = row * tile_size;
				uint32_t w = std::min(tile_size, width_ - x);
				uint32_t h = std::min(tile_size, height_ - y);
				out[row * cols + col] = hash_tile(data_, stride_, x, y, w, h);
			}
		}
	};
	// Below this many tile rows (720 px at 16 px tiles), one thread is
	// quicker than waking the helpers.
	constexpr uint32_t kBandMinRows = 45;
	BandPool &pool = BandPool::get();
	uint32_t bands = (uint32_t)pool.bands();
	if (rows < kBandMinRows || bands == 1) {
		hash_rows(0, rows);
		return true;
	}
	uint32_t per_band = (rows + bands - 1) / bands;
	pool.run([&](size_t i) {
		uint32_t row0 = (uint32_t)i * per_band;
		hash_rows(std::min(row0, rows), std::min(row0 + per_band, rows));
	});
	return true;
}

bool CpuTileSource::append_rects(const std::vector<gdp::RefineRect> &rects, std::vector<uint8_t> *out) {
	for (const gdp::RefineRect &rect : rects) {
		size_t row_bytes = (size_t)rect.width * gdp::kRefineBytesPerPixel;
		size_t base = out->size();
		out->resize(base + row_bytes * rect.height);
		for (uint32_t row = 0; row < rect.height; row++) {
			gdp::refine_pack_row(data_ + (size_t)(rect.y + row) * stride_ + (size_t)rect.x * 4,
				out->data() + base + row * row_bytes, rect.width);
		}
	}
	return true;
}

} // namespace wraith
