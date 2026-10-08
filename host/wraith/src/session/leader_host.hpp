// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The session-leader half of a SessionHost: forks one leader process,
// adds the output size to its environment, ends the session when it
// exits, and answers a client's LogoutRequest by asking it to log out.
// ScreencastHost keeps everything that depends on the compositor.
#pragma once

#include "session/session_process.hpp"
#include "session/session_host.hpp"

namespace wraith {

class LeaderHost : public SessionHost {
public:
	// A client-requested logout (a spectre LogoutRequest,
	// docs/design/login-and-sessions.md#teardown): asks the leader to end gracefully
	// (SessionProcess::begin_logout) and exits once it is gone. With no
	// leader running at all, exits immediately.
	void request_logout() override;

	// True once the host is shutting down *because the desktop session
	// ended* (its leader exited or a logout was requested), as opposed to
	// any other reason to stop. GdpSession reads it to close an attached
	// client with SESSION_ENDED rather than a plain close.
	bool session_ended() const override { return session_ended_; }

protected:
	// Forks `spec` as the session leader on `loop`, with GHOST_OUTPUT_WIDTH
	// and GHOST_OUTPUT_HEIGHT set from `width`/`height` in addition to
	// spec.env. False if the fork failed. When the leader later exits, the
	// session is over: session_ended() turns true and terminate() is
	// called.
	bool start_leader(struct wl_event_loop *loop, LaunchSpec spec, unsigned width, unsigned height);

	SessionProcess session_process_;
	bool logout_requested_ = false;
	bool session_ended_ = false;
};

} // namespace wraith
