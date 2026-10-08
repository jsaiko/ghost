// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/session_process.hpp"

#include <wayland-server-core.h>

#include "util/log.hpp"

#include <csignal>
#include <cstdlib>
#include <ctime>

#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace wraith {

namespace {

// glibc has no pidfd_open() wrapper as of the version this targets; the
// syscall itself has been stable since Linux 5.3.
int pidfd_open(pid_t pid) {
	return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
}

constexpr int kTermGraceMs = 500;
// How long a graceful logout gets before the leader is terminated
// outright. Long enough for a DE to put up
// its "unsaved changes" prompts and have them answered.
constexpr int kLogoutGraceMs = 30'000;

// The child-side half of every spawn here: own session, the spec's
// environment, then /bin/sh -c. Never returns.
[[noreturn]] void exec_in_child(const std::string &command,
	const std::vector<std::pair<std::string, std::string>> &env) {
	setsid();
	for (const auto &[key, value] : env) {
		setenv(key.c_str(), value.c_str(), 1);
	}
	execl("/bin/sh", "/bin/sh", "-c", command.c_str(), (void *)nullptr);
	_exit(127);
}

} // namespace

bool SessionProcess::start(struct wl_event_loop *loop, const LaunchSpec &spec,
	std::function<void(int)> on_exit) {
	pid_t pid = fork();
	if (pid < 0) {
		WLOG_ERROR("session: fork failed");
		return false;
	}

	if (pid == 0) {
		// New process group: terminate() below can signal the whole tree
		// (a shell, and whatever it execs) with one kill(-pid, ...).
		exec_in_child(spec.command, spec.env);
	}

	loop_ = loop;
	pid_ = pid;
	on_exit_ = std::move(on_exit);
	logout_command_ = spec.logout_command;
	env_ = spec.env;
	logout_started_ = false;

	int fd = pidfd_open(pid);
	if (fd < 0) {
		WLOG_ERROR("session: pidfd_open failed, session-exit tracking disabled");
		return true;
	}
	pidfd_ = fd;

	pidfd_source_.reset(wl_event_loop_add_fd(
		loop, fd, WL_EVENT_READABLE,
		[](int, uint32_t, void *data) {
			auto *self = static_cast<SessionProcess *>(data);
			int status = 0;
			waitpid(self->pid_, &status, 0);
			// Removing a source from inside its own dispatch is fine:
			// libwayland only unlinks it here and frees it once the
			// dispatch returns. Off the loop first, then the fd it
			// watched (the loop holds its own dup of it).
			self->pidfd_source_.reset();
			close(self->pidfd_);
			self->pidfd_ = -1;
			self->pid_ = -1;
			self->handle_exited(status);
			return 0;
		},
		this));

	return true;
}

void SessionProcess::handle_exited(int status) {
	logout_timer_.reset();
	if (on_exit_) {
		auto on_exit = std::move(on_exit_);
		on_exit_ = nullptr;
		on_exit(status);
	}
}

void SessionProcess::begin_logout() {
	if (pid_ <= 0 || logout_started_) {
		return;
	}
	logout_started_ = true;

	if (!logout_command_.empty()) {
		WLOG_INFO("session: logout requested, running: %s", logout_command_.c_str());
		// Double fork so init reaps the helper: it's fire-and-forget, and
		// the thing actually being waited on is the leader's pidfd.
		pid_t pid = fork();
		if (pid == 0) {
			pid_t grandchild = fork();
			if (grandchild == 0) {
				exec_in_child(logout_command_, env_);
			}
			_exit(grandchild < 0 ? 1 : 0);
		}
		if (pid < 0) {
			WLOG_ERROR("session: fork for LogoutExec failed, falling back to SIGTERM");
			kill(pid_, SIGTERM);
		} else {
			int status = 0;
			waitpid(pid, &status, 0);
		}
	} else {
		WLOG_INFO("session: logout requested, sending SIGTERM to leader %d", (int)pid_);
		kill(pid_, SIGTERM);
	}

	if (!loop_) {
		return;
	}
	logout_timer_.reset(wl_event_loop_add_timer(
		loop_,
		[](void *data) {
			auto *self = static_cast<SessionProcess *>(data);
			if (self->pid_ <= 0) {
				return 0;
			}
			WLOG_ERROR("session: leader %d still running %d s after logout, terminating it", (int)self->pid_,
				kLogoutGraceMs / 1000);
			// terminate() reaps the leader itself, so the pidfd callback
			// never runs for it -- report the exit here instead.
			self->terminate();
			self->handle_exited(-1);
			return 0;
		},
		this));
	wl_event_source_timer_update(logout_timer_.get(), kLogoutGraceMs);
}

void SessionProcess::terminate() {
	logout_timer_.reset();
	if (pid_ <= 0) {
		return;
	}

	pid_t pid = pid_;
	kill(-pid, SIGTERM);

	struct timespec grace = {0, kTermGraceMs * 1000000L};
	nanosleep(&grace, nullptr);

	if (kill(-pid, 0) == 0) {
		kill(-pid, SIGKILL);
	}

	int status = 0;
	waitpid(pid, &status, 0);

	pidfd_source_.reset();
	if (pidfd_ >= 0) {
		close(pidfd_);
		pidfd_ = -1;
	}
	pid_ = -1;
}

} // namespace wraith
