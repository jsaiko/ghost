// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The "Show statistics" overlay: a few lines of client-side numbers in the
// top-left corner of the stream window, plus a latency bar (green at 0ms,
// red at kLatencyBarMaxMs+, width proportional). Off by default; toggled
// from the session menu (ui/session_menu.hpp).
//
// Everything here is measured on the client (decode/present time, frame
// rate, bytes received) or read from the QUIC connection (RTT); there's no
// clock correlation with wraith yet, so none of it is glass-to-glass
// latency -- see docs/design/spectre-client.md#limitations.
#pragma once

#include "ui/draw_list.hpp"
#include "ui/font.hpp"

#include <cstdint>
#include <string>

namespace spectre {

struct StatsSample {
	uint32_t width = 0;
	uint32_t height = 0;
	std::string codec;
	std::string encoder; // SessionAccept.encoder ("vaapi+refine" etc.), empty if unreported
	std::string decoder; // Decoder::backend_name(): "vaapi (hardware)", "ffmpeg (software)"...
	bool audio = false;
	double fps = 0.0;        // decoded frames per second over the last window
	double decode_ms = 0.0;  // mean decode time over the window
	double present_ms = 0.0; // mean present time over the window
	// wraith's capture-to-send time over the last window that had frames;
	// all 0 until a host that reports it sends one.
	double host_min_ms = 0.0;
	double host_avg_ms = 0.0;
	double host_max_ms = 0.0;
	double rtt_ms = 0.0;         // QUIC's smoothed RTT
	uint32_t lost_recent = 0;    // frames never fully reassembled, of the last...
	uint32_t window_span = 0;    // ...this many frame_ids (up to 64)
	double video_mbps = 0.0;     // coded video received, megabits/s
	std::string network_profile; // what -N asked for: "auto", "lan", "internet", "mobile"
	// What wraith runs with (SessionClient::host_network_profile()), empty
	// if unreported.
	std::string host_network_profile;
	bool via_gateway = false; // relayed by Veil's gateway (SessionClient::via_gateway()), else direct
};

// Appends the overlay for `sample`, laid out for `font`. `latency_ms` is
// decode+present of the most recent frame (what the bar shows -- it
// reacts per frame, the text averages). `top` is where the video starts
// in window pixels (below the docked toolbar, if there is one).
void build_stats_overlay(UiDrawList &out, const UiFont &font, const StatsSample &sample, double latency_ms,
	float top = 0.0f);

} // namespace spectre
