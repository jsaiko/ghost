// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Plain-assert unit test for the embedded UI font (ui/font.hpp): the
// face rasterizes at the sizes the display scales produce, every printable
// ASCII glyph gets a non-empty atlas cell and a positive advance, and the
// line metrics are sane. With a path argument, also writes a PGM of a
// sample line rendered through the same glyph metrics the renderer uses,
// for checking the rasterizer by eye.
#include "ui/font.hpp"

#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace spectre;

namespace {

void check_size(int px) {
	UiFont font;
	assert(font.build(px));
	assert(font.valid());
	assert(font.pixel_height() == px);
	assert(font.ascent() > 0 && font.ascent() < px);
	assert(font.line_height() >= px);
	assert(font.atlas_width() >= 128 && font.atlas_height() >= 128);
	for (char c = '!'; c <= '~'; c++) {
		const UiFont::Glyph &g = font.glyph(c);
		assert(g.xadvance > 0.0f);
		assert(g.x1 > g.x0 && g.y1 > g.y0);
	}
	assert(font.glyph(' ').xadvance > 0.0f);
	assert(font.text_width("Hello") > font.text_width("Hi"));
	// Unknown characters fall back to '?' rather than nothing.
	assert(font.glyph('\x01').xadvance == font.glyph('?').xadvance);
}

// Draws `text` the way OverlayRenderer::record_text does (quad per glyph,
// bilinear sample of the atlas), into an 8-bit canvas.
void render_line(const UiFont &font, const std::string &text, int x, int y, std::vector<uint8_t> &canvas,
	int cw, int ch) {
	float pen = (float)x;
	float baseline = (float)(y + font.ascent());
	for (char c : text) {
		const UiFont::Glyph &g = font.glyph(c);
		int qx0 = (int)std::floor(pen + g.xoff + 0.5f), qy0 = (int)std::floor(baseline + g.yoff + 0.5f);
		int qw = (int)std::lround(g.xoff2 - g.xoff), qh = (int)std::lround(g.yoff2 - g.yoff);
		for (int py = 0; py < qh; py++) {
			for (int px = 0; px < qw; px++) {
				float u = g.x0 + (px + 0.5f) / qw * (g.x1 - g.x0);
				float v = g.y0 + (py + 0.5f) / qh * (g.y1 - g.y0);
				int ix = (int)u, iy = (int)v;
				float fx = u - ix, fy = v - iy;
				auto at = [&](int ax, int ay) {
					ax = std::clamp(ax, 0, font.atlas_width() - 1);
					ay = std::clamp(ay, 0, font.atlas_height() - 1);
					return (float)font.atlas()[(size_t)ay * font.atlas_width() + ax];
				};
				float s = at(ix, iy) * (1 - fx) * (1 - fy) + at(ix + 1, iy) * fx * (1 - fy) +
					at(ix, iy + 1) * (1 - fx) * fy + at(ix + 1, iy + 1) * fx * fy;
				int cx = qx0 + px, cy = qy0 + py;
				if (cx >= 0 && cx < cw && cy >= 0 && cy < ch) {
					uint8_t &dst = canvas[(size_t)cy * cw + cx];
					dst = (uint8_t)std::max((int)dst, (int)s);
				}
			}
		}
		pen += g.xadvance;
	}
}

} // namespace

int main(int argc, char **argv) {
	check_size(12);
	check_size(14);
	check_size(28);
	check_size(56);

	if (argc > 1) {
		const int cw = 900, ch = 220;
		std::vector<uint8_t> canvas((size_t)cw * ch, 20);
		int y = 8;
		for (int px : {14, 20, 28}) {
			UiFont font;
			assert(font.build(px));
			render_line(font, "alice@alice-pc  [x] Show statistics  End session", 8, y, canvas, cw, ch);
			y += font.line_height();
			render_line(font, "encode vaapi (hardware)  fps 60.0  rtt 0.4 ms  loss 0/64", 8, y, canvas, cw,
				ch);
			y += font.line_height() + 6;
		}
		FILE *f = fopen(argv[1], "wb");
		assert(f);
		fprintf(f, "P5\n%d %d\n255\n", cw, ch);
		fwrite(canvas.data(), 1, canvas.size(), f);
		fclose(f);
	}
	printf("ui_font_test: ok\n");
	return 0;
}
