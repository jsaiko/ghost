// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// libFuzzer target for the lossless refinement container parser
// (gdp/refine.hpp, gdp-spec.md §9.5): wire-supplied
// counts and lengths drive its offsets and its Zstd output size. Build with
// GDP_ENABLE_FUZZING=ON (needs clang).
#include "gdp/refine.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	const uint8_t *base = nullptr;
	size_t base_len = 0;
	gdp::RefineLayer layer;
	if (!gdp::refine_parse_frame(data, size, &base, &base_len, &layer)) {
		return 0;
	}
	// Touch the reported base bounds so a mislocated base shows up under
	// ASan, and hold parse to its size contract.
	if (base_len > 0) {
		volatile uint8_t sink = base[0];
		sink ^= base[base_len - 1];
		(void)sink;
	}
	if (layer.tile_pixels.size() != gdp::refine_layer_pixel_bytes(layer.tiles)) {
		__builtin_trap();
	}
	return 0;
}
