// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// The two protocol identifiers every GDP peer must agree on
// (gdp-spec.md §2.1, §4.2, §16). The ALPN string is the compatibility
// boundary: a change to what existing bytes mean bumps it. The wire
// version is what LobbyHello.protocol_version carries; ghostd's
// GDP_WIRE_VERSION (host/ghostd/src/lobby.rs) is the other copy.
#pragma once

#include <cstdint>

namespace gdp {

// ALPN for both the lobby and the session connections.
inline constexpr const char *kAlpn = "gdp/1";

// LobbyHello.protocol_version.
inline constexpr uint32_t kWireVersion = 1;

} // namespace gdp
