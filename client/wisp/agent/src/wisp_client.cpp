// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "wisp_client.h"

namespace wisp_agent {

namespace {

constexpr const char *kWispAlpn = "wisp/1";
// Veil counts a connection that has sent nothing for its [hosts]
// idle_timeout (30 s by default) as gone, and so does libgdp; a quiet
// client pings well inside that.
constexpr uint32_t kKeepAliveMs = 10000;

} // namespace

bool WispClient::connect(const std::string &host, uint16_t port, gdp::wisp::WispHello hello) {
	hello_ = std::move(hello);
	return connect_transport(host, port, kWispAlpn);
}

void WispClient::on_connected() {
	stream_ = conn_->open_control_stream();
	if (!stream_) {
		conn_->close();
		return;
	}
	conn_->set_keep_alive(kKeepAliveMs);
	stream_->on_data = [this](const uint8_t *data, size_t len) { handle_stream_data(data, len); };
	gdp::wisp::WispEnvelope env;
	*env.mutable_hello() = hello_;
	if (!stream_->send_message(env)) {
		conn_->close();
	}
}

void WispClient::on_connection_shutdown() {
	stream_ = nullptr;
	if (on_closed) {
		std::string reason =
			certificate_rejected() ? "Veil's certificate doesn't match veil_cert=" : conn_->shutdown_reason();
		on_closed(reason, conn_->shutdown_error_code());
	}
}

void WispClient::send_session(const gdp::wisp::WispSession &session) {
	if (!stream_ || !welcomed_) {
		return;
	}
	gdp::wisp::WispEnvelope env;
	*env.mutable_session() = session;
	stream_->send_message(env);
}

void WispClient::handle_stream_data(const uint8_t *data, size_t len) {
	gdp::wisp::WispEnvelope env;
	bool ok = reader_.feed_and_drain(data, len, &env, [&] {
		if (env.has_welcome()) {
			welcomed_ = true;
			if (on_profile) {
				on_profile(env.welcome().profile());
			}
		} else if (env.has_profile() && on_profile) {
			on_profile(env.profile().profile());
		} else if (env.has_command() && on_command) {
			on_command(env.command().action());
		}
		return true;
	});
	if (!ok) {
		conn_->close(reader_.error_code());
	}
}

} // namespace wisp_agent
