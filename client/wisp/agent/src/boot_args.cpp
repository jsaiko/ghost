// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "boot_args.h"

#include "gdp/cert_fingerprint.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

constexpr uint16_t kVeilPortDefault = 4442; // veild's [lobby] port default (host/veil/src/config.rs)

std::string cmdline_value(const std::vector<std::string> &args, const std::string &key) {
	const std::string prefix = key + '=';
	for (const std::string &arg : args) {
		if (arg.compare(0, prefix.size(), prefix) == 0) {
			return arg.substr(prefix.size());
		}
	}
	return std::string();
}

bool parse_port(const std::string &text, uint16_t *port) {
	if (text.empty() || text.size() > 5) {
		return false;
	}
	unsigned value = 0;
	for (char c : text) {
		if (!isdigit((unsigned char)c)) {
			return false;
		}
		value = value * 10 + (c - '0');
	}
	if (value == 0 || value > 65535) {
		return false;
	}
	*port = (uint16_t)value;
	return true;
}

// "host", "host:port", "[v6]" or "[v6]:port"; a bare IPv6 literal is all
// host. As the greeter's parse_host_and_port().
bool parse_host_and_port(const std::string &input, std::string *host, uint16_t *port) {
	*port = kVeilPortDefault;
	if (!input.empty() && input[0] == '[') {
		size_t close = input.find(']');
		if (close == std::string::npos) {
			return false;
		}
		std::string rest = input.substr(close + 1);
		if (!rest.empty() && (rest[0] != ':' || !parse_port(rest.substr(1), port))) {
			return false;
		}
		*host = input.substr(1, close - 1);
		return !host->empty();
	}
	size_t colon = input.rfind(':');
	if (colon == std::string::npos || input.find(':') != colon) {
		*host = input;
		return !host->empty();
	}
	*host = input.substr(0, colon);
	return !host->empty() && parse_port(input.substr(colon + 1), port);
}

std::string normalize_hex(const std::string &input) {
	std::string hex;
	for (char c : input) {
		if (c != ':' && c != ' ') {
			hex += (char)tolower((unsigned char)c);
		}
	}
	return gdp::is_sha256_hex(hex) ? hex : std::string();
}

} // namespace

BootArgs BootArgs::load(const std::string &veil_override, const std::string &cert_override,
	const std::string &key_override) {
	std::vector<std::string> args;
	std::ifstream file("/proc/cmdline");
	std::string word;
	while (file >> word) {
		args.push_back(word);
	}
	std::string veil = !veil_override.empty() ? veil_override : cmdline_value(args, "veil");
	std::string cert = !cert_override.empty() ? cert_override : cmdline_value(args, "veil_cert");
	std::string key = !key_override.empty() ? key_override : cmdline_value(args, "wisp_key");

	BootArgs boot;
	if (veil.empty()) {
		boot.error = "no veil= on the kernel command line";
	} else if (!parse_host_and_port(veil, &boot.veil_host, &boot.veil_port)) {
		boot.error = "veil=" + veil + " is not host[:port]";
	} else if ((boot.veil_cert = normalize_hex(cert)).empty()) {
		boot.error = "no valid veil_cert= on the kernel command line (64 hex digits)";
	} else if ((boot.wisp_key = normalize_hex(key)).empty()) {
		// Same shape as a fingerprint: 32 bytes in hex.
		boot.error =
			"no valid wisp_key= on the kernel command line (64 hex digits; the boot server's WISP_KEY)";
	}
	return boot;
}
