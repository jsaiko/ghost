// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// TLS for QUIC: OpenSSL (3.5+, its own QUIC TLS API) driven through
// ngtcp2_crypto_ossl.
#pragma once

#include "gdp/transport.hpp"

#include <ngtcp2/ngtcp2_crypto.h>

#include <string>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct ngtcp2_crypto_ossl_ctx ngtcp2_crypto_ossl_ctx;

namespace gdp {

// One per listen() (its certificate and ALPN) or per connect().
class TlsContext {
public:
	TlsContext() = default;
	~TlsContext();
	TlsContext(const TlsContext &) = delete;
	TlsContext &operator=(const TlsContext &) = delete;

	// PEM files; memfd /proc/self/fd paths work as well as real ones.
	// Prints why on failure.
	bool init_server(const std::string &alpn, const std::string &cert_file, const std::string &key_file);
	// The server's certificate is accepted whatever it is: GDP pins it by
	// fingerprint (peer_certificate_sha256()). With `ca` non-empty, OpenSSL
	// also checks it against those CAs and the name dialed, and the result
	// is only reported (peer_certificate_ca_verified()).
	bool init_client(const std::string &alpn, const CaTrust &ca);

	SSL_CTX *get() const { return ctx_; }
	const std::string &alpn() const { return alpn_; }

private:
	SSL_CTX *ctx_ = nullptr;
	std::string alpn_;
	bool check_ca_ = false;
	friend class TlsSession;
};

// One connection's TLS state. `conn_ref` must outlive it; its get_conn is
// how ngtcp2_crypto_ossl finds the ngtcp2_conn from inside OpenSSL.
class TlsSession {
public:
	TlsSession() = default;
	~TlsSession();
	TlsSession(const TlsSession &) = delete;
	TlsSession &operator=(const TlsSession &) = delete;

	bool init_server(const TlsContext &ctx, ngtcp2_crypto_conn_ref *conn_ref);
	// `server_name` is sent as SNI unless it's an IP literal.
	bool init_client(const TlsContext &ctx, ngtcp2_crypto_conn_ref *conn_ref, const std::string &server_name);

	// What ngtcp2_conn_set_tls_native_handle() takes.
	ngtcp2_crypto_ossl_ctx *native_handle() const { return ossl_ctx_; }

	// Lowercase hex SHA-256 of the peer's DER certificate; empty if it sent
	// none. Valid once the handshake has completed.
	std::string peer_certificate_sha256() const;
	// Whether that certificate chains to the context's CaTrust and is valid
	// for `server_name`. Valid once the handshake has completed.
	bool peer_certificate_ca_verified() const;

private:
	SSL *ssl() const;

	ngtcp2_crypto_ossl_ctx *ossl_ctx_ = nullptr;
	bool check_ca_ = false;
};

} // namespace gdp
