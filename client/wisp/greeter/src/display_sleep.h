// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Turns the display off after the profile's display_sleep_minutes without
// input, at the sign-in screen and in a session alike: swayidle, told by
// labwc when input stops, runs `wlopm --off` (wlr-output-power-management),
// and any input turns it back on. spectre holds the screen awake only for a
// while after gamepad input, which labwc never sees. A new value from Veil
// applies at once: profile.json is watched.
#pragma once

#include <QObject>
#include <QString>

class QFileSystemWatcher;
class QProcess;

class DisplaySleep : public QObject {
	Q_OBJECT

public:
	DisplaySleep(const QString &run_dir, QObject *parent = nullptr);
	~DisplaySleep() override;

private:
	void reload();
	void start();
	void stop();

	QString run_dir_;
	QFileSystemWatcher *watcher_ = nullptr;
	QProcess *swayidle_ = nullptr;
	unsigned minutes_ = 0; // what swayidle_ runs with; 0: not running
	bool missing_ = false; // swayidle isn't installed (a desktop test run)
};
