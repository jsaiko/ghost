// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Lossless refinement container round-trip and malformed-input coverage
// (gdp/refine.hpp, gdp-spec.md §9.5). The parse side
// reads bytes straight off the wire, so the rejection cases matter as much
// as the round trip.
#include "gdp/refine.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

gdp::RefineRect rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
	gdp::RefineRect r;
	r.x = x;
	r.y = y;
	r.width = w;
	r.height = h;
	return r;
}

// Fills `layer.tile_pixels` with a deterministic pattern sized to its
// tiles, so a round trip that silently truncates or reorders is caught.
void fill_tile_pixels(gdp::RefineLayer *layer) {
	layer->tile_pixels.resize(gdp::refine_layer_pixel_bytes(layer->tiles));
	for (size_t i = 0; i < layer->tile_pixels.size(); i++) {
		layer->tile_pixels[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
	}
}

void test_roundtrip_with_tiles() {
	const uint8_t base[] = {0, 0, 0, 1, 0x65, 0xde, 0xad, 0xbe, 0xef};

	gdp::RefineLayer layer;
	layer.reset = true;
	layer.clears.push_back(rect(0, 0, 64, 64));
	layer.clears.push_back(rect(128, 64, 64, 64));
	layer.tiles.push_back(rect(64, 0, 64, 64));
	layer.tiles.push_back(rect(192, 256, 32, 16));
	fill_tile_pixels(&layer);

	std::vector<uint8_t> packed;
	assert(gdp::refine_pack_frame(base, sizeof(base), layer, 1, &packed));

	const uint8_t *base_out = nullptr;
	size_t base_len_out = 0;
	gdp::RefineLayer parsed;
	assert(gdp::refine_parse_frame(packed.data(), packed.size(), &base_out, &base_len_out, &parsed));

	assert(base_len_out == sizeof(base));
	assert(memcmp(base_out, base, sizeof(base)) == 0);
	assert(parsed.reset);
	assert(parsed.clears.size() == 2);
	assert(parsed.clears[1].x == 128 && parsed.clears[1].y == 64);
	assert(parsed.tiles.size() == 2);
	assert(parsed.tiles[1].width == 32 && parsed.tiles[1].height == 16);
	assert(parsed.tile_pixels == layer.tile_pixels);

	// The whole point of the layer: 64x64 of a repeating pattern must not
	// cost anything like its raw size on the wire.
	assert(packed.size() < sizeof(base) + layer.tile_pixels.size());
}

void test_roundtrip_base_only() {
	const uint8_t base[] = {0, 0, 0, 1, 0x41, 0x11, 0x22};

	gdp::RefineLayer layer; // no reset, no clears, no tiles
	assert(layer.empty());

	std::vector<uint8_t> packed;
	assert(gdp::refine_pack_frame(base, sizeof(base), layer, 1, &packed));
	// Header plus the base bitstream and nothing else -- a refined session
	// with a quiet lossless layer costs RefineHeader::kSize bytes a frame
	// over the bare codec.
	assert(packed.size() == gdp::RefineHeader::kSize + sizeof(base));

	const uint8_t *base_out = nullptr;
	size_t base_len_out = 0;
	gdp::RefineLayer parsed;
	assert(gdp::refine_parse_frame(packed.data(), packed.size(), &base_out, &base_len_out, &parsed));
	assert(base_len_out == sizeof(base));
	assert(memcmp(base_out, base, sizeof(base)) == 0);
	assert(parsed.empty());
}

// A layer-only frame (gdp-spec.md §9.5): no base bytes at all, just tiles
// for the picture the client already has. wraith's idle pump sends these
// so a settling desktop costs no video encode or decode.
void test_roundtrip_layer_only() {
	gdp::RefineLayer layer;
	gdp::RefineRect tile;
	tile.x = 64;
	tile.y = 0;
	tile.width = 2;
	tile.height = 2;
	layer.tiles.push_back(tile);
	layer.tile_pixels.assign(2 * 2 * gdp::kRefineBytesPerPixel, 0xab);

	std::vector<uint8_t> packed;
	assert(gdp::refine_pack_frame(nullptr, 0, layer, 1, &packed));

	const uint8_t *base_out = reinterpret_cast<const uint8_t *>(1);
	size_t base_len_out = 99;
	gdp::RefineLayer parsed;
	assert(gdp::refine_parse_frame(packed.data(), packed.size(), &base_out, &base_len_out, &parsed));
	assert(base_len_out == 0);
	assert(!parsed.reset);
	assert(parsed.clears.empty());
	assert(parsed.tiles.size() == 1);
	assert(parsed.tile_pixels == layer.tile_pixels);
}

// A regression guard for a header whose declared size disagrees with the
// fields in it: the tail field then lands on top of the first base bytes,
// which a base starting with H.264's 00 00 00 01 start code hides
// completely. Both a large base and one whose leading bytes are non-zero,
// so an overlap shows up as a mismatch rather than as luck.
void test_header_does_not_overlap_base() {
	std::vector<uint8_t> base(140000);
	for (size_t i = 0; i < base.size(); i++) {
		base[i] = static_cast<uint8_t>(i * 31 + 7);
	}

	gdp::RefineLayer layer;
	for (int i = 0; i < 40; i++) {
		layer.tiles.push_back(
			rect(static_cast<uint16_t>((i % 10) * 64), static_cast<uint16_t>((i / 10) * 64), 64, 64));
	}
	fill_tile_pixels(&layer);

	std::vector<uint8_t> packed;
	assert(gdp::refine_pack_frame(base.data(), base.size(), layer, 1, &packed));

	const uint8_t *base_out = nullptr;
	size_t base_len_out = 0;
	gdp::RefineLayer parsed;
	assert(gdp::refine_parse_frame(packed.data(), packed.size(), &base_out, &base_len_out, &parsed));
	assert(base_len_out == base.size());
	assert(base_out != nullptr && memcmp(base_out, base.data(), base.size()) == 0);
	assert(parsed.tiles.size() == layer.tiles.size());
	assert(parsed.tile_pixels == layer.tile_pixels);
}

void test_pack_rejects_mismatched_pixels() {
	const uint8_t base[] = {0, 0, 0, 1, 0x65};
	gdp::RefineLayer layer;
	layer.tiles.push_back(rect(0, 0, 8, 8));
	layer.tile_pixels.resize(8 * 8 * gdp::kRefineBytesPerPixel - 1); // one byte short

	std::vector<uint8_t> packed;
	assert(!gdp::refine_pack_frame(base, sizeof(base), layer, 1, &packed));
}

void test_parse_rejects_malformed() {
	const uint8_t base[] = {0, 0, 0, 1, 0x65, 0x01, 0x02, 0x03};
	gdp::RefineLayer layer;
	layer.clears.push_back(rect(0, 0, 16, 16));
	layer.tiles.push_back(rect(16, 16, 16, 16));
	fill_tile_pixels(&layer);

	std::vector<uint8_t> packed;
	assert(gdp::refine_pack_frame(base, sizeof(base), layer, 1, &packed));

	const uint8_t *base_out = nullptr;
	size_t base_len_out = 0;
	gdp::RefineLayer parsed;

	assert(!gdp::refine_parse_frame(packed.data(), 0, &base_out, &base_len_out, &parsed));
	assert(!gdp::refine_parse_frame(packed.data(), gdp::RefineHeader::kSize - 1, &base_out, &base_len_out,
		&parsed));

	// Every truncation of a valid payload must be rejected, not
	// half-applied.
	for (size_t len = gdp::RefineHeader::kSize; len < packed.size(); len++) {
		assert(!gdp::refine_parse_frame(packed.data(), len, &base_out, &base_len_out, &parsed));
	}

	std::vector<uint8_t> bad = packed;
	bad[0] = 0x00; // wrong magic
	assert(!gdp::refine_parse_frame(bad.data(), bad.size(), &base_out, &base_len_out, &parsed));

	bad = packed;
	bad[1] = gdp::RefineHeader::kVersion + 1; // a version this build doesn't know
	assert(!gdp::refine_parse_frame(bad.data(), bad.size(), &base_out, &base_len_out, &parsed));

	bad = packed;
	bad[2] |= 1u << 1; // a reserved flag bit
	assert(!gdp::refine_parse_frame(bad.data(), bad.size(), &base_out, &base_len_out, &parsed));

	bad = packed;
	bad[3] = gdp::kRefineFormatBgr8 + 1; // a tile format this build doesn't declare
	assert(!gdp::refine_parse_frame(bad.data(), bad.size(), &base_out, &base_len_out, &parsed));

	bad = packed;
	bad[5] = 1; // the reserved byte
	assert(!gdp::refine_parse_frame(bad.data(), bad.size(), &base_out, &base_len_out, &parsed));

	// Tile rects claiming more pixels than any frame may carry are dropped
	// before anything that size is allocated.
	bad = packed;
	size_t tile_rect = gdp::RefineHeader::kSize + sizeof(base) + gdp::RefineHeader::kRectSize;
	for (size_t i = 4; i < 8; i++) {
		bad[tile_rect + i] = 0xff; // width and height 65535
	}
	assert(!gdp::refine_parse_frame(bad.data(), bad.size(), &base_out, &base_len_out, &parsed));

	// A base_len larger than the payload must not be trusted as an offset.
	bad = packed;
	bad[8] = 0xff;
	bad[9] = 0xff;
	bad[10] = 0xff;
	bad[11] = 0x7f;
	assert(!gdp::refine_parse_frame(bad.data(), bad.size(), &base_out, &base_len_out, &parsed));

	// Corrupting the Zstd blob must fail the frame rather than yield
	// partially-decompressed pixels.
	bad = packed;
	bad[bad.size() - 1] ^= 0xff;
	bad[bad.size() - 2] ^= 0xff;
	if (gdp::refine_parse_frame(bad.data(), bad.size(), &base_out, &base_len_out, &parsed)) {
		// Zstd's frame checksum is optional, so a flipped byte may still
		// decode -- what must never happen is a short/over-long result.
		assert(parsed.tile_pixels.size() == gdp::refine_layer_pixel_bytes(parsed.tiles));
	}
}

} // namespace

// The row helpers against a byte-by-byte reference, at widths that cover
// the four-pixel steps, the scalar tail and both together. Pack drops the
// X byte whatever it holds; unpack writes 0xff there and touches nothing
// past `width` pixels.
void test_pack_unpack_rows() {
	for (uint32_t width = 0; width <= 9; width++) {
		std::vector<uint8_t> xrgb(width * 4);
		for (size_t i = 0; i < xrgb.size(); i++) {
			xrgb[i] = static_cast<uint8_t>(i * 37 + 11);
		}
		std::vector<uint8_t> packed(width * 3 + 1, 0xee);
		gdp::refine_pack_row(xrgb.data(), packed.data(), width);
		for (uint32_t x = 0; x < width; x++) {
			for (int c = 0; c < 3; c++) {
				assert(packed[x * 3 + c] == xrgb[x * 4 + c]);
			}
		}
		assert(packed[width * 3] == 0xee);

		std::vector<uint8_t> back(width * 4 + 4, 0xee);
		gdp::refine_unpack_row(packed.data(), back.data(), width);
		for (uint32_t x = 0; x < width; x++) {
			for (int c = 0; c < 3; c++) {
				assert(back[x * 4 + c] == xrgb[x * 4 + c]);
			}
			assert(back[x * 4 + 3] == 0xff);
		}
		for (size_t i = width * 4; i < back.size(); i++) {
			assert(back[i] == 0xee);
		}
	}
}

int main() {
	test_pack_unpack_rows();
	test_roundtrip_with_tiles();
	test_roundtrip_base_only();
	test_roundtrip_layer_only();
	test_header_does_not_overlap_base();
	test_pack_rejects_mismatched_pixels();
	test_parse_rejects_malformed();
	printf("refine_test: all tests passed\n");
	return 0;
}
