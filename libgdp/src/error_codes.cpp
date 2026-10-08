// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/error_codes.hpp"

namespace gdp {

std::string describe_error_code(uint64_t code) {
	switch (static_cast<ErrorCode>(code)) {
	case ErrorCode::kNone: return "closed";
	case ErrorCode::kVersionMismatch: return "protocol version mismatch";
	case ErrorCode::kMalformedFrame: return "malformed frame";
	case ErrorCode::kFrameTooLarge: return "frame too large";
	case ErrorCode::kNoCommonCodec: return "no common video codec";
	case ErrorCode::kUnknownSessionType: return "unknown session type";
	case ErrorCode::kUnsupported: return "not supported by the peer";
	case ErrorCode::kAuthFailed: return "authentication failed";
	case ErrorCode::kHostAuthFailed: return "the host rejected the password";
	case ErrorCode::kNotEntitled: return "not entitled to that host";
	case ErrorCode::kLocalSessionActive: return "logged in locally on the host";
	case ErrorCode::kAlreadyConnected: return "already connected from another client";
	case ErrorCode::kHostOffline: return "host offline";
	case ErrorCode::kHostFull: return "host has no free session slots";
	case ErrorCode::kSessionStartFailed: return "session start failed";
	case ErrorCode::kSessionEnded: return "session ended";
	case ErrorCode::kEndedByLocalLogin: return "signed out by a login on the host itself";
	case ErrorCode::kTakenOver: return "session taken over by another client";
	}
	// A code this build doesn't know: say what kind of failure its group is.
	const char *kind = nullptr;
	switch (error_group(code)) {
	case 10: kind = "protocol error"; break;
	case 20: kind = "access refused"; break;
	case 30: kind = "host unavailable"; break;
	case 40: kind = "session ended"; break;
	}
	std::string text = "application error " + std::to_string(code);
	return kind ? std::string(kind) + " (" + text + ")" : text;
}

} // namespace gdp
