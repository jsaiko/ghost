// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// wraith's settings file, /etc/ghost/wraith.toml, next to ghostd.toml
// (packaging/config/wraith.toml lists every key, commented out at its default).
// Read once at startup, before anything that uses it is built; wraith -C
// points at another file, `wraith --check-config` validates one and prints
// what would be in effect.
//
// These are the host-wide knobs -- a file, since environment variables are
// awkward to reach a wraith that ghostd starts through a systemd user
// unit -- plus the lossless refinement policy's numbers. Per-session
// choices stay on the command line and in the session profiles.
#pragma once

#include "util/log.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wraith {

inline constexpr const char *kDefaultConfigPath = "/etc/ghost/wraith.toml";

// The rate controller's network profile (session.proto's NetworkProfile,
// AUTO resolved), as far as settings care: [refine.<name>] overrides.
enum class LinkProfile { kLan, kInternet, kMobile };
inline constexpr LinkProfile kLinkProfiles[] = {LinkProfile::kLan, LinkProfile::kInternet,
	LinkProfile::kMobile};
const char *link_profile_name(LinkProfile profile); // "lan", "internet", "mobile"

// Lossless refinement's tile policy; encode/refine/tile_tracker.hpp
// explains each (TileTrackerConfig).
struct RefineSettings {
	uint32_t settle_ms = 80;
	// The lossless layer's bandwidth, as a share of the video's current
	// target bitrate (which the rate controller moves with the link), on
	// top of it; 0 is no limit but the per-frame cap. `burst_ms` of that
	// rate can go out at once.
	uint32_t bandwidth_percent = 50;
	uint32_t burst_ms = 500;
};

// One profile's [refine.<name>] section: each key it sets replaces
// [refine]'s for sessions on that profile.
struct RefineOverrides {
	std::optional<uint32_t> settle_ms;
	std::optional<uint32_t> bandwidth_percent;
	std::optional<uint32_t> burst_ms;
};

// The built-in [refine.<profile>] sections (see WraithConfig::refine):
// internet and mobile settle more slowly.
inline RefineOverrides default_refine_overrides(LinkProfile profile) {
	RefineOverrides overrides;
	switch (profile) {
	case LinkProfile::kLan: break;
	case LinkProfile::kInternet: overrides.settle_ms = 150; break;
	case LinkProfile::kMobile: overrides.settle_ms = 250; break;
	}
	return overrides;
}

struct WraithConfig {
	struct Log {
		// Info: what a session did and why (codec, rate changes, profile
		// switches, refinement repairs). Debug adds the periodic
		// summaries -- capture, encode, refinement, client timing -- for
		// chasing a performance problem.
		LogLevel level = LogLevel::Info;
	} log;

	struct Network {
		// QUIC's own congestion controller under wraith's rate control:
		// "cubic" or "bbr". GdpSession's constructor says why.
		std::string congestion_control = "cubic";
		// Log every StatsReport's measurements and the rate controller's
		// target (GdpSession::rate_trace_).
		bool rate_trace = false;
		// Video datagrams are paced (gdp::Connection::set_pacing_rate) at
		// pacing_multiplier times the rate controller's target, never
		// below pacing_floor_mbps: a keyframe at 10G line rate overflows
		// the buffer of any slower hop on the way. 0 turns pacing off.
		uint32_t pacing_multiplier = 10;
		uint32_t pacing_floor_mbps = 500;
		// Parity on video (FecController): "auto" follows the loss the
		// client reports, "off" sends none and doesn't offer the "fec"
		// capability, "fixed" always sends fec_percent.
		std::string fec = "auto";
		uint32_t fec_percent = 20;
	} network;

	// [refine], and [refine.lan] / [refine.internet] / [refine.mobile]
	// over it, indexed by LinkProfile. By default the slower links wait
	// longer before sending a tile losslessly (150 ms internet, 250 ms
	// mobile), so text being typed or a view being scrolled is re-sent
	// less often while it's still changing.
	RefineSettings refine;
	RefineOverrides refine_profiles[3] = {default_refine_overrides(LinkProfile::kLan),
		default_refine_overrides(LinkProfile::kInternet), default_refine_overrides(LinkProfile::kMobile)};
	// [refine] with `profile`'s overrides applied.
	RefineSettings refine_for(LinkProfile profile) const;

	struct Encode {
		// Frames between periodic keyframes; 0, the default, sends none but
		// the ones asked for (EncoderConfig::gop_size says why).
		uint32_t gop = 0;
		// The rate controller's ceiling (RateController), and the bitrate
		// the encoder opens at; wraith -b overrides it.
		uint32_t max_bitrate_mbps = 80;
		// The software (x264) encoder even where a hardware one works,
		// like -F (encoder_factory.cpp).
		bool force_software = false;
		// NVENC encodes straight from the GPU; false uploads every frame
		// from host memory (NvencEncoder).
		bool nvenc_zero_copy = true;
		// Bits per component the encoder codes at (EncoderConfig::bit_depth):
		// 0 is "auto" -- 10 whenever the client declares it for the
		// session's codec and a backend opens at it, else 8; 8 forces the
		// 8-bit baseline for every session.
		uint32_t bit_depth = 0;
		// PyroWave's ceiling, in bits per output pixel per frame: where a
		// pyrowave session's rate controller tops out instead of
		// max_bitrate_mbps (SessionServices::encode_bitrate_bps()). 1.0 is
		// where dense text stops showing halos -- ~500 Mbit/s at 4K60.
		double pyrowave_bpp = 1.0;
		// The highest frame rate a client may ask for (DisplayDescriptor.
		// refresh_mhz, gdp_session.cpp's session_refresh_hz()); a client
		// that asks for none gets 60, or this if it is lower. The software
	// encoder stops at 60 regardless (gdp_session.cpp's kMaxSoftwareRefreshHz).
		uint32_t max_fps = 144;
		// [encode.codecs]: which wire codecs wraith offers in negotiation
		// (supported_video_codecs) and ranks GPUs by (render_node.cpp).
		// AV1 and PyroWave are off by default; PyroWave is never used to
		// rank GPUs (every Vulkan GPU can run it).
		struct Codecs {
			bool pyrowave = false;
			bool av1 = false;
			bool h265 = true;
			bool h264 = true;
		} codecs;
		// `token` is a gdp codec token ("pyrowave", "av1", "h265", "h264").
		bool codec_enabled(const std::string &token) const;
	} encode;

	// [files]: file transfer from the client (gdp-spec.md §7.12,
	// session/file_receiver.hpp).
	struct Files {
		// Offer the "file" capability at all.
		bool receive = true;
		// Let the client fetch the files the user copies (a FileList);
		// false sends their paths as clipboard text instead, as before.
		bool send = true;
		// Where received files go; empty is the user's download directory
		// (file_receive_directory()). A leading "~/" is the home directory.
		std::string directory;
		// Take the folders a client shares (file-share): mounted read-only
		// for the session (session/client_files.hpp).
		bool shares = true;
		// Give each shared folder a sidebar entry in Nautilus and Dolphin.
		bool bookmarks = true;
	} files;

	struct Display {
		// Set the desktop's output scale from the client's
		// DisplayDescriptor.scale, 100% when it sends none (GNOME, KDE and
		// wlroots desktops; GNOME rounds it to a scale mutter supports).
		// false leaves the scale to the desktop's own settings.
		bool client_scale = true;
	} display;
};

// The settings in effect: the defaults until a load_config() succeeds.
const WraithConfig &config();

struct ConfigLoad {
	bool ok = true;       // false: `error` says why, and nothing was applied
	bool missing = false; // no file at the path; the defaults stand, ok stays true
	std::string error;
	// Unknown sections or keys: reported, otherwise ignored.
	std::vector<std::string> warnings;
};

// Reads `path` over the defaults and makes it what config() returns. A
// file that doesn't parse, or has a value of the wrong type or out of
// range, is rejected whole -- no half-applied file.
ConfigLoad load_config(const std::string &path);

// `config` as TOML, every key set: what `wraith --check-config` prints.
std::string describe_config(const WraithConfig &config);

} // namespace wraith
