// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/cert_fingerprint.hpp"

#include <cstring>

namespace gdp {

namespace {

// FIPS 180-4 §4.2.2.
// clang-format off: a table laid out by hand
constexpr uint32_t kRoundConstants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
// clang-format on

uint32_t rotr(uint32_t x, int n) {
	return (x >> n) | (x << (32 - n));
}

void compress(uint32_t state[8], const uint8_t block[64]) {
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 |
			(uint32_t)block[i * 4 + 2] << 8 | (uint32_t)block[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
	uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 =
			h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRoundConstants[i] + w[i];
		uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}

} // namespace

std::array<uint8_t, 32> sha256(const uint8_t *data, size_t len) {
	uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
		0x5be0cd19};
	size_t full = len / 64;
	for (size_t i = 0; i < full; i++) {
		compress(state, data + i * 64);
	}
	// Padding: 0x80, zeros, then the bit length big-endian in the last 8
	// bytes -- one block, or two when the tail leaves no room for it.
	uint8_t tail[128] = {};
	size_t rest = len - full * 64;
	if (rest) {
		memcpy(tail, data + full * 64, rest);
	}
	tail[rest] = 0x80;
	size_t tail_len = rest < 56 ? 64 : 128;
	uint64_t bits = (uint64_t)len * 8;
	for (int i = 0; i < 8; i++) {
		tail[tail_len - 1 - i] = (uint8_t)(bits >> (i * 8));
	}
	compress(state, tail);
	if (tail_len == 128) {
		compress(state, tail + 64);
	}

	std::array<uint8_t, 32> out;
	for (int i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(state[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
		out[i * 4 + 3] = (uint8_t)state[i];
	}
	return out;
}

std::string sha256_hex(const uint8_t *data, size_t len) {
	static const char kDigits[] = "0123456789abcdef";
	std::array<uint8_t, 32> digest = sha256(data, len);
	std::string hex;
	hex.reserve(64);
	for (uint8_t byte : digest) {
		hex += kDigits[byte >> 4];
		hex += kDigits[byte & 0xf];
	}
	return hex;
}

bool is_sha256_hex(const std::string &s) {
	if (s.size() != 64) {
		return false;
	}
	for (char c : s) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

std::string format_fingerprint(const std::string &sha256_hex) {
	std::string out;
	for (size_t i = 0; i < sha256_hex.size(); i += 2) {
		if (i) {
			out += ':';
		}
		for (size_t j = i; j < i + 2 && j < sha256_hex.size(); j++) {
			char c = sha256_hex[j];
			out += (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c;
		}
	}
	return out;
}

} // namespace gdp
