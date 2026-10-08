// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// The skeleton every client-role GDP object shares: own a Transport and
// the one Connection it makes, hand the host event loop notify_fd() /
// dispatch(), and turn the connection's state changes into two hooks a
// subclass fills in. LobbyClient (lobby phase, gdp-spec.md §4) and
// spectre's SessionClient (session phase, §6) are the two subclasses; what
// differs between them -- which streams get opened, which Envelope is
// spoken on them -- lives entirely in on_connected() and the subclass's
// own stream handlers.
#pragma once

#include "gdp/transport.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace gdp {

class ClientConnection {
public:
	ClientConnection();
	virtual ~ClientConnection();
	ClientConnection(const ClientConnection &) = delete;
	ClientConnection &operator=(const ClientConnection &) = delete;

	// Becomes readable whenever dispatch() has work; see Transport.
	int notify_fd() const;
	void dispatch();

	// Host pinning (gdp-spec.md §2.3). Called from
	// dispatch() once the handshake completes, with the server
	// certificate's gdp::sha256_hex() fingerprint, before on_connected()
	// sends a single byte. Returning false closes the connection with
	// nothing sent -- no username, no token -- and the subclass then
	// reports a failure as for any other drop, with
	// certificate_rejected() true. Must be set before connecting: a
	// connection with no check at all is refused, never trusted.
	std::function<bool(const std::string &cert_sha256)> on_certificate;

	// Whether the last connection was closed because on_certificate
	// refused it (or was unset).
	bool certificate_rejected() const { return certificate_rejected_; }

	// CAs to check the server's certificate against as well (gdp-spec.md
	// §2.3), for a lobby client that accepts a CA-issued certificate in
	// place of a pin. Set before connecting. Empty by default: a
	// connection whose only trust root is a pin needs no CA check.
	CaTrust ca_trust;

	// Whether the server's certificate chains to `ca_trust` and is valid
	// for the name dialed. Meaningful from inside on_certificate on.
	bool certificate_ca_verified() const { return conn_ && conn_->peer_certificate_ca_verified(); }

protected:
	// Starts the QUIC connection on `alpn` (kAlpn, gdp/version.hpp, for
	// GDP itself; Wisp's agent speaks `wisp/1`). Returns false only on
	// immediate local failure (mirrors Transport::connect()); success or
	// failure of the actual handshake arrives via the hooks below, from
	// dispatch(). The certificate is checked only by on_certificate (with
	// certificate_ca_verified() to consult when ca_trust is set).
	bool connect_transport(const std::string &host, uint16_t port, const std::string &alpn);

	// The handshake completed: open streams, send the hello.
	virtual void on_connected() = 0;
	// The connection is gone (ConnectionState::kShutdown), whether closed
	// by the peer, dropped, or closed locally. conn_ is still non-null but
	// no longer usable.
	virtual void on_connection_shutdown() = 0;

	Transport transport_;
	std::unique_ptr<Connection> conn_;

private:
	bool certificate_rejected_ = false;
};

} // namespace gdp
