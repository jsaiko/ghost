// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "agent_files.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

// profile.json, or an empty object (with no file, or one that doesn't
// parse: the defaults then).
QJsonObject read_profile(const QString &run_dir) {
	QFile file(QDir(run_dir).filePath("profile.json"));
	if (!file.open(QIODevice::ReadOnly)) {
		return QJsonObject();
	}
	QJsonParseError error;
	QJsonObject p = QJsonDocument::fromJson(file.readAll(), &error).object();
	if (error.error != QJsonParseError::NoError) {
		qWarning("wisp-greeter: %s: %s; using the default settings", qPrintable(file.fileName()),
			qPrintable(error.errorString()));
		return QJsonObject();
	}
	return p;
}

} // namespace

SpectreSettings load_profile(const QString &run_dir) {
	SpectreSettings settings;
	QJsonObject p = read_profile(run_dir);
	auto text = [&](const char *key, QString *field) { *field = p.value(key).toString(*field); };
	auto flag = [&](const char *key, bool *field) { *field = p.value(key).toBool(*field); };
	text("resolution", &settings.resolution);
	text("view", &settings.view);
	text("preferred_decoder", &settings.preferred_decoder);
	text("preferred_codec", &settings.preferred_codec);
	flag("lossless_refinement", &settings.lossless_refinement);
	flag("allow_pyrowave", &settings.allow_pyrowave);
	text("network_profile", &settings.network_profile);
	flag("forward_gamepads", &settings.forward_gamepads);
	flag("microphone", &settings.microphone);
	return settings;
}

bool load_debug_logging(const QString &run_dir) {
	return read_profile(run_dir).value("debug_logging").toBool(false);
}

unsigned load_display_sleep_minutes(const QString &run_dir) {
	return (unsigned)read_profile(run_dir).value("display_sleep_minutes").toInt(kDefaultDisplaySleepMinutes);
}

void write_session(const QString &run_dir, const QString &user, const QString &host_name,
	qint64 spectre_pid) {
	QJsonObject session{
		{"user", user},
		{"host_name", host_name},
		{"pid", spectre_pid},
	};
	QSaveFile file(QDir(run_dir).filePath("session.json"));
	if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(session).toJson()) < 0 ||
		!file.commit()) {
		qWarning("wisp-greeter: can't write %s: %s", qPrintable(file.fileName()),
			qPrintable(file.errorString()));
	}
}

void clear_session(const QString &run_dir) {
	QFile::remove(QDir(run_dir).filePath("session.json"));
}
