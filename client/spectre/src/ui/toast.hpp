// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// A short message centred over the video, for a few seconds or until
// taken down: the hint that says how to release the mouse when it was
// captured with a click (the toolbar button or the menu), since the
// captured pointer can no longer reach that button to turn it off again;
// the view, resolution and mute changes; and the "logging out" notice that
// stays up until the session ends.
//
// Pure state + layout, like ui/tile_debug_overlay.hpp: the caller shows
// it, retires it from its main loop with expire(), and draws what build()
// appends.
#pragma once

#include "ui/draw_list.hpp"
#include "ui/font.hpp"

#include <cstdint>
#include <string>

namespace spectre {

class Toast {
public:
	// Shows `text` until now_us + `duration_us`, replacing whatever was up.
	// `now_us` is gdp::monotonic_us().
	void show(std::string text, uint64_t now_us, uint64_t duration_us);
	// Shows `text` until hide().
	void show(std::string text);

	void hide() { text_.clear(); }

	// Drops the message once its time is up. Returns true if it went away,
	// i.e. the caller needs a redraw -- an idle desktop sends no frame that
	// would repaint it away otherwise.
	bool expire(uint64_t now_us);

	// Appends the message (nothing when none is up), centred horizontally
	// in the `win_w` x `win_h` window and a quarter of the way down the
	// video, which starts `top` window pixels down (below the docked
	// toolbar).
	void build(UiDrawList &out, const UiFont &font, int win_w, int win_h, float top) const;

private:
	std::string text_;
	uint64_t expires_us_ = 0;
};

} // namespace spectre
