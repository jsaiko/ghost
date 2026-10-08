// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// wisp-greeter: the Wisp thin client's login screen. Fullscreen under labwc
// (client/wisp/image), it signs in to the Veil named on the kernel command
// line -- trusting only the certificate fingerprint named there too -- and
// runs `spectre -K` for the chosen desktop, coming back when it exits.
// spectre's other flags come from the profile Veil sends every thin client,
// which wisp-agent leaves in /run/wisp/profile.json.
// spectre-qt's sister: same libgdp lobby client and the same child-process
// model, none of its desktop-launcher parts (host history, settings, TOFU).
#include "boot_config.h"
#include "display_sleep.h"
#include "greeter_window.h"

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);
	app.setApplicationName("wisp-greeter");

	QCommandLineParser parser;
	parser.setApplicationDescription("Wisp login screen");
	parser.addHelpOption();
	QCommandLineOption veil_option("veil",
		"Veil address (host[:port]) instead of veil= on the kernel command line.", "host[:port]");
	QCommandLineOption cert_option("veil-cert",
		"Veil certificate SHA-256 instead of veil_cert= on the kernel command line.", "sha256");
	QCommandLineOption window_option("windowed",
		"Run in a window instead of fullscreen (testing on a desktop).");
	QCommandLineOption run_dir_option("run-dir",
		"Where wisp-agent's profile.json and the greeter's session.json live.", "dir", "/run/wisp");
	parser.addOption(veil_option);
	parser.addOption(cert_option);
	parser.addOption(window_option);
	parser.addOption(run_dir_option);
	parser.process(app);

	DisplaySleep display_sleep(parser.value(run_dir_option));
	GreeterWindow window(BootConfig::load(parser.value(veil_option), parser.value(cert_option)),
		parser.value(run_dir_option));
	if (parser.isSet(window_option)) {
		window.resize(1280, 800);
		window.show();
	} else {
		window.showFullScreen();
	}
	return app.exec();
}
