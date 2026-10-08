// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "util/render_node.hpp"

#include "encode/encoder_factory.hpp"
#include "gdp/video_codec.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <xf86drm.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace wraith {

namespace {

std::vector<std::string> render_node_paths() {
	std::vector<std::string> paths;
	int count = drmGetDevices2(0, nullptr, 0);
	if (count <= 0) {
		return paths;
	}
	std::vector<drmDevicePtr> devices((size_t)count);
	count = drmGetDevices2(0, devices.data(), devices.size());
	if (count < 0) {
		return paths;
	}
	for (int i = 0; i < count; i++) {
		if (devices[i]->available_nodes & (1 << DRM_NODE_RENDER)) {
			paths.emplace_back(devices[i]->nodes[DRM_NODE_RENDER]);
		}
	}
	drmFreeDevices(devices.data(), count);
	return paths;
}

// One bit per codec in preference order, most preferred highest, so
// comparing ranks compares "best codec encodable", then the next, ...
// A codec off in wraith.toml's [encode.codecs] is never offered, so it
// doesn't count.
unsigned encode_rank(const std::vector<std::string> &encodable) {
	const std::vector<std::string> &tokens = gdp::all_video_codec_tokens();
	unsigned rank = 0;
	for (size_t i = 0; i < tokens.size(); i++) {
		if (config().encode.codec_enabled(tokens[i]) &&
			std::find(encodable.begin(), encodable.end(), tokens[i]) != encodable.end()) {
			rank |= 1u << (tokens.size() - 1 - i);
		}
	}
	return rank;
}

std::string join(const std::vector<std::string> &items) {
	std::string out;
	for (const std::string &item : items) {
		out += out.empty() ? item : ", " + item;
	}
	return out.empty() ? "none" : out;
}

std::string choose_render_node() {
	const char *pinned = getenv("WLR_RENDER_DRM_DEVICE");
	if (pinned && *pinned) {
		WLOG_INFO("render node: %s (WLR_RENDER_DRM_DEVICE)", pinned);
		return pinned;
	}

	std::vector<std::string> paths = render_node_paths();
	if (paths.size() <= 1) {
		return paths.empty() ? std::string() : paths.front();
	}
	std::string best;
	unsigned best_rank = 0;
	for (const std::string &path : paths) {
		std::vector<std::string> encodable;
		int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			encodable = hardware_encodable_codecs(fd);
			::close(fd);
		}
		WLOG_INFO("render node: %s encodes %s", path.c_str(), join(encodable).c_str());
		unsigned rank = encode_rank(encodable);
		if (best.empty() || rank > best_rank) {
			best = path;
			best_rank = rank;
		}
	}
	WLOG_INFO("render node: using %s", best.c_str());
	return best;
}

} // namespace

const std::string &preferred_render_node() {
	static const std::string path = choose_render_node();
	return path;
}

int open_render_node(dev_t want) {
	if (want != 0) {
		for (const std::string &path : render_node_paths()) {
			struct stat st;
			if (stat(path.c_str(), &st) == 0 && st.st_rdev == want) {
				int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
				if (fd >= 0) {
					return fd;
				}
				break;
			}
		}
	}
	const std::string &path = preferred_render_node();
	return path.empty() ? -1 : open(path.c_str(), O_RDWR | O_CLOEXEC);
}

} // namespace wraith
