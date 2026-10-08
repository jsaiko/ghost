// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The one font spectre draws its own UI with (toolbar, menu, toasts,
// statistics overlay): a subset of DejaVu Sans (resources/fonts/, Latin-1
// only, ~16KB) embedded in the binary and rasterized at runtime with
// stb_truetype into an R8 coverage atlas at whatever pixel size the display
// scale calls for -- so text is properly anti-aliased at every size rather
// than an integer-scaled pixel grid, and there's still nothing to locate on
// disk. OverlayRenderer uploads the atlas as a texture and draws each glyph
// as a quad using the metrics here; the layout code (ui/) only ever asks
// for text widths and line heights.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spectre {

class UiFont {
public:
	struct Glyph {
		// Atlas rectangle, in texels.
		float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		// Where to draw relative to the pen position on the baseline, in
		// output pixels: the quad spans (pen.x + xoff, baseline + yoff) to
		// (pen.x + xoff2, baseline + yoff2). Smaller than the atlas rect
		// because glyphs are rasterized 2x oversampled (see build()).
		float xoff = 0, yoff = 0, xoff2 = 0, yoff2 = 0;
		float xadvance = 0;
	};

	// Rasterizes the embedded face at `pixel_height` (the font's em
	// height, roughly cap height * 1.4). Replaces any previous build.
	// False only if the atlas couldn't be packed at any size, which the
	// embedded face never fails.
	bool build(int pixel_height);
	bool valid() const { return !atlas_.empty(); }

	int pixel_height() const { return pixel_height_; }
	// Baseline-to-baseline distance, and the baseline's offset below the
	// top of a line box -- layout places text by the top of its line box.
	int line_height() const { return line_height_; }
	int ascent() const { return ascent_; }

	int atlas_width() const { return atlas_width_; }
	int atlas_height() const { return atlas_height_; }
	// atlas_width * atlas_height coverage bytes, row-major.
	const std::vector<uint8_t> &atlas() const { return atlas_; }

	// ASCII 0x20..0x7E; anything else draws as '?'.
	const Glyph &glyph(char c) const;
	float text_width(const std::string &text) const;

private:
	int pixel_height_ = 0;
	int line_height_ = 0;
	int ascent_ = 0;
	int atlas_width_ = 0;
	int atlas_height_ = 0;
	std::vector<uint8_t> atlas_;
	std::vector<Glyph> glyphs_; // indexed by c - 0x20
};

} // namespace spectre
