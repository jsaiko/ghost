// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The client machine's own output volume -- the local desktop's, not the
// remote session's -- for the session menu's volume row (on a Wisp thin
// client, the only volume control there is). Goes through PipeWire's wpctl on the default sink, so it
// works on outputs without a hardware mixer (HDMI) too; where wpctl isn't
// there or fails, get() says so and the menu leaves the row out. Each call
// runs wpctl and waits for it (a few ms).
#pragma once

#include <optional>

namespace spectre {

struct SystemVolumeState {
	double volume = 0.0; // 0..1 (PipeWire allows more; the menu doesn't)
	bool muted = false;
};

std::optional<SystemVolumeState> get_system_volume();
// Sets the default sink to `volume` (0..1) and unmutes it if `unmute`.
// False if wpctl failed.
bool set_system_volume(double volume, bool unmute);

} // namespace spectre
