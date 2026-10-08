// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// What wisp-agent reports about the machine in its WispHello
// (libgdp/proto/wisp.proto), read from /proc and /sys.
#pragma once

#include "wisp.pb.h"

#include <string>

namespace wisp_agent {

// The interface holding the default route (the boot NIC), or empty while
// there is none yet.
std::string default_route_interface();

// `iface`'s MAC address, lowercase with colons; empty if unreadable.
std::string interface_mac(const std::string &iface);

// The facts that don't change while the client is up: CPU, memory, GPUs,
// displays, the NIC's speed. `spectre_path` is run with --probe-decoders
// for SystemInfo.hw_decode.
gdp::wisp::SystemInfo read_system_info(const std::string &iface, const std::string &spectre_path);

// The rest of the hello, which can change between connections (addresses,
// hostname), plus `system`. The key and session are the caller's.
gdp::wisp::WispHello make_hello(const std::string &mac, const gdp::wisp::SystemInfo &system);

} // namespace wisp_agent
