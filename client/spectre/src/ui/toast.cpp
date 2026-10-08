// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/toast.hpp"

#include <algorithm>
#include <cmath>

namespace spectre {

namespace {

// The session menu's panel colours, so the two read as one UI.
const UiColor kBackdrop{0.07f, 0.08f, 0.10f, 0.94f};
const UiColor kBorder{0.30f, 0.34f, 0.42f, 1.0f};
const UiColor kText{0.92f, 0.92f, 0.94f, 1.0f};

} // namespace

void Toast::show(std::string text, uint64_t now_us, uint64_t duration_us) {
	text_ = std::move(text);
	expires_us_ = now_us + duration_us;
}

void Toast::show(std::string text) {
	text_ = std::move(text);
	expires_us_ = UINT64_MAX;
}

bool Toast::expire(uint64_t now_us) {
	if (text_.empty() || now_us < expires_us_) {
		return false;
	}
	text_.clear();
	return true;
}

void Toast::build(UiDrawList &out, const UiFont &font, int win_w, int win_h, float top) const {
	if (text_.empty() || !font.valid()) {
		return;
	}
	const float s = (float)font.pixel_height() / 4.0f; // see session_menu.cpp
	const float pad_x = 4 * s;
	const float pad_y = 3 * s;
	const float glyph_h = (float)font.line_height();

	float panel_w = std::ceil(font.text_width(text_) + 2 * pad_x);
	float panel_h = std::ceil(glyph_h + 2 * pad_y);
	float panel_x = std::floor(((float)win_w - panel_w) / 2.0f);
	float panel_y = std::floor(top + std::max(0.0f, (float)win_h - top) / 4.0f - panel_h / 2.0f);

	const float border = std::max(1.0f, std::floor(s / 2));
	out.rect(panel_x - border, panel_y - border, panel_w + 2 * border, panel_h + 2 * border, kBorder);
	out.rect(panel_x, panel_y, panel_w, panel_h, kBackdrop);
	out.text(panel_x + pad_x, panel_y + pad_y, kText, text_);
}

} // namespace spectre
