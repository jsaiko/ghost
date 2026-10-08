// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "prefs.hpp"

#include "log.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

namespace spectre {

namespace {

std::string prefs_path() {
	char *dir = SDL_GetPrefPath("spectre", "spectre");
	if (!dir) {
		return {};
	}
	std::string path = std::string(dir) + "spectre.conf";
	SDL_free(dir);
	return path;
}

} // namespace

Prefs load_prefs() {
	Prefs prefs;
	std::string path = prefs_path();
	std::ifstream in(path);
	std::string line;
	while (std::getline(in, line)) {
		size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string key = line.substr(0, eq);
		std::string value = line.substr(eq + 1);
		if (key == "mouse_sensitivity") {
			char *end = nullptr;
			double v = std::strtod(value.c_str(), &end);
			if (end != value.c_str() && std::isfinite(v)) {
				prefs.mouse_sensitivity = std::clamp(v, kMinMouseSensitivity, kMaxMouseSensitivity);
			}
		} else if (key == "view") {
			if (value == "actual") {
				prefs.actual_size = true;
			} else if (value == "fit") {
				prefs.actual_size = false;
			}
		} else if (key == "lossless") {
			if (value == "on") {
				prefs.lossless = true;
			} else if (value == "off") {
				prefs.lossless = false;
			}
		}
	}
	return prefs;
}

void save_prefs(const Prefs &prefs) {
	std::string path = prefs_path();
	if (path.empty()) {
		SLOG_ERROR("spectre: no preferences directory (%s); settings not saved", SDL_GetError());
		return;
	}
	std::ofstream out(path, std::ios::trunc);
	out << "mouse_sensitivity=" << prefs.mouse_sensitivity << "\n";
	out << "view=" << (prefs.actual_size ? "actual" : "fit") << "\n";
	out << "lossless=" << (prefs.lossless ? "on" : "off") << "\n";
	if (!out) {
		SLOG_ERROR("spectre: could not write %s; settings not saved", path.c_str());
	}
}

} // namespace spectre
