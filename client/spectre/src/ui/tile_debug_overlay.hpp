// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// A debugging aid for the lossless refinement layer (gdp/refine.hpp): every tile
// the host writes into the overlay plane is outlined in red for a fraction
// of a second, so it is visible at a glance which regions are being
// refreshed losslessly and how often.
//
// Off unless spectre was started with -D (StreamOptions::debug_tile_outlines,
// spectre-qt's Debug section). It only ever has anything to show on a refined
// session -- a plain h264/h265/av1 stream carries no tiles.
//
// Pure state + layout, like ui/session_menu.hpp: it collects rectangles in
// *frame* pixels, ages them out, and emits window-pixel outlines into a
// UiDrawList. The caller does the drawing.
#pragma once

#include "gdp/refine.hpp"
#include "ui/draw_list.hpp"

#include <cstdint>
#include <vector>

namespace spectre {

class TileDebugOverlay {
public:
	// Records this frame's tiles, if enabled. `now_us` is
	// gdp::monotonic_us(); each outline is shown until now_us + the hold
	// time. Clears (and the whole-plane reset) are not marked: they take
	// pixels *off* the lossless plane, and what this is for is seeing what
	// the host chose to refresh.
	void add_layer(const gdp::RefineLayer &layer, uint64_t now_us);

	// Drops outlines whose hold time has passed. Returns true if any went
	// away, i.e. the caller needs a redraw -- an idle desktop sends no
	// further frames, so without this the last boxes would stay up until
	// something else happened to repaint.
	bool expire(uint64_t now_us);

	// Forgets everything (a decoder restart, a session ending).
	void clear() { boxes_.clear(); }

	void set_enabled(bool enabled);

	// A rectangle in window pixels.
	struct Area {
		float x = 0, y = 0, w = 0, h = 0;
	};
	// Appends one 2px outline per live tile (nothing when there are none),
	// mapping `frame_w`x`frame_h` frame pixels onto `picture`, where the
	// video is drawn (stretched to fill it, or 1:1 and possibly hanging off
	// the window in the actual-size view), and clipping them to `visible`,
	// the part of the window the video shows in.
	void build(UiDrawList &out, uint32_t frame_w, uint32_t frame_h, const Area &picture,
		const Area &visible) const;

private:
	struct Box {
		gdp::RefineRect rect;
		uint64_t expires_us = 0;
	};

	bool enabled_ = false;
	std::vector<Box> boxes_;
};

} // namespace spectre
