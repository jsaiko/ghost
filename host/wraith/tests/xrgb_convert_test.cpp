// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// xrgb_to_i420()'s SIMD paths must be byte-identical to the scalar one, not
// merely close. Lossless refinement hashes source pixels to decide what has settled and
// compares them against what the client already holds, so a conversion that
// drifted by a least-significant bit would show up as tiles that never stop
// being re-sent -- and a plain software H.264 session would just quietly
// have wrong colours.
//
// This runs whichever implementation the host CPU dispatches to (see
// xrgb_to_i420_impl_name()) against the reference, so on a machine without
// AVX2 it degenerates to comparing scalar with itself and still passes.
#include "encode/software/xrgb_convert.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

// Compares the two implementations over one frame size, filled by `fill`.
void compare(const char *label, uint32_t width, uint32_t height, uint8_t (*fill)(size_t)) {
	// A stride wider than the row, since the compositor's readback buffer is
	// not required to be tightly packed and the vector paths index by it.
	uint32_t stride = width * 4 + 64;
	std::vector<uint8_t> src((size_t)stride * height);
	for (size_t i = 0; i < src.size(); i++) {
		src[i] = fill(i);
	}

	size_t y_size = (size_t)width * height;
	size_t chroma = (size_t)((width + 1) / 2) * ((height + 1) / 2);
	std::vector<uint8_t> want(y_size + 2 * chroma, 0xaa);
	std::vector<uint8_t> got(y_size + 2 * chroma, 0x55);

	wraith::xrgb_to_i420_scalar(src.data(), width, height, stride, want.data(), want.data() + y_size,
		want.data() + y_size + chroma);
	wraith::xrgb_to_i420(src.data(), width, height, stride, got.data(), got.data() + y_size,
		got.data() + y_size + chroma);

	size_t diff = 0;
	size_t first = 0;
	for (size_t i = 0; i < want.size(); i++) {
		if (want[i] != got[i]) {
			if (diff == 0) {
				first = i;
			}
			diff++;
		}
	}
	if (diff != 0) {
		fprintf(stderr, "%s (%ux%u): %zu of %zu bytes differ, first at %zu (want %u, got %u)\n", label, width,
			height, diff, want.size(), first, want[first], got[first]);
	}
	CHECK(diff == 0);
}

uint8_t fill_gradient(size_t i) {
	return (uint8_t)i;
}

// Spread out enough to exercise the full 0..255 range of each channel
// independently rather than a correlated ramp.
uint8_t fill_scattered(size_t i) {
	return (uint8_t)((i * 2654435761u) >> 13);
}

uint8_t fill_saturated(size_t i) {
	// Alternating extremes: the coefficient sums are largest and smallest at
	// the channel extremes, which is where a wrong shift or a saturating
	// intermediate would show.
	return (i % 3 == 0) ? 0xff : ((i % 3 == 1) ? 0x00 : 0x80);
}

} // namespace

int main() {
	printf("xrgb_convert_test: dispatching to \"%s\"\n", wraith::xrgb_to_i420_impl_name());

	// Real output sizes.
	compare("gradient", 1920, 1080, fill_gradient);
	compare("scattered", 1920, 1080, fill_scattered);
	compare("saturated", 1920, 1080, fill_saturated);
	compare("scattered", 3840, 2160, fill_scattered);
	compare("scattered", 2560, 1440, fill_scattered);

	// Odd and small sizes, where the vector loops hand off to their scalar
	// tails -- including widths below one vector's worth, so the tail runs
	// alone, and odd widths/heights, where the chroma planes round up and the
	// last chroma column has only one source pixel to read.
	compare("scattered", 1919, 1079, fill_scattered);
	compare("scattered", 17, 3, fill_scattered);
	compare("scattered", 7, 5, fill_scattered);
	compare("scattered", 1, 1, fill_scattered);
	compare("saturated", 15, 15, fill_saturated);
	compare("scattered", 64, 64, fill_scattered);

	if (g_failures != 0) {
		fprintf(stderr, "xrgb_convert_test: %d check(s) failed\n", g_failures);
		return 1;
	}
	printf("xrgb_convert_test: ok\n");
	return 0;
}
