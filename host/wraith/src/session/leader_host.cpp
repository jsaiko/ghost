// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/leader_host.hpp"

#include "util/log.hpp"

#include <string>

namespace wraith {

bool LeaderHost::start_leader(struct wl_event_loop *loop, LaunchSpec spec, unsigned width, unsigned height) {
	spec.env.emplace_back("GHOST_OUTPUT_WIDTH", std::to_string(width));
	spec.env.emplace_back("GHOST_OUTPUT_HEIGHT", std::to_string(height));

	return session_process_.start(loop, spec, [this](int status) {
		WLOG_INFO("session: leader process exited (status %d)", status);
		session_ended_ = true;
		terminate();
	});
}

void LeaderHost::request_logout() {
	if (logout_requested_) {
		return;
	}
	logout_requested_ = true;
	if (session_process_.running()) {
		session_process_.begin_logout();
		return;
	}
	WLOG_INFO("session: logout requested with no session leader running, exiting");
	session_ended_ = true;
	terminate();
}

} // namespace wraith
