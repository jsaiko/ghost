// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "system_info.h"

#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

namespace wisp_agent {

namespace {

std::string read_file(const std::string &path) {
	std::ifstream file(path, std::ios::binary);
	std::stringstream buf;
	buf << file.rdbuf();
	return buf.str();
}

std::string trim(std::string s) {
	s.erase(0, s.find_first_not_of(" \t\r\n"));
	s.erase(s.find_last_not_of(" \t\r\n") + 1);
	return s;
}

std::string first_line(const std::string &path) {
	std::ifstream file(path);
	std::string line;
	std::getline(file, line);
	return trim(line);
}

std::vector<std::string> list_dir(const std::string &path) {
	std::vector<std::string> names;
	if (DIR *dir = opendir(path.c_str())) {
		while (dirent *e = readdir(dir)) {
			if (e->d_name[0] != '.') {
				names.push_back(e->d_name);
			}
		}
		closedir(dir);
	}
	std::sort(names.begin(), names.end());
	return names;
}

std::string link_basename(const std::string &path) {
	char buf[4096];
	ssize_t n = readlink(path.c_str(), buf, sizeof(buf) - 1);
	if (n <= 0) {
		return std::string();
	}
	buf[n] = '\0';
	const char *slash = strrchr(buf, '/');
	return slash ? slash + 1 : buf;
}

// `key: value` from /proc/cpuinfo or /proc/meminfo.
std::string proc_field(const std::string &text, const std::string &key) {
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		size_t colon = line.find(':');
		if (colon != std::string::npos && trim(line.substr(0, colon)) == key) {
			return trim(line.substr(colon + 1));
		}
	}
	return std::string();
}

std::string cpu_model() {
	std::string cpuinfo = read_file("/proc/cpuinfo");
	// x86 says "model name"; a Raspberry Pi says "Model" for the board.
	for (const char *key : {"model name", "Model", "Hardware"}) {
		std::string value = proc_field(cpuinfo, key);
		if (!value.empty()) {
			return value;
		}
	}
	return std::string();
}

// The device's name from pci.ids (pciutils), or "vendor:device".
std::string pci_name(const std::string &vendor, const std::string &device) {
	for (const char *path : {"/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids", "/usr/share/pci.ids"}) {
		std::ifstream ids(path);
		if (!ids) {
			continue;
		}
		std::string line, vendor_name;
		bool in_vendor = false;
		while (std::getline(ids, line)) {
			if (line.empty() || line[0] == '#') {
				continue;
			}
			if (line[0] != '\t') {
				if (in_vendor) {
					break; // the device isn't listed under its vendor
				}
				if (line.compare(0, 4, vendor) == 0) {
					in_vendor = true;
					vendor_name = trim(line.substr(4));
				}
			} else if (in_vendor && line.size() > 5 && line[1] != '\t' && line.compare(1, 4, device) == 0) {
				return vendor_name + " " + trim(line.substr(5));
			}
		}
		if (!vendor_name.empty()) {
			return vendor_name + " " + device;
		}
	}
	return vendor + ":" + device;
}

void add_gpus(gdp::wisp::SystemInfo *system) {
	std::set<std::string> seen;
	for (const std::string &name : list_dir("/sys/class/drm")) {
		// card0, card1, ... (card0-HDMI-A-1 is a connector).
		if (name.compare(0, 4, "card") != 0 || name.find('-') != std::string::npos) {
			continue;
		}
		std::string dev = "/sys/class/drm/" + name + "/device";
		std::string where = link_basename(dev);
		if (where.empty() || !seen.insert(where).second) {
			continue;
		}
		auto *gpu = system->add_gpus();
		gpu->set_driver(link_basename(dev + "/driver"));
		std::string vendor = first_line(dev + "/vendor");
		std::string device = first_line(dev + "/device");
		if (vendor.size() == 6 && device.size() == 6) { // "0x1002"
			gpu->set_name(pci_name(vendor.substr(2), device.substr(2)));
		} else {
			// Not PCI (a Raspberry Pi's vc4): the device tree's name.
			std::string compatible;
			std::istringstream uevent(read_file(dev + "/uevent"));
			std::string line;
			while (std::getline(uevent, line)) {
				if (line.compare(0, 16, "OF_COMPATIBLE_0=") == 0) {
					compatible = line.substr(16);
				}
			}
			gpu->set_name(compatible.empty() ? where : compatible);
		}
	}
}

// The refresh rate of the EDID's first detailed timing (the preferred
// mode), in millihertz; 0 if there is none.
uint32_t edid_refresh_mhz(const std::string &edid) {
	if (edid.size() < 128) {
		return 0;
	}
	const auto *d = reinterpret_cast<const uint8_t *>(edid.data()) + 54;
	uint64_t clock_hz = (uint64_t)(d[0] | d[1] << 8) * 10000;
	uint64_t htotal = (d[2] | (d[4] & 0xF0) << 4) + (d[3] | (d[4] & 0x0F) << 8);
	uint64_t vtotal = (d[5] | (d[7] & 0xF0) << 4) + (d[6] | (d[7] & 0x0F) << 8);
	if (clock_hz == 0 || htotal == 0 || vtotal == 0) {
		return 0;
	}
	return (uint32_t)((clock_hz * 1000 + htotal * vtotal / 2) / (htotal * vtotal));
}

void add_displays(gdp::wisp::SystemInfo *system) {
	for (const std::string &name : list_dir("/sys/class/drm")) {
		size_t dash = name.find('-');
		if (name.compare(0, 4, "card") != 0 || dash == std::string::npos) {
			continue;
		}
		std::string dir = "/sys/class/drm/" + name;
		// wisp-outputs forces every monitor but the first off; those read
		// as disconnected here, which is what the client actually uses.
		if (first_line(dir + "/status") != "connected") {
			continue;
		}
		auto *display = system->add_displays();
		display->set_connector(name.substr(dash + 1));
		unsigned w = 0, h = 0;
		if (sscanf(first_line(dir + "/modes").c_str(), "%ux%u", &w, &h) == 2) {
			display->set_width(w);
			display->set_height(h);
		}
		display->set_refresh_mhz(edid_refresh_mhz(read_file(dir + "/edid")));
	}
}

void add_decoders(gdp::wisp::SystemInfo *system, const std::string &spectre_path) {
	if (spectre_path.empty() || access(spectre_path.c_str(), X_OK) != 0) {
		return;
	}
	std::string command = "'" + spectre_path + "' --probe-decoders 2>/dev/null";
	FILE *out = popen(command.c_str(), "r");
	if (!out) {
		return;
	}
	char line[128];
	while (fgets(line, sizeof(line), out)) {
		std::string path = trim(line);
		if (!path.empty()) {
			system->add_hw_decode(path);
		}
	}
	pclose(out);
}

} // namespace

std::string default_route_interface() {
	std::ifstream route("/proc/net/route");
	std::string line;
	std::getline(route, line); // header
	while (std::getline(route, line)) {
		std::istringstream fields(line);
		std::string iface, destination, gateway;
		unsigned flags = 0;
		fields >> iface >> destination >> gateway >> std::hex >> flags;
		if (destination == "00000000" && (flags & 0x1) /* RTF_UP */) {
			return iface;
		}
	}
	// IPv6-only: ::/0 in /proc/net/ipv6_route.
	std::ifstream route6("/proc/net/ipv6_route");
	while (std::getline(route6, line)) {
		std::istringstream fields(line);
		std::string dest, dest_len, src, src_len, next_hop, metric, refcnt, use, flags, iface;
		fields >> dest >> dest_len >> src >> src_len >> next_hop >> metric >> refcnt >> use >> flags >> iface;
		if (dest == std::string(32, '0') && dest_len == "00" && iface != "lo" && !iface.empty()) {
			return iface;
		}
	}
	return std::string();
}

std::string interface_mac(const std::string &iface) {
	std::string mac = first_line("/sys/class/net/" + iface + "/address");
	std::transform(mac.begin(), mac.end(), mac.begin(), [](unsigned char c) { return (char)tolower(c); });
	return mac.size() == 17 ? mac : std::string();
}

gdp::wisp::SystemInfo read_system_info(const std::string &iface, const std::string &spectre_path) {
	gdp::wisp::SystemInfo system;
	system.set_cpu_model(cpu_model());
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	system.set_cpu_cores(cores > 0 ? (uint32_t)cores : 0);
	unsigned long long kb = 0;
	if (sscanf(proc_field(read_file("/proc/meminfo"), "MemTotal").c_str(), "%llu", &kb) == 1) {
		system.set_memory_bytes(kb * 1024);
	}
	add_gpus(&system);
	add_decoders(&system, spectre_path);
	add_displays(&system);
	// -1 when the link is down or the driver doesn't say (Wi-Fi).
	long speed = atol(first_line("/sys/class/net/" + iface + "/speed").c_str());
	system.set_nic_speed_mbps(speed > 0 ? (uint32_t)speed : 0);
	return system;
}

gdp::wisp::WispHello make_hello(const std::string &mac, const gdp::wisp::SystemInfo &system) {
	gdp::wisp::WispHello hello;
	hello.set_mac(mac);
	char host[256] = {};
	if (gethostname(host, sizeof(host) - 1) == 0) {
		hello.set_hostname(host);
	}
	ifaddrs *addrs = nullptr;
	if (getifaddrs(&addrs) == 0) {
		for (ifaddrs *a = addrs; a; a = a->ifa_next) {
			if (!a->ifa_addr || (a->ifa_flags & IFF_LOOPBACK)) {
				continue;
			}
			char text[INET6_ADDRSTRLEN] = {};
			if (a->ifa_addr->sa_family == AF_INET) {
				inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr, text,
					sizeof(text));
			} else if (a->ifa_addr->sa_family == AF_INET6) {
				const in6_addr &ip = reinterpret_cast<sockaddr_in6 *>(a->ifa_addr)->sin6_addr;
				if (IN6_IS_ADDR_LINKLOCAL(&ip)) {
					continue;
				}
				inet_ntop(AF_INET6, &ip, text, sizeof(text));
			} else {
				continue;
			}
			hello.add_ips(text);
		}
		freeifaddrs(addrs);
	}
	utsname uts{};
	if (uname(&uts) == 0) {
		hello.set_arch(uts.machine);
	}
	hello.set_image_version(first_line("/etc/wisp-release"));
	double uptime = 0;
	if (sscanf(read_file("/proc/uptime").c_str(), "%lf", &uptime) == 1 && uptime > 0) {
		hello.set_uptime_secs((uint64_t)uptime);
	}
	*hello.mutable_system() = system;
	return hello;
}

} // namespace wisp_agent
