// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The configurable chord that opens the session menu (spectre -k, default
// "lctrl+lalt+lgui"). A chord is a set of keys that must all be held; it
// fires on the key-down that completes it. Sided names ("lctrl") match
// one scancode, unsided ones ("ctrl") either side, so "ctrl+alt+super"
// is a valid, looser spelling of the default.
#pragma once

#include <SDL3/SDL_scancode.h>

#include <string>
#include <vector>

namespace spectre {

struct HotkeyChord {
	// One entry per key in the chord; each entry lists the scancodes that
	// satisfy it (two for an unsided modifier, one otherwise).
	std::vector<std::vector<SDL_Scancode>> keys;

	// True if `scancode` is one of the chord's keys at all -- the cheap
	// pre-check before consulting the keyboard state.
	bool involves(SDL_Scancode scancode) const;

	// True if every key of the chord has one of its scancodes down in
	// `keystate` (SDL_GetKeyboardState()'s array, indexed by scancode).
	bool all_held(const bool *keystate) const;

	// Human-readable form, e.g. "Left Ctrl+Left Alt+Left GUI", for logs.
	std::string describe() const;
};

// Parses "lctrl+lalt+lgui" style specs (case-insensitive; '+' separated;
// modifier aliases plus any SDL scancode name with '_' for spaces, e.g.
// "f12", "scroll_lock"). Returns false with *error set on an unknown or
// empty key name (an empty spec is one empty name).
bool parse_hotkey_chord(const std::string &spec, HotkeyChord *out, std::string *error);

// The compiled-in default: left Ctrl + left Alt + left Super/Win.
extern const char *const kDefaultMenuHotkey;

} // namespace spectre
