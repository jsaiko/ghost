// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QMap>
#include <QString>

// spectre's trust-on-first-use pin store (gdp-spec.md §2.3):
// one login-server certificate fingerprint per host. Only the lobby's
// certificate is pinned here -- the session's own (an ephemeral one per
// wraith) is vouched for by the pinned lobby in Redirect.cert_sha256 and
// handed to spectre as -P. A lobby certificate a trusted CA issued for
// the name typed needs no pin at all (ConnectWindow's on_certificate):
// the platform's CAs count, and so do any in ca_bundle_path().
//
// A plain text file (~/.config/spectre/known_hosts on Linux,
// ~/Library/Preferences/spectre/known_hosts on macOS), one
// "host:port sha256hex" line per host, in the spirit of ssh's known_hosts:
// readable, and a changed host key is fixed by deleting one line. The host
// is the string as typed (lowercased), so "box" and "box.lan" are separate
// entries, the same as ssh.
class KnownHosts {
public:
	enum class Status {
		kTrusted,  // pinned, and this is the pinned certificate
		kUnknown,  // never seen: first use
		kMismatch, // pinned to a different certificate
	};

	// Loads the file; a missing or unreadable one is just an empty store.
	KnownHosts();

	Status check(const QString &host, uint16_t port, const QString &cert_sha256) const;
	// Records (or replaces) the pin and writes the file. Returns false,
	// with `*error` set, if the write failed -- the pin is still held in
	// memory for this run.
	bool trust(const QString &host, uint16_t port, const QString &cert_sha256, QString *error);

	static QString path();
	// Further CAs spectre-qt trusts for login servers, a PEM bundle beside
	// known_hosts (an organization's own CA, deployed by its admins). It
	// need not exist.
	static QString ca_bundle_path();

private:
	static QString key(const QString &host, uint16_t port);

	QMap<QString, QString> pins_; // key() -> sha256 hex
};
