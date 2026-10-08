// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// A non-blocking UDP socket with what QUIC needs from it: the don't-
// fragment bit for path MTU discovery, the local address each packet
// arrived on (so a reply leaves from the address the peer used), and
// batched receives.
#pragma once

#include "socket_platform.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace gdp {

// What each socket asks for as its receive and send buffers.
inline constexpr int kSocketBufferSize = 8 << 20;

// A sockaddr that owns its storage, with the length that goes with it.
struct SockAddr {
	sockaddr_storage storage{};
	socklen_t len = 0;

	sockaddr *get() { return reinterpret_cast<sockaddr *>(&storage); }
	const sockaddr *get() const { return reinterpret_cast<const sockaddr *>(&storage); }
	void set(const sockaddr *sa, socklen_t salen) {
		memcpy(&storage, sa, salen);
		len = salen;
	}
	// IPv4 or v4-mapped IPv6: the path carries IPv4 headers either way.
	bool is_ipv4() const;
	uint16_t port() const;
	std::string to_string() const;
	// The address alone, a v4-mapped IPv6 address as its IPv4 form.
	std::string host() const;
};

class UdpSocket {
public:
	UdpSocket() = default;
	~UdpSocket();
	UdpSocket(const UdpSocket &) = delete;
	UdpSocket &operator=(const UdpSocket &) = delete;

	// Server: binds the "any" address on `port`, dual-stack when the host
	// has IPv6, IPv4 otherwise. `*address_in_use` says whether a failure was only
	// EADDRINUSE.
	bool bind_any(uint16_t port, bool *address_in_use);
	// Client: an ephemeral local port connect()ed to `remote`, so ICMP
	// errors come back as ECONNREFUSED/EHOSTUNREACH on the next receive.
	bool connect_to(const SockAddr &remote);

	socket_t fd() const { return fd_; }
	const SockAddr &local() const { return local_; }

	struct Packet {
		const uint8_t *data;
		size_t len;
		SockAddr remote;
		// The address it arrived on, port included: `local()` with the
		// wildcard address replaced by the real one.
		SockAddr local;
	};
	// Reads everything already queued, up to `max_packets`, handing each
	// packet to `fn`. Returns 0, or the errno of a socket error (a pending
	// ICMP error on a connected socket surfaces here).
	int receive(size_t max_packets, const std::function<void(const Packet &)> &fn);

	// Sends one packet. `local` picks the source address on a wildcard
	// socket (null: let routing pick); `remote` is ignored on a connected
	// socket. Returns 0, EAGAIN/EWOULDBLOCK when the socket buffer is full
	// (retry later), or another errno (the packet is gone).
	int send(const uint8_t *data, size_t len, const SockAddr *local, const SockAddr *remote);

private:
	void set_common_options();

	socket_t fd_ = kInvalidSocket;
	bool connected_ = false;
	SockAddr local_;
};

} // namespace gdp
