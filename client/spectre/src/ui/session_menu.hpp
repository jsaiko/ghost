// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The client-side session menu: a small panel drawn in the middle of the
// stream window (never sent to the remote session) that the menu hotkey
// (ui/hotkey.hpp, spectre -k) toggles. While it's up, keyboard and mouse
// input drive the menu instead of being forwarded -- see StreamSession.
//
// Pages: the main list, the resolution choices, and a confirmation step
// before ending the session. The main list runs in groups set apart by
// gaps: audio (a row of glyph buttons for session audio and microphone
// mute, whichever are open, Left/Right choosing among them; a volume
// slider for this machine's own output when the caller has one), display
// (fullscreen except in kiosk mode, the view, fit to window or actual
// size, the remote resolution, lossless on a refinement session,
// statistics), input (mouse capture, mouse sensitivity), then Disconnect
// and End session; Esc closes it. The confirmation step is there because
// a logout has no undo, and unsaved work in the remote desktop is at
// stake. An X in the top-right corner closes it from any page. Once the
// logout is on the wire the menu is gone and the caller shows its own
// "logging out" notice. Pure state and layout: it emits a UiDrawList and
// knows nothing about Vulkan or the network; the caller performs whatever
// MenuAction it returns.
#pragma once

#include "ui/draw_list.hpp"
#include "ui/font.hpp"
#include "ui/glyphs.hpp"

#include <SDL3/SDL_scancode.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace spectre {

enum class MenuAction {
	kNone,                 // nothing to do (selection or page may have changed: redraw)
	kToggleStats,          // flip the statistics overlay (the caller also closes the menu)
	kToggleCaptureMouse,   // flip mouse capture (ditto)
	kToggleLossless,       // flip the lossless layer (ditto)
	kToggleFullscreen,     // flip the window between windowed and fullscreen (ditto)
	kToggleActualSize,     // flip the view between fit-to-window and actual size (ditto)
	kToggleControllerMode, // flip forwarded controllers between raw and Xbox emulation (ditto)
	kSensitivityChanged,   // the slider moved: read mouse_sensitivity() (the menu stays open)
	kVolumeChanged,        // the volume slider moved: read volume() (ditto)
	// Flip session audio's or the microphone's mute; the caller updates
	// set_speaker_muted() / set_mic_muted() (the menu stays open).
	kToggleSpeakerMute,
	kToggleMicMute,
	kChangeResolution, // a new remote resolution was picked: read chosen_width()/chosen_height()
	kDisconnect,       // close spectre, leaving the remote session running
	kEndSession,       // send LogoutRequest (the user confirmed)
	kClose,            // hide the menu
};

class SessionMenu {
public:
	bool visible() const { return visible_; }
	// show() always opens on the main page with the first item selected.
	void show();
	// Like show(), but straight onto the end-session confirmation (the
	// toolbar's End Session button); Esc/Cancel then close the menu, back
	// to where the user was, rather than leading to the main page.
	void show_end_confirmation();
	void hide();

	// The panel's title line: the remote session as "user@host". Falls
	// back to "spectre" while empty.
	void set_title(std::string title) { title_ = std::move(title); }
	// Which label the statistics row shows ("Show"/"Hide Statistics").
	// Owned by the caller.
	void set_stats_enabled(bool enabled) { stats_enabled_ = enabled; }
	// Which label the capture row shows ("Capture"/"Release Mouse").
	// Owned by the caller -- see StreamSession::relative_mouse_.
	void set_capture_mouse(bool on) { capture_mouse_ = on; }
	// Which label the lossless row shows ("Lossless Refinement: On" or
	// "Off"), or nullopt for no row: the session didn't negotiate
	// refinement. Owned by the caller (StreamSession::lossless_).
	void set_lossless(std::optional<bool> on) { lossless_ = on; }
	// Which label the fullscreen row shows ("Enter"/"Exit Fullscreen").
	// Owned by the caller (the SDL window flags).
	void set_fullscreen(bool fullscreen) { fullscreen_ = fullscreen; }
	// Which label the view row shows, and whether sizes bigger than the
	// screen are marked "(scrolls)" rather than "(scaled down)". Owned by
	// the caller (StreamSession::actual_size_).
	void set_actual_size(bool on) { actual_size_ = on; }
	// Which label the controller row shows ("Controllers: Raw" or
	// "Controllers: Xbox Emulation"), or nullopt for no row: the session
	// can't forward controllers raw, so there is nothing to switch. Owned
	// by the caller (StreamOptions::raw_gamepads).
	void set_controller_mode(std::optional<bool> raw) { controllers_raw_ = raw; }
	// The glyph row's mute buttons: session audio (AudioPlayer) and the
	// microphone (MicrophoneCapture), each nullopt for no button (not
	// open). Owned by the caller.
	void set_speaker_muted(std::optional<bool> muted) { speaker_muted_ = muted; }
	void set_mic_muted(std::optional<bool> muted) { mic_muted_ = muted; }
	// Kiosk mode (StreamOptions::kiosk): no fullscreen row at all.
	void set_kiosk(bool kiosk) { kiosk_ = kiosk; }
	// The remote output's current size, shown on the main page and marked
	// among the choices. Owned by the caller (DisplaysChanged updates it).
	void set_resolution(uint32_t width, uint32_t height) {
		resolution_w_ = width;
		resolution_h_ = height;
	}
	// The local screen's size in pixels, for the "Match this display"
	// choice; 0x0 leaves that choice out.
	void set_screen_resolution(uint32_t width, uint32_t height) {
		screen_w_ = width;
		screen_h_ = height;
	}
	// The size picked with kChangeResolution.
	uint32_t chosen_width() const { return chosen_w_; }
	uint32_t chosen_height() const { return chosen_h_; }
	// The mouse sensitivity slider's value, kMinMouseSensitivity to
	// kMaxMouseSensitivity (prefs.hpp). Set by the caller before show();
	// the slider changes it and says so with kSensitivityChanged.
	void set_mouse_sensitivity(double value);
	double mouse_sensitivity() const { return mouse_sensitivity_; }
	// The local output's volume, 0 to 1 (audio/system_volume.hpp), or
	// nullopt for no volume row. Set by the caller before show(); the
	// slider changes it and says so with kVolumeChanged.
	void set_volume(std::optional<double> value);
	double volume() const { return volume_.value_or(0.0); }
	// Key-down handling (repeats included, they just move the selection
	// again). Up/Down/Tab move, Enter/Space/Keypad Enter activate,
	// Left/Right step a slider when it's selected or choose a glyph on the
	// glyph row, Escape backs out of
	// the confirmation or closes the menu.
	MenuAction handle_key(SDL_Scancode scancode);
	// Pointer at window pixel `x`,`y`: hovering a row selects it, and the
	// close X lights up under it. While a slider is being dragged it
	// follows the pointer instead (kSensitivityChanged/kVolumeChanged).
	MenuAction handle_pointer_motion(float x, float y);
	// Left button down at window pixel `x`,`y` activates the row under it,
	// or closes the menu on the X; anything else is ignored. On a
	// slider's row it starts a drag, and on its track it sets the value there.
	MenuAction handle_pointer_press(float x, float y);
	// Left button up: ends a slider drag.
	void handle_pointer_release() { dragging_slider_ = false; }

	// Appends this frame's primitives, centered in a win_w x win_h window
	// and laid out for `font`, and records the row rectangles the pointer
	// handlers hit-test against. No-op when hidden.
	void build(UiDrawList &out, const UiFont &font, int win_w, int win_h);

private:
	enum class Page { kMain, kResolution, kConfirmEnd };
	enum class ItemId {
		kGlyphRow,
		kStatsToggle,
		kCaptureMouseToggle,
		kLosslessToggle,
		kFullscreenToggle,
		kViewToggle,
		kControllerMode,
		kMouseSensitivity,
		kVolume,
		kResolution,
		kResolutionChoice,
		kResolutionBack,
		kDisconnect,
		kEndSession,
		kConfirmLogout,
		kCancelConfirm,
	};
	// The main page's groups, set apart by gaps.
	enum class Group { kNone, kAudio, kDisplay, kInput, kEnd };
	struct Item {
		ItemId id;
		std::string label;
		bool danger = false; // drawn in the warning color
		bool slider = false; // a slider (kMouseSensitivity, kVolume), not a button
		bool glyphs = false; // the glyph row (kGlyphRow)
		Group group = Group::kNone;
		uint32_t width = 0, height = 0; // kResolutionChoice's size
	};
	struct HitRect {
		float x, y, w, h;
	};

	// The glyph row's buttons, left to right; empty for no row.
	struct GlyphButton {
		MenuAction action;
		AudioGlyph glyph;
		std::string label; // what activating it does, shown beside the row
	};
	std::vector<GlyphButton> glyph_buttons() const;
	bool glyph_row_selected() const;

	void set_page(Page page);
	std::vector<Item> items_for_page() const;
	MenuAction activate(size_t index);
	// Back to the main page with `id`'s row selected (after backing out
	// of the confirmation step).
	void return_to_main(ItemId id);
	int hit_test(float x, float y) const;
	// The selected row's slider, or nullopt when it isn't one.
	std::optional<ItemId> selected_slider() const;
	// A slider's value and what moving it to `value` (snapped and clamped)
	// returns: kNone when it didn't change.
	double slider_value(ItemId id) const;
	MenuAction set_slider_value(ItemId id, double value);
	// The value for window x `x` on row `index`'s track.
	double slider_value_at(ItemId id, size_t index, float x) const;

	bool visible_ = false;
	// Opened by show_end_confirmation(): backing out of the confirmation
	// closes the menu.
	bool confirm_only_ = false;
	std::string title_;
	bool stats_enabled_ = false;
	bool capture_mouse_ = false;
	std::optional<bool> lossless_;
	bool fullscreen_ = false;
	bool actual_size_ = false;
	bool kiosk_ = false;
	double mouse_sensitivity_ = 1.0;
	std::optional<double> volume_;
	std::optional<bool> controllers_raw_;
	std::optional<bool> speaker_muted_;
	std::optional<bool> mic_muted_;
	// The glyph chosen on the glyph row, and each glyph's cell from the
	// last build().
	size_t glyph_selected_ = 0;
	std::vector<HitRect> glyph_rects_;
	uint32_t resolution_w_ = 0, resolution_h_ = 0;
	uint32_t screen_w_ = 0, screen_h_ = 0;
	uint32_t chosen_w_ = 0, chosen_h_ = 0;
	// The slider being dragged, while dragging_slider_.
	bool dragging_slider_ = false;
	ItemId drag_id_ = ItemId::kMouseSensitivity;
	size_t drag_index_ = 0;
	Page page_ = Page::kMain;
	size_t selected_ = 0;
	std::vector<HitRect> item_rects_;
	// The close X, from the last build(); empty (0x0) while hidden.
	HitRect close_rect_{0, 0, 0, 0};
	// The whole panel, border included, from the last build(); a press
	// outside it closes the menu.
	HitRect panel_rect_{0, 0, 0, 0};
	// Each row's slider track (0x0 for other rows), from the last build().
	std::vector<HitRect> slider_tracks_;
	bool close_hovered_ = false;
};

} // namespace spectre
