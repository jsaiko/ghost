// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/toolbar.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>

namespace spectre {

namespace {

// The session menu's palette (session_menu.cpp), so the two read as one UI.
const UiColor kBackdrop{0.07f, 0.08f, 0.10f, 0.94f};
const UiColor kBorder{0.30f, 0.34f, 0.42f, 1.0f};
const UiColor kButton{0.14f, 0.15f, 0.19f, 1.0f};
const UiColor kHover{0.22f, 0.25f, 0.31f, 1.0f};
const UiColor kActive{0.20f, 0.36f, 0.60f, 1.0f};
const UiColor kActiveHover{0.26f, 0.44f, 0.72f, 1.0f};
const UiColor kText{0.92f, 0.92f, 0.94f, 1.0f};
const UiColor kTitle{0.60f, 0.78f, 1.00f, 1.0f};
const UiColor kDanger{1.00f, 0.55f, 0.50f, 1.0f};
// The close icon's hover, the usual window-close red.
const UiColor kCloseHover{0.78f, 0.19f, 0.17f, 1.0f};

// How close to the top edge the pointer must come to reveal the fullscreen
// panel. A fullscreen pointer stops at row 0; the slack covers fractional
// positions on scaled desktops.
constexpr float kRevealEdgePx = 1.0f;

struct Metrics {
	float s; // a quarter of the em height, as in session_menu.cpp
	float glyph_h;
	float button_h;
	float pad; // around the row of buttons
	float gap; // between buttons
	float bar_h;
	float border;
};

Metrics metrics_for(const UiFont &font) {
	Metrics m;
	m.s = (float)font.pixel_height() / 4.0f;
	m.glyph_h = (float)font.line_height();
	m.button_h = std::ceil(m.glyph_h + 2 * m.s);
	m.pad = std::ceil(m.s);
	m.gap = std::ceil(m.s);
	m.border = std::max(1.0f, std::floor(m.s / 2));
	m.bar_h = m.button_h + 2 * m.pad + m.border;
	return m;
}

} // namespace

void Toolbar::set_fullscreen(bool fullscreen) {
	if (fullscreen == fullscreen_) {
		return;
	}
	fullscreen_ = fullscreen;
	revealed_ = false;
	hovered_ = -1;
}

float Toolbar::reserved_height(const UiFont &font) const {
	return fullscreen_ ? 0.0f : docked_height(font);
}

float Toolbar::docked_height(const UiFont &font) {
	return font.valid() ? metrics_for(font).bar_h : 0.0f;
}

std::vector<Toolbar::Button> Toolbar::buttons() const {
	std::vector<Button> out;
	if (speaker_muted_) {
		out.push_back({ToolbarAction::kToggleSpeakerMute, "", false, false,
			*speaker_muted_ ? Icon::kSpeakerMuted : Icon::kSpeaker});
	}
	if (mic_muted_) {
		out.push_back(
			{ToolbarAction::kToggleMicMute, "", false, false, *mic_muted_ ? Icon::kMicMuted : Icon::kMic});
	}
	out.push_back({ToolbarAction::kOpenMenu, "Menu"});
	out.push_back({ToolbarAction::kToggleCaptureMouse, "Capture Mouse", capture_mouse_});
	if (!kiosk_) {
		out.push_back({ToolbarAction::kToggleFullscreen, fullscreen_ ? "Exit Fullscreen" : "Fullscreen"});
	}
	out.push_back({ToolbarAction::kEndSession, "End Session", false, true});
	if (fullscreen_) {
		// A kiosk keeps close -- the way out when the remote desktop or the
		// link has frozen -- but not minimize.
		if (!kiosk_) {
			out.push_back({ToolbarAction::kMinimize, "", false, false, Icon::kMinimize});
		}
		out.push_back({ToolbarAction::kClose, "", false, false, Icon::kClose});
	} else if (native_size_shown_) {
		out.push_back({ToolbarAction::kNativeSize, "", false, false, Icon::kNativeSize});
	}
	return out;
}

void Toolbar::draw_icon(UiDrawList &out, Icon icon, const Rect &r, float size, float stroke, UiColor color) {
	const float x = std::floor(r.x + (r.w - size) / 2);
	const float y = std::floor(r.y + (r.h - size) / 2);
	switch (icon) {
	case Icon::kNone: break;
	case Icon::kMinimize: out.rect(x, y + size - stroke, size, stroke, color); break;
	case Icon::kClose: out.x_glyph(x, y, size, stroke, color); break;
	case Icon::kSpeaker:
	case Icon::kSpeakerMuted:
	case Icon::kMic:
	case Icon::kMicMuted: {
		// Two thirds of the button, with lines a little under the window
		// controls' stroke (they're anti-aliased, and so read heavier).
		const int glyph = (int)std::floor(r.h * 0.66f);
		AudioGlyph which = icon == Icon::kSpeaker ? AudioGlyph::kSpeaker
			: icon == Icon::kSpeakerMuted         ? AudioGlyph::kSpeakerMuted
			: icon == Icon::kMic                  ? AudioGlyph::kMic
												  : AudioGlyph::kMicMuted;
		draw_audio_glyph(out, which, std::floor(r.x + (r.w - (float)glyph) / 2),
			std::floor(r.y + (r.h - (float)glyph) / 2), glyph, 0.8f * stroke, color, kDanger);
		break;
	}
	case Icon::kNativeSize: {
		// A viewfinder: four corner brackets framing a solid square, the
		// picture shown at its own size.
		const float arm = std::floor(size / 3);
		out.rect(x, y, arm, stroke, color);
		out.rect(x, y, stroke, arm, color);
		out.rect(x + size - arm, y, arm, stroke, color);
		out.rect(x + size - stroke, y, stroke, arm, color);
		out.rect(x, y + size - stroke, arm, stroke, color);
		out.rect(x, y + size - arm, stroke, arm, color);
		out.rect(x + size - arm, y + size - stroke, arm, stroke, color);
		out.rect(x + size - stroke, y + size - arm, stroke, arm, color);
		const float inner = size - 2 * (stroke + std::max(stroke, std::floor(size / 6)));
		const float off = std::floor((size - inner) / 2);
		out.rect(x + off, y + off, inner, inner, color);
		break;
	}
	}
}

int Toolbar::hit_test(float x, float y) const {
	for (size_t i = 0; i < button_rects_.size(); i++) {
		if (button_rects_[i].contains(x, y)) {
			return (int)i;
		}
	}
	return -1;
}

bool Toolbar::handle_pointer_motion(float x, float y) {
	if (fullscreen_) {
		if (!revealed_) {
			if (y > kRevealEdgePx || x < bar_.x || x >= bar_.x + bar_.w) {
				return false;
			}
			revealed_ = true;
		}
		// Some slack around the panel before it hides, so brushing past
		// its edge doesn't make it flicker.
		Rect keep{bar_.x - hide_margin_, 0.0f, bar_.w + 2 * hide_margin_, bar_.h + hide_margin_};
		if (!keep.contains(x, y)) {
			handle_pointer_left();
			return false;
		}
	}
	hovered_ = hit_test(x, y);
	return bar_.contains(x, y);
}

ToolbarAction Toolbar::handle_pointer_press(float x, float y) {
	if (!visible()) {
		return ToolbarAction::kNone;
	}
	int hit = hit_test(x, y);
	if (hit < 0) {
		return ToolbarAction::kNone;
	}
	std::vector<Button> btns = buttons();
	return (size_t)hit < btns.size() ? btns[(size_t)hit].action : ToolbarAction::kNone;
}

void Toolbar::handle_pointer_left() {
	hovered_ = -1;
	revealed_ = false;
}

void Toolbar::build(UiDrawList &out, const UiFont &font, int win_w, int win_h) {
	bar_ = Rect{};
	button_rects_.clear();
	if (!font.valid() || win_w <= 0 || win_h <= 0) {
		return;
	}
	const Metrics m = metrics_for(font);
	const float label_pad = 3 * m.s; // either side of a button's label

	std::vector<Button> btns = buttons();
	std::vector<float> widths;
	// Extra space ahead of the first window control, setting it apart
	// from the session's own buttons.
	const float icon_gap = 3 * m.gap;
	auto gap_before = [&](size_t i) {
		return (window_control(btns[i].icon) && !window_control(btns[i - 1].icon)) ? icon_gap : m.gap;
	};
	float row_w = 0.0f;
	for (size_t i = 0; i < btns.size(); i++) {
		const Button &b = btns[i];
		widths.push_back(
			b.icon != Icon::kNone ? m.button_h : std::ceil(font.text_width(b.label) + 2 * label_pad));
		row_w += widths.back() + (i > 0 ? gap_before(i) : 0.0f);
	}

	// The fullscreen panel names the session on its left; a window's own
	// title bar already does.
	bool show_title = fullscreen_ && !title_.empty();
	float title_w = show_title ? std::ceil(font.text_width(title_) + 4 * m.s) : 0.0f;

	float x;
	if (fullscreen_) {
		float panel_w = title_w + row_w + 2 * m.pad;
		bar_ = Rect{std::floor(((float)win_w - panel_w) / 2.0f), 0.0f, panel_w, m.bar_h};
		hide_margin_ = m.bar_h;
		x = bar_.x + m.pad + title_w;
	} else {
		bar_ = Rect{0.0f, 0.0f, (float)win_w, m.bar_h};
		hide_margin_ = 0.0f;
		// Centred; a window too narrow for the row starts it at the left
		// edge and lets the right end go off.
		x = std::max(m.pad, std::floor(((float)win_w - row_w) / 2.0f));
	}
	const float button_y = m.pad;
	for (size_t i = 0; i < btns.size(); i++) {
		if (i > 0) {
			x += gap_before(i);
		}
		button_rects_.push_back(Rect{x, button_y, widths[i], m.button_h});
		x += widths[i];
	}
	if (hovered_ >= (int)button_rects_.size()) {
		hovered_ = -1;
	}

	if (!visible()) {
		return;
	}
	if (fullscreen_) {
		// Hangs from the top edge: bordered on the other three sides.
		out.rect(bar_.x - m.border, 0.0f, bar_.w + 2 * m.border, bar_.h, kBorder);
		out.rect(bar_.x, 0.0f, bar_.w, bar_.h - m.border, kBackdrop);
	} else {
		out.rect(0.0f, 0.0f, bar_.w, bar_.h - m.border, kBackdrop);
		out.rect(0.0f, bar_.h - m.border, bar_.w, m.border, kBorder);
	}
	if (show_title) {
		out.text(bar_.x + m.pad + m.s, button_y + (m.button_h - m.glyph_h) / 2, kTitle, title_);
	}
	// Icons a little under half the button's height, drawn in 2 px lines
	// on a 100% desktop, scaling with it (the audio glyphs size themselves
	// from the stroke).
	const float icon_size = std::floor(m.button_h * 0.45f);
	const float icon_stroke = std::max(2.0f, std::round(m.s * 0.5f));
	for (size_t i = 0; i < btns.size(); i++) {
		const Rect &r = button_rects_[i];
		bool hovered = (int)i == hovered_;
		UiColor fill = btns[i].active ? (hovered ? kActiveHover : kActive) : (hovered ? kHover : kButton);
		if (hovered && btns[i].action == ToolbarAction::kClose) {
			fill = kCloseHover;
		}
		out.rect(r.x, r.y, r.w, r.h, fill);
		if (btns[i].icon != Icon::kNone) {
			draw_icon(out, btns[i].icon, r, icon_size, icon_stroke, kText);
		} else {
			out.text(r.x + label_pad, r.y + (r.h - m.glyph_h) / 2, btns[i].danger ? kDanger : kText,
				btns[i].label);
		}
	}
}

} // namespace spectre
