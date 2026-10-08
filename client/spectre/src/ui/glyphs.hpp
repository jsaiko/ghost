// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Glyphs spectre's UI draws where its ASCII-only font has nothing: the
// speaker and microphone of the mute buttons (ui/toolbar.hpp,
// ui/session_menu.hpp). Drawn from distance functions over the unit
// square and anti-aliased into a UiDrawList's rects, so they scale
// smoothly rather than snapping to whole pixels.
#pragma once

#include "ui/draw_list.hpp"

namespace spectre {

enum class AudioGlyph {
	kSpeaker,      // a speaker sounding
	kSpeakerMuted, // the speaker with an X in the `mark` color
	kMic,          // a microphone on its stand
	kMicMuted,     // the microphone struck through in the `mark` color
};

// Draws `glyph` `size` px square with its top-left at window pixel
// `x`,`y`, its lines `stroke` px wide.
void draw_audio_glyph(UiDrawList &out, AudioGlyph glyph, float x, float y, int size, float stroke,
	UiColor color, UiColor mark);

} // namespace spectre
