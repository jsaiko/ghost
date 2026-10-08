// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/dmabuf_tile_source.hpp"

namespace wraith {

void DmabufTileSource::bind(const DmabufFrame &frame, void *token) {
	frame_ = frame;
	token_ = token;
	failed_ = false;
}

bool DmabufTileSource::hash_tiles(uint32_t tile_size, uint64_t *out) {
	if (!reader_.hash_tiles(frame_, token_, tile_size, out)) {
		failed_ = true;
		return false;
	}
	return true;
}

bool DmabufTileSource::append_rects(const std::vector<gdp::RefineRect> &rects, std::vector<uint8_t> *out) {
	if (!reader_.read_rects(frame_, token_, rects, &gathered_)) {
		failed_ = true;
		return false;
	}
	// The gathered rects are already packed back to back; only the X byte
	// is left to drop.
	const uint32_t *src = gathered_.data();
	for (const gdp::RefineRect &rect : rects) {
		size_t row_bytes = (size_t)rect.width * gdp::kRefineBytesPerPixel;
		size_t base = out->size();
		out->resize(base + row_bytes * rect.height);
		for (uint32_t row = 0; row < rect.height; row++) {
			gdp::refine_pack_row(reinterpret_cast<const uint8_t *>(src), out->data() + base + row * row_bytes,
				rect.width);
			src += rect.width;
		}
	}
	return true;
}

} // namespace wraith
