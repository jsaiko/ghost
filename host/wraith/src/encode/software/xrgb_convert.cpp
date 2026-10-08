// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/software/xrgb_convert.hpp"

#include "util/band_pool.hpp"

#include <algorithm>

#if defined(__x86_64__) || defined(__i386__)
#define WRAITH_X86 1
#include <immintrin.h>
#endif

namespace wraith {

namespace {

inline uint8_t clamp_u8(int v) {
	return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

} // namespace

void xrgb_to_i420_scalar(const uint8_t *src, uint32_t width, uint32_t height, uint32_t src_stride,
	uint8_t *y_plane, uint8_t *u_plane, uint8_t *v_plane) {
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *row = src + (size_t)y * src_stride;
		uint8_t *y_row = y_plane + (size_t)y * width;
		for (uint32_t x = 0; x < width; x++) {
			// XRGB8888, little-endian in memory: B, G, R, X.
			int b = row[x * 4 + 0];
			int g = row[x * 4 + 1];
			int r = row[x * 4 + 2];
			y_row[x] = clamp_u8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
		}
	}

	uint32_t chroma_width = (width + 1) / 2;
	uint32_t chroma_height = (height + 1) / 2;
	for (uint32_t cy = 0; cy < chroma_height; cy++) {
		const uint8_t *row = src + (size_t)(cy * 2) * src_stride;
		uint8_t *u_row = u_plane + (size_t)cy * chroma_width;
		uint8_t *v_row = v_plane + (size_t)cy * chroma_width;
		for (uint32_t cx = 0; cx < chroma_width; cx++) {
			uint32_t x = cx * 2;
			int b = row[x * 4 + 0];
			int g = row[x * 4 + 1];
			int r = row[x * 4 + 2];
			u_row[cx] = clamp_u8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
			v_row[cx] = clamp_u8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
		}
	}
}

#if WRAITH_X86

namespace {

// Gathers one channel from 4 BGRX pixels into 4 zero-extended 16-bit lanes.
// shuffle_epi8 is lane-local, so each 128-bit lane handles its own 4 pixels
// and the two lanes together cover 8.
#define WRAITH_CHAN(o) o, -1, o + 4, -1, o + 8, -1, o + 12, -1, -1, -1, -1, -1, -1, -1, -1, -1

} // namespace

// Vectorised identically to the scalar version's arithmetic, including the
// rounding and the shift, so the output is byte-for-byte the same rather
// than merely close -- lossless refinement compares source pixels against what the client
// already has, and "close" would mean tiles that never stop being re-sent.
__attribute__((target("avx2"))) static void xrgb_to_i420_avx2(const uint8_t *src, uint32_t width,
	uint32_t height, uint32_t src_stride, uint8_t *y_plane, uint8_t *u_plane, uint8_t *v_plane) {
	const __m256i bs = _mm256_setr_epi8(WRAITH_CHAN(0), WRAITH_CHAN(0));
	const __m256i gs = _mm256_setr_epi8(WRAITH_CHAN(1), WRAITH_CHAN(1));
	const __m256i rs = _mm256_setr_epi8(WRAITH_CHAN(2), WRAITH_CHAN(2));
	const __m256i bias = _mm256_set1_epi16(128);
	// Each lane's 4 results sit in its low 4 16-bit slots, so packus
	// interleaves 4 payload bytes with 4 of junk per lane; this gathers the
	// payload back down into the low 8 bytes.
	const __m128i compact = _mm_setr_epi8(0, 1, 2, 3, 8, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1, -1);

	// Y stays in *unsigned* 16-bit: 66*255 + 129*255 + 25*255 + 128 = 56228,
	// under 65536, so mullo/add need no widening and the shift is logical.
	const __m256i c66 = _mm256_set1_epi16(66);
	const __m256i c129 = _mm256_set1_epi16(129);
	const __m256i c25 = _mm256_set1_epi16(25);
	const __m256i y_off = _mm256_set1_epi16(16);
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *row = src + (size_t)y * src_stride;
		uint8_t *y_row = y_plane + (size_t)y * width;
		uint32_t x = 0;
		for (; x + 8 <= width; x += 8) {
			__m256i px = _mm256_loadu_si256((const __m256i *)(row + (size_t)x * 4));
			__m256i b = _mm256_shuffle_epi8(px, bs);
			__m256i g = _mm256_shuffle_epi8(px, gs);
			__m256i r = _mm256_shuffle_epi8(px, rs);
			__m256i a = _mm256_add_epi16(_mm256_mullo_epi16(r, c66), _mm256_mullo_epi16(g, c129));
			a = _mm256_add_epi16(a, _mm256_mullo_epi16(b, c25));
			a = _mm256_add_epi16(_mm256_srli_epi16(_mm256_add_epi16(a, bias), 8), y_off);
			_mm_storel_epi64((__m128i *)(y_row + x),
				_mm_shuffle_epi8(_mm_packus_epi16(_mm256_castsi256_si128(a), _mm256_extracti128_si256(a, 1)),
					compact));
		}
		for (; x < width; x++) {
			int b = row[x * 4 + 0];
			int g = row[x * 4 + 1];
			int r = row[x * 4 + 2];
			y_row[x] = clamp_u8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
		}
	}

	// U/V need *signed* 16-bit (negative coefficients) and an arithmetic
	// shift. The result lands in 16..240 either way, so packus's unsigned
	// saturation stands in for clamp_u8() without ever actually clamping.
	const __m256i cu_r = _mm256_set1_epi16(-38);
	const __m256i cu_g = _mm256_set1_epi16(-74);
	const __m256i cu_b = _mm256_set1_epi16(112);
	const __m256i cv_r = _mm256_set1_epi16(112);
	const __m256i cv_g = _mm256_set1_epi16(-94);
	const __m256i cv_b = _mm256_set1_epi16(-18);
	// A pixel is exactly one dword, so "every other pixel" is a dword gather.
	const __m256i even = _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6);
	uint32_t chroma_width = (width + 1) / 2;
	uint32_t chroma_height = (height + 1) / 2;
	for (uint32_t cy = 0; cy < chroma_height; cy++) {
		const uint8_t *row = src + (size_t)(cy * 2) * src_stride;
		uint8_t *u_row = u_plane + (size_t)cy * chroma_width;
		uint8_t *v_row = v_plane + (size_t)cy * chroma_width;
		uint32_t cx = 0;
		// 8 chroma outputs consume 16 source pixels, so the bound is on the
		// source width rather than the chroma width -- an odd `width` makes
		// the last chroma column read a pixel that isn't there otherwise.
		for (; cx + 8 <= chroma_width && cx * 2 + 16 <= width; cx += 8) {
			__m256i p0 = _mm256_loadu_si256((const __m256i *)(row + (size_t)(cx * 2) * 4));
			__m256i p1 = _mm256_loadu_si256((const __m256i *)(row + (size_t)(cx * 2 + 8) * 4));
			// Even pixels of each load into its low lane, then the two low
			// lanes joined: 8 even pixels, 4 per lane, as the shuffles want.
			__m256i px = _mm256_permute2x128_si256(_mm256_permutevar8x32_epi32(p0, even),
				_mm256_permutevar8x32_epi32(p1, even), 0x20);
			__m256i b = _mm256_shuffle_epi8(px, bs);
			__m256i g = _mm256_shuffle_epi8(px, gs);
			__m256i r = _mm256_shuffle_epi8(px, rs);
			__m256i u =
				_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(r, cu_r), _mm256_mullo_epi16(g, cu_g)),
					_mm256_mullo_epi16(b, cu_b));
			__m256i v =
				_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(r, cv_r), _mm256_mullo_epi16(g, cv_g)),
					_mm256_mullo_epi16(b, cv_b));
			u = _mm256_add_epi16(_mm256_srai_epi16(_mm256_add_epi16(u, bias), 8), bias);
			v = _mm256_add_epi16(_mm256_srai_epi16(_mm256_add_epi16(v, bias), 8), bias);
			_mm_storel_epi64((__m128i *)(u_row + cx),
				_mm_shuffle_epi8(_mm_packus_epi16(_mm256_castsi256_si128(u), _mm256_extracti128_si256(u, 1)),
					compact));
			_mm_storel_epi64((__m128i *)(v_row + cx),
				_mm_shuffle_epi8(_mm_packus_epi16(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1)),
					compact));
		}
		for (; cx < chroma_width; cx++) {
			uint32_t x = cx * 2;
			int b = row[x * 4 + 0];
			int g = row[x * 4 + 1];
			int r = row[x * 4 + 2];
			u_row[cx] = clamp_u8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
			v_row[cx] = clamp_u8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
		}
	}
}

#undef WRAITH_CHAN

namespace {

bool have_avx2() {
	static const bool yes = __builtin_cpu_supports("avx2");
	return yes;
}

} // namespace

#endif // WRAITH_X86

namespace {

void convert_one(const uint8_t *src, uint32_t width, uint32_t height, uint32_t src_stride, uint8_t *y_plane,
	uint8_t *u_plane, uint8_t *v_plane) {
#if WRAITH_X86
	if (have_avx2()) {
		xrgb_to_i420_avx2(src, width, height, src_stride, y_plane, u_plane, v_plane);
		return;
	}
#endif
	xrgb_to_i420_scalar(src, width, height, src_stride, y_plane, u_plane, v_plane);
}

// Below this many rows a frame converts fast enough on one thread that
// waking the helpers costs more than it saves.
constexpr uint32_t kBandMinRows = 720;

} // namespace

void xrgb_to_i420(const uint8_t *src, uint32_t width, uint32_t height, uint32_t src_stride, uint8_t *y_plane,
	uint8_t *u_plane, uint8_t *v_plane) {
	BandPool &pool = BandPool::get();
	size_t bands = pool.bands();
	if (height < kBandMinRows || bands == 1) {
		convert_one(src, width, height, src_stride, y_plane, u_plane, v_plane);
		return;
	}
	// Bands start on even rows, so each covers whole chroma rows: rows
	// convert independently and two of them make one U/V row, which keeps
	// the result byte-identical to a single pass.
	uint32_t band_rows = ((height + (uint32_t)bands - 1) / (uint32_t)bands + 1) & ~1u;
	uint32_t chroma_width = (width + 1) / 2;
	pool.run([&](size_t i) {
		uint32_t y0 = (uint32_t)i * band_rows;
		if (y0 >= height) {
			return;
		}
		uint32_t rows = std::min(band_rows, height - y0);
		convert_one(src + (size_t)y0 * src_stride, width, rows, src_stride, y_plane + (size_t)y0 * width,
			u_plane + (size_t)(y0 / 2) * chroma_width, v_plane + (size_t)(y0 / 2) * chroma_width);
	});
}

const char *xrgb_to_i420_impl_name() {
#if WRAITH_X86
	if (have_avx2()) {
		return "avx2";
	}
#endif
	return "scalar";
}

} // namespace wraith
