// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "spectre_settings.h"

#include <QGuiApplication>
#include <QScreen>

QStringList SpectreSettings::to_args(const QScreen *screen, bool debug) const {
	QStringList args;
	if (fullscreen) {
		args << "-f";
	}
	// "native" is stored rather than the platform's own token so a settings
	// file means the same thing on any OS; spectre -X wants the real one.
	if (preferred_decoder == QLatin1String("native")) {
#if defined(_WIN32)
		args << "-X" << "d3d11va";
#elif defined(__APPLE__)
		args << "-X" << "videotoolbox";
#else
		args << "-X" << "vaapi";
#endif
	} else if (preferred_decoder == QLatin1String("software")) {
		args << "-X" << "software";
	}
	if (resolution.isEmpty()) {
		if (!screen) {
			screen = QGuiApplication::primaryScreen();
		}
		if (screen) {
			// Physical pixels, not Qt's logical ones: a 200%-scaled 4K
			// panel should get a 3840x2160 stream, not a blurry 1920x1080.
			QSize size = screen->geometry().size() * screen->devicePixelRatio();
			args << "-r" << QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
		}
	} else {
		args << "-r" << resolution;
	}
	if (!preferred_codec.isEmpty()) {
		args << "-C" << preferred_codec;
	}
	// Auto is spectre's own default, so it needs no flag.
	if (!network_profile.isEmpty() && network_profile != QLatin1String("auto")) {
		args << "-N" << network_profile;
	}
	args << "-R" << (lossless_refinement ? "on" : "off");
	if (debug && debug_tile_outlines) {
		args << "-D";
	}
	if (debug && debug_kiosk) {
		args << "-K";
	}
	if (forward_gamepads) {
		args << "-G";
	}
	if (microphone) {
		args << "-M";
	}
	if (!allow_pyrowave) {
		args << "-W";
	}
	if (follow_window) {
		args << "-A";
	}
	if (!view.isEmpty()) {
		args << "-V" << view;
	}
	return args;
}
