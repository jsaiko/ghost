// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// The few socket basics that differ between Winsock and POSIX, so the rest
// of the transport can be written once: the socket handle type, closing
// one, reading the last socket error, and Winsock's one-time start-up.
//
// Errors are reported everywhere as POSIX errno values (EAGAIN,
// ECONNREFUSED, ...). On Windows socket_error() translates Winsock's own
// codes into them -- MSVC's <cerrno> defines the POSIX names.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>

namespace gdp {

#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;

inline void close_socket(socket_t s) {
	closesocket(s);
}

// The last socket call's error, as an errno value.
inline int socket_error() {
	switch (int err = WSAGetLastError()) {
	case WSAEWOULDBLOCK: return EAGAIN;
	case WSAEINTR: return EINTR;
	case WSAEMSGSIZE: return EMSGSIZE;
	case WSAENOBUFS: return ENOBUFS;
	// A connected UDP socket learns of an ICMP port unreachable as a
	// "reset" on its next receive.
	case WSAECONNRESET:
	case WSAECONNREFUSED: return ECONNREFUSED;
	case WSAEHOSTUNREACH: return EHOSTUNREACH;
	case WSAENETUNREACH:
	case WSAENETRESET: return ENETUNREACH;
	case WSAEADDRINUSE: return EADDRINUSE;
	default: return err;
	}
}

// Winsock needs WSAStartup() before any socket call, name lookups
// included. Once per process; WSACleanup() is never called, as with the
// other libraries' global state (it only matters right before exit).
inline void net_init() {
	static const bool started = [] {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	(void)started;
}
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;

inline void close_socket(socket_t s) {
	close(s);
}

inline int socket_error() {
	return errno;
}

inline void net_init() {}
#endif

} // namespace gdp
