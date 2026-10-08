// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Client-side options for the `spectre` stream process, and the spectre(1)
// flags they imply (to_args()). spectre-qt keeps them in QSettings and
// edits them in its Settings dialog; wisp-greeter fills them from the
// profile Veil sends every thin client (docs/design/wisp.md). One mapping
// to flags serves both.
#pragma once

#include <QString>
#include <QStringList>

class QScreen;

struct SpectreSettings {
	// spectre -f
	bool fullscreen = false;
	// Which decoder spectre tries first (spectre -X), as a stored token:
	// "vulkan" (Vulkan Video, the default -- no flag needed), "native"
	// (VA-API on Linux, D3D11VA on Windows, VideoToolbox on macOS) or
	// "software". A GPU decoder that can't open falls back to the other,
	// then to software; NVIDIA under Linux never tries VA-API, having none.
	QString preferred_decoder = QStringLiteral("vulkan");
	// Video codec to prefer (spectre -C), as a gdp/video_codec.hpp wire
	// token ("h264", "h265", ...), or empty for "auto" -- let spectre offer
	// its own default order (gdp::all_video_codec_tokens(): pyrowave on a
	// wired LAN, then av1, h265, h264). The host still has the final say;
	// this only reorders what spectre asks for.
	QString preferred_codec;
	// spectre -N: the network profile SessionHello asks the host's rate
	// control for -- "auto" (the default: the host classifies the link
	// itself), "lan", "internet" or "mobile".
	QString network_profile = QStringLiteral("auto");
	// Lossless refinement, a lossless tile layer over the video (any
	// codec) that makes settled text/UI bit-exact at the cost of hashing
	// every frame on the host. Becomes spectre -R on or off, so every
	// session starts as set here, whatever the session menu's Lossless
	// Refinement row was last left at; that row still switches it
	// mid-session. spectre-qt's "Start with Lossless Refinement on" and a
	// Wisp thin client's profile (Veil).
	bool lossless_refinement = true;
	// Offer pyrowave when the host is on a wired LAN (spectre decides the
	// link; unticked passes spectre -W). On by default: it only ever comes
	// into play on a host that opted in to it.
	bool allow_pyrowave = true;
	// spectre -D: briefly outline each lossless refinement tile in red as
	// it lands, so it's visible which regions the host is refreshing
	// losslessly. A debugging aid; it draws nothing without refinement,
	// and only takes effect in a spectre-qt -D run (see to_args()).
	bool debug_tile_outlines = false;
	// Launch spectre in kiosk mode (-K), as Wisp's greeter does, to test
	// kiosk behaviour from a desktop. Only takes effect in a -D run.
	bool debug_kiosk = false;
	// Forward local gamepads to the host as virtual controllers (spectre
	// -G). On by default here, though spectre's own default is off;
	// unticked, spectre never offers the capability at all.
	bool forward_gamepads = true;
	// Send this machine's microphone to the host (spectre -M). Off by
	// default everywhere: nothing records until the user ticks it.
	bool microphone = false;
	// "WIDTHxHEIGHT"; empty (the default) for "match this display",
	// resolved at launch to the physical pixel size of the screen
	// spectre-qt is on, whether spectre then opens windowed or fullscreen.
	// Becomes spectre -r, which lands in SessionHello.displays (gdp-spec.md
	// §7.2): a new session starts at it, and one that's already running is
	// resized to it on reconnect. It's a best-effort ask, not a guarantee;
	// SessionAccept's negotiated size is whatever the session actually ends
	// up at.
	QString resolution;
	// spectre -A: ask the host for the window's size whenever it settles
	// at a new one. Off by default -- the picture scales to the window,
	// and the session menu changes the resolution.
	bool follow_window = false;
	// spectre -V: "fit" scales the picture to the window, "actual" shows it
	// 1:1 and pans; empty (the default) passes nothing, so spectre uses
	// whichever its session menu last picked.
	QString view;

	// The spectre(1) flags these settings imply, appended to the connection
	// arguments by the launcher. `screen` is the display "match this
	// display" resolves against; null falls back to the primary screen.
	// `debug` is spectre-qt's own -D: the Debug group's options only apply
	// when it is set, since that group is hidden (and so can't be switched
	// back off) otherwise.
	QStringList to_args(const QScreen *screen, bool debug) const;
};
