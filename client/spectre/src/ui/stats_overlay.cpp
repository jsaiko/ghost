// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "ui/stats_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace spectre {

namespace {

// The latency bar's full-scale value.
constexpr double kLatencyBarMaxMs = 100.0;

const UiColor kBackdrop{0.05f, 0.06f, 0.08f, 0.72f};
const UiColor kLabel{0.60f, 0.78f, 1.00f, 1.0f};
const UiColor kText{0.92f, 0.92f, 0.94f, 1.0f};

std::string fmt(const char *format, double value) {
	char buf[64];
	snprintf(buf, sizeof(buf), format, value);
	return buf;
}

// SessionAccept.encoder spelled out as hardware/software, since that's the
// question being asked when this overlay is up. wraith names the backend
// row it opened and appends "+refine" when refinement wraps it.
std::string describe_encoder(const std::string &encoder) {
	if (encoder.empty()) {
		return "unknown";
	}
	const std::string kRefineSuffix = "+refine";
	std::string backend = encoder;
	std::string suffix;
	if (backend.size() > kRefineSuffix.size() &&
		backend.compare(backend.size() - kRefineSuffix.size(), kRefineSuffix.size(), kRefineSuffix) == 0) {
		backend.resize(backend.size() - kRefineSuffix.size());
		suffix = " + refine";
	}
	if (backend == "vaapi") {
		return "vaapi (hardware)" + suffix;
	}
	if (backend == "nvenc") {
		return "nvenc (hardware)" + suffix;
	}
	if (backend == "software") {
		return "x264 (software)" + suffix;
	}
	return encoder;
}

// "net lan" when the profile was forced, "net auto: lan" when wraith
// classified the link itself -- which it can revise mid-session.
std::string describe_network_profile(const StatsSample &sample) {
	const std::string &host =
		sample.host_network_profile.empty() ? std::string("unknown") : sample.host_network_profile;
	if (sample.network_profile == "auto") {
		return "net auto: " + host;
	}
	return "net " + (sample.host_network_profile.empty() ? sample.network_profile : host);
}

} // namespace

void build_stats_overlay(UiDrawList &out, const UiFont &font, const StatsSample &sample, double latency_ms,
	float top) {
	if (!font.valid()) {
		return;
	}
	const float s = (float)font.pixel_height() / 4.0f; // see session_menu.cpp
	const float line_h = (float)font.line_height();
	const float pad = 2 * s;
	const float margin = 2 * s;

	// Encode is wraith's side (SessionAccept.encoder), decode is ours.
	std::string encoder = describe_encoder(sample.encoder);
	const std::string &decoder = sample.decoder;
	std::vector<std::string> lines = {
		std::to_string(sample.width) + "x" + std::to_string(sample.height) + " " + sample.codec +
			(sample.audio ? " + audio" : ""),
		"encode " + encoder,
		"decode " + decoder,
		"fps " + fmt("%.1f", sample.fps) + "   decode " + fmt("%.1f", sample.decode_ms) + " ms   present " +
			fmt("%.1f", sample.present_ms) + " ms",
		"rtt " + fmt("%.1f", sample.rtt_ms) + " ms   loss " + std::to_string(sample.lost_recent) + "/" +
			std::to_string(std::max<uint32_t>(sample.window_span, 1)) + "   video " +
			fmt("%.1f", sample.video_mbps) + " Mb/s",
		describe_network_profile(sample) + "   connection " + (sample.via_gateway ? "proxied" : "direct"),
	};
	if (sample.host_avg_ms > 0.0) { // a host that reports it
		lines.insert(lines.begin() + 3,
			"host " + fmt("%.1f", sample.host_min_ms) + " / " + fmt("%.1f", sample.host_avg_ms) + " / " +
				fmt("%.1f", sample.host_max_ms) + " ms (min/avg/max)");
	}

	float widest = 0.0f;
	for (const auto &l : lines) {
		widest = std::max(widest, font.text_width(l));
	}
	const float bar_h = std::max(2.0f, std::floor(s));
	float panel_w = std::ceil(widest + 2 * pad);
	float panel_h = pad + (float)lines.size() * line_h + s + bar_h + pad;
	out.rect(margin, top + margin, panel_w, panel_h, kBackdrop);

	float y = top + margin + pad;
	for (size_t i = 0; i < lines.size(); i++) {
		out.text(margin + pad, y, i == 0 ? kLabel : kText, lines[i]);
		y += line_h;
	}
	y += s;

	// The latency bar: grows rightward and shifts green to red with the
	// latest frame's decode+present time.
	double t = std::clamp(latency_ms / kLatencyBarMaxMs, 0.0, 1.0);
	float bar_w = std::max(s, (float)((panel_w - 2 * pad) * t));
	out.rect(margin + pad, y, bar_w, bar_h, UiColor{(float)t, (float)(1.0 - t), 0.0f, 0.9f});
}

} // namespace spectre
