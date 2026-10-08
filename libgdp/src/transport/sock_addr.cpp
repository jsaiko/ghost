// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "udp_socket.hpp"

namespace gdp {

namespace {

bool is_v4_mapped(const sockaddr_in6 *sa) {
	return IN6_IS_ADDR_V4MAPPED(&sa->sin6_addr);
}

} // namespace

bool SockAddr::is_ipv4() const {
	if (storage.ss_family == AF_INET) {
		return true;
	}
	return storage.ss_family == AF_INET6 && is_v4_mapped(reinterpret_cast<const sockaddr_in6 *>(&storage));
}

uint16_t SockAddr::port() const {
	if (storage.ss_family == AF_INET) {
		return ntohs(reinterpret_cast<const sockaddr_in *>(&storage)->sin_port);
	}
	if (storage.ss_family == AF_INET6) {
		return ntohs(reinterpret_cast<const sockaddr_in6 *>(&storage)->sin6_port);
	}
	return 0;
}

std::string SockAddr::host() const {
	char buf[INET6_ADDRSTRLEN] = {};
	if (storage.ss_family == AF_INET) {
		inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(&storage)->sin_addr, buf, sizeof(buf));
		return buf;
	}
	if (storage.ss_family == AF_INET6) {
		const auto *sa6 = reinterpret_cast<const sockaddr_in6 *>(&storage);
		if (is_v4_mapped(sa6)) {
			inet_ntop(AF_INET, &sa6->sin6_addr.s6_addr[12], buf, sizeof(buf));
		} else {
			inet_ntop(AF_INET6, &sa6->sin6_addr, buf, sizeof(buf));
		}
		return buf;
	}
	return "?";
}

std::string SockAddr::to_string() const {
	char buf[INET6_ADDRSTRLEN] = {};
	if (storage.ss_family == AF_INET) {
		inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in *>(&storage)->sin_addr, buf, sizeof(buf));
		return std::string(buf) + ":" + std::to_string(port());
	}
	if (storage.ss_family == AF_INET6) {
		inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 *>(&storage)->sin6_addr, buf, sizeof(buf));
		return "[" + std::string(buf) + "]:" + std::to_string(port());
	}
	return "?";
}

} // namespace gdp
