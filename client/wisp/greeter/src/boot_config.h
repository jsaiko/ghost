// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// What the Wisp boot server told this client: which Veil to sign in to
// and the fingerprint its certificate must have. Both arrive on the kernel
// command line (veil=host:port veil_cert=<64 hex>, rendered by the boot
// server's entrypoint from VEIL_HOST / VEIL_CERT_SHA256); --veil and
// --veil-cert override them for running the greeter on a desktop.
#pragma once

#include <QString>

#include <cstdint>

struct BootConfig {
	QString veil_host; // without the port; brackets stripped from IPv6
	uint16_t veil_port = 0;
	QString veil_cert; // 64 lowercase hex digits, no colons
	QString error;     // non-empty: unusable, and why (the greeter refuses to sign in)

	// Reads /proc/cmdline, then applies the overrides (empty = not given).
	static BootConfig load(const QString &veil_override, const QString &cert_override);
};

// "host", "host:port", "[v6]" or "[v6]:port"; a bare IPv6 literal is all
// host. False on a malformed port. Same rules as spectre-qt's host field.
bool parse_host_and_port(const QString &input, uint16_t default_port, QString *host, uint16_t *port);

// Strips colons and spaces and lowercases; empty unless 64 hex digits.
QString normalize_fingerprint(const QString &input);
