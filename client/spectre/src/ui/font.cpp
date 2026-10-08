// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/font.hpp"

#include "ui_font_ttf.h" // kUiFontTtf: resources/fonts/DejaVuSans-subset.ttf, embedded by CMake

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>

namespace spectre {

namespace {

constexpr int kFirstChar = 0x20;
constexpr int kCharCount = 0x7F - 0x20; // 0x20..0x7E
// Rasterize at 2x in each axis and let the linear sampler average it
// down: noticeably crisper stems at small sizes than plain 1x coverage.
constexpr unsigned kOversample = 2;

} // namespace

bool UiFont::build(int pixel_height) {
	pixel_height = std::clamp(pixel_height, 6, 128);

	stbtt_fontinfo info;
	if (!stbtt_InitFont(&info, kUiFontTtf, stbtt_GetFontOffsetForIndex(kUiFontTtf, 0))) {
		return false;
	}
	float scale = stbtt_ScaleForPixelHeight(&info, (float)pixel_height);
	int ascent = 0, descent = 0, line_gap = 0;
	stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);

	// Smallest power-of-two atlas the glyph set fits in at this size. The
	// previous build is dropped first, so a failure leaves the font
	// invalid rather than pairing an old atlas with new metrics.
	*this = UiFont();
	std::vector<stbtt_packedchar> packed(kCharCount);
	for (int size = 128; size <= 4096; size *= 2) {
		std::vector<uint8_t> pixels((size_t)size * size, 0);
		stbtt_pack_context pack;
		if (!stbtt_PackBegin(&pack, pixels.data(), size, size, 0, 1, nullptr)) {
			return false;
		}
		stbtt_PackSetOversampling(&pack, kOversample, kOversample);
		int ok = stbtt_PackFontRange(&pack, kUiFontTtf, 0, (float)pixel_height, kFirstChar, kCharCount,
			packed.data());
		stbtt_PackEnd(&pack);
		if (!ok) {
			continue; // didn't fit; try the next size up
		}
		atlas_ = std::move(pixels);
		atlas_width_ = size;
		atlas_height_ = size;
		break;
	}
	if (atlas_.empty()) {
		return false;
	}

	pixel_height_ = pixel_height;
	ascent_ = (int)std::lround(ascent * scale);
	line_height_ = (int)std::lround((ascent - descent + line_gap) * scale);

	glyphs_.resize(kCharCount);
	for (int i = 0; i < kCharCount; i++) {
		const stbtt_packedchar &p = packed[i];
		Glyph &g = glyphs_[i];
		g.x0 = (float)p.x0;
		g.y0 = (float)p.y0;
		g.x1 = (float)p.x1;
		g.y1 = (float)p.y1;
		g.xoff = p.xoff;
		g.yoff = p.yoff;
		g.xoff2 = p.xoff2;
		g.yoff2 = p.yoff2;
		g.xadvance = p.xadvance;
	}
	return true;
}

const UiFont::Glyph &UiFont::glyph(char c) const {
	static const Glyph kEmpty{};
	if (glyphs_.empty()) {
		return kEmpty;
	}
	unsigned char uc = (unsigned char)c;
	if (uc < kFirstChar || uc >= kFirstChar + kCharCount) {
		uc = '?';
	}
	return glyphs_[uc - kFirstChar];
}

float UiFont::text_width(const std::string &text) const {
	float width = 0.0f;
	for (char c : text) {
		width += glyph(c).xadvance;
	}
	return width;
}

} // namespace spectre
