// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "display_sleep.h"

#include "agent_files.h"

#include <QFileSystemWatcher>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

DisplaySleep::DisplaySleep(const QString &run_dir, QObject *parent) : QObject(parent), run_dir_(run_dir) {
	// The directory, not the file: the agent replaces profile.json by
	// renaming over it, which a file watch doesn't survive.
	watcher_ = new QFileSystemWatcher(QStringList{run_dir_}, this);
	connect(watcher_, &QFileSystemWatcher::directoryChanged, this, &DisplaySleep::reload);
	reload();
}

DisplaySleep::~DisplaySleep() {
	stop();
}

void DisplaySleep::reload() {
	unsigned minutes = load_display_sleep_minutes(run_dir_);
	if (minutes == minutes_ && (minutes == 0 || swayidle_)) {
		return;
	}
	stop();
	minutes_ = minutes;
	if (minutes_ > 0) {
		start();
	}
}

void DisplaySleep::start() {
	if (missing_) {
		return;
	}
	if (QStandardPaths::findExecutable("swayidle").isEmpty()) {
		missing_ = true;
		qWarning("wisp-greeter: no swayidle, so the display never sleeps");
		return;
	}
	swayidle_ = new QProcess(this);
	swayidle_->setProcessChannelMode(QProcess::ForwardedChannels);
	// -w: run the off command to the end before going on.
	swayidle_->start("swayidle",
		{"-w", "timeout", QString::number(minutes_ * 60), "wlopm --off '*'", "resume", "wlopm --on '*'"});
	qInfo("wisp-greeter: the display sleeps after %u minutes without input", minutes_);
	connect(swayidle_, &QProcess::finished, this, [this]() {
		// Not stopped by us: try again in a while.
		qWarning("wisp-greeter: swayidle exited; restarting it");
		swayidle_->deleteLater();
		swayidle_ = nullptr;
		QTimer::singleShot(5000, this, &DisplaySleep::reload);
	});
}

void DisplaySleep::stop() {
	if (!swayidle_) {
		return;
	}
	swayidle_->disconnect(this);
	swayidle_->terminate();
	swayidle_->waitForFinished(2000);
	delete swayidle_;
	swayidle_ = nullptr;
	// It may have turned the display off just before.
	QProcess::startDetached("wlopm", {"--on", "*"});
}
