// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/tile_debug_overlay.hpp"

#include <algorithm>

namespace spectre {

namespace {

// How long an outline stays up. Long enough to see on a 60Hz display
// without the screen turning into a solid red grid while a window is being
// repainted continuously.
constexpr uint64_t kHoldUs = 250000;

// Outline thickness, in window pixels.
constexpr float kBorderPx = 2.0f;

const UiColor kOutline{1.0f, 0.15f, 0.15f, 0.85f};

// A frame's worth of tiles is normally a handful; this only bounds the
// pathological case (a host sending thousands of one-tile rects) so the
// debug overlay can't grow without limit. Oldest first, so the cap drops
// the boxes that were about to expire anyway.
constexpr size_t kMaxBoxes = 2048;

} // namespace

void TileDebugOverlay::set_enabled(bool enabled) {
	enabled_ = enabled;
	if (!enabled_) {
		boxes_.clear();
	}
}

void TileDebugOverlay::add_layer(const gdp::RefineLayer &layer, uint64_t now_us) {
	if (!enabled_) {
		return;
	}
	for (const gdp::RefineRect &rect : layer.tiles) {
		if (rect.width == 0 || rect.height == 0) {
			continue;
		}
		boxes_.push_back({rect, now_us + kHoldUs});
	}
	if (boxes_.size() > kMaxBoxes) {
		boxes_.erase(boxes_.begin(), boxes_.begin() + (boxes_.size() - kMaxBoxes));
	}
}

bool TileDebugOverlay::expire(uint64_t now_us) {
	size_t before = boxes_.size();
	// add_layer() appends with a fixed hold, so the vector is already in
	// expiry order: everything to drop is at the front.
	auto first_live =
		std::find_if(boxes_.begin(), boxes_.end(), [&](const Box &box) { return box.expires_us > now_us; });
	boxes_.erase(boxes_.begin(), first_live);
	return boxes_.size() != before;
}

void TileDebugOverlay::build(UiDrawList &out, uint32_t frame_w, uint32_t frame_h, const Area &picture,
	const Area &visible) const {
	if (boxes_.empty() || frame_w == 0 || frame_h == 0 || picture.w <= 0 || picture.h <= 0) {
		return;
	}
	const float scale_x = picture.w / (float)frame_w;
	const float scale_y = picture.h / (float)frame_h;
	const float right = visible.x + visible.w;
	const float bottom = visible.y + visible.h;

	for (const Box &box : boxes_) {
		float x = picture.x + box.rect.x * scale_x;
		float y = picture.y + box.rect.y * scale_y;
		float x1 = x + box.rect.width * scale_x;
		float y1 = y + box.rect.height * scale_y;
		// Keep the outline inside the visible video so a tile flush
		// against an edge (or cut by it, panned) still shows all four
		// sides.
		x = std::max(x, visible.x);
		y = std::max(y, visible.y);
		float w = std::min(x1, right) - x;
		float h = std::min(y1, bottom) - y;
		if (w <= 0.0f || h <= 0.0f) {
			continue;
		}
		// A tile smaller than the border in either direction is drawn
		// filled rather than as four overlapping edges.
		float border = std::min(kBorderPx, std::min(w, h) / 2.0f);
		out.rect(x, y, w, border, kOutline);                                    // top
		out.rect(x, y + h - border, w, border, kOutline);                       // bottom
		out.rect(x, y + border, border, h - 2 * border, kOutline);              // left
		out.rect(x + w - border, y + border, border, h - 2 * border, kOutline); // right
	}
}

} // namespace spectre
