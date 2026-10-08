// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Pins gdp-spec.md §12's error-code numbers. They are on the wire (in
// LobbyError and in every QUIC CONNECTION_CLOSE), so once peers exist they
// never change: this fails if one does, and if lobby.proto's LobbyErrorCode
// and gdp::ErrorCode drift apart.
#include "gdp/error_codes.hpp"

#include "lobby.pb.h"

// Plain assert()s: make sure a Release build (-DNDEBUG) can't compile them
// away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

namespace {

struct Pinned {
	gdp::ErrorCode code;
	gdp::lobby::LobbyErrorCode proto;
	uint64_t number;
	const char *text;
};

constexpr Pinned kTable[] = {
	{gdp::ErrorCode::kNone, gdp::lobby::LOBBY_ERROR_UNSPECIFIED, 0, "closed"},
	{gdp::ErrorCode::kVersionMismatch, gdp::lobby::LOBBY_ERROR_VERSION_MISMATCH, 10,
		"protocol version mismatch"},
	{gdp::ErrorCode::kMalformedFrame, gdp::lobby::LOBBY_ERROR_MALFORMED_FRAME, 11, "malformed frame"},
	{gdp::ErrorCode::kFrameTooLarge, gdp::lobby::LOBBY_ERROR_FRAME_TOO_LARGE, 12, "frame too large"},
	{gdp::ErrorCode::kNoCommonCodec, gdp::lobby::LOBBY_ERROR_NO_COMMON_CODEC, 13, "no common video codec"},
	{gdp::ErrorCode::kUnknownSessionType, gdp::lobby::LOBBY_ERROR_UNKNOWN_SESSION_TYPE, 14,
		"unknown session type"},
	{gdp::ErrorCode::kUnsupported, gdp::lobby::LOBBY_ERROR_UNSUPPORTED, 15, "not supported by the peer"},
	{gdp::ErrorCode::kAuthFailed, gdp::lobby::LOBBY_ERROR_AUTH_FAILED, 20, "authentication failed"},
	{gdp::ErrorCode::kHostAuthFailed, gdp::lobby::LOBBY_ERROR_HOST_AUTH_FAILED, 21,
		"the host rejected the password"},
	{gdp::ErrorCode::kNotEntitled, gdp::lobby::LOBBY_ERROR_NOT_ENTITLED, 22, "not entitled to that host"},
	{gdp::ErrorCode::kLocalSessionActive, gdp::lobby::LOBBY_ERROR_LOCAL_SESSION_ACTIVE, 23,
		"logged in locally on the host"},
	{gdp::ErrorCode::kAlreadyConnected, gdp::lobby::LOBBY_ERROR_ALREADY_CONNECTED, 24,
		"already connected from another client"},
	{gdp::ErrorCode::kHostOffline, gdp::lobby::LOBBY_ERROR_HOST_OFFLINE, 30, "host offline"},
	{gdp::ErrorCode::kHostFull, gdp::lobby::LOBBY_ERROR_HOST_FULL, 31, "host has no free session slots"},
	{gdp::ErrorCode::kSessionStartFailed, gdp::lobby::LOBBY_ERROR_SESSION_START_FAILED, 32,
		"session start failed"},
	{gdp::ErrorCode::kSessionEnded, gdp::lobby::LOBBY_ERROR_SESSION_ENDED, 40, "session ended"},
	{gdp::ErrorCode::kEndedByLocalLogin, gdp::lobby::LOBBY_ERROR_ENDED_BY_LOCAL_LOGIN, 41,
		"signed out by a login on the host itself"},
	{gdp::ErrorCode::kTakenOver, gdp::lobby::LOBBY_ERROR_TAKEN_OVER, 42,
		"session taken over by another client"},
};

} // namespace

int main() {
	for (const Pinned &p : kTable) {
		// The number is what goes on the wire, in both spellings.
		assert(static_cast<uint64_t>(p.code) == p.number);
		assert(static_cast<uint64_t>(p.proto) == p.number);
		assert(gdp::describe_error_code(p.number) == p.text);
	}

	// Every number in the groups' ranges that is not in the table has no
	// name of its own: it is described by its group.
	for (uint64_t code = 1; code < 50; ++code) {
		bool known = false;
		for (const Pinned &p : kTable) {
			known = known || p.number == code;
		}
		if (known) {
			continue;
		}
		std::string text = gdp::describe_error_code(code);
		assert(text.find("application error " + std::to_string(code)) != std::string::npos);
		bool grouped = gdp::error_group(code) >= 10 && gdp::error_group(code) <= 40;
		assert(grouped == (text.rfind("application error", 0) != 0));
	}
	assert(gdp::describe_error_code(25) == "access refused (application error 25)");
	assert(gdp::describe_error_code(999) == "application error 999");
	assert(gdp::error_group(0) == 0 && gdp::error_group(9) == 0 && gdp::error_group(24) == 20);

	printf("error_codes_test: all checks passed\n");
	return 0;
}
