// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "tls.hpp"

#include "gdp/cert_fingerprint.hpp"

#include <ngtcp2/ngtcp2_crypto_ossl.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include "socket_platform.hpp"

#if defined(_WIN32)
#include <wincrypt.h>
#endif

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace gdp {

namespace {

void init_once() {
	static std::once_flag once;
	std::call_once(once, [] {
		// Optional, but ngtcp2 recommends it: it works around a performance
		// regression in OpenSSL's provider lookups.
		ngtcp2_crypto_ossl_init();
	});
}

void print_ssl_error(const char *what) {
	unsigned long err = ERR_get_error();
	char buf[256];
	ERR_error_string_n(err, buf, sizeof(buf));
	fprintf(stderr, "gdp: %s failed: %s\n", what, buf);
}

// ALPN in wire format: a length byte before the protocol name.
std::vector<unsigned char> alpn_wire(const std::string &alpn) {
	std::vector<unsigned char> wire;
	wire.push_back(static_cast<unsigned char>(alpn.size()));
	wire.insert(wire.end(), alpn.begin(), alpn.end());
	return wire;
}

// Server: select our one ALPN if the client offers it. Anything else ends
// the handshake with no_application_protocol, which the client reports as
// "ALPN negotiation failed".
int select_alpn(SSL *, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
	unsigned int inlen, void *arg) {
	const auto *want = static_cast<const std::string *>(arg);
	for (unsigned int i = 0; i < inlen;) {
		unsigned int len = in[i];
		if (i + 1 + len > inlen) {
			break;
		}
		if (len == want->size() && memcmp(in + i + 1, want->data(), len) == 0) {
			*out = in + i + 1;
			*outlen = static_cast<unsigned char>(len);
			return SSL_TLSEXT_ERR_OK;
		}
		i += 1 + len;
	}
	return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// CaTrust::system: the platform's own roots into `ctx`'s store. OpenSSL's
// default paths are the platform store only on Linux and the BSDs; a
// macOS or Windows OpenSSL (vcpkg's) points them at a directory nothing
// fills.
void load_system_roots(SSL_CTX *ctx) {
#if defined(_WIN32)
	HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
	if (!store) {
		fprintf(stderr, "gdp: opening the Windows ROOT certificate store failed\n");
		return;
	}
	X509_STORE *x509_store = SSL_CTX_get_cert_store(ctx);
	for (PCCERT_CONTEXT c = CertEnumCertificatesInStore(store, nullptr); c;
		c = CertEnumCertificatesInStore(store, c)) {
		const unsigned char *der = c->pbCertEncoded;
		if (X509 *x509 = d2i_X509(nullptr, &der, static_cast<long>(c->cbCertEncoded))) {
			X509_STORE_add_cert(x509_store, x509); // a duplicate fails harmlessly
			X509_free(x509);
		}
	}
	CertCloseStore(store, 0);
	ERR_clear_error();
#elif defined(__APPLE__)
	// The bundle macOS ships for its own command-line tools: the system
	// roots (not ones an admin added to a keychain -- those go in the
	// CaTrust::extra_file instead).
	if (SSL_CTX_load_verify_file(ctx, "/etc/ssl/cert.pem") != 1) {
		print_ssl_error("loading /etc/ssl/cert.pem");
	}
#else
	if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
		print_ssl_error("SSL_CTX_set_default_verify_paths");
	}
#endif
}

bool is_ip_literal(const std::string &host) {
	unsigned char buf[sizeof(in6_addr)];
	return inet_pton(AF_INET, host.c_str(), buf) == 1 || inet_pton(AF_INET6, host.c_str(), buf) == 1;
}

} // namespace

TlsContext::~TlsContext() {
	if (ctx_) {
		SSL_CTX_free(ctx_);
	}
}

bool TlsContext::init_server(const std::string &alpn, const std::string &cert_file,
	const std::string &key_file) {
	init_once();
	alpn_ = alpn;
	ctx_ = SSL_CTX_new(TLS_server_method());
	if (!ctx_) {
		print_ssl_error("SSL_CTX_new (server)");
		return false;
	}
	SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
	SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);
	// No resumption: nothing uses it, and it would need 0-RTT's anti-replay.
	SSL_CTX_set_num_tickets(ctx_, 0);
	SSL_CTX_set_options(ctx_, SSL_OP_NO_TICKET);
	SSL_CTX_set_alpn_select_cb(ctx_, select_alpn, &alpn_);
	if (SSL_CTX_use_certificate_chain_file(ctx_, cert_file.c_str()) != 1) {
		print_ssl_error("SSL_CTX_use_certificate_chain_file");
		return false;
	}
	if (SSL_CTX_use_PrivateKey_file(ctx_, key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
		print_ssl_error("SSL_CTX_use_PrivateKey_file");
		return false;
	}
	if (SSL_CTX_check_private_key(ctx_) != 1) {
		print_ssl_error("SSL_CTX_check_private_key");
		return false;
	}
	return true;
}

bool TlsContext::init_client(const std::string &alpn, const CaTrust &ca) {
	init_once();
	alpn_ = alpn;
	ctx_ = SSL_CTX_new(TLS_client_method());
	if (!ctx_) {
		print_ssl_error("SSL_CTX_new (client)");
		return false;
	}
	SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);
	SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);
	// SSL_VERIFY_NONE still runs the chain check on a client and keeps its
	// outcome (SSL_get_verify_result()); it only stops a failure from
	// ending the handshake. That outcome is all CaTrust asks for.
	SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
	check_ca_ = ca.any();
	if (ca.system) {
		load_system_roots(ctx_);
	}
	if (!ca.extra_file.empty() && SSL_CTX_load_verify_file(ctx_, ca.extra_file.c_str()) != 1) {
		print_ssl_error(("loading the CA file " + ca.extra_file).c_str());
	}
	return true;
}

TlsSession::~TlsSession() {
	if (!ossl_ctx_) {
		return;
	}
	if (SSL *s = ssl()) {
		// The conn_ref may already be gone; OpenSSL must not reach it.
		SSL_set_app_data(s, nullptr);
		SSL_free(s);
	}
	ngtcp2_crypto_ossl_ctx_del(ossl_ctx_);
}

SSL *TlsSession::ssl() const {
	return ossl_ctx_ ? ngtcp2_crypto_ossl_ctx_get_ssl(ossl_ctx_) : nullptr;
}

bool TlsSession::init_server(const TlsContext &ctx, ngtcp2_crypto_conn_ref *conn_ref) {
	if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx_, nullptr) != 0) {
		return false;
	}
	SSL *s = SSL_new(ctx.get());
	if (!s) {
		print_ssl_error("SSL_new (server)");
		return false;
	}
	ngtcp2_crypto_ossl_ctx_set_ssl(ossl_ctx_, s);
	if (ngtcp2_crypto_ossl_configure_server_session(s) != 0) {
		fprintf(stderr, "gdp: ngtcp2_crypto_ossl_configure_server_session failed\n");
		return false;
	}
	SSL_set_app_data(s, conn_ref);
	SSL_set_accept_state(s);
	return true;
}

bool TlsSession::init_client(const TlsContext &ctx, ngtcp2_crypto_conn_ref *conn_ref,
	const std::string &server_name) {
	if (ngtcp2_crypto_ossl_ctx_new(&ossl_ctx_, nullptr) != 0) {
		return false;
	}
	SSL *s = SSL_new(ctx.get());
	if (!s) {
		print_ssl_error("SSL_new (client)");
		return false;
	}
	ngtcp2_crypto_ossl_ctx_set_ssl(ossl_ctx_, s);
	if (ngtcp2_crypto_ossl_configure_client_session(s) != 0) {
		fprintf(stderr, "gdp: ngtcp2_crypto_ossl_configure_client_session failed\n");
		return false;
	}
	SSL_set_app_data(s, conn_ref);
	SSL_set_connect_state(s);
	std::vector<unsigned char> wire = alpn_wire(ctx.alpn());
	SSL_set_alpn_protos(s, wire.data(), static_cast<unsigned int>(wire.size()));
	if (!is_ip_literal(server_name)) {
		SSL_set_tlsext_host_name(s, server_name.c_str());
	}
	check_ca_ = ctx.check_ca_;
	if (check_ca_) {
		// The name the user dialed is what the certificate must be for:
		// an address literal against its IP SANs, anything else against
		// its DNS names.
		X509_VERIFY_PARAM *param = SSL_get0_param(s);
		X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
		int ok = is_ip_literal(server_name) ? X509_VERIFY_PARAM_set1_ip_asc(param, server_name.c_str())
											: X509_VERIFY_PARAM_set1_host(param, server_name.c_str(), 0);
		if (ok != 1) {
			check_ca_ = false; // a name OpenSSL can't check never verifies
		}
	}
	return true;
}

std::string TlsSession::peer_certificate_sha256() const {
	SSL *s = ssl();
	X509 *cert = s ? SSL_get0_peer_certificate(s) : nullptr;
	if (!cert) {
		return std::string();
	}
	unsigned char *der = nullptr;
	int len = i2d_X509(cert, &der);
	if (len <= 0) {
		return std::string();
	}
	std::string hex = sha256_hex(der, static_cast<size_t>(len));
	OPENSSL_free(der);
	return hex;
}

bool TlsSession::peer_certificate_ca_verified() const {
	SSL *s = ssl();
	return check_ca_ && s && SSL_get0_peer_certificate(s) && SSL_get_verify_result(s) == X509_V_OK;
}

} // namespace gdp
