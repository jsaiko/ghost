// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// The client side of the GDP lobby phase (gdp-spec.md §4):
// authenticate with ghostd -- or with Veil, the broker, which then asks
// which host to use -- over a single QUIC bidi stream and come away
// with a Redirect to a wraith session -- everything spectre's
// SessionClient (client/spectre/src/net/session_client.hpp) needs to actually
// connect. Lives in libgdp rather than either client because it depends
// on nothing but the protocol library: spectre-qt's login form drives
// it, libgdp's own gdp_lobby_client_test exercises it against a real
// ghostd, and the headless `spectre` binary never uses it at all (it is
// direct-connect only, handed the resolved host/port/token/fingerprint
// as plain -h/-p/-t/-P arguments once this finishes).
#pragma once

#include "gdp/client_connection.hpp"
#include "gdp/framing.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gdp {

// One selectable desktop from
// SessionList.available_types.
struct SessionTypeInfo {
	std::string id;
	std::string name;
};

// An already-running session for this uid (SessionList.sessions). ghostd
// lists at most one -- one session per uid -- and any SessionOpen then
// reattaches to it; spectre-qt uses it to show a reattach notice.
struct RunningSessionInfo {
	std::string session_id;
	std::string session_type;
	int64_t started_at_unix = 0;
	int64_t last_active_unix = 0;
	// Another client is viewing it now: connecting needs take_over
	// (gdp-spec.md §6.4), which the user should be asked about first.
	bool viewer_attached = false;
};

struct SessionListInfo {
	std::vector<SessionTypeInfo> types;
	std::string default_type; // an id from `types`, or empty if `types` is empty
	std::vector<RunningSessionInfo> running;
};

// One host a Veil user may log in to (DeviceList, gdp-spec.md §4
// "Brokered login").
struct DeviceInfo {
	std::string id;
	std::string name;
	bool online = false;
	bool has_session = false; // this user has a session running there
	std::string session_type; // that session's type, when has_session
	// The desktops the host offers (empty when offline or unreported) and
	// the one it defaults to, so the client can pick one with the host.
	std::vector<SessionTypeInfo> types;
	std::string default_type;
};

class LobbyClient : public ClientConnection {
public:
	LobbyClient();
	~LobbyClient() override;

	// Connects to ghostd and sends LobbyHello for `username`. The rest of
	// the exchange (gdp-spec.md §4's AuthChallenge/AuthResponse loop,
	// then SessionOpen) is driven from dispatch() and the callbacks below;
	// there's nothing further to call here. Returns false only on
	// immediate local failure (mirrors gdp::Transport::connect()).
	bool connect(const std::string &host, uint16_t port, const std::string &username);

	// Fires once per PAM prompt (gdp-spec.md §4.5: the exchange repeats until
	// authentication resolves). `echo` says whether the answer is normally-visible input
	// (a username-like prompt) or not (a password). The caller must
	// eventually call respond() -- exactly once, from any point after this
	// fires and before the next on_auth_prompt/on_redirect/on_error -- to
	// keep the exchange moving; there is no timeout on this side.
	std::function<void(const std::string &prompt, bool echo)> on_auth_prompt;
	void respond(const std::string &answer);

	// Fires once auth succeeds, before any Redirect: the session types this
	// host can offer and its recommended default (gdp-spec.md §4's
	// "Session types" paragraph). If left unset, the exchange proceeds as
	// if open_session("") had been called immediately, for callers that
	// don't care. If set, the caller must
	// eventually call open_session() -- exactly once, same contract as
	// respond() -- to keep the exchange moving.
	std::function<void(const SessionListInfo &list)> on_session_list;

	// Fires once auth succeeds when the server is Veil, the broker: the
	// hosts this user may use, running sessions first. The caller must
	// eventually call select_device() -- exactly once, same contract as
	// respond(). The chosen host's own exchange follows: more
	// on_auth_prompt calls if its PAM stack asks for anything beyond the
	// password (Veil answers that one itself), then on_session_list.
	// Against a plain ghostd this never fires. Left unset, a DeviceList
	// ends the login with on_error.
	std::function<void(const std::vector<DeviceInfo> &devices)> on_device_list;
	// `device_id` must be one of the ids from the preceding on_device_list.
	void select_device(const std::string &device_id);
	// `type_id` must be one of SessionListInfo::types' ids from the
	// preceding on_session_list, or empty to mean "use default_type".
	void open_session(const std::string &type_id);

	// Fires once, on success: `token` is opaque and expires at
	// `expiry_unix` (gdp-spec.md §4) -- hand all four straight to
	// SessionClient::connect() (or the `spectre` binary's -h/-p/-t) well
	// before that, since ghostd's TOKEN_TTL_SECS is short. `host` is
	// already filled in: an empty Redirect.host means "the lobby's own
	// host" (gdp-spec.md §4.7), and the host passed to connect() is
	// substituted here, so the callback never sees an empty string.
	// `cert_sha256` is the fingerprint the session's wraith will present
	// (gdp-spec.md §2.3) -- trustworthy because it
	// arrived over this connection, which on_certificate already pinned;
	// the session connection must be checked against it (spectre -P).
	std::function<void(const std::string &host, uint16_t port, const std::string &token, int64_t expiry_unix,
		const std::string &cert_sha256)>
		on_redirect;
	// Fires exactly once on any failure -- a LobbyError from ghostd, a
	// malformed frame, on_certificate refusing the host (check
	// certificate_rejected()), or the connection dropping before a
	// Redirect/Error ever arrived -- and ends the exchange; neither on_redirect nor
	// another on_auth_prompt follows.
	std::function<void(const std::string &message)> on_error;
	// Once on_error has fired: the gdp-spec.md §12 code behind it -- the
	// LobbyError's, or the connection's close code -- for a caller that
	// words some failures itself (HOST_OFFLINE, HOST_AUTH_FAILED, ...).
	// 0 when there was none (a local failure, a rejected certificate).
	uint64_t error_code() const { return error_code_; }

private:
	void on_connected() override;
	void on_connection_shutdown() override;
	void handle_stream_data(const uint8_t *data, size_t len);
	void fail(const std::string &message);

	// The lobby phase is one bidi stream at QUIC stream ID 0 (gdp-spec.md
	// §2.2) -- open_control_stream() opens exactly that stream; its name is
	// session-phase terminology this connection never uses otherwise
	// (open_input_stream() is never called).
	Stream *stream_ = nullptr;
	FrameReader reader_;
	std::string username_;
	// The host string as passed to connect(), kept verbatim to stand in
	// for an empty Redirect.host. Deliberately not the resolved address:
	// re-resolving the same name is what keeps SNI and certificate
	// validation working for the session connection.
	std::string lobby_host_;
	bool done_ = false; // on_redirect or on_error already fired
	uint64_t error_code_ = 0;
};

} // namespace gdp
