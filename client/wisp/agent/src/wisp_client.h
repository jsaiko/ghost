// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// wisp-agent's connection to Veil: `wisp/1` on the lobby port
// (libgdp/proto/wisp.proto). One bidi stream: the hello, then session
// changes from here, profiles and administrators' commands from Veil, for
// as long as both are up.
#pragma once

#include "gdp/client_connection.hpp"
#include "gdp/framing.hpp"

#include "wisp.pb.h"

#include <cstdint>
#include <functional>
#include <string>

namespace wisp_agent {

class WispClient : public gdp::ClientConnection {
public:
	// Connects and, once the certificate checks out (on_certificate, which
	// must be set), sends `hello`. Returns false only on immediate local
	// failure.
	bool connect(const std::string &host, uint16_t port, gdp::wisp::WispHello hello);

	// Tells Veil a session started or ended (all empty). Ignored before
	// the welcome: the hello carries the session then.
	void send_session(const gdp::wisp::WispSession &session);

	bool welcomed() const { return welcomed_; }

	// Veil's profile: once in its welcome, then whenever an admin changes it.
	std::function<void(const gdp::wisp::ClientProfile &profile)> on_profile;
	// An administrator's log out, restart or shut down.
	std::function<void(gdp::wisp::WispAction action)> on_command;
	// Fires once when the connection is gone, with its reason and the
	// gdp-spec.md §12 code Veil closed it with (0 if none).
	std::function<void(const std::string &reason, uint64_t code)> on_closed;

private:
	void on_connected() override;
	void on_connection_shutdown() override;
	void handle_stream_data(const uint8_t *data, size_t len);

	gdp::Stream *stream_ = nullptr;
	gdp::FrameReader reader_;
	gdp::wisp::WispHello hello_;
	bool welcomed_ = false;
};

} // namespace wisp_agent
