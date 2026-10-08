// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// UdpSocket on POSIX sockets (Linux, macOS); udp_socket_win.cpp is Winsock.
#if defined(__APPLE__)
// in6_pktinfo and IPV6_RECVPKTINFO (RFC 3542) are behind this on macOS.
#define __APPLE_USE_RFC_3542
#endif
#include "udp_socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/ip.h>
#include <unistd.h>

#include <cerrno>
#include <vector>

namespace gdp {

namespace {

// Larger than any packet a GDP peer sends: every endpoint advertises
// kMaxUdpPayload (quic_conn.hpp) as its max_udp_payload_size. A bigger one
// arrives truncated and is dropped.
constexpr size_t kRecvBufferSize = 2048;
constexpr size_t kRecvBatch = 32;

} // namespace

UdpSocket::~UdpSocket() {
	if (fd_ >= 0) {
		close(fd_);
	}
}

void UdpSocket::set_common_options() {
	fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
	fcntl(fd_, F_SETFD, FD_CLOEXEC);
	int on = 1;
	(void)on;
	// Room for a keyframe's burst, which arrives faster than the network
	// thread may get to it.
	// Linux caps it quietly (net.core.rmem_max / wmem_max); macOS refuses
	// anything past kern.ipc.maxsockbuf outright and keeps its ~40 KiB
	// default, so step down until a size is taken.
	for (int option : {SO_RCVBUF, SO_SNDBUF}) {
		for (int buffer = kSocketBufferSize; buffer >= (256 << 10); buffer /= 2) {
			if (setsockopt(fd_, SOL_SOCKET, option, &buffer, sizeof(buffer)) == 0) {
				break;
			}
		}
	}
	// The don't-fragment bit, without the kernel's own PMTU cache clamping
	// what we send: QUIC's path MTU discovery probes past it and learns
	// from what gets acked (IP_PMTUDISC_PROBE), where DO would fail the
	// probe locally on a stale cached value.
#if defined(__linux__)
	int probe = IP_PMTUDISC_PROBE;
	setsockopt(fd_, IPPROTO_IP, IP_MTU_DISCOVER, &probe, sizeof(probe));
	if (local_.storage.ss_family == AF_INET6) {
		int probe6 = IPV6_PMTUDISC_PROBE;
		setsockopt(fd_, IPPROTO_IPV6, IPV6_MTU_DISCOVER, &probe6, sizeof(probe6));
	}
#elif defined(IP_DONTFRAG)
	setsockopt(fd_, IPPROTO_IP, IP_DONTFRAG, &on, sizeof(on));
	if (local_.storage.ss_family == AF_INET6) {
		setsockopt(fd_, IPPROTO_IPV6, IPV6_DONTFRAG, &on, sizeof(on));
	}
#endif
}

bool UdpSocket::bind_any(uint16_t port, bool *address_in_use) {
	if (address_in_use) {
		*address_in_use = false;
	}
	fd_ = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd_ >= 0) {
		int off = 0;
		setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
		sockaddr_in6 sa{};
		sa.sin6_family = AF_INET6;
		sa.sin6_addr = in6addr_any;
		sa.sin6_port = htons(port);
		local_.set(reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
		int on = 1;
		setsockopt(fd_, IPPROTO_IPV6, IPV6_RECVPKTINFO, &on, sizeof(on));
	} else {
		// No IPv6 on this host.
		fd_ = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd_ < 0) {
			return false;
		}
		sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_ANY);
		sa.sin_port = htons(port);
		local_.set(reinterpret_cast<sockaddr *>(&sa), sizeof(sa));
		int on = 1;
#if defined(IP_PKTINFO)
		setsockopt(fd_, IPPROTO_IP, IP_PKTINFO, &on, sizeof(on));
#elif defined(IP_RECVDSTADDR)
		setsockopt(fd_, IPPROTO_IP, IP_RECVDSTADDR, &on, sizeof(on));
#endif
	}
	set_common_options();
	if (bind(fd_, local_.get(), local_.len) != 0) {
		if (errno == EADDRINUSE && address_in_use) {
			*address_in_use = true;
		}
		close(fd_);
		fd_ = -1;
		return false;
	}
	// The port actually bound, when `port` was 0.
	local_.len = sizeof(local_.storage);
	getsockname(fd_, local_.get(), &local_.len);
	return true;
}

bool UdpSocket::connect_to(const SockAddr &remote) {
	fd_ = socket(remote.storage.ss_family, SOCK_DGRAM, 0);
	if (fd_ < 0) {
		return false;
	}
	local_.storage.ss_family = remote.storage.ss_family;
	set_common_options();
	if (connect(fd_, remote.get(), remote.len) != 0) {
		close(fd_);
		fd_ = -1;
		return false;
	}
	connected_ = true;
	local_.len = sizeof(local_.storage);
	getsockname(fd_, local_.get(), &local_.len);
	return true;
}

namespace {

// The packet's destination address, from its IPV6_PKTINFO/IP_PKTINFO
// control message, with the socket's own port. Falls back to the socket's
// address as bound.
SockAddr local_address_of(const msghdr &msg, const SockAddr &bound) {
	SockAddr local = bound;
	for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(const_cast<msghdr *>(&msg), c)) {
		if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO) {
			in6_pktinfo info;
			memcpy(&info, CMSG_DATA(c), sizeof(info));
			reinterpret_cast<sockaddr_in6 *>(&local.storage)->sin6_addr = info.ipi6_addr;
		}
#if defined(IP_PKTINFO)
		if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO && local.storage.ss_family == AF_INET) {
			in_pktinfo info;
			memcpy(&info, CMSG_DATA(c), sizeof(info));
			reinterpret_cast<sockaddr_in *>(&local.storage)->sin_addr = info.ipi_addr;
		}
#elif defined(IP_RECVDSTADDR)
		if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVDSTADDR &&
			local.storage.ss_family == AF_INET) {
			memcpy(&reinterpret_cast<sockaddr_in *>(&local.storage)->sin_addr, CMSG_DATA(c), sizeof(in_addr));
		}
#endif
	}
	return local;
}

constexpr size_t kControlSize = CMSG_SPACE(sizeof(in6_pktinfo)) + CMSG_SPACE(sizeof(in_addr)) + 64;

} // namespace

int UdpSocket::receive(size_t max_packets, const std::function<void(const Packet &)> &fn) {
	struct Slot {
		uint8_t data[kRecvBufferSize];
		sockaddr_storage remote;
		alignas(cmsghdr) uint8_t control[kControlSize];
	};
	thread_local std::vector<Slot> slots(kRecvBatch);
#if defined(__linux__)
	thread_local std::vector<mmsghdr> msgs(kRecvBatch);
	thread_local std::vector<iovec> iovs(kRecvBatch);
#endif

	size_t received = 0;
	while (received < max_packets) {
#if defined(__linux__)
		size_t batch = std::min(kRecvBatch, max_packets - received);
		for (size_t i = 0; i < batch; i++) {
			iovs[i] = {slots[i].data, kRecvBufferSize};
			msghdr &h = msgs[i].msg_hdr;
			h = {};
			h.msg_name = &slots[i].remote;
			h.msg_namelen = sizeof(slots[i].remote);
			h.msg_iov = &iovs[i];
			h.msg_iovlen = 1;
			h.msg_control = slots[i].control;
			h.msg_controllen = kControlSize;
			msgs[i].msg_len = 0;
		}
		int n = recvmmsg(fd_, msgs.data(), batch, MSG_DONTWAIT, nullptr);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				return 0;
			}
			return errno;
		}
		for (int i = 0; i < n; i++) {
			const msghdr &h = msgs[i].msg_hdr;
			if (h.msg_flags & MSG_TRUNC) {
				continue;
			}
			Packet pkt;
			pkt.data = slots[i].data;
			pkt.len = msgs[i].msg_len;
			if (connected_) {
				pkt.local = local_;
				pkt.remote.len = 0;
			} else {
				pkt.remote.set(reinterpret_cast<sockaddr *>(&slots[i].remote), h.msg_namelen);
				pkt.local = local_address_of(h, local_);
			}
			fn(pkt);
		}
		received += n;
		if ((size_t)n < batch) {
			return 0;
		}
#else
		Slot &slot = slots[0];
		iovec iov{slot.data, kRecvBufferSize};
		msghdr h{};
		h.msg_name = &slot.remote;
		h.msg_namelen = sizeof(slot.remote);
		h.msg_iov = &iov;
		h.msg_iovlen = 1;
		h.msg_control = slot.control;
		h.msg_controllen = kControlSize;
		ssize_t n = recvmsg(fd_, &h, MSG_DONTWAIT);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				return 0;
			}
			return errno;
		}
		received++;
		if (h.msg_flags & MSG_TRUNC) {
			continue;
		}
		Packet pkt;
		pkt.data = slot.data;
		pkt.len = (size_t)n;
		if (connected_) {
			pkt.local = local_;
		} else {
			pkt.remote.set(reinterpret_cast<sockaddr *>(&slot.remote), h.msg_namelen);
			pkt.local = local_address_of(h, local_);
		}
		fn(pkt);
#endif
	}
	return 0;
}

int UdpSocket::send(const uint8_t *data, size_t len, const SockAddr *local, const SockAddr *remote) {
	iovec iov{const_cast<uint8_t *>(data), len};
	msghdr h{};
	h.msg_iov = &iov;
	h.msg_iovlen = 1;
	alignas(cmsghdr) uint8_t control[kControlSize] = {};
	if (!connected_) {
		h.msg_name = const_cast<sockaddr *>(remote->get());
		h.msg_namelen = remote->len;
		// Pin the source address to the one the peer sent to, which a
		// multi-homed host's routing wouldn't necessarily pick.
		if (local && local->storage.ss_family == AF_INET6) {
			const auto *sa = reinterpret_cast<const sockaddr_in6 *>(&local->storage);
			if (!IN6_IS_ADDR_UNSPECIFIED(&sa->sin6_addr)) {
				h.msg_control = control;
				h.msg_controllen = CMSG_SPACE(sizeof(in6_pktinfo));
				cmsghdr *c = CMSG_FIRSTHDR(&h);
				c->cmsg_level = IPPROTO_IPV6;
				c->cmsg_type = IPV6_PKTINFO;
				c->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
				in6_pktinfo info{};
				info.ipi6_addr = sa->sin6_addr;
				memcpy(CMSG_DATA(c), &info, sizeof(info));
			}
		}
#if defined(IP_PKTINFO)
		else if (local && local->storage.ss_family == AF_INET) {
			const auto *sa = reinterpret_cast<const sockaddr_in *>(&local->storage);
			if (sa->sin_addr.s_addr != htonl(INADDR_ANY)) {
				h.msg_control = control;
				h.msg_controllen = CMSG_SPACE(sizeof(in_pktinfo));
				cmsghdr *c = CMSG_FIRSTHDR(&h);
				c->cmsg_level = IPPROTO_IP;
				c->cmsg_type = IP_PKTINFO;
				c->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
				in_pktinfo info{};
				info.ipi_spec_dst = sa->sin_addr;
				memcpy(CMSG_DATA(c), &info, sizeof(info));
			}
		}
#endif
	}
	for (;;) {
		ssize_t n = sendmsg(fd_, &h, MSG_DONTWAIT);
		if (n >= 0) {
			return 0;
		}
		if (errno == EINTR) {
			continue;
		}
		return errno;
	}
}

} // namespace gdp
