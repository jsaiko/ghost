// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// gdp-spec.md §12's error codes, as used on the QUIC CONNECTION_CLOSE
// application error code (Connection::close(code) / shutdown_error_code()).
// Numerically identical to libgdp/proto/lobby.proto's LobbyErrorCode -- one table
// serves both the LobbyError message and the connection-close path -- but
// kept as a plain enum here so transport.hpp users (wraith's session side,
// which never speaks the lobby protocol) don't need the generated lobby
// headers for it.
#pragma once

#include <cstdint>
#include <string>

namespace gdp {

enum class ErrorCode : uint64_t {
	kNone = 0, // normal close, nothing to report

	// 1x: the peer broke the protocol or asked for something unsupported.
	kVersionMismatch = 10,    // LobbyHello.protocol_version unsupported
	kMalformedFrame = 11,     // protobuf parse failure, or a message out of protocol order
	kFrameTooLarge = 12,      // §3.1 length prefix exceeded 1 MiB
	kNoCommonCodec = 13,      // SessionHello.codecs had nothing the host can encode
	kUnknownSessionType = 14, // SessionOpen.session_type didn't match any advertised type
	kUnsupported = 15,        // a stream kind or feature the peer doesn't implement or didn't negotiate

	// 2x: who you are and what you may use.
	kAuthFailed = 20,         // PAM/token auth failed
	kHostAuthFailed = 21,     // Veil: Veil accepted the password, the host refused it
	kNotEntitled = 22,        // Veil: the selected device isn't one of this user's
	kLocalSessionActive = 23, // this user is logged in on the host itself (ghostd refuses the login)
	kAlreadyConnected = 24,   // this user's session already has its one viewer

	// 3x: the host can't take the session right now.
	kHostOffline = 30,        // Veil: the selected device isn't connected to it
	kHostFull = 31,           // the host has no capacity for another session (every session port taken)
	kSessionStartFailed = 32, // logind/systemd couldn't start wraith

	// 4x: a session that ended on purpose.
	kSessionEnded =
		40, // the desktop session ended (logout, LogoutRequest, or crash) -- not a connection failure
	kEndedByLocalLogin = 41, // the session was ended because its user logged in on the host itself
	kTakenOver = 42,         // another client of the same user displaced this viewer (SessionHello.take_over)
};

// The group a code belongs to: its tens digit times ten (10, 20, 30, 40),
// 0 for kNone. A receiver that doesn't know a code can still treat it as
// its group's kind of failure.
constexpr uint64_t error_group(uint64_t code) {
	return code / 10 * 10;
}

// Human-readable text for a code received from the peer, e.g.
// "authentication failed"; "application error N" for codes this build
// doesn't know (a newer peer's addition to the table).
std::string describe_error_code(uint64_t code);

} // namespace gdp
