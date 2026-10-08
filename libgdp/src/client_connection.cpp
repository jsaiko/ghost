// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/client_connection.hpp"

namespace gdp {

ClientConnection::ClientConnection() = default;
ClientConnection::~ClientConnection() = default;

int ClientConnection::notify_fd() const {
	return transport_.notify_fd();
}

void ClientConnection::dispatch() {
	transport_.dispatch();
}

bool ClientConnection::connect_transport(const std::string &host, uint16_t port, const std::string &alpn) {
	conn_ = transport_.connect(host, port, alpn, ca_trust);
	if (!conn_) {
		return false;
	}
	certificate_rejected_ = false;
	conn_->on_state_changed = [this](ConnectionState state) {
		if (state == ConnectionState::kConnected) {
			const std::string &fingerprint = conn_->peer_certificate_sha256();
			if (!on_certificate || fingerprint.empty() || !on_certificate(fingerprint)) {
				// kShutdown follows from a later dispatch(), and with it
				// the subclass's usual failure report.
				certificate_rejected_ = true;
				conn_->close();
				return;
			}
			on_connected();
		} else if (state == ConnectionState::kShutdown) {
			on_connection_shutdown();
		}
	};
	// Client role: we open the streams, never receive them.
	conn_->on_control_stream = nullptr;
	conn_->on_input_stream = nullptr;
	return true;
}

} // namespace gdp
