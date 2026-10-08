// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/refine.hpp"

#include "le_bytes.hpp"

#include <zstd.h>

#include <cstring>

namespace gdp {

namespace {

using le::get_u16;
using le::get_u32;
using le::put_u16;
using le::put_u32;

void put_rects(uint8_t *out, const std::vector<RefineRect> &rects) {
	for (size_t i = 0; i < rects.size(); i++) {
		uint8_t *p = out + i * RefineHeader::kRectSize;
		put_u16(p + 0, rects[i].x);
		put_u16(p + 2, rects[i].y);
		put_u16(p + 4, rects[i].width);
		put_u16(p + 6, rects[i].height);
	}
}

void get_rects(const uint8_t *in, size_t count, std::vector<RefineRect> *out) {
	out->resize(count);
	for (size_t i = 0; i < count; i++) {
		const uint8_t *p = in + i * RefineHeader::kRectSize;
		(*out)[i].x = get_u16(p + 0);
		(*out)[i].y = get_u16(p + 2);
		(*out)[i].width = get_u16(p + 4);
		(*out)[i].height = get_u16(p + 6);
	}
}

} // namespace

// Both assume a little-endian host, as the rest of the tree does for
// XRGB8888 (B, G, R, X in memory): pixel n of a 64-bit word is bits
// 32n..32n+23, with its X byte above them.
void refine_pack_row(const uint8_t *xrgb, uint8_t *out, uint32_t width) {
	uint32_t x = 0;
	for (; x + 4 <= width; x += 4) {
		uint64_t a, b;
		memcpy(&a, xrgb + x * 4, 8);
		memcpy(&b, xrgb + x * 4 + 8, 8);
		uint64_t lo = (a & 0xffffffull) | ((a >> 8) & 0xffffff000000ull) | (b << 48);
		uint32_t hi = (uint32_t)((b >> 16) & 0xffull) | (uint32_t)((b >> 24) & 0xffffff00ull);
		memcpy(out + x * 3, &lo, 8);
		memcpy(out + x * 3 + 8, &hi, 4);
	}
	for (; x < width; x++) {
		memcpy(out + x * 3, xrgb + x * 4, 3);
	}
}

void refine_unpack_row(const uint8_t *in, uint8_t *xrgb, uint32_t width) {
	constexpr uint64_t kOpaque = 0xff000000ff000000ull;
	uint32_t x = 0;
	for (; x + 4 <= width; x += 4) {
		uint64_t lo;
		uint32_t hi;
		memcpy(&lo, in + x * 3, 8);
		memcpy(&hi, in + x * 3 + 8, 4);
		uint64_t a = (lo & 0xffffffull) | ((lo << 8) & 0xffffff00000000ull) | kOpaque;
		uint64_t b = ((lo >> 48) & 0xffffull) | ((uint64_t)(hi & 0xffu) << 16) |
			(((uint64_t)hi << 24) & 0xffffff00000000ull) | kOpaque;
		memcpy(xrgb + x * 4, &a, 8);
		memcpy(xrgb + x * 4 + 8, &b, 8);
	}
	for (; x < width; x++) {
		memcpy(xrgb + x * 4, in + x * 3, 3);
		xrgb[x * 4 + 3] = 0xff;
	}
}

size_t refine_layer_pixel_bytes(const std::vector<RefineRect> &tiles) {
	size_t total = 0;
	for (const RefineRect &tile : tiles) {
		total += static_cast<size_t>(tile.width) * tile.height * kRefineBytesPerPixel;
	}
	return total;
}

bool refine_pack_frame(const uint8_t *base, size_t base_len, const RefineLayer &layer, int zstd_level,
	std::vector<uint8_t> *out) {
	if (layer.format != kRefineFormatBgr8 || layer.tiles.size() > UINT16_MAX ||
		layer.clears.size() > UINT8_MAX) {
		return false;
	}
	if (layer.tile_pixels.size() != refine_layer_pixel_bytes(layer.tiles) ||
		layer.tile_pixels.size() > kMaxRefineLayerPixelBytes) {
		return false;
	}

	// One Zstd frame over all the tiles at once rather than one per tile:
	// desktop tiles repeat each other heavily (the same glyphs, the same
	// window chrome), and a single frame lets the compressor see across
	// tile boundaries. The client decompresses it in one call too.
	std::vector<uint8_t> compressed;
	if (!layer.tile_pixels.empty()) {
		size_t bound = ZSTD_compressBound(layer.tile_pixels.size());
		compressed.resize(bound);
		size_t written = ZSTD_compress(compressed.data(), bound, layer.tile_pixels.data(),
			layer.tile_pixels.size(), zstd_level);
		if (ZSTD_isError(written)) {
			return false;
		}
		compressed.resize(written);
	}

	size_t rects_bytes = (layer.clears.size() + layer.tiles.size()) * RefineHeader::kRectSize;
	out->resize(RefineHeader::kSize + base_len + rects_bytes + compressed.size());
	uint8_t *p = out->data();

	p[0] = RefineHeader::kMagic;
	p[1] = RefineHeader::kVersion;
	p[2] = static_cast<uint8_t>(layer.reset ? RefineHeader::kFlagReset : 0);
	p[3] = layer.format;
	p[4] = static_cast<uint8_t>(layer.clears.size());
	p[5] = 0;
	put_u16(p + 6, static_cast<uint16_t>(layer.tiles.size()));
	put_u32(p + 8, static_cast<uint32_t>(base_len));
	put_u32(p + 12, static_cast<uint32_t>(compressed.size()));
	p += RefineHeader::kSize;

	if (base_len > 0) {
		memcpy(p, base, base_len);
		p += base_len;
	}
	put_rects(p, layer.clears);
	p += layer.clears.size() * RefineHeader::kRectSize;
	put_rects(p, layer.tiles);
	p += layer.tiles.size() * RefineHeader::kRectSize;
	if (!compressed.empty()) {
		memcpy(p, compressed.data(), compressed.size());
	}
	return true;
}

bool refine_parse_frame(const uint8_t *payload, size_t payload_len, const uint8_t **base_out,
	size_t *base_len_out, RefineLayer *layer_out) {
	if (payload_len < RefineHeader::kSize) {
		return false;
	}
	if (payload[0] != RefineHeader::kMagic || payload[1] != RefineHeader::kVersion) {
		return false;
	}

	uint8_t flags = payload[2];
	uint8_t format = payload[3];
	// gdp-spec.md §9.5: reserved bits and the reserved byte must be 0, and
	// BGR8 is the only format this build declares.
	if ((flags & ~RefineHeader::kFlagReset) != 0 || format != kRefineFormatBgr8 || payload[5] != 0) {
		return false;
	}
	size_t clear_count = payload[4];
	size_t tile_count = get_u16(payload + 6);
	size_t base_len = get_u32(payload + 8);
	size_t tiles_len = get_u32(payload + 12);

	// Checked against payload_len before any of them is used as an offset,
	// and summed in size_t (each term is bounded by a u32, so on a 64-bit
	// size_t the sum cannot wrap).
	size_t rects_bytes = (clear_count + tile_count) * RefineHeader::kRectSize;
	size_t needed = RefineHeader::kSize + base_len + rects_bytes + tiles_len;
	if (needed > payload_len) {
		return false;
	}

	const uint8_t *p = payload + RefineHeader::kSize;
	*base_out = p;
	*base_len_out = base_len;
	p += base_len;

	layer_out->reset = (flags & RefineHeader::kFlagReset) != 0;
	layer_out->format = format;
	get_rects(p, clear_count, &layer_out->clears);
	p += clear_count * RefineHeader::kRectSize;
	get_rects(p, tile_count, &layer_out->tiles);
	p += tile_count * RefineHeader::kRectSize;

	size_t expected_pixels = refine_layer_pixel_bytes(layer_out->tiles);
	layer_out->tile_pixels.clear();
	if (tiles_len == 0) {
		// No compressed blob is only consistent with no tiles at all --
		// a tile list with nothing to fill it is malformed.
		return expected_pixels == 0;
	}
	if (expected_pixels == 0 || expected_pixels > kMaxRefineLayerPixelBytes) {
		return false;
	}
	// ZSTD_compress() always records the content size; a frame that
	// states a different one is rejected before anything is allocated.
	unsigned long long content_size = ZSTD_getFrameContentSize(p, tiles_len);
	if (content_size != ZSTD_CONTENTSIZE_UNKNOWN && content_size != expected_pixels) {
		return false;
	}

	// Sized from the (capped) tile rects, not from the frame's own content
	// size field, which an unknown size leaves unset.
	layer_out->tile_pixels.resize(expected_pixels);
	size_t written = ZSTD_decompress(layer_out->tile_pixels.data(), expected_pixels, p, tiles_len);
	if (ZSTD_isError(written) || written != expected_pixels) {
		layer_out->tile_pixels.clear();
		return false;
	}
	return true;
}

} // namespace gdp
