// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/session_menu.hpp"

#include "prefs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace spectre {

namespace {

const UiColor kBackdrop{0.07f, 0.08f, 0.10f, 0.94f};
const UiColor kBorder{0.30f, 0.34f, 0.42f, 1.0f};
const UiColor kTitle{0.60f, 0.78f, 1.00f, 1.0f};
const UiColor kText{0.92f, 0.92f, 0.94f, 1.0f};
const UiColor kDim{0.55f, 0.57f, 0.62f, 1.0f};
const UiColor kHighlight{0.20f, 0.36f, 0.60f, 1.0f};
const UiColor kDanger{1.00f, 0.55f, 0.50f, 1.0f};
// The close X's hover, the usual window-close red (as on the toolbar).
const UiColor kCloseHover{0.78f, 0.19f, 0.17f, 1.0f};
// The slider: its track, the part up to the value, and the handle.
const UiColor kTrack{0.30f, 0.34f, 0.42f, 1.0f};
const UiColor kTrackFill{0.60f, 0.78f, 1.00f, 1.0f};
// A glyph button's cell when not chosen (the toolbar's button color).
const UiColor kCell{0.14f, 0.15f, 0.19f, 1.0f};

// The resolution page's fixed choices, after "Match this display": the
// same list spectre-qt's settings dialog offers (settings_dialog.cpp).
constexpr struct {
	uint32_t width, height;
} kResolutions[] = {
	{1280, 720},
	{1366, 768},
	{1600, 900},
	{1920, 1080},
	{2560, 1440},
	{3840, 2160},
};

std::string size_text(uint32_t width, uint32_t height) {
	return std::to_string(width) + "x" + std::to_string(height);
}

// One Left/Right press, and what a drag snaps to (either slider).
constexpr double kSensitivityStep = 0.05;
constexpr double kVolumeStep = 0.05;

// The sensitivity track is logarithmic, so 1x sits in the middle of
// 0.25x..4x and halving feels the same as doubling; volume is linear.
double sensitivity_fraction(double value) {
	return std::log(value / kMinMouseSensitivity) / std::log(kMaxMouseSensitivity / kMinMouseSensitivity);
}

double clamp_sensitivity(double value) {
	value = std::round(value / kSensitivityStep) * kSensitivityStep;
	return std::clamp(value, kMinMouseSensitivity, kMaxMouseSensitivity);
}

double clamp_volume(double value) {
	return std::clamp(std::round(value / kVolumeStep) * kVolumeStep, 0.0, 1.0);
}

} // namespace

void SessionMenu::show() {
	visible_ = true;
	confirm_only_ = false;
	glyph_selected_ = 0;
	set_page(Page::kMain);
}

void SessionMenu::show_end_confirmation() {
	visible_ = true;
	confirm_only_ = true;
	set_page(Page::kConfirmEnd);
}

void SessionMenu::set_mouse_sensitivity(double value) {
	mouse_sensitivity_ = clamp_sensitivity(value);
}

void SessionMenu::set_volume(std::optional<double> value) {
	volume_ = value ? std::optional<double>(clamp_volume(*value)) : std::nullopt;
}

std::optional<SessionMenu::ItemId> SessionMenu::selected_slider() const {
	std::vector<Item> items = items_for_page();
	if (selected_ < items.size() && items[selected_].slider) {
		return items[selected_].id;
	}
	return std::nullopt;
}

double SessionMenu::slider_value(ItemId id) const {
	return id == ItemId::kVolume ? volume() : mouse_sensitivity_;
}

MenuAction SessionMenu::set_slider_value(ItemId id, double value) {
	if (id == ItemId::kVolume) {
		double next = clamp_volume(value);
		if (next == volume()) {
			return MenuAction::kNone;
		}
		volume_ = next;
		return MenuAction::kVolumeChanged;
	}
	double next = clamp_sensitivity(value);
	if (next == mouse_sensitivity_) {
		return MenuAction::kNone;
	}
	mouse_sensitivity_ = next;
	return MenuAction::kSensitivityChanged;
}

double SessionMenu::slider_value_at(ItemId id, size_t index, float x) const {
	const HitRect *track = index < slider_tracks_.size() ? &slider_tracks_[index] : nullptr;
	if (!track || track->w <= 0) {
		return slider_value(id);
	}
	double t = std::clamp((double)(x - track->x) / track->w, 0.0, 1.0);
	if (id == ItemId::kVolume) {
		return t;
	}
	return kMinMouseSensitivity * std::pow(kMaxMouseSensitivity / kMinMouseSensitivity, t);
}

void SessionMenu::hide() {
	visible_ = false;
	dragging_slider_ = false;
	item_rects_.clear();
	close_rect_ = HitRect{0, 0, 0, 0};
	panel_rect_ = HitRect{0, 0, 0, 0};
	close_hovered_ = false;
}

void SessionMenu::set_page(Page page) {
	page_ = page;
	selected_ = 0;
	item_rects_.clear();
}

std::vector<SessionMenu::GlyphButton> SessionMenu::glyph_buttons() const {
	std::vector<GlyphButton> out;
	if (speaker_muted_) {
		out.push_back({MenuAction::kToggleSpeakerMute,
			*speaker_muted_ ? AudioGlyph::kSpeakerMuted : AudioGlyph::kSpeaker,
			*speaker_muted_ ? "Unmute session audio" : "Mute session audio"});
	}
	if (mic_muted_) {
		out.push_back({MenuAction::kToggleMicMute, *mic_muted_ ? AudioGlyph::kMicMuted : AudioGlyph::kMic,
			*mic_muted_ ? "Unmute session microphone" : "Mute session microphone"});
	}
	return out;
}

bool SessionMenu::glyph_row_selected() const {
	std::vector<Item> items = items_for_page();
	return selected_ < items.size() && items[selected_].glyphs;
}

std::vector<SessionMenu::Item> SessionMenu::items_for_page() const {
	switch (page_) {
	case Page::kMain: {
		std::vector<Item> items;
		auto add = [&](Group group, Item item) {
			item.group = group;
			items.push_back(std::move(item));
		};
		if (!glyph_buttons().empty()) {
			Item row{ItemId::kGlyphRow, ""};
			row.glyphs = true;
			add(Group::kAudio, std::move(row));
		}
		if (volume_) {
			add(Group::kAudio, {ItemId::kVolume, "Volume", false, true});
		}
		if (!kiosk_) {
			add(Group::kDisplay,
				{ItemId::kFullscreenToggle, fullscreen_ ? "Exit Fullscreen" : "Enter Fullscreen"});
		}
		add(Group::kDisplay,
			{ItemId::kViewToggle, actual_size_ ? "View: Actual Size" : "View: Fit to Window"});
		if (resolution_w_ > 0) {
			add(Group::kDisplay,
				{ItemId::kResolution, "Resolution: " + size_text(resolution_w_, resolution_h_)});
		}
		if (lossless_) {
			add(Group::kDisplay,
				{ItemId::kLosslessToggle,
					*lossless_ ? "Lossless Refinement: On" : "Lossless Refinement: Off"});
		}
		add(Group::kDisplay, {ItemId::kStatsToggle, stats_enabled_ ? "Hide Statistics" : "Show Statistics"});
		add(Group::kInput, {ItemId::kCaptureMouseToggle, capture_mouse_ ? "Release Mouse" : "Capture Mouse"});
		add(Group::kInput, {ItemId::kMouseSensitivity, "Mouse Sensitivity", false, true});
		if (controllers_raw_) {
			add(Group::kInput,
				{ItemId::kControllerMode,
					*controllers_raw_ ? "Controllers: Raw" : "Controllers: Xbox Emulation"});
		}
		add(Group::kEnd, {ItemId::kDisconnect, "Disconnect"});
		add(Group::kEnd, {ItemId::kEndSession, "End session"});
		return items;
	}
	case Page::kResolution: {
		std::vector<Item> items;
		auto add = [&](std::string label, uint32_t w, uint32_t h) {
			if (w == resolution_w_ && h == resolution_h_) {
				label += "  (current)";
			}
			// Bigger than this screen either way: it will be drawn smaller,
			// or, at actual size, only part of it at a time.
			if (screen_w_ > 0 && (w > screen_w_ || h > screen_h_)) {
				label += actual_size_ ? "  (scrolls)" : "  (scaled down)";
			}
			Item item{ItemId::kResolutionChoice, std::move(label)};
			item.width = w;
			item.height = h;
			items.push_back(std::move(item));
		};
		if (screen_w_ > 0 && screen_h_ > 0) {
			add("Match this display (" + size_text(screen_w_, screen_h_) + ")", screen_w_, screen_h_);
		}
		for (const auto &r : kResolutions) {
			add(size_text(r.width, r.height), r.width, r.height);
		}
		items.push_back({ItemId::kResolutionBack, "Back"});
		return items;
	}
	case Page::kConfirmEnd:
		return {
			{ItemId::kConfirmLogout, "Log out now", true},
			{ItemId::kCancelConfirm, "Cancel"},
		};
	}
	return {};
}

MenuAction SessionMenu::activate(size_t index) {
	std::vector<Item> items = items_for_page();
	if (index >= items.size()) {
		return MenuAction::kNone;
	}
	switch (items[index].id) {
	case ItemId::kStatsToggle: return MenuAction::kToggleStats;
	case ItemId::kCaptureMouseToggle: return MenuAction::kToggleCaptureMouse;
	case ItemId::kLosslessToggle: return MenuAction::kToggleLossless;
	case ItemId::kFullscreenToggle: return MenuAction::kToggleFullscreen;
	case ItemId::kViewToggle: return MenuAction::kToggleActualSize;
	case ItemId::kControllerMode: return MenuAction::kToggleControllerMode;
	case ItemId::kGlyphRow: {
		std::vector<GlyphButton> glyphs = glyph_buttons();
		return glyphs.empty() ? MenuAction::kNone
							  : glyphs[std::min(glyph_selected_, glyphs.size() - 1)].action;
	}
	case ItemId::kMouseSensitivity:
	case ItemId::kVolume: return MenuAction::kNone; // adjusted with Left/Right or the pointer
	case ItemId::kResolution: {
		set_page(Page::kResolution);
		// Start on the current size, so Enter alone changes nothing.
		std::vector<Item> choices = items_for_page();
		for (size_t i = 0; i < choices.size(); i++) {
			if (choices[i].width == resolution_w_ && choices[i].height == resolution_h_) {
				selected_ = i;
				break;
			}
		}
		return MenuAction::kNone;
	}
	case ItemId::kResolutionChoice:
		if (items[index].width == resolution_w_ && items[index].height == resolution_h_) {
			return_to_main(ItemId::kResolution);
			return MenuAction::kNone;
		}
		chosen_w_ = items[index].width;
		chosen_h_ = items[index].height;
		return MenuAction::kChangeResolution;
	case ItemId::kResolutionBack: return_to_main(ItemId::kResolution); return MenuAction::kNone;
	case ItemId::kDisconnect: return MenuAction::kDisconnect;
	case ItemId::kEndSession: set_page(Page::kConfirmEnd); return MenuAction::kNone;
	case ItemId::kConfirmLogout: return MenuAction::kEndSession;
	case ItemId::kCancelConfirm:
		if (confirm_only_) {
			return MenuAction::kClose;
		}
		return_to_main(ItemId::kEndSession);
		return MenuAction::kNone;
	}
	return MenuAction::kNone;
}

void SessionMenu::return_to_main(ItemId id) {
	set_page(Page::kMain);
	std::vector<Item> items = items_for_page();
	for (size_t i = 0; i < items.size(); i++) {
		if (items[i].id == id) {
			selected_ = i;
			break;
		}
	}
}

MenuAction SessionMenu::handle_key(SDL_Scancode scancode) {
	size_t count = items_for_page().size();
	switch (scancode) {
	case SDL_SCANCODE_UP:
	case SDL_SCANCODE_KP_8: selected_ = (selected_ + count - 1) % count; return MenuAction::kNone;
	case SDL_SCANCODE_DOWN:
	case SDL_SCANCODE_KP_2:
	case SDL_SCANCODE_TAB: selected_ = (selected_ + 1) % count; return MenuAction::kNone;
	case SDL_SCANCODE_LEFT:
	case SDL_SCANCODE_KP_4:
	case SDL_SCANCODE_RIGHT:
	case SDL_SCANCODE_KP_6: {
		bool right = scancode == SDL_SCANCODE_RIGHT || scancode == SDL_SCANCODE_KP_6;
		if (glyph_row_selected()) {
			size_t count = glyph_buttons().size();
			if (right && glyph_selected_ + 1 < count) {
				glyph_selected_++;
			} else if (!right && glyph_selected_ > 0) {
				glyph_selected_--;
			}
			return MenuAction::kNone;
		}
		std::optional<ItemId> slider = selected_slider();
		if (!slider) {
			return MenuAction::kNone;
		}
		double step = *slider == ItemId::kVolume ? kVolumeStep : kSensitivityStep;
		return set_slider_value(*slider, slider_value(*slider) + (right ? step : -step));
	}
	case SDL_SCANCODE_RETURN:
	case SDL_SCANCODE_KP_ENTER:
	case SDL_SCANCODE_SPACE: return activate(selected_);
	case SDL_SCANCODE_ESCAPE:
		if (page_ == Page::kConfirmEnd) {
			if (confirm_only_) {
				return MenuAction::kClose;
			}
			return_to_main(ItemId::kEndSession);
			return MenuAction::kNone;
		}
		if (page_ == Page::kResolution) {
			return_to_main(ItemId::kResolution);
			return MenuAction::kNone;
		}
		return MenuAction::kClose;
	default: return MenuAction::kNone;
	}
}

int SessionMenu::hit_test(float x, float y) const {
	for (size_t i = 0; i < item_rects_.size(); i++) {
		const HitRect &r = item_rects_[i];
		if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
			return (int)i;
		}
	}
	return -1;
}

MenuAction SessionMenu::handle_pointer_motion(float x, float y) {
	if (dragging_slider_) {
		return set_slider_value(drag_id_, slider_value_at(drag_id_, drag_index_, x));
	}
	const HitRect &c = close_rect_;
	close_hovered_ = x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h;
	int hit = hit_test(x, y);
	if (hit >= 0) {
		selected_ = (size_t)hit;
	}
	for (size_t i = 0; i < glyph_rects_.size(); i++) {
		const HitRect &g = glyph_rects_[i];
		if (x >= g.x && x < g.x + g.w && y >= g.y && y < g.y + g.h) {
			glyph_selected_ = i;
		}
	}
	return MenuAction::kNone;
}

MenuAction SessionMenu::handle_pointer_press(float x, float y) {
	const HitRect &c = close_rect_;
	if (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h) {
		return MenuAction::kClose;
	}
	// A press outside the panel closes it -- once it's been laid out, so
	// one landing between show() and the first build() doesn't.
	const HitRect &p = panel_rect_;
	if (p.w > 0 && !(x >= p.x && x < p.x + p.w && y >= p.y && y < p.y + p.h)) {
		return MenuAction::kClose;
	}
	int hit = hit_test(x, y);
	if (hit < 0) {
		return MenuAction::kNone;
	}
	selected_ = (size_t)hit;
	if (glyph_row_selected()) {
		// Only a glyph itself toggles; the rest of the row is just the row.
		for (size_t i = 0; i < glyph_rects_.size(); i++) {
			const HitRect &g = glyph_rects_[i];
			if (x >= g.x && x < g.x + g.w && y >= g.y && y < g.y + g.h) {
				glyph_selected_ = i;
				return activate(selected_);
			}
		}
		return MenuAction::kNone;
	}
	if (std::optional<ItemId> slider = selected_slider()) {
		// Anywhere on the row picks it up, so the track needn't be hit
		// exactly; the value jumps to where the press landed on it only
		// when it's on the track itself.
		dragging_slider_ = true;
		drag_id_ = *slider;
		drag_index_ = selected_;
		const HitRect &t = slider_tracks_[selected_];
		if (x >= t.x && x < t.x + t.w) {
			return set_slider_value(drag_id_, slider_value_at(drag_id_, drag_index_, x));
		}
		return MenuAction::kNone;
	}
	return activate(selected_);
}

void SessionMenu::build(UiDrawList &out, const UiFont &font, int win_w, int win_h) {
	item_rects_.clear();
	close_rect_ = HitRect{0, 0, 0, 0};
	panel_rect_ = HitRect{0, 0, 0, 0};
	slider_tracks_.clear();
	glyph_rects_.clear();
	if (!visible_ || !font.valid()) {
		return;
	}
	// Everything is sized off the font so the panel scales with the
	// display: `s` is a quarter of the em height (~3.5px at 14px).
	const float s = (float)font.pixel_height() / 4.0f;
	const float glyph_h = (float)font.line_height();
	const float row_h = glyph_h + 3 * s;
	const float pad = 4 * s;
	const float gap = 2 * s;

	std::string title = title_.empty() ? "spectre" : title_;
	std::vector<std::string> notes;
	switch (page_) {
	case Page::kMain: notes = {"Session menu"}; break;
	case Page::kResolution:
		notes = {"Remote resolution", "The window resizes to fit; fullscreen stays fullscreen."};
		break;
	case Page::kConfirmEnd:
		notes = {"End the remote session?", "This logs out of the remote desktop;",
			"unsaved work there may be lost."};
		break;
	}
	// Esc backs out of the confirmation rather than closing (handle_key).
	std::vector<Item> items = items_for_page();
	const char *kSliderHint = "Up/Down select   Left/Right adjust   Esc close";
	const char *kGlyphHint = "Left/Right choose   Enter toggle   Esc close";
	std::string hint = page_ == Page::kConfirmEnd && confirm_only_
		? "Up/Down select   Enter activate   Esc close"
		: page_ != Page::kMain ? "Up/Down select   Enter activate   Esc back"
		: selected_slider()    ? kSliderHint
		: glyph_row_selected() ? kGlyphHint
							   : "Up/Down select   Enter activate   Esc close";
	// The glyph row: square cells a row high, centred, with a line of
	// smaller text below them saying what the chosen one does while the
	// row is selected (the line is kept either way, so the panel doesn't
	// change size as the selection moves).
	constexpr float kLabelScale = 0.8f;
	std::vector<GlyphButton> glyphs = glyph_buttons();
	const float audio_glyph_size = std::floor(row_h * 0.66f);
	const float glyph_stroke = 0.8f * std::max(2.0f, std::round(s * 0.5f));
	float glyph_label_w = 0.0f;
	for (const GlyphButton &g : glyphs) {
		glyph_label_w = std::max(glyph_label_w, std::ceil(font.text_width(g.label) * kLabelScale));
	}
	const float glyph_cells_w = (float)glyphs.size() * (row_h + gap) - gap;
	const float glyph_label_h = std::ceil(glyph_h * kLabelScale);
	const float glyph_row_h = row_h + gap / 2 + glyph_label_h + gap;
	const float glyph_row_w = std::max(glyph_cells_w, glyph_label_w);
	const bool has_glyph_row =
		std::any_of(items.begin(), items.end(), [](const Item &it) { return it.glyphs; });
	// The main page's groups, set apart by a gap between each.
	auto space_before = [&](size_t i) {
		return i > 0 && items[i].group != items[i - 1].group ? 2 * gap : 0.0f;
	};
	float groups_h = 0.0f;
	for (size_t i = 0; i < items.size(); i++) {
		groups_h += space_before(i);
	}

	// A slider row: its label, a track, and the value ("1.00x", "40%") at
	// the right. The value's width is taken from the widest either can be,
	// so the track doesn't shift as it changes and two tracks line up.
	auto value_text = [&](ItemId id) {
		char text[16];
		if (id == ItemId::kVolume) {
			snprintf(text, sizeof(text), "%d%%", (int)std::lround(volume() * 100));
		} else {
			snprintf(text, sizeof(text), "%.2fx", mouse_sensitivity_);
		}
		return std::string(text);
	};
	const float value_w = std::ceil(std::max(font.text_width("4.00x"), font.text_width("100%")));
	const float min_track_w = 8 * (float)font.pixel_height();

	// Panel width from the widest line; a minimum so the short main page
	// doesn't look cramped.
	const float close_size = std::ceil(glyph_h + s); // the close X's square, below
	float widest = 24.0f * (float)font.pixel_height();
	widest = std::max(widest, font.text_width(hint));
	// Sized for the slider's hint too, so the panel doesn't change width
	// as the selection moves onto the slider and off it.
	if (std::any_of(items.begin(), items.end(), [](const Item &it) { return it.slider; })) {
		widest = std::max(widest, font.text_width(kSliderHint));
	}
	if (!glyphs.empty() && page_ == Page::kMain) {
		widest = std::max(widest, std::max(font.text_width(kGlyphHint), glyph_row_w + pad));
	}
	widest = std::max(widest, font.text_width(title) + close_size);
	for (const auto &n : notes) {
		widest = std::max(widest, font.text_width(n));
	}
	for (const auto &it : items) {
		float w = font.text_width(it.label) + pad;
		if (it.slider) {
			w += 2 * gap + min_track_w + value_w;
		}
		widest = std::max(widest, w);
	}
	float panel_w = std::ceil(widest + 2 * pad);
	float panel_h = pad + glyph_h + gap                   // title
		+ (float)notes.size() * (glyph_h + gap / 2) + gap // notes
		+ (float)items.size() * row_h + gap               // items
		+ (has_glyph_row ? glyph_row_h - row_h : 0.0f)    // the glyph row's label line
		+ groups_h                                        // gaps between groups
		+ glyph_h + pad;                                  // hint
	float panel_x = std::floor(((float)win_w - panel_w) / 2.0f);
	float panel_y = std::floor(((float)win_h - panel_h) / 2.0f);

	const float border = std::max(1.0f, std::floor(s / 2));
	panel_rect_ = HitRect{panel_x - border, panel_y - border, panel_w + 2 * border, panel_h + 2 * border};
	out.rect(panel_rect_.x, panel_rect_.y, panel_rect_.w, panel_rect_.h, kBorder);
	out.rect(panel_x, panel_y, panel_w, panel_h, kBackdrop);

	// The close X: a square the height of the title line in the top-right
	// corner, as far in from the edges as the title is from the top-left.
	close_rect_ =
		HitRect{panel_x + panel_w - pad / 2 - close_size, panel_y + pad / 2, close_size, close_size};
	if (close_hovered_) {
		out.rect(close_rect_.x, close_rect_.y, close_rect_.w, close_rect_.h, kCloseHover);
	}
	const float glyph_size = std::floor(close_size * 0.45f);
	out.x_glyph(std::floor(close_rect_.x + (close_size - glyph_size) / 2),
		std::floor(close_rect_.y + (close_size - glyph_size) / 2), glyph_size,
		std::max(2.0f, std::round(s * 0.5f)), close_hovered_ ? kText : kDim);

	float y = panel_y + pad;
	out.text(panel_x + pad, y, kTitle, title);
	y += glyph_h + gap;
	for (const auto &n : notes) {
		out.text(panel_x + pad, y, page_ != Page::kMain && &n == &notes.front() ? kText : kDim, n);
		y += glyph_h + gap / 2;
	}
	y += gap;
	for (size_t i = 0; i < items.size(); i++) {
		float row_x = panel_x + pad / 2;
		float row_w = panel_w - pad;
		y += space_before(i);
		if (items[i].glyphs) {
			const size_t chosen = std::min(glyph_selected_, glyphs.size() - 1);
			float cell_x = std::floor(row_x + (row_w - glyph_cells_w) / 2);
			for (size_t g = 0; g < glyphs.size(); g++) {
				out.rect(cell_x, y, row_h, row_h, i == selected_ && g == chosen ? kHighlight : kCell);
				draw_audio_glyph(out, glyphs[g].glyph, std::floor(cell_x + (row_h - audio_glyph_size) / 2),
					std::floor(y + (row_h - audio_glyph_size) / 2), (int)audio_glyph_size, glyph_stroke,
					kText, kDanger);
				glyph_rects_.push_back({cell_x, y, row_h, row_h});
				cell_x += row_h + gap;
			}
			if (i == selected_) {
				const std::string &label = glyphs[chosen].label;
				float label_w = font.text_width(label) * kLabelScale;
				out.text(std::floor(row_x + (row_w - label_w) / 2), y + row_h + gap / 2, kDim, label,
					kLabelScale);
			}
			slider_tracks_.push_back({0, 0, 0, 0});
			item_rects_.push_back({row_x, y, row_w, glyph_row_h});
			y += glyph_row_h;
			continue;
		}
		if (i == selected_) {
			out.rect(row_x, y, row_w, row_h, kHighlight);
		}
		out.text(row_x + pad / 2, y + (row_h - glyph_h) / 2, items[i].danger ? kDanger : kText,
			items[i].label);
		if (items[i].slider) {
			float value_x = row_x + row_w - pad / 2 - value_w;
			float track_x = row_x + pad / 2 + std::ceil(font.text_width(items[i].label)) + 2 * gap;
			float track_w = std::max(1.0f, value_x - gap - track_x);
			float track_h = std::max(2.0f, std::floor(s / 2));
			float track_y = std::floor(y + (row_h - track_h) / 2);
			float knob_w = std::max(4.0f, std::floor(s));
			float knob_h = std::floor(glyph_h * 0.8f);
			float fraction = items[i].id == ItemId::kVolume ? (float)volume()
															: (float)sensitivity_fraction(mouse_sensitivity_);
			float fill_w = std::floor(track_w * fraction);
			out.rect(track_x, track_y, track_w, track_h, kTrack);
			out.rect(track_x, track_y, fill_w, track_h, kTrackFill);
			out.rect(std::floor(track_x + fill_w - knob_w / 2), std::floor(y + (row_h - knob_h) / 2), knob_w,
				knob_h, kText);
			out.text(value_x, y + (row_h - glyph_h) / 2, kText, value_text(items[i].id));
			slider_tracks_.push_back({track_x, y, track_w, row_h});
		} else {
			slider_tracks_.push_back({0, 0, 0, 0});
		}
		item_rects_.push_back({row_x, y, row_w, row_h});
		y += row_h;
	}
	y += gap;
	out.text(panel_x + pad, y, kDim, hint);
}

} // namespace spectre
