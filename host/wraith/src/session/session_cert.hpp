// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The throwaway GDP certificate a ghostd-mediated (-G) wraith presents
// (gdp-spec.md §2.3). wraith runs as the logged-in
// user, so any key it holds is readable by that user -- the host's own
// identity key therefore stays with ghostd (readable by its own group
// only), and each session generates a fresh self-signed certificate
// instead. Its fingerprint goes
// to ghostd in SessionReady, which vouches for it to spectre in
// Redirect.cert_sha256 over the already-pinned lobby connection. The worst
// a user can do with their own session's key is impersonate their own
// session.
#pragma once

#include <string>

namespace wraith {

class SessionCert {
public:
	SessionCert() = default;
	~SessionCert();
	SessionCert(const SessionCert &) = delete;
	SessionCert &operator=(const SessionCert &) = delete;

	// Generates an ECDSA P-256 key and a self-signed certificate for it.
	// The PEMs live in memfds, never on disk; cert_path()/key_path() are
	// their /proc/self/fd paths, which is the form gdp::Transport::listen()
	// takes (OpenSSL reads them like any other file).
	bool generate(std::string *error);

	const std::string &cert_path() const { return cert_path_; }
	const std::string &key_path() const { return key_path_; }
	// gdp::sha256_hex() of the certificate's DER.
	const std::string &fingerprint() const { return fingerprint_; }

private:
	int cert_fd_ = -1;
	int key_fd_ = -1;
	std::string cert_path_;
	std::string key_path_;
	std::string fingerprint_;
};

} // namespace wraith
