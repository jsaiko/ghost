// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// UdpSocket on Winsock. What differs from udp_socket_posix.cpp:
//  - no batched receive (Windows has no recvmmsg): one recvfrom() a packet;
//  - no source-address pinning (that needs WSARecvMsg/WSASendMsg): a
//    listener's replies leave from whatever address routing picks, which
//    only matters on a multi-homed host -- and only clients run on Windows;
//  - the don't-fragment bit is IP_DONTFRAGMENT / IPV6_DONTFRAG;
//  - an unconnected socket has SIO_UDP_CONNRESET off, or one ICMP port
//    unreachable (a client that went away) would fail its next receive.
#include "udp_socket.hpp"

#include <mswsock.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <vector>

namespace gdp {

namespace {

// See udp_socket_posix.cpp.
constexpr size_t kRecvBufferSize = 2048;

} // namespace

UdpSocket::~UdpSocket() {
	if (fd_ != kInvalidSocket) {
		close_socket(fd_);
	}
}

void UdpSocket::set_common_options() {
	u_long nonblocking = 1;
	ioctlsocket(fd_, FIONBIO, &nonblocking);
	// See udp_socket_posix.cpp. Windows' default is only 64 KiB.
	int buffer = kSocketBufferSize;
	setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&buffer), sizeof(buffer));
	setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&buffer), sizeof(buffer));
	// Path MTU discovery probes past the path MTU and learns from what is
	// acked; without DF a probe too big would be fragmented and "succeed".
	DWORD on = 1;
	setsockopt(fd_, IPPROTO_IP, IP_DONTFRAGMENT, reinterpret_cast<const char *>(&on), sizeof(on));
	if (local_.storage.ss_family == AF_INET6) {
		setsockopt(fd_, IPPROTO_IPV6, IPV6_DONTFRAG, reinterpret_cast<const char *>(&on), sizeof(on));
	}
}

bool UdpSocket::bind_any(uint16_t port, bool *address_in_use) {
	if (address_in_use) {
		*address_in_use = false;
	}
	fd_ = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (fd_ != kInvalidSocket) {
		DWORD off = 0;
		setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&off), sizeof(off));
		sockaddr_in6 sa{};
		sa.sin6_family = AF_INET6;
		sa.sin6_addr = in6addr_any;
		sa.sin6_port = htons(port);
		local_.set(reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
	} else {
		fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (fd_ == kInvalidSocket) {
			return false;
		}
		sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_ANY);
		sa.sin_port = htons(port);
		local_.set(reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
	}
	set_common_options();
	BOOL report_resets = FALSE;
	DWORD returned = 0;
	WSAIoctl(fd_, SIO_UDP_CONNRESET, &report_resets, sizeof(report_resets), nullptr, 0, &returned, nullptr,
		nullptr);
	if (bind(fd_, local_.get(), local_.len) != 0) {
		if (socket_error() == EADDRINUSE && address_in_use) {
			*address_in_use = true;
		}
		close_socket(fd_);
		fd_ = kInvalidSocket;
		return false;
	}
	local_.len = sizeof(local_.storage);
	getsockname(fd_, local_.get(), &local_.len);
	return true;
}

bool UdpSocket::connect_to(const SockAddr &remote) {
	fd_ = socket(remote.storage.ss_family, SOCK_DGRAM, IPPROTO_UDP);
	if (fd_ == kInvalidSocket) {
		return false;
	}
	local_.storage.ss_family = remote.storage.ss_family;
	set_common_options();
	if (connect(fd_, remote.get(), remote.len) != 0) {
		close_socket(fd_);
		fd_ = kInvalidSocket;
		return false;
	}
	connected_ = true;
	local_.len = sizeof(local_.storage);
	getsockname(fd_, local_.get(), &local_.len);
	return true;
}

int UdpSocket::receive(size_t max_packets, const std::function<void(const Packet &)> &fn) {
	thread_local std::vector<uint8_t> buf(kRecvBufferSize);
	for (size_t received = 0; received < max_packets; received++) {
		Packet pkt;
		sockaddr_storage from{};
		int from_len = sizeof(from);
		int n = recvfrom(fd_, reinterpret_cast<char *>(buf.data()), static_cast<int>(buf.size()), 0,
			reinterpret_cast<sockaddr *>(&from), &from_len);
		if (n < 0) {
			int err = socket_error();
			if (err == EMSGSIZE) {
				continue; // larger than any GDP packet: dropped, as a truncated one is elsewhere
			}
			if (err == EAGAIN || err == EINTR) {
				return 0;
			}
			return err;
		}
		pkt.data = buf.data();
		pkt.len = static_cast<size_t>(n);
		pkt.local = local_;
		if (!connected_) {
			pkt.remote.set(reinterpret_cast<sockaddr *>(&from), from_len);
		}
		fn(pkt);
	}
	return 0;
}

int UdpSocket::send(const uint8_t *data, size_t len, const SockAddr *, const SockAddr *remote) {
	int n;
	if (connected_) {
		n = ::send(fd_, reinterpret_cast<const char *>(data), static_cast<int>(len), 0);
	} else {
		n = sendto(fd_, reinterpret_cast<const char *>(data), static_cast<int>(len), 0, remote->get(),
			remote->len);
	}
	return n < 0 ? socket_error() : 0;
}

} // namespace gdp
