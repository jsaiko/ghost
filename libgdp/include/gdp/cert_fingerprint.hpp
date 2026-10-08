// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Certificate fingerprints for host pinning (gdp-spec.md §2.3): the lowercase hex SHA-256 of a
// certificate's DER encoding, the same value `openssl x509 -noout -fingerprint -sha256` prints
// (minus its colons and case). SHA-256 is implemented here rather than borrowed: it predates libgdp
// linking OpenSSL (for QUIC's TLS), and stays so this file has no dependency of its own.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gdp {

std::array<uint8_t, 32> sha256(const uint8_t *data, size_t len);

// 64 lowercase hex digits: the wire and storage form (Redirect.cert_sha256,
// SessionReady.cert_sha256, spectre -P, spectre-qt's known_hosts).
std::string sha256_hex(const uint8_t *data, size_t len);

// Whether `s` is a well-formed sha256_hex() value.
bool is_sha256_hex(const std::string &s);

// For showing a person: uppercase, colon-separated byte pairs, the form
// browsers and `openssl x509 -fingerprint` print.
std::string format_fingerprint(const std::string &sha256_hex);

} // namespace gdp
