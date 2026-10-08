// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The Qt6 launcher: login, host list, session picker, settings and
// diagnostics. It logs in to a host's lobby (ghostd) or Veil's, picks or
// resumes a session, pins the login server's certificate, and launches the
// actual stream client (`spectre`) as a separate process. SDL3's event loop
// and Qt's don't share a main thread well, and Moonlight-qt's own answer to
// this (a background SDL thread bridged to Qt via queued signals) is more
// machinery than a launcher needs when a plain child process does the same
// job.
//
// The form itself is a Designer .ui (connect_window.ui) so it can be edited
// visually in Qt Creator; connect_window.{h,cpp} holds the ConnectWindow
// class that loads it.
#include "connect_window.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);
	// Where QSettings puts SpectreSettings (settings_dialog.h's
	// load_settings()) -- without these it would key off the executable
	// name and land somewhere unpredictable. On Linux this is
	// ~/.config/spectre/spectre-qt.conf.
	app.setOrganizationName("spectre");
	app.setApplicationName("spectre-qt");
	// The application-wide default: covers windows that don't set their own
	// windowIcon in .ui (SessionTypeDialog, QMessageBox popups). ConnectWindow
	// sets its own in connect_window.ui, so it doesn't need this too.
	app.setWindowIcon(QIcon(":/icons/app-icon.png"));

	QCommandLineParser parser;
	parser.setApplicationDescription("spectre-qt");
	parser.addHelpOption();
	QCommandLineOption debug_option({"D", "debug"},
		"Print the spectre launch command and spectre's debug output to the console.");
	parser.addOption(debug_option);
	parser.process(app);

	ConnectWindow window(parser.isSet(debug_option));
	window.show();
	return app.exec();
}
