// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/session_cert.hpp"

#include "gdp/cert_fingerprint.hpp"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <vector>

namespace wraith {

namespace {

struct Free {
	void operator()(EVP_PKEY *p) const { EVP_PKEY_free(p); }
	void operator()(X509 *p) const { X509_free(p); }
	void operator()(BIO *p) const { BIO_free(p); }
};
template <typename T> using Owned = std::unique_ptr<T, Free>;

// Writes `bio`'s contents into a new memfd. Returns the fd, or -1.
int memfd_from_bio(const char *name, BIO *bio) {
	char *data = nullptr;
	long len = BIO_get_mem_data(bio, &data);
	int fd = memfd_create(name, MFD_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	for (long off = 0; off < len;) {
		ssize_t n = write(fd, data + off, (size_t)(len - off));
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			close(fd);
			return -1;
		}
		off += n;
	}
	return fd;
}

} // namespace

SessionCert::~SessionCert() {
	if (cert_fd_ >= 0) {
		close(cert_fd_);
	}
	if (key_fd_ >= 0) {
		close(key_fd_);
	}
}

bool SessionCert::generate(std::string *error) {
	Owned<EVP_PKEY> key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"));
	Owned<X509> cert(X509_new());
	if (!key || !cert) {
		*error = "key generation failed";
		return false;
	}
	// Nothing ever checks the name or the validity window -- spectre pins
	// the exact certificate -- so they're just plausible: a CN naming what
	// this is, and a window wide enough that no clock skew or session
	// length could put it out of range for a TLS library that does look.
	X509_set_version(cert.get(), 2);
	uint64_t serial = 0;
	if (getentropy(&serial, sizeof(serial)) != 0) {
		*error = "getentropy failed";
		return false;
	}
	ASN1_INTEGER_set_uint64(X509_get_serialNumber(cert.get()), serial >> 1);
	X509_gmtime_adj(X509_getm_notBefore(cert.get()), -24 * 60 * 60);
	X509_gmtime_adj(X509_getm_notAfter(cert.get()), 10L * 365 * 24 * 60 * 60);
	X509_NAME *name = X509_get_subject_name(cert.get());
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
		reinterpret_cast<const unsigned char *>("wraith session"), -1, -1, 0);
	X509_set_issuer_name(cert.get(), name);
	if (!X509_set_pubkey(cert.get(), key.get()) || !X509_sign(cert.get(), key.get(), EVP_sha256())) {
		*error = "certificate signing failed";
		return false;
	}

	int der_len = i2d_X509(cert.get(), nullptr);
	if (der_len <= 0) {
		*error = "certificate encoding failed";
		return false;
	}
	std::vector<uint8_t> der((size_t)der_len);
	uint8_t *p = der.data();
	i2d_X509(cert.get(), &p);
	fingerprint_ = gdp::sha256_hex(der.data(), der.size());

	Owned<BIO> cert_pem(BIO_new(BIO_s_mem()));
	Owned<BIO> key_pem(BIO_new(BIO_s_mem()));
	if (!cert_pem || !key_pem || !PEM_write_bio_X509(cert_pem.get(), cert.get()) ||
		!PEM_write_bio_PrivateKey(key_pem.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr)) {
		*error = "PEM encoding failed";
		return false;
	}
	cert_fd_ = memfd_from_bio("wraith-session-cert", cert_pem.get());
	key_fd_ = memfd_from_bio("wraith-session-key", key_pem.get());
	if (cert_fd_ < 0 || key_fd_ < 0) {
		*error = std::string("memfd: ") + strerror(errno);
		return false;
	}
	cert_path_ = "/proc/self/fd/" + std::to_string(cert_fd_);
	key_path_ = "/proc/self/fd/" + std::to_string(key_fd_);
	return true;
}

} // namespace wraith
