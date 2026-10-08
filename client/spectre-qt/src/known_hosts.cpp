// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "known_hosts.h"

#include "gdp/cert_fingerprint.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

KnownHosts::KnownHosts() {
	QFile file(path());
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return;
	}
	QTextStream in(&file);
	while (!in.atEnd()) {
		QString line = in.readLine().trimmed();
		if (line.isEmpty() || line.startsWith('#')) {
			continue;
		}
		QStringList fields = line.split(' ', Qt::SkipEmptyParts);
		// A malformed line is skipped, not fatal: at worst that host is
		// treated as unknown and asked about again.
		if (fields.size() != 2 || !gdp::is_sha256_hex(fields[1].toStdString())) {
			continue;
		}
		pins_.insert(fields[0], fields[1]);
	}
}

QString KnownHosts::path() {
	// GenericConfigLocation, not QSettings' own: this is spectre's file,
	// beside spectre-qt.conf in ~/.config/spectre/ rather than in a
	// spectre-qt-specific place.
	return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
		.filePath("spectre/known_hosts");
}

QString KnownHosts::ca_bundle_path() {
	return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
		.filePath("spectre/ca-certificates.pem");
}

QString KnownHosts::key(const QString &host, uint16_t port) {
	QString h = host.toLower();
	// Bracket IPv6 literals so the port stays unambiguous.
	if (h.contains(':')) {
		h = '[' + h + ']';
	}
	return h + ':' + QString::number(port);
}

KnownHosts::Status KnownHosts::check(const QString &host, uint16_t port, const QString &cert_sha256) const {
	auto it = pins_.find(key(host, port));
	if (it == pins_.end()) {
		return Status::kUnknown;
	}
	return *it == cert_sha256 ? Status::kTrusted : Status::kMismatch;
}

bool KnownHosts::trust(const QString &host, uint16_t port, const QString &cert_sha256, QString *error) {
	pins_.insert(key(host, port), cert_sha256);

	QString file_path = path();
	QDir().mkpath(QFileInfo(file_path).path());
	// QSaveFile writes to a temporary and renames over the original, so a
	// crash mid-write can't leave a truncated file that forgets every pin.
	QSaveFile file(file_path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		*error = file.errorString();
		return false;
	}
	QTextStream out(&file);
	out << "# spectre known hosts: login-server certificate SHA-256 per host.\n"
		<< "# Delete a host's line to be asked about it again.\n";
	for (auto it = pins_.cbegin(); it != pins_.cend(); ++it) {
		out << it.key() << ' ' << it.value() << '\n';
	}
	out.flush();
	if (!file.commit()) {
		*error = file.errorString();
		return false;
	}
	return true;
}
