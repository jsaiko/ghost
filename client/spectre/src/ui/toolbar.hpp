// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The client-side toolbar: a row of buttons along the top of the stream
// window for the session menu's everyday actions (speaker and microphone
// mute, mouse capture, fullscreen, end session), so they're a click away
// rather than behind the menu hotkey -- plus a button that opens the menu
// itself, for whoever doesn't know the hotkey. Like the menu, it's
// spectre's own chrome and never part of the remote picture.
//
// Two modes, following the window:
// - Windowed: docked. A full-width band that is always shown, its buttons
//   centred in it; the video is drawn below it rather than under it
//   (reserved_height() tells the caller how much to leave free), so
//   nothing on the remote desktop is ever covered.
// - Fullscreen: auto-hide. A compact panel centered on the top edge,
//   floating over the video, shown only while the pointer is on it. It
//   appears when the pointer hits the top edge of the screen within the
//   panel's span (so the corners, where desktops put their own hot spots,
//   stay the remote's) and goes away once the pointer moves off it.
//   With no window title bar to offer them, it ends with minimize and
//   close icons of its own. Windowed, that spot holds a native-size icon
//   instead: back to one stream pixel per screen pixel.
//
// Pure state + layout, like ui/session_menu.hpp: the caller feeds it
// pointer positions in window pixels, performs whatever ToolbarAction
// comes back, and draws the UiDrawList build() emits.
#pragma once

#include "ui/draw_list.hpp"
#include "ui/font.hpp"

#include <optional>
#include <string>
#include <vector>

namespace spectre {

enum class ToolbarAction {
	kNone,
	kOpenMenu,
	kToggleCaptureMouse,
	kToggleFullscreen,
	kToggleSpeakerMute, // session audio, played by spectre
	kToggleMicMute,
	kEndSession, // ask first: the caller opens the menu's confirmation page
	kMinimize,   // fullscreen only
	kClose,      // fullscreen only: close spectre, leaving the session running
	kNativeSize, // windowed only: resize the window to the stream's size
};

class Toolbar {
public:
	// Shown on the left of the fullscreen panel, where there's no window
	// title bar saying which session this is ("user@host").
	void set_title(std::string title) { title_ = std::move(title); }
	// Button state, owned by the caller; a toggle that's on is drawn
	// highlighted.
	void set_capture_mouse(bool on) { capture_mouse_ = on; }
	// Mute state of the session audio spectre plays and of the microphone
	// it sends, or nullopt for no button (that audio isn't open).
	void set_speaker_muted(std::optional<bool> muted) { speaker_muted_ = muted; }
	void set_mic_muted(std::optional<bool> muted) { mic_muted_ = muted; }
	// Switches between docked (windowed) and auto-hide (fullscreen). The
	// fullscreen panel always starts hidden.
	void set_fullscreen(bool fullscreen);
	bool fullscreen() const { return fullscreen_; }
	// Kiosk mode (StreamOptions::kiosk): no fullscreen button and no
	// minimize icon; close stays.
	void set_kiosk(bool kiosk) { kiosk_ = kiosk; }
	// Whether the windowed toolbar shows the native-size icon. Off under
	// spectre -A, where the window already sets the remote size.
	void set_native_size_shown(bool shown) { native_size_shown_ = shown; }

	// Window pixels the video must leave free at the top: the band's height
	// when docked, 0 in fullscreen, where the panel floats over the video.
	float reserved_height(const UiFont &font) const;
	// The docked band's height, whichever mode the toolbar is in now.
	static float docked_height(const UiFont &font);

	bool visible() const { return !fullscreen_ || revealed_; }
	// Fullscreen only: whether window x `x` is inside the hidden panel's
	// span, where pushing against the top edge reveals it (as of the last
	// build()). Always false windowed.
	bool in_reveal_span(float x) const { return fullscreen_ && x >= bar_.x && x < bar_.x + bar_.w; }

	// Pointer at window pixel `x`,`y`: hovers the button under it and, in
	// fullscreen, reveals or hides the panel. Returns true when the pointer
	// is on the toolbar, i.e. belongs to it rather than to the remote.
	// Hit-tests against the layout of the last build().
	bool handle_pointer_motion(float x, float y);
	// Left button down at window pixel `x`,`y`: the action of the button
	// under it, if any.
	ToolbarAction handle_pointer_press(float x, float y);
	// The pointer left the window, or was captured for relative mode:
	// nothing is hovered any more and the fullscreen panel hides.
	void handle_pointer_left();

	// Lays the toolbar out for a win_w x win_h window and `font`, records
	// the rectangles the pointer handlers hit-test against, and appends the
	// primitives when visible. Called every frame either way: the hidden
	// fullscreen panel's span is what the reveal test checks.
	void build(UiDrawList &out, const UiFont &font, int win_w, int win_h);

private:
	// What an icon button draws instead of a label: the font is ASCII
	// only, so these are drawn (see draw_icon()).
	enum class Icon { kNone, kMinimize, kClose, kNativeSize, kSpeaker, kSpeakerMuted, kMic, kMicMuted };
	// The window controls, set apart from the session's buttons.
	static bool window_control(Icon icon) {
		return icon == Icon::kMinimize || icon == Icon::kClose || icon == Icon::kNativeSize;
	}
	struct Button {
		ToolbarAction action;
		std::string label;
		bool active = false;     // a toggle that is on
		bool danger = false;     // drawn in the warning color
		Icon icon = Icon::kNone; // square, label unused
	};
	struct Rect {
		float x = 0, y = 0, w = 0, h = 0;
		bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
	};

	std::vector<Button> buttons() const;
	// Draws `icon` centred in `r`, `size` px square with `stroke` px lines.
	static void draw_icon(UiDrawList &out, Icon icon, const Rect &r, float size, float stroke, UiColor color);
	int hit_test(float x, float y) const;

	std::string title_;
	bool capture_mouse_ = false;
	std::optional<bool> speaker_muted_;
	std::optional<bool> mic_muted_;
	bool fullscreen_ = false;
	bool kiosk_ = false;
	bool native_size_shown_ = true;
	bool revealed_ = false;
	int hovered_ = -1;
	// From the last build(): the bar (the docked band, or the fullscreen
	// panel whether shown or not), one rect per button, and how far the
	// pointer may stray from the panel before it hides.
	Rect bar_;
	std::vector<Rect> button_rects_;
	float hide_margin_ = 0.0f;
};

} // namespace spectre
