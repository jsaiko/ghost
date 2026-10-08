// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// SHA-256 against the FIPS 180-2 / NIST CAVP vectors, including the
// padding edge cases (a 55-byte tail fits the length in one block, 56 and
// 64 spill into a second), plus the fingerprint helpers.
#include "gdp/cert_fingerprint.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

namespace {

std::string hex_of(const std::string &s) {
	return gdp::sha256_hex(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

} // namespace

int main() {
	assert(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	assert(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	assert(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	assert(
		hex_of(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
	assert(
		hex_of(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
	assert(
		hex_of(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
	assert(hex_of(std::string(1000000, 'a')) ==
		"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

	std::string fp = hex_of("abc");
	assert(gdp::is_sha256_hex(fp));
	assert(!gdp::is_sha256_hex(fp.substr(1)));
	assert(!gdp::is_sha256_hex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"));
	assert(gdp::format_fingerprint("ba7816bf") == "BA:78:16:BF");

	printf("cert_fingerprint_test: OK\n");
	return 0;
}
