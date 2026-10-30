// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/fec.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define GDP_FEC_X86 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define GDP_FEC_TARGET(isa)
#else
#define GDP_FEC_TARGET(isa) __attribute__((target(isa)))
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#define GDP_FEC_NEON 1
#include <arm_neon.h>
#endif

namespace gdp {

namespace {

// GF(2^8) with the polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11d), generator 2.
struct GfTables {
	std::array<uint8_t, 512> exp{};
	std::array<uint8_t, 256> log{};
};

const GfTables &gf() {
	static const GfTables tables = [] {
		GfTables t;
		unsigned x = 1;
		for (int i = 0; i < 255; i++) {
			t.exp[i] = (uint8_t)x;
			t.log[x] = (uint8_t)i;
			x <<= 1;
			if (x & 0x100) {
				x ^= 0x11d;
			}
		}
		// Doubled, so exp[log a + log b] needs no reduction mod 255.
		for (int i = 255; i < 512; i++) {
			t.exp[i] = t.exp[i - 255];
		}
		return t;
	}();
	return tables;
}

uint8_t gf_mul(uint8_t a, uint8_t b) {
	if (a == 0 || b == 0) {
		return 0;
	}
	const GfTables &t = gf();
	return t.exp[t.log[a] + t.log[b]];
}

uint8_t gf_inv(uint8_t a) {
	const GfTables &t = gf();
	return t.exp[255 - t.log[a]]; // a != 0: every caller's is
}

// The generator's entry for parity shard j and data shard i (fec.hpp).
uint8_t coefficient(size_t j, size_t i) {
	return gf_inv((uint8_t)((255 - j) ^ i));
}

// c * x for every x below 16, and for every x << 4: a product is the xor of
// its low and high nibbles' products, which is what the shuffle-based
// multiply-adds look up 16 or 32 bytes at a time.
void nibble_tables(uint8_t c, uint8_t lo[16], uint8_t hi[16]) {
	for (unsigned x = 0; x < 16; x++) {
		lo[x] = gf_mul(c, (uint8_t)x);
		hi[x] = gf_mul(c, (uint8_t)(x << 4));
	}
}

void muladd_tail(uint8_t *dst, const uint8_t *src, size_t len, const uint8_t lo[16], const uint8_t hi[16]) {
	for (size_t i = 0; i < len; i++) {
		dst[i] ^= lo[src[i] & 0x0f] ^ hi[src[i] >> 4];
	}
}

// dst[0, len) ^= c * src[0, len).
void muladd_scalar(uint8_t *dst, const uint8_t *src, size_t len, uint8_t c) {
	uint8_t lo[16], hi[16];
	nibble_tables(c, lo, hi);
	muladd_tail(dst, src, len, lo, hi);
}

#if defined(GDP_FEC_X86)
GDP_FEC_TARGET("ssse3")
void muladd_ssse3(uint8_t *dst, const uint8_t *src, size_t len, uint8_t c) {
	alignas(16) uint8_t lo[16], hi[16];
	nibble_tables(c, lo, hi);
	const __m128i tlo = _mm_load_si128((const __m128i *)lo);
	const __m128i thi = _mm_load_si128((const __m128i *)hi);
	const __m128i mask = _mm_set1_epi8(0x0f);
	size_t i = 0;
	for (; i + 16 <= len; i += 16) {
		__m128i s = _mm_loadu_si128((const __m128i *)(src + i));
		__m128i l = _mm_and_si128(s, mask);
		__m128i h = _mm_and_si128(_mm_srli_epi64(s, 4), mask);
		__m128i p = _mm_xor_si128(_mm_shuffle_epi8(tlo, l), _mm_shuffle_epi8(thi, h));
		__m128i d = _mm_loadu_si128((const __m128i *)(dst + i));
		_mm_storeu_si128((__m128i *)(dst + i), _mm_xor_si128(d, p));
	}
	muladd_tail(dst + i, src + i, len - i, lo, hi);
}

GDP_FEC_TARGET("avx2")
void muladd_avx2(uint8_t *dst, const uint8_t *src, size_t len, uint8_t c) {
	alignas(16) uint8_t lo[16], hi[16];
	nibble_tables(c, lo, hi);
	const __m256i tlo = _mm256_broadcastsi128_si256(_mm_load_si128((const __m128i *)lo));
	const __m256i thi = _mm256_broadcastsi128_si256(_mm_load_si128((const __m128i *)hi));
	const __m256i mask = _mm256_set1_epi8(0x0f);
	size_t i = 0;
	for (; i + 32 <= len; i += 32) {
		__m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
		__m256i l = _mm256_and_si256(s, mask);
		__m256i h = _mm256_and_si256(_mm256_srli_epi64(s, 4), mask);
		__m256i p = _mm256_xor_si256(_mm256_shuffle_epi8(tlo, l), _mm256_shuffle_epi8(thi, h));
		__m256i d = _mm256_loadu_si256((const __m256i *)(dst + i));
		_mm256_storeu_si256((__m256i *)(dst + i), _mm256_xor_si256(d, p));
	}
	muladd_tail(dst + i, src + i, len - i, lo, hi);
}

#if defined(_MSC_VER) && !defined(__clang__)
bool cpu_has_ssse3() {
	int r[4];
	__cpuid(r, 1);
	return (r[2] & (1 << 9)) != 0;
}

bool cpu_has_avx2() {
	int r[4];
	__cpuid(r, 0);
	if (r[0] < 7) {
		return false;
	}
	__cpuid(r, 1);
	// AVX and OSXSAVE, and the OS saving the YMM registers.
	if ((r[2] & (1 << 27)) == 0 || (r[2] & (1 << 28)) == 0 || (_xgetbv(0) & 6) != 6) {
		return false;
	}
	__cpuidex(r, 7, 0);
	return (r[1] & (1 << 5)) != 0;
}
#else
bool cpu_has_ssse3() {
	return __builtin_cpu_supports("ssse3");
}

bool cpu_has_avx2() {
	return __builtin_cpu_supports("avx2");
}
#endif
#endif

#if defined(GDP_FEC_NEON)
void muladd_neon(uint8_t *dst, const uint8_t *src, size_t len, uint8_t c) {
	uint8_t lo[16], hi[16];
	nibble_tables(c, lo, hi);
	const uint8x16_t tlo = vld1q_u8(lo);
	const uint8x16_t thi = vld1q_u8(hi);
	const uint8x16_t mask = vdupq_n_u8(0x0f);
	size_t i = 0;
	for (; i + 16 <= len; i += 16) {
		uint8x16_t s = vld1q_u8(src + i);
		uint8x16_t p = veorq_u8(vqtbl1q_u8(tlo, vandq_u8(s, mask)), vqtbl1q_u8(thi, vshrq_n_u8(s, 4)));
		vst1q_u8(dst + i, veorq_u8(vld1q_u8(dst + i), p));
	}
	muladd_tail(dst + i, src + i, len - i, lo, hi);
}
#endif

using MulAdd = void (*)(uint8_t *dst, const uint8_t *src, size_t len, uint8_t c);

struct Implementation {
	MulAdd muladd;
	const char *name;
};

const Implementation &implementation() {
	static const Implementation impl = []() -> Implementation {
		// $GDP_FEC_ISA picks a lesser one than the CPU allows ("ssse3",
		// "scalar"), so the tests run every path on one machine.
		const char *isa = std::getenv("GDP_FEC_ISA");
		std::string wanted = isa ? isa : "";
		if (wanted == "scalar") {
			return {muladd_scalar, "scalar"};
		}
#if defined(GDP_FEC_X86)
		if (cpu_has_avx2() && wanted != "ssse3") {
			return {muladd_avx2, "avx2"};
		}
		if (cpu_has_ssse3()) {
			return {muladd_ssse3, "ssse3"};
		}
#elif defined(GDP_FEC_NEON)
		return {muladd_neon, "neon"};
#endif
		return {muladd_scalar, "scalar"};
	}();
	return impl;
}

void muladd(uint8_t *dst, const uint8_t *src, size_t len, uint8_t c) {
	if (c == 0 || len == 0) {
		return;
	}
	if (c == 1) {
		for (size_t i = 0; i < len; i++) {
			dst[i] ^= src[i];
		}
		return;
	}
	implementation().muladd(dst, src, len, c);
}

// Inverts the e x e matrix `a` (row-major) into `inv` by Gauss-Jordan
// elimination. False if singular, which a Cauchy submatrix never is.
bool invert(std::vector<uint8_t> &a, std::vector<uint8_t> &inv, size_t e) {
	inv.assign(e * e, 0);
	for (size_t i = 0; i < e; i++) {
		inv[i * e + i] = 1;
	}
	for (size_t col = 0; col < e; col++) {
		size_t pivot = col;
		while (pivot < e && a[pivot * e + col] == 0) {
			pivot++;
		}
		if (pivot == e) {
			return false;
		}
		if (pivot != col) {
			std::swap_ranges(a.begin() + pivot * e, a.begin() + pivot * e + e, a.begin() + col * e);
			std::swap_ranges(inv.begin() + pivot * e, inv.begin() + pivot * e + e, inv.begin() + col * e);
		}
		uint8_t scale = gf_inv(a[col * e + col]);
		for (size_t c = 0; c < e; c++) {
			a[col * e + c] = gf_mul(a[col * e + c], scale);
			inv[col * e + c] = gf_mul(inv[col * e + c], scale);
		}
		for (size_t r = 0; r < e; r++) {
			uint8_t f = a[r * e + col];
			if (r == col || f == 0) {
				continue;
			}
			for (size_t c = 0; c < e; c++) {
				a[r * e + c] ^= gf_mul(f, a[col * e + c]);
				inv[r * e + c] ^= gf_mul(f, inv[col * e + c]);
			}
		}
	}
	return true;
}

} // namespace

void fec_encode(const uint8_t *const *data, const size_t *data_len, size_t k, uint8_t *const *parity,
	size_t m, size_t shard_len) {
	for (size_t j = 0; j < m; j++) {
		std::memset(parity[j], 0, shard_len);
		for (size_t i = 0; i < k; i++) {
			muladd(parity[j], data[i], std::min(data_len[i], shard_len), coefficient(j, i));
		}
	}
}

bool fec_recover(uint8_t *const *data, const bool *have_data, size_t k, const uint8_t *const *parity,
	const bool *have_parity, size_t m, size_t shard_len) {
	std::vector<size_t> missing;
	for (size_t i = 0; i < k; i++) {
		if (!have_data[i]) {
			missing.push_back(i);
		}
	}
	const size_t e = missing.size();
	if (e == 0) {
		return true;
	}
	std::vector<size_t> rows;
	for (size_t j = 0; j < m && rows.size() < e; j++) {
		if (have_parity[j]) {
			rows.push_back(j);
		}
	}
	if (rows.size() < e) {
		return false;
	}

	// The chosen parity rows restricted to the missing columns, inverted.
	std::vector<uint8_t> a(e * e), inv;
	for (size_t r = 0; r < e; r++) {
		for (size_t c = 0; c < e; c++) {
			a[r * e + c] = coefficient(rows[r], missing[c]);
		}
	}
	if (!invert(a, inv, e)) {
		return false;
	}

	// Each chosen parity shard less what the present data contributed to
	// it: what the missing shards alone sum to.
	std::vector<uint8_t> syndrome(e * shard_len);
	for (size_t r = 0; r < e; r++) {
		uint8_t *s = syndrome.data() + r * shard_len;
		std::memcpy(s, parity[rows[r]], shard_len);
		for (size_t i = 0; i < k; i++) {
			if (have_data[i]) {
				muladd(s, data[i], shard_len, coefficient(rows[r], i));
			}
		}
	}
	for (size_t c = 0; c < e; c++) {
		uint8_t *out = data[missing[c]];
		std::memset(out, 0, shard_len);
		for (size_t r = 0; r < e; r++) {
			muladd(out, syndrome.data() + r * shard_len, shard_len, inv[c * e + r]);
		}
	}
	return true;
}

const char *fec_implementation() {
	return implementation().name;
}

size_t send_video_parity(const VideoDatagramHeader &hdr, const uint8_t *data, size_t len, size_t max_payload,
	const std::function<size_t(size_t k)> &parity_for,
	const std::function<bool(const uint8_t *datagram, size_t datagram_len)> &send,
	std::vector<uint8_t> *scratch) {
	// The same clamp as slice_video_frame(), so the shards are its slices.
	const size_t shard_len = std::clamp<size_t>(max_payload, 1, kMaxSlicePayload);
	if (len == 0) {
		return 0; // an empty frame's one empty slice has nothing to protect
	}
	const size_t slices = (len + shard_len - 1) / shard_len;
	const size_t blocks = fec_block_count(slices);
	if (blocks == 0) {
		return 0;
	}

	std::array<size_t, kFecMaxBlocks> counts{};
	std::array<size_t, kFecMaxBlocks> first{}; // each block's first shard in *scratch
	size_t total = 0, most = 0;
	for (size_t b = 0; b < blocks; b++) {
		size_t k = fec_block_data(slices, blocks, b);
		counts[b] = std::min(parity_for(k), kFecMaxBlockShards - k);
		first[b] = total;
		total += counts[b];
		most = std::max(most, counts[b]);
	}
	if (total == 0) {
		return 0;
	}
	scratch->resize(total * shard_len);

	for (size_t b = 0; b < blocks; b++) {
		if (counts[b] == 0) {
			continue;
		}
		size_t k = fec_block_data(slices, blocks, b);
		std::array<const uint8_t *, kFecMaxBlockData> shards;
		std::array<size_t, kFecMaxBlockData> shard_lens;
		for (size_t j = 0; j < k; j++) {
			size_t offset = (b + j * blocks) * shard_len;
			shards[j] = data + offset;
			shard_lens[j] = std::min(shard_len, len - offset);
		}
		std::array<uint8_t *, kFecMaxBlockShards> parity;
		for (size_t q = 0; q < counts[b]; q++) {
			parity[q] = scratch->data() + (first[b] + q) * shard_len;
		}
		fec_encode(shards.data(), shard_lens.data(), k, parity.data(), counts[b], shard_len);
	}

	VideoParityHeader ph;
	ph.stream_id = hdr.stream_id;
	ph.frame_id = hdr.frame_id;
	ph.block_count = (uint8_t)blocks;
	ph.flags = hdr.flags;
	ph.pts = hdr.pts;
	ph.frame_len = (uint32_t)len;
	uint8_t datagram[1 + VideoParityHeader::kSize + kMaxSlicePayload];
	size_t sent = 0;
	for (size_t q = 0; q < most; q++) {
		for (size_t b = 0; b < blocks; b++) {
			if (q >= counts[b]) {
				continue;
			}
			ph.block_idx = (uint8_t)b;
			ph.data_shards = (uint8_t)fec_block_data(slices, blocks, b);
			ph.parity_idx = (uint8_t)q;
			ph.parity_count = (uint8_t)counts[b];
			ph.encode(datagram);
			std::memcpy(datagram + 1 + VideoParityHeader::kSize, scratch->data() + (first[b] + q) * shard_len,
				shard_len);
			if (!send(datagram, 1 + VideoParityHeader::kSize + shard_len)) {
				return sent;
			}
			sent += shard_len;
		}
	}
	return sent;
}

} // namespace gdp
