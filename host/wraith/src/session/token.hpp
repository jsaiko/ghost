// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Verifies the session token ghostd mints for spectre (gdp-spec.md §4.8)
// against the session secret ghostd hands wraith over the control socket
// (SessionInit, seat_client.hpp). host/authticket/src/lib.rs (mint_session_token) is the paired
// implementation.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wraith {

// Returns true only if `token` decodes to the expected length, its HMAC
// tag verifies (constant-time) against `session_secret` and wraith's own
// `uid`, and its embedded expiry hasn't passed `now_unix`.
bool verify_redirect_token(const std::string &token, const std::vector<uint8_t> &session_secret, uint32_t uid,
	int64_t now_unix);

// verify_redirect_token() that accepts each token once: a token that
// verified is spent, and presenting it again fails until it expires
// (gdp-spec.md §4.8). Spent tokens are keyed by nonce, not by string --
// the last base64url character carries two bits the decoder ignores, so
// one token has several spellings. Safe to call from any thread.
class RedirectTokenGate {
public:
	RedirectTokenGate(std::vector<uint8_t> session_secret, uint32_t uid);

	bool accept(const std::string &token, int64_t now_unix);

private:
	const std::vector<uint8_t> session_secret_;
	const uint32_t uid_;
	std::mutex mutex_;
	std::map<std::array<uint8_t, 16>, int64_t> spent_; // nonce -> expiry
};

} // namespace wraith
