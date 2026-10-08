// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/hotkey.hpp"

#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cctype>

namespace spectre {

const char *const kDefaultMenuHotkey = "lctrl+lalt+lgui";

namespace {

struct Alias {
	const char *name;
	SDL_Scancode a;
	SDL_Scancode b; // SDL_SCANCODE_UNKNOWN when sided
};

// Both spellings of each side (Super/Win/Meta/GUI all mean the same key --
// SDL calls it GUI, Linux calls it Super, Windows calls it Win).
const Alias kAliases[] = {
	{"ctrl", SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL},
	{"control", SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL},
	{"lctrl", SDL_SCANCODE_LCTRL, SDL_SCANCODE_UNKNOWN},
	{"rctrl", SDL_SCANCODE_RCTRL, SDL_SCANCODE_UNKNOWN},
	{"alt", SDL_SCANCODE_LALT, SDL_SCANCODE_RALT},
	{"lalt", SDL_SCANCODE_LALT, SDL_SCANCODE_UNKNOWN},
	{"ralt", SDL_SCANCODE_RALT, SDL_SCANCODE_UNKNOWN},
	{"shift", SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT},
	{"lshift", SDL_SCANCODE_LSHIFT, SDL_SCANCODE_UNKNOWN},
	{"rshift", SDL_SCANCODE_RSHIFT, SDL_SCANCODE_UNKNOWN},
	{"super", SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI},
	{"win", SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI},
	{"meta", SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI},
	{"gui", SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI},
	{"lsuper", SDL_SCANCODE_LGUI, SDL_SCANCODE_UNKNOWN},
	{"lwin", SDL_SCANCODE_LGUI, SDL_SCANCODE_UNKNOWN},
	{"lmeta", SDL_SCANCODE_LGUI, SDL_SCANCODE_UNKNOWN},
	{"lgui", SDL_SCANCODE_LGUI, SDL_SCANCODE_UNKNOWN},
	{"rsuper", SDL_SCANCODE_RGUI, SDL_SCANCODE_UNKNOWN},
	{"rwin", SDL_SCANCODE_RGUI, SDL_SCANCODE_UNKNOWN},
	{"rmeta", SDL_SCANCODE_RGUI, SDL_SCANCODE_UNKNOWN},
	{"rgui", SDL_SCANCODE_RGUI, SDL_SCANCODE_UNKNOWN},
};

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string trim(const std::string &s) {
	size_t start = s.find_first_not_of(" \t");
	if (start == std::string::npos) {
		return "";
	}
	size_t end = s.find_last_not_of(" \t");
	return s.substr(start, end - start + 1);
}

} // namespace

bool HotkeyChord::involves(SDL_Scancode scancode) const {
	for (const auto &alternatives : keys) {
		if (std::find(alternatives.begin(), alternatives.end(), scancode) != alternatives.end()) {
			return true;
		}
	}
	return false;
}

bool HotkeyChord::all_held(const bool *keystate) const {
	for (const auto &alternatives : keys) {
		bool held = false;
		for (SDL_Scancode sc : alternatives) {
			if (keystate[sc]) {
				held = true;
				break;
			}
		}
		if (!held) {
			return false;
		}
	}
	return !keys.empty();
}

std::string HotkeyChord::describe() const {
	std::string out;
	for (const auto &alternatives : keys) {
		if (!out.empty()) {
			out += "+";
		}
		const char *name = SDL_GetScancodeName(alternatives.front());
		std::string label = (name && *name) ? name : "?";
		if (alternatives.size() > 1) {
			// Unsided: strip the "Left " SDL puts on the first alternative.
			if (label.rfind("Left ", 0) == 0) {
				label = label.substr(5);
			}
		}
		out += label;
	}
	return out;
}

bool parse_hotkey_chord(const std::string &spec, HotkeyChord *out, std::string *error) {
	HotkeyChord chord;
	size_t pos = 0;
	while (pos <= spec.size()) {
		size_t plus = spec.find('+', pos);
		std::string token =
			trim(spec.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos));
		pos = (plus == std::string::npos) ? spec.size() + 1 : plus + 1;
		if (token.empty()) {
			if (error) {
				*error = "empty key name in \"" + spec + "\"";
			}
			return false;
		}
		std::string name = lower(token);
		std::vector<SDL_Scancode> alternatives;
		for (const Alias &alias : kAliases) {
			if (name == alias.name) {
				alternatives.push_back(alias.a);
				if (alias.b != SDL_SCANCODE_UNKNOWN) {
					alternatives.push_back(alias.b);
				}
				break;
			}
		}
		if (alternatives.empty()) {
			// SDL's own names, with '_' standing in for the spaces SDL
			// uses ("Scroll Lock", "Keypad 5") so the spec stays one
			// shell word. SDL's lookup is case-insensitive already.
			std::string sdl_name = token;
			std::replace(sdl_name.begin(), sdl_name.end(), '_', ' ');
			SDL_Scancode sc = SDL_GetScancodeFromName(sdl_name.c_str());
			if (sc == SDL_SCANCODE_UNKNOWN) {
				if (error) {
					*error = "unknown key \"" + token + "\"";
				}
				return false;
			}
			alternatives.push_back(sc);
		}
		chord.keys.push_back(std::move(alternatives));
	}
	// The loop runs at least once and rejects an empty name, so an empty
	// spec never gets here.
	*out = std::move(chord);
	return true;
}

} // namespace spectre
