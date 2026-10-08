// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "boot_config.h"

#include "gdp/cert_fingerprint.hpp"

#include <QFile>
#include <QStringList>

namespace {

constexpr uint16_t kVeilPortDefault = 4442; // veild's [lobby] port default (host/veil/src/config.rs)

// The value of `key=` on the kernel command line, or empty.
QString cmdline_value(const QStringList &args, const QString &key) {
	const QString prefix = key + '=';
	for (const QString &arg : args) {
		if (arg.startsWith(prefix)) {
			return arg.mid(prefix.size());
		}
	}
	return QString();
}

} // namespace

bool parse_host_and_port(const QString &input, uint16_t default_port, QString *host, uint16_t *port) {
	if (input.startsWith('[')) {
		int close = input.indexOf(']');
		if (close < 0) {
			return false;
		}
		QString rest = input.mid(close + 1);
		uint value = default_port;
		if (!rest.isEmpty()) {
			bool ok = false;
			value = rest.startsWith(':') ? rest.mid(1).toUInt(&ok) : 0;
			if (!ok || value == 0 || value > 65535) {
				return false;
			}
		}
		*host = input.mid(1, close - 1);
		*port = (uint16_t)value;
		return true;
	}
	int colon = input.lastIndexOf(':');
	if (colon < 0 || input.indexOf(':') != colon) {
		*host = input;
		*port = default_port;
		return true;
	}
	bool ok = false;
	uint value = input.mid(colon + 1).toUInt(&ok);
	if (!ok || value == 0 || value > 65535) {
		return false;
	}
	*host = input.left(colon);
	*port = (uint16_t)value;
	return true;
}

QString normalize_fingerprint(const QString &input) {
	QString hex = input;
	hex.remove(':').remove(' ');
	hex = hex.toLower();
	return gdp::is_sha256_hex(hex.toStdString()) ? hex : QString();
}

BootConfig BootConfig::load(const QString &veil_override, const QString &cert_override) {
	QStringList args;
	QFile file("/proc/cmdline");
	if (file.open(QIODevice::ReadOnly)) {
		args = QString::fromLocal8Bit(file.readAll()).simplified().split(' ', Qt::SkipEmptyParts);
	}
	QString veil = !veil_override.isEmpty() ? veil_override : cmdline_value(args, "veil");
	QString cert = !cert_override.isEmpty() ? cert_override : cmdline_value(args, "veil_cert");

	BootConfig config;
	if (veil.isEmpty()) {
		config.error =
			"This client was not told which Veil server to use (no veil= on the kernel command line).";
		return config;
	}
	if (!parse_host_and_port(veil, kVeilPortDefault, &config.veil_host, &config.veil_port) ||
		config.veil_host.isEmpty()) {
		config.error = QString("The Veil address \"%1\" is not valid.").arg(veil);
		return config;
	}
	// Refuse rather than fall back to trusting whatever answers: a kiosk has
	// nobody to judge a fingerprint and nowhere to remember one.
	if (cert.isEmpty()) {
		config.error = "This client was not told the Veil server's certificate fingerprint "
					   "(no veil_cert= on the kernel command line), so it cannot verify the server.";
		return config;
	}
	config.veil_cert = normalize_fingerprint(cert);
	if (config.veil_cert.isEmpty()) {
		config.error = "The Veil certificate fingerprint on the kernel command line is not 64 hex digits.";
	}
	return config;
}
