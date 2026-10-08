// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "net/lan_link.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <dirent.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#elif defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <ntddndis.h>

#include <cstdio>
#include <cstring>
#endif

namespace spectre {

#if defined(__linux__)

namespace {

constexpr int kMinimumMbps = 1000;

// What the kernel would route to `addr` by: the interface index, whether
// the route goes through a gateway, and whether `addr` is this machine.
struct Route {
	int oif = 0;
	bool via_gateway = false;
	bool local = false;
};

bool kernel_route(const sockaddr_storage &addr, Route *out) {
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0) {
		return false;
	}
	struct {
		nlmsghdr nh;
		rtmsg rt;
		char attrs[64];
	} req = {};
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
	req.nh.nlmsg_type = RTM_GETROUTE;
	req.nh.nlmsg_flags = NLM_F_REQUEST;
	req.nh.nlmsg_seq = 1;
	req.rt.rtm_family = (unsigned char)addr.ss_family;
	const void *dst;
	size_t dst_len;
	if (addr.ss_family == AF_INET) {
		dst = &reinterpret_cast<const sockaddr_in &>(addr).sin_addr;
		dst_len = 4;
	} else {
		dst = &reinterpret_cast<const sockaddr_in6 &>(addr).sin6_addr;
		dst_len = 16;
	}
	req.rt.rtm_dst_len = (unsigned char)(dst_len * 8);
	auto *rta = reinterpret_cast<rtattr *>(reinterpret_cast<char *>(&req) + NLMSG_ALIGN(req.nh.nlmsg_len));
	rta->rta_type = RTA_DST;
	rta->rta_len = (unsigned short)RTA_LENGTH(dst_len);
	memcpy(RTA_DATA(rta), dst, dst_len);
	req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) + RTA_ALIGN(rta->rta_len);

	bool ok = false;
	if (send(fd, &req, req.nh.nlmsg_len, 0) == (ssize_t)req.nh.nlmsg_len) {
		char buf[8192];
		ssize_t n = recv(fd, buf, sizeof(buf), 0);
		for (auto *nh = reinterpret_cast<nlmsghdr *>(buf); n > 0 && NLMSG_OK(nh, (unsigned)n);
			nh = NLMSG_NEXT(nh, n)) {
			if (nh->nlmsg_type != RTM_NEWROUTE) {
				continue; // NLMSG_ERROR: no route
			}
			auto *rt = reinterpret_cast<rtmsg *>(NLMSG_DATA(nh));
			out->local = rt->rtm_type == RTN_LOCAL;
			int len = (int)RTM_PAYLOAD(nh);
			for (auto *a = RTM_RTA(rt); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
				if (a->rta_type == RTA_OIF) {
					memcpy(&out->oif, RTA_DATA(a), sizeof(int));
				} else if (a->rta_type == RTA_GATEWAY || a->rta_type == RTA_VIA) {
					out->via_gateway = true;
				}
			}
			ok = out->oif != 0;
			break;
		}
	}
	close(fd);
	return ok;
}

bool exists(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

// The interface's link speed in Mbit/s if it is wired and reports one;
// otherwise the fastest wired interface under it (a bridge's ports, a
// bond's slaves, a VLAN's parent -- each a lower_* link in sysfs); 0 if
// neither. Wi-Fi is 0 whatever it reports.
int wired_speed_mbps(const std::string &name, int depth = 0) {
	const std::string dir = "/sys/class/net/" + name;
	if (depth > 4 || exists(dir + "/wireless") || exists(dir + "/phy80211")) {
		return 0;
	}
	int speed = 0;
	if (FILE *f = fopen((dir + "/speed").c_str(), "r")) {
		if (fscanf(f, "%d", &speed) != 1 || speed < 0) {
			speed = 0; // EINVAL on a link with no speed, -1 when down
		}
		fclose(f);
	}
	if (speed > 0) {
		return speed;
	}
	if (DIR *d = opendir(dir.c_str())) {
		while (dirent *e = readdir(d)) {
			if (strncmp(e->d_name, "lower_", 6) == 0) {
				speed = std::max(speed, wired_speed_mbps(e->d_name + 6, depth + 1));
			}
		}
		closedir(d);
	}
	return speed;
}

} // namespace

LanLink probe_lan_link(const std::string &host, uint16_t port) {
	LanLink link;
	addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo *res = nullptr;
	char port_str[8];
	snprintf(port_str, sizeof(port_str), "%u", port);
	if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
		link.description = "can't resolve " + host;
		return link;
	}
	sockaddr_storage addr = {};
	memcpy(&addr, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);

	Route route;
	if (!kernel_route(addr, &route)) {
		link.description = "no route to " + host;
		return link;
	}
	if (route.local) {
		link.qualifies = true;
		link.description = host + " is this machine";
		return link;
	}
	char ifname[IF_NAMESIZE] = {};
	if (!if_indextoname((unsigned)route.oif, ifname)) {
		link.description = "route to " + host + " has no interface name";
		return link;
	}
	if (route.via_gateway) {
		link.description = std::string(ifname) + " reaches " + host + " through a router";
		return link;
	}
	int mbps = wired_speed_mbps(ifname);
	if (mbps == 0) {
		link.description = std::string(ifname) + " isn't a wired link with a known speed";
		return link;
	}
	link.description = std::string(ifname) + ", wired, " + std::to_string(mbps) + " Mbit/s, direct";
	if (mbps < kMinimumMbps) {
		link.description += " (under 1 Gbit/s)";
		return link;
	}
	link.qualifies = true;
	return link;
}

#elif defined(_WIN32)

namespace {

constexpr ULONG64 kMinimumBitsPerSecond = 1000ull * 1000 * 1000;

// The interface's alias ("Ethernet 2"), for the log.
std::string interface_name(const MIB_IF_ROW2 &row) {
	char name[256] = {};
	WideCharToMultiByte(CP_UTF8, 0, row.Alias, -1, name, sizeof(name) - 1, nullptr, nullptr);
	return name;
}

} // namespace

LanLink probe_lan_link(const std::string &host, uint16_t port) {
	LanLink link;
	// Name lookups need Winsock started; libgdp starts it too, but only once
	// it opens a socket, which is after this.
	static const bool winsock = [] {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	(void)winsock;

	addrinfo hints = {};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo *res = nullptr;
	char port_str[8];
	snprintf(port_str, sizeof(port_str), "%u", port);
	if (getaddrinfo(host.c_str(), port_str, &hints, &res) != 0 || !res) {
		link.description = "can't resolve " + host;
		return link;
	}
	SOCKADDR_INET dest = {};
	memcpy(&dest, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);

	MIB_IPFORWARD_ROW2 route = {};
	SOCKADDR_INET source = {};
	if (GetBestRoute2(nullptr, 0, nullptr, &dest, 0, &route, &source) != NO_ERROR) {
		link.description = "no route to " + host;
		return link;
	}
	MIB_IF_ROW2 row = {};
	row.InterfaceIndex = route.InterfaceIndex;
	if (GetIfEntry2(&row) != NO_ERROR) {
		link.description = "route to " + host + " has no interface";
		return link;
	}
	if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) {
		link.qualifies = true;
		link.description = host + " is this machine";
		return link;
	}
	const std::string name = interface_name(row);
	// An on-link route's next hop is the unspecified address; anything
	// else is a router.
	bool via_gateway = route.NextHop.si_family == AF_INET6
		? !IN6_IS_ADDR_UNSPECIFIED(&route.NextHop.Ipv6.sin6_addr)
		: route.NextHop.Ipv4.sin_addr.s_addr != INADDR_ANY;
	if (via_gateway) {
		link.description = name + " reaches " + host + " through a router";
		return link;
	}
	// Wired Ethernet only: Wi-Fi reports IF_TYPE_IEEE80211, and a virtual
	// adapter whose medium isn't 802.3 doesn't count either.
	if (row.Type != IF_TYPE_ETHERNET_CSMACD || row.PhysicalMediumType != NdisPhysicalMedium802_3 ||
		row.ReceiveLinkSpeed == 0) {
		link.description = name + " isn't a wired link with a known speed";
		return link;
	}
	ULONG64 bps = row.ReceiveLinkSpeed;
	link.description = name + ", wired, " + std::to_string(bps / 1000000) + " Mbit/s, direct";
	if (bps < kMinimumBitsPerSecond) {
		link.description += " (under 1 Gbit/s)";
		return link;
	}
	link.qualifies = true;
	return link;
}

#else

LanLink probe_lan_link(const std::string &, uint16_t) {
	LanLink link;
	link.description = "the wired-LAN check isn't implemented on this platform";
	return link;
}

#endif

} // namespace spectre
