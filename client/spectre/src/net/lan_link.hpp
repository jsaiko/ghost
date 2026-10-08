// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Whether this client reaches a host over a wired LAN fast enough for the
// "pyrowave" codec (gdp-spec.md §6.6): the rule spectre applies before it
// offers pyrowave at all. pyrowave runs at several hundred Mbit/s, so it is
// offered only when
// - the host is directly reachable: the kernel's route to it has no
//   gateway, so no router (and no internet) sits between -- or it is this
//   very machine;
// - the interface that route leaves by is wired, at 1 Gbit/s or more. A
//   bridge, bond or VLAN reports the speed of the ports under it (sysfs
//   lower_* links), the fastest wired one. Wi-Fi, and anything whose speed
//   can't be read (VPN and tunnel interfaces), doesn't qualify.
// Through Veil's gateway pyrowave is never used either way: wraith drops it
// when SessionHello.via_gateway says so.
//
// Linux (netlink + sysfs) and Windows (GetBestRoute2 + GetIfEntry2, where
// a wired link is Ethernet on an 802.3 medium); on macOS the answer is
// always no, and spectre -C pyrowave is the way to ask for it anyway.
#pragma once

#include <cstdint>
#include <string>

namespace spectre {

struct LanLink {
	bool qualifies = false;
	// For the log: the interface and speed, or why it doesn't qualify.
	std::string description;
};

// Resolves `host` the way the session connection will and asks the kernel
// how it would route there. Blocking (name resolution), so call it before
// connecting, as codec negotiation needs anyway.
LanLink probe_lan_link(const std::string &host, uint16_t port);

} // namespace spectre
