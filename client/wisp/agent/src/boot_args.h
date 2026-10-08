// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// What the boot server put on the kernel command line for the agent:
// veil=host[:port], veil_cert=<sha256> (the same two the greeter reads,
// client/wisp/greeter/src/boot_config.h) and wisp_key=<64 hex>. Flags
// override each one, for running the agent on a desktop.
#pragma once

#include <cstdint>
#include <string>

struct BootArgs {
	std::string veil_host; // without the port; brackets stripped from IPv6
	uint16_t veil_port = 0;
	std::string veil_cert; // 64 lowercase hex digits
	std::string wisp_key;  // 64 lowercase hex digits
	std::string error;     // non-empty: unusable, and why

	// Reads /proc/cmdline; a non-empty override replaces its value.
	static BootArgs load(const std::string &veil_override, const std::string &cert_override,
		const std::string &key_override);
};
