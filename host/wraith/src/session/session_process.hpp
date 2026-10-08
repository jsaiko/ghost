// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The session's leader process: the one child wraith forks to actually
// run a desktop -- a sessions.d profile's `Exec` (a plain command, or a
// launcher script like packaging/ghost/sessions/plasma-screencast).
//
// Tracked via pidfd rather than wl_event_loop_add_signal: by the time a
// session starts, libgdp and PipeWire have their own threads running, and
// SIGCHLD delivery to a multi-threaded process picks an arbitrary thread
// -- a signal-based watch could silently never fire on this one. A pidfd
// is readable exactly once, when the process exits, regardless of which
// thread it's polled from.
#pragma once

#include "util/event_source.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

struct wl_event_loop;

namespace wraith {

struct LaunchSpec {
	std::string command;                                  // run via /bin/sh -c
	std::vector<std::pair<std::string, std::string>> env; // additional exported vars
	// How begin_logout() asks the desktop to end itself (a profile's
	// LogoutExec=, on a client's LogoutRequest).
	// Run with the same environment as `command`. Empty: SIGTERM the leader instead.
	std::string logout_command;
};

class SessionProcess {
public:
	SessionProcess() = default;
	~SessionProcess() { terminate(); }

	SessionProcess(const SessionProcess &) = delete;
	SessionProcess &operator=(const SessionProcess &) = delete;

	// Forks and execs `spec.command`. `on_exit` runs on the compositor's
	// event loop once the child has exited (from the pidfd becoming
	// readable), and is only ever invoked once. Returns false if the fork
	// itself failed; a failure to exec inside the child instead surfaces
	// as an ordinary (exit code 127) process exit.
	bool start(struct wl_event_loop *loop, const LaunchSpec &spec, std::function<void(int)> on_exit);

	// Graceful logout (docs/design/login-and-sessions.md#teardown): runs the spec's
	// logout_command if it has one, otherwise SIGTERMs the leader alone
	// (not its process group -- the DE's own handler gets to run). If the
	// leader is still around after kLogoutGraceMs, escalates to
	// terminate(). Either way `on_exit` fires exactly once when the leader
	// is gone, same as a self-initiated exit. Idempotent; a no-op with
	// nothing running.
	void begin_logout();

	// Idempotent: SIGTERM to the process group, a short grace period,
	// then SIGKILL, then reap. Safe to call with nothing running. Does
	// not fire `on_exit` -- this is the teardown path, not an event.
	void terminate();

	bool running() const { return pid_ > 0; }

private:
	void handle_exited(int status);

	pid_t pid_ = -1;
	std::function<void(int)> on_exit_;
	EventSource pidfd_source_;
	int pidfd_ = -1;

	struct wl_event_loop *loop_ = nullptr;
	std::string logout_command_;
	std::vector<std::pair<std::string, std::string>> env_;
	bool logout_started_ = false;
	EventSource logout_timer_;
};

} // namespace wraith
