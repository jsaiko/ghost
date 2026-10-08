// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/lobby_client.hpp"

#include "gdp/version.hpp"

#include "lobby.pb.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace gdp {

namespace {

// lobby.proto's comment on LobbyHello.client_id: "opaque, client-chosen
// (e.g. hostname + install UUID)". ghostd only ever logs it (lobby.rs) --
// no install-UUID mechanism exists yet, so the hostname alone is enough to
// tell one client from another in that log.
std::string local_client_id() {
#ifdef _WIN32
	// gethostname() exists in winsock2.h too, but needs WSAStartup() first
	// (WSANOTINITIALISED otherwise) -- GetComputerNameA() needs no socket
	// library init for what's just a display label here.
	char buf[256];
	DWORD size = sizeof(buf);
	if (!GetComputerNameA(buf, &size)) {
		return "gdp-client";
	}
	return buf;
#else
	char buf[256] = {};
	if (gethostname(buf, sizeof(buf) - 1) != 0) {
		return "gdp-client";
	}
	return buf;
#endif
}

} // namespace

LobbyClient::LobbyClient() = default;
LobbyClient::~LobbyClient() = default;

bool LobbyClient::connect(const std::string &host, uint16_t port, const std::string &username) {
	username_ = username;
	lobby_host_ = host;
	return connect_transport(host, port, kAlpn);
}

void LobbyClient::on_connected() {
	stream_ = conn_->open_control_stream();
	if (!stream_) {
		fail("failed to open lobby stream");
		return;
	}
	stream_->on_data = [this](const uint8_t *data, size_t len) { handle_stream_data(data, len); };

	lobby::LobbyEnvelope env;
	auto *hello = env.mutable_hello();
	hello->set_protocol_version(kWireVersion);
	hello->set_client_id(local_client_id());
	hello->set_auth_method(lobby::AUTH_METHOD_PASSWORD);
	hello->set_username(username_);
	if (!stream_->send_message(env)) {
		fail("failed to encode LobbyHello");
	}
}

void LobbyClient::on_connection_shutdown() {
	// Expected once a Redirect/LobbyError has already been delivered
	// (gdp-spec.md §4.2: the host closes either way) -- only an error if
	// the connection dropped before either arrived.
	if (!done_) {
		if (certificate_rejected()) {
			fail("the host's certificate was not accepted");
			return;
		}
		const std::string &reason = conn_->shutdown_reason();
		error_code_ = conn_->shutdown_error_code();
		fail(reason.empty() ? "connection closed before completing login"
							: "connection closed before completing login: " + reason);
	}
}

void LobbyClient::respond(const std::string &answer) {
	if (!stream_) {
		return;
	}
	lobby::LobbyEnvelope env;
	env.mutable_auth_response()->set_response(answer);
	if (!stream_->send_message(env)) {
		fail("failed to encode AuthResponse");
	}
}

void LobbyClient::open_session(const std::string &type_id) {
	if (!stream_) {
		return;
	}
	lobby::LobbyEnvelope env;
	env.mutable_session_open()->set_session_type(type_id);
	if (!stream_->send_message(env)) {
		fail("failed to encode SessionOpen");
	}
}

void LobbyClient::select_device(const std::string &device_id) {
	if (!stream_) {
		return;
	}
	lobby::LobbyEnvelope env;
	env.mutable_device_select()->set_device_id(device_id);
	if (!stream_->send_message(env)) {
		fail("failed to encode DeviceSelect");
	}
}

void LobbyClient::handle_stream_data(const uint8_t *data, size_t len) {
	lobby::LobbyEnvelope env;
	bool ok = reader_.feed_and_drain(data, len, &env, [&] {
		if (env.has_auth_challenge()) {
			if (on_auth_prompt) {
				on_auth_prompt(env.auth_challenge().prompt(), env.auth_challenge().echo_input());
			}
		} else if (env.has_device_list()) {
			if (!on_device_list) {
				fail("this server is a Veil broker, and this client can't choose a host");
				return false;
			}
			std::vector<DeviceInfo> devices;
			for (const auto &d : env.device_list().devices()) {
				DeviceInfo info{d.id(), d.name(), d.online(), d.has_session(), d.session_type(), {},
					d.default_type()};
				for (const auto &t : d.available_types()) {
					info.types.push_back({t.id(), t.name()});
				}
				devices.push_back(std::move(info));
			}
			on_device_list(devices);
			if (done_) {
				return false; // a synchronous select_device() call failed and called fail()
			}
		} else if (env.has_session_list()) {
			if (on_session_list) {
				SessionListInfo list;
				for (const auto &t : env.session_list().available_types()) {
					list.types.push_back({t.id(), t.name()});
				}
				list.default_type = env.session_list().default_type();
				for (const auto &s : env.session_list().sessions()) {
					list.running.push_back({s.session_id(), s.session_type(), s.started_at_unix(),
						s.last_active_unix(), s.viewer_attached()});
				}
				// The caller's open_session() call (from anywhere after
				// this returns) is what actually sends SessionOpen -- same
				// exactly-once, any-point contract as on_auth_prompt/
				// respond().
				on_session_list(list);
				if (done_) {
					return false; // a synchronous open_session() call failed and called fail()
				}
			} else {
				// No picker attached: request the server's default.
				open_session("");
				if (done_) {
					return false; // open_session()'s send failed and called fail()
				}
			}
		} else if (env.has_redirect()) {
			done_ = true;
			const auto &r = env.redirect();
			if (on_redirect) {
				// gdp-spec.md §4.7: no host means the session is on the
				// same host we just reached the lobby on.
				const std::string &host = r.host().empty() ? lobby_host_ : r.host();
				on_redirect(host, (uint16_t)r.port(), r.token(), r.expiry_unix(), r.cert_sha256());
			}
			return false;
		} else if (env.has_error()) {
			error_code_ = static_cast<uint64_t>(env.error().code());
			fail(env.error().message());
			return false;
		}
		// LobbyHello/AuthResponse/SessionOpen: client-to-server only, never
		// expected back.
		return true;
	});
	if (!ok) {
		fail("malformed lobby frame");
	}
}

void LobbyClient::fail(const std::string &message) {
	if (done_) {
		return;
	}
	done_ = true;
	if (on_error) {
		on_error(message);
	}
}

} // namespace gdp
