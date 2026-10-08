// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// spectre's own preferences: settings changed from inside a session (the
// session menu) that should still hold next time, as opposed to the
// per-connection options spectre-qt passes on the command line. One
// "key=value" line per setting in spectre.conf under SDL_GetPrefPath()
// (~/.local/share/spectre/spectre/ on Linux, spectre\spectre under
// %APPDATA% on Windows). Unknown keys and unparsable values are ignored, so
// a file from a newer or older spectre never stops one from starting.
#pragma once

namespace spectre {

struct Prefs {
	// Multiplier on relative (captured) mouse motion; absolute motion
	// maps the window onto the remote output 1:1 and ignores it.
	double mouse_sensitivity = 1.0;
	// The view the session menu last picked: the remote picture at 1:1,
	// panned by pushing against the window's edges, rather than scaled to
	// fit the window. spectre -V overrides it for one session.
	bool actual_size = false;
	// The menu's Lossless Refinement row: whether the host builds the
	// lossless layer (off sends RefinePause). spectre -R overrides it for
	// one session.
	bool lossless = true;
};

inline constexpr double kMinMouseSensitivity = 0.25;
inline constexpr double kMaxMouseSensitivity = 4.0;

// The saved preferences, or the defaults for anything missing.
Prefs load_prefs();
// Writes `prefs` out; logs and carries on if it can't.
void save_prefs(const Prefs &prefs);

} // namespace spectre
