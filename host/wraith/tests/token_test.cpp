// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Plain-assert unit test (same convention as libgdp/tests) for the
// redirect-token verifier. The accepted token is the fixed vector from
// gdp-spec.md §4.8; authticket's unit test mints the same
// string from the same inputs, which is what ties the two implementations
// together.
#include "session/token.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace wraith;

namespace {

// gdp-spec.md §4.8's test vector.
const uint32_t kUid = 1000;
const int64_t kExpiry = 1700000000;
const char *const kToken = "AAECAwQFBgcICQoLDA0ODwDxU2UAAAAAHY7kB4TExZ85hTOXTtXACSjrhGKISi6aE3-_pISD_XI";

std::vector<uint8_t> secret() {
	std::vector<uint8_t> s(32);
	for (size_t i = 0; i < s.size(); i++) {
		s[i] = static_cast<uint8_t>(i);
	}
	return s;
}

void test_vector_verifies() {
	assert(verify_redirect_token(kToken, secret(), kUid, kExpiry - 10));
	assert(verify_redirect_token(kToken, secret(), kUid, kExpiry)); // expiry itself is still valid
}

void test_expired() {
	assert(!verify_redirect_token(kToken, secret(), kUid, kExpiry + 1));
}

void test_wrong_uid() {
	assert(!verify_redirect_token(kToken, secret(), kUid + 1, kExpiry - 10));
}

void test_wrong_secret() {
	std::vector<uint8_t> s = secret();
	s[0] ^= 0x01;
	assert(!verify_redirect_token(kToken, s, kUid, kExpiry - 10));
}

void test_tampered() {
	std::string t = kToken;
	// Second-to-last base64 char: fully inside the tag's final byte. (The
	// last char's two low bits are padding the decoder ignores, so a
	// one-bit change there is not a tamper.)
	t[t.size() - 2] = (t[t.size() - 2] == 'A') ? 'B' : 'A';
	assert(!verify_redirect_token(t, secret(), kUid, kExpiry - 10));

	// Flip a nonce character: the MAC no longer matches.
	t = kToken;
	t[0] = 'B';
	assert(!verify_redirect_token(t, secret(), kUid, kExpiry - 10));

	// Flip an expiry character: still fails the MAC even though the
	// decoded expiry may be in the future.
	t = kToken;
	t[22] = (t[22] == 'x') ? 'y' : 'x';
	assert(!verify_redirect_token(t, secret(), kUid, kExpiry - 10));
}

void test_gate_accepts_once() {
	RedirectTokenGate gate(secret(), kUid);
	assert(gate.accept(kToken, kExpiry - 10));
	assert(!gate.accept(kToken, kExpiry - 5));

	// The same token spelled differently (the last character's two
	// ignored bits) is still the same token.
	std::string respelled = kToken;
	respelled.back() = static_cast<char>(respelled.back() + 1);
	assert(verify_redirect_token(respelled, secret(), kUid, kExpiry - 10));
	assert(!gate.accept(respelled, kExpiry - 5));
}

void test_gate_rejects_invalid() {
	RedirectTokenGate gate(secret(), kUid);
	assert(!gate.accept(kToken, kExpiry + 1));
	assert(!gate.accept("not base64url!", kExpiry - 10));
	// A refused token isn't spent.
	assert(gate.accept(kToken, kExpiry - 10));
}

void test_malformed() {
	assert(!verify_redirect_token("", secret(), kUid, kExpiry - 10));
	assert(!verify_redirect_token("not base64url!", secret(), kUid, kExpiry - 10));
	assert(!verify_redirect_token(std::string(kToken) + "=", secret(), kUid, kExpiry - 10)); // padding
	assert(!verify_redirect_token(std::string(kToken).substr(0, 40), secret(), kUid, kExpiry - 10));
	assert(!verify_redirect_token(std::string(kToken) + "AAAA", secret(), kUid, kExpiry - 10));
}

} // namespace

int main() {
	test_vector_verifies();
	test_expired();
	test_wrong_uid();
	test_wrong_secret();
	test_tampered();
	test_malformed();
	test_gate_accepts_once();
	test_gate_rejects_invalid();
	printf("token_test: ok\n");
	return 0;
}
