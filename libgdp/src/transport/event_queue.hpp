// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// The bridge from the network thread to the host's thread: what
// Transport::notify_fd() and Transport::dispatch() are built on.
#pragma once

#if defined(__linux__)
#include <sys/eventfd.h>
#include <unistd.h>
#elif !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <deque>
#include <functional>
#include <mutex>

namespace gdp {

// notify_fd()'s wakeup signal is an eventfd on Linux and the read end of a
// non-blocking self-pipe on other POSIX systems (macOS has no eventfd). On
// Windows there's no equivalent fd to hand a host event loop, so callers
// there (spectre's stream_session.cpp) just poll dispatch() on a timer
// instead -- fd_ stays -1 and post()/drain() skip the read/write, but the
// actual mutex-protected queue below (what dispatch() drains) is unchanged
// and fully portable.
class EventQueue {
public:
	EventQueue() {
#if defined(__linux__)
		fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#elif !defined(_WIN32)
		int fds[2];
		if (pipe(fds) == 0) {
			for (int fd : fds) {
				fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
				fcntl(fd, F_SETFD, FD_CLOEXEC);
			}
			fd_ = fds[0];
			write_fd_ = fds[1];
		}
#endif
	}
	~EventQueue() {
#ifndef _WIN32
		if (fd_ >= 0) {
			close(fd_);
		}
		if (write_fd_ >= 0) {
			close(write_fd_);
		}
#endif
	}
	EventQueue(const EventQueue &) = delete;
	EventQueue &operator=(const EventQueue &) = delete;

	int fd() const { return fd_; }

	// Safe to call from any thread, including concurrently.
	void post(std::function<void()> fn) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			queue_.push_back(std::move(fn));
		}
#if defined(__linux__)
		uint64_t one = 1;
		ssize_t n = write(fd_, &one, sizeof(one));
		(void)n; // best-effort wakeup; the queue itself is the source of truth
#elif !defined(_WIN32)
		char one = 1;
		ssize_t n = write(write_fd_, &one, 1);
		(void)n; // EAGAIN on a full pipe is fine: it's readable already
#endif
	}

	size_t drain() {
#if defined(__linux__)
		uint64_t ignored;
		ssize_t n = read(fd_, &ignored, sizeof(ignored));
		(void)n; // EAGAIN or a stale count are both fine, see below
#elif !defined(_WIN32)
		// A pipe holds one byte per post(), not one counter: empty it.
		char ignored[256];
		while (read(fd_, ignored, sizeof(ignored)) > 0) {}
#endif

		size_t processed = 0;
		for (;;) {
			std::function<void()> fn;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (queue_.empty()) {
					break;
				}
				fn = std::move(queue_.front());
				queue_.pop_front();
			}
			fn(); // outside the lock: closures may re-enter (e.g. open a stream)
			processed++;
		}
		return processed;
	}

	// Drops everything queued, unrun. The closures are destroyed outside
	// the lock: one may hold the last reference to a connection.
	void clear() {
		std::deque<std::function<void()>> dropped;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			dropped.swap(queue_);
		}
	}

private:
	int fd_ = -1;
	int write_fd_ = -1; // self-pipe's write end (non-Linux POSIX only)
	std::mutex mutex_;
	std::deque<std::function<void()>> queue_;
};

} // namespace gdp
