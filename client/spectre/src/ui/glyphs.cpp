// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace spectre {

namespace {

struct Vec {
	float x, y;
};

float length(float x, float y) {
	return std::sqrt(x * x + y * y);
}

// Distance from `p` to the segment `a`-`b`.
float segment_distance(Vec p, Vec a, Vec b) {
	float dx = b.x - a.x, dy = b.y - a.y;
	float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
	return length(p.x - a.x - t * dx, p.y - a.y - t * dy);
}

// Distance from `p` to the arc of radius `radius` around `c` between
// angles `from` and `to` (radians, y down, from < to).
float arc_distance(Vec p, Vec c, float radius, float from, float to) {
	float angle = std::atan2(p.y - c.y, p.x - c.x);
	if (angle >= from && angle <= to) {
		return std::fabs(length(p.x - c.x, p.y - c.y) - radius);
	}
	Vec a{c.x + radius * std::cos(from), c.y + radius * std::sin(from)};
	Vec b{c.x + radius * std::cos(to), c.y + radius * std::sin(to)};
	return std::min(length(p.x - a.x, p.y - a.y), length(p.x - b.x, p.y - b.y));
}

// What covers a point of a glyph: its body, and a mark over it in another
// color (a mute X or slash).
struct Cover {
	bool body = false;
	bool mark = false;
};

// Draws a glyph defined over the unit square, `size` px square at `x`,`y`,
// anti-aliased: each pixel is sampled 4x4 and drawn at the share that is
// covered, with runs of equal coverage along a row merged into one rect.
template <typename Shape>
void draw_shape(UiDrawList &out, float x, float y, int size, Shape shape, UiColor body, UiColor mark) {
	constexpr int kSamples = 4;
	std::vector<int> body_cover((size_t)size), mark_cover((size_t)size);
	auto emit = [&](const std::vector<int> &cover, float row, UiColor color) {
		for (int i = 0; i < size;) {
			int j = i;
			while (j < size && cover[(size_t)j] == cover[(size_t)i]) {
				j++;
			}
			if (cover[(size_t)i] > 0) {
				UiColor c = color;
				c.a *= (float)cover[(size_t)i] / (kSamples * kSamples);
				out.rect(x + (float)i, row, (float)(j - i), 1.0f, c);
			}
			i = j;
		}
	};
	for (int py = 0; py < size; py++) {
		for (int px = 0; px < size; px++) {
			int b = 0, m = 0;
			for (int sy = 0; sy < kSamples; sy++) {
				for (int sx = 0; sx < kSamples; sx++) {
					Cover c = shape(Vec{((float)px + ((float)sx + 0.5f) / kSamples) / (float)size,
						((float)py + ((float)sy + 0.5f) / kSamples) / (float)size});
					b += c.body;
					m += c.mark;
				}
			}
			body_cover[(size_t)px] = b;
			mark_cover[(size_t)px] = m;
		}
		emit(body_cover, y + (float)py, body);
		emit(mark_cover, y + (float)py, mark);
	}
}

} // namespace

void draw_audio_glyph(UiDrawList &out, AudioGlyph glyph, float x, float y, int size, float stroke,
	UiColor color, UiColor mark) {
	const float half = 0.5f * stroke / (float)size; // half a line, in glyph units
	constexpr float kPi = 3.14159265f;
	switch (glyph) {
	case AudioGlyph::kSpeaker:
	case AudioGlyph::kSpeakerMuted: {
		const bool muted = glyph == AudioGlyph::kSpeakerMuted;
		draw_shape(
			out, x, y, size,
			[&](Vec p) {
				Cover c;
				// A box, and a cone widening to the right from it.
				bool box = p.x >= 0.06f && p.x <= 0.28f && std::fabs(p.y - 0.5f) <= 0.13f;
				bool cone = p.x >= 0.27f && p.x <= 0.5f &&
					std::fabs(p.y - 0.5f) <= 0.13f + (p.x - 0.27f) * (0.36f - 0.13f) / (0.5f - 0.27f);
				c.body = box || cone;
				if (muted) {
					c.mark = segment_distance(p, {0.64f, 0.36f}, {0.92f, 0.64f}) <= half ||
						segment_distance(p, {0.64f, 0.64f}, {0.92f, 0.36f}) <= half;
				} else {
					const Vec centre{0.46f, 0.5f};
					c.body = c.body || arc_distance(p, centre, 0.22f, -0.27f * kPi, 0.27f * kPi) <= half ||
						arc_distance(p, centre, 0.4f, -0.3f * kPi, 0.3f * kPi) <= half;
				}
				return c;
			},
			color, mark);
		break;
	}
	case AudioGlyph::kMic:
	case AudioGlyph::kMicMuted: {
		const bool muted = glyph == AudioGlyph::kMicMuted;
		const Vec slash_a{0.16f, 0.1f}, slash_b{0.84f, 0.9f};
		draw_shape(
			out, x, y, size,
			[&](Vec p) {
				Cover c;
				// A capsule in a U-shaped cradle, on a stem and base.
				c.body = segment_distance(p, {0.5f, 0.2f}, {0.5f, 0.42f}) <= 0.13f ||
					arc_distance(p, {0.5f, 0.44f}, 0.25f, 0.0f, kPi) <= half ||
					segment_distance(p, {0.5f, 0.69f}, {0.5f, 0.86f}) <= half ||
					segment_distance(p, {0.34f, 0.88f}, {0.66f, 0.88f}) <= half;
				if (muted) {
					// Struck through, with a gap cut either side of the stroke so
					// it stands apart from the microphone.
					float d = segment_distance(p, slash_a, slash_b);
					c.mark = d <= half;
					c.body = c.body && d > 3 * half;
				}
				return c;
			},
			color, mark);
		break;
	}
	}
}

} // namespace spectre
