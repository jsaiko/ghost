// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/token.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <iterator>

namespace wraith {

namespace {

constexpr size_t kNonceLen = 16;
constexpr size_t kExpiryLen = 8;
constexpr size_t kTagLen = 32; // HMAC-SHA256

// base64url (RFC 4648 §5), no padding -- the only variant
// authticket's mint_session_token ever produces. Rejects anything else outright
// rather than best-effort decoding a malformed/foreign token.
bool base64url_decode(const std::string &in, std::vector<uint8_t> *out) {
	auto value = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '-') return 62;
		if (c == '_') return 63;
		return -1;
	};
	out->clear();
	out->reserve(in.size() * 3 / 4 + 3);
	uint32_t buf = 0;
	int bits = 0;
	for (char c : in) {
		int v = value(c);
		if (v < 0) {
			return false;
		}
		buf = (buf << 6) | static_cast<uint32_t>(v);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out->push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
		}
	}
	return true;
}

int64_t read_le_i64(const uint8_t *p) {
	uint64_t v = 0;
	for (int i = 7; i >= 0; --i) {
		v = (v << 8) | p[i];
	}
	return static_cast<int64_t>(v);
}

// The check behind both entry points. On success, `nonce` and `expiry`
// (if given) receive the token's own.
bool verify(const std::string &token, const std::vector<uint8_t> &session_secret, uint32_t uid,
	int64_t now_unix, std::array<uint8_t, kNonceLen> *nonce_out, int64_t *expiry_out) {
	std::vector<uint8_t> packed;
	if (!base64url_decode(token, &packed)) {
		return false;
	}
	if (packed.size() != kNonceLen + kExpiryLen + kTagLen) {
		return false;
	}
	const uint8_t *nonce = packed.data();
	const uint8_t *expiry_bytes = packed.data() + kNonceLen;
	const uint8_t *tag = packed.data() + kNonceLen + kExpiryLen;

	const int64_t expiry = read_le_i64(expiry_bytes);
	if (now_unix > expiry) {
		return false;
	}

	// MAC input: uid(LE 4) || nonce(16) || expiry(LE 8) -- must match
	// authticket's mint_session_token() exactly (gdp-spec.md §4.8).
	std::array<uint8_t, 4> uid_le{
		static_cast<uint8_t>(uid & 0xFF),
		static_cast<uint8_t>((uid >> 8) & 0xFF),
		static_cast<uint8_t>((uid >> 16) & 0xFF),
		static_cast<uint8_t>((uid >> 24) & 0xFF),
	};
	std::vector<uint8_t> mac_input;
	mac_input.reserve(uid_le.size() + kNonceLen + kExpiryLen);
	mac_input.insert(mac_input.end(), uid_le.begin(), uid_le.end());
	mac_input.insert(mac_input.end(), nonce, nonce + kNonceLen);
	mac_input.insert(mac_input.end(), expiry_bytes, expiry_bytes + kExpiryLen);

	std::array<uint8_t, kTagLen> computed{};
	unsigned int computed_len = 0;
	if (!HMAC(EVP_sha256(), session_secret.data(), static_cast<int>(session_secret.size()), mac_input.data(),
			mac_input.size(), computed.data(), &computed_len)) {
		return false;
	}

	if (computed_len != kTagLen || CRYPTO_memcmp(computed.data(), tag, kTagLen) != 0) {
		return false;
	}
	if (nonce_out) {
		std::copy(nonce, nonce + kNonceLen, nonce_out->begin());
	}
	if (expiry_out) {
		*expiry_out = expiry;
	}
	return true;
}

} // namespace

bool verify_redirect_token(const std::string &token, const std::vector<uint8_t> &session_secret, uint32_t uid,
	int64_t now_unix) {
	return verify(token, session_secret, uid, now_unix, nullptr, nullptr);
}

RedirectTokenGate::RedirectTokenGate(std::vector<uint8_t> session_secret, uint32_t uid)
	: session_secret_(std::move(session_secret)), uid_(uid) {}

bool RedirectTokenGate::accept(const std::string &token, int64_t now_unix) {
	std::array<uint8_t, kNonceLen> nonce{};
	int64_t expiry = 0;
	if (!verify(token, session_secret_, uid_, now_unix, &nonce, &expiry)) {
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	// Forget tokens that have expired: verify() refuses them on its own.
	for (auto it = spent_.begin(); it != spent_.end();) {
		it = now_unix > it->second ? spent_.erase(it) : std::next(it);
	}
	return spent_.emplace(nonce, expiry).second;
}

} // namespace wraith
