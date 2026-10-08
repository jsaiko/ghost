// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "net_loop.hpp"

#ifdef _WIN32
// WSAPoll and WSAPOLLFD come from winsock2.h (socket_platform.hpp).
#else
#include <poll.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#else
#include <fcntl.h>
#endif
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <ctime>

namespace gdp {

namespace {

#ifdef _WIN32
using pollfd_t = WSAPOLLFD;
#else
using pollfd_t = pollfd;
#endif

} // namespace

uint64_t now_ns() {
	using namespace std::chrono;
	return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

NetLoop::NetLoop() {
#if defined(_WIN32)
	net_init();
	// A UDP socket connect()ed to its own loopback address: sending it a
	// byte makes it readable, which is all a wakeup needs.
	wake_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
	if (wake_fd_ != kInvalidSocket) {
		sockaddr_in sa{};
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		int len = sizeof(sa);
		u_long nonblocking = 1;
		if (bind(wake_fd_, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0 ||
			getsockname(wake_fd_, reinterpret_cast<sockaddr *>(&sa), &len) != 0 ||
			connect(wake_fd_, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0 ||
			ioctlsocket(wake_fd_, FIONBIO, &nonblocking) != 0) {
			close_socket(wake_fd_);
			wake_fd_ = kInvalidSocket;
		}
	}
#elif defined(__linux__)
	wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
#else
	int fds[2];
	if (pipe(fds) == 0) {
		for (int fd : fds) {
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
			fcntl(fd, F_SETFD, FD_CLOEXEC);
		}
		wake_fd_ = fds[0];
		wake_write_fd_ = fds[1];
	}
#endif
	thread_ = std::thread([this] { run(); });
}

NetLoop::~NetLoop() {
	stop();
#ifdef _WIN32
	if (wake_fd_ != kInvalidSocket) {
		close_socket(wake_fd_);
	}
#else
	if (wake_fd_ >= 0) {
		close(wake_fd_);
	}
	if (wake_write_fd_ >= 0) {
		close(wake_write_fd_);
	}
#endif
}

void NetLoop::post(std::function<void()> fn) {
	{
		std::lock_guard<std::mutex> lock(tasks_mutex_);
		if (stopping_) {
			return;
		}
		tasks_.push_back(std::move(fn));
	}
	wake();
}

void NetLoop::wake() {
	// One wakeup in flight is enough: the loop clears the flag before it
	// looks at anything, so a wake() after that always lands.
	if (wake_pending_.exchange(true, std::memory_order_acq_rel)) {
		return;
	}
#if defined(_WIN32)
	char one = 1;
	::send(wake_fd_, &one, 1, 0);
#elif defined(__linux__)
	uint64_t one = 1;
	ssize_t n = write(wake_fd_, &one, sizeof(one));
	(void)n;
#else
	char one = 1;
	ssize_t n = write(wake_write_fd_, &one, 1);
	(void)n;
#endif
}

void NetLoop::add(std::shared_ptr<Endpoint> endpoint) {
	endpoints_.push_back(std::move(endpoint));
}

void NetLoop::remove(Endpoint *endpoint) {
	auto it = std::find_if(endpoints_.begin(), endpoints_.end(),
		[endpoint](const std::shared_ptr<Endpoint> &e) { return e.get() == endpoint; });
	if (it != endpoints_.end()) {
		endpoints_.erase(it);
	}
}

void NetLoop::stop() {
	if (!thread_.joinable()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(tasks_mutex_);
		stopping_ = true;
	}
	wake_pending_.store(false, std::memory_order_release);
	wake();
	thread_.join();
}

void NetLoop::run_tasks() {
	std::vector<std::function<void()>> tasks;
	{
		std::lock_guard<std::mutex> lock(tasks_mutex_);
		tasks.swap(tasks_);
	}
	for (auto &task : tasks) {
		task();
	}
}

namespace {

// Waits for any of `fds` or until `deadline` (ns; UINT64_MAX for none).
// Nanosecond timeouts on Linux; elsewhere milliseconds, rounded up, so a
// timer never fires early.
int wait_for(std::vector<pollfd_t> &fds, uint64_t deadline, uint64_t now) {
#if defined(__linux__)
	if (deadline == UINT64_MAX) {
		return ppoll(fds.data(), fds.size(), nullptr, nullptr);
	}
	uint64_t wait = deadline > now ? deadline - now : 0;
	timespec ts{static_cast<time_t>(wait / 1'000'000'000), static_cast<long>(wait % 1'000'000'000)};
	return ppoll(fds.data(), fds.size(), &ts, nullptr);
#else
	int timeout_ms = -1;
	if (deadline != UINT64_MAX) {
		uint64_t wait = deadline > now ? deadline - now : 0;
		timeout_ms = static_cast<int>(std::min<uint64_t>((wait + 999'999) / 1'000'000, 60'000));
	}
#ifdef _WIN32
	return WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
#else
	return poll(fds.data(), fds.size(), timeout_ms);
#endif
#endif
}

} // namespace

void NetLoop::run() {
	std::vector<pollfd_t> fds;
	std::vector<std::shared_ptr<Endpoint>> polled;
	for (;;) {
		wake_pending_.store(false, std::memory_order_release);
		run_tasks();

		// A copy, because service() may remove its own endpoint.
		polled = endpoints_;
		uint64_t now = now_ns();
		for (auto &endpoint : polled) {
			endpoint->service(now);
		}

		bool stopping;
		{
			std::lock_guard<std::mutex> lock(tasks_mutex_);
			stopping = stopping_;
		}
		if (stopping) {
			// Whatever was posted before stop() (a destructor's close) has
			// run and been written out above. Nothing waits on a closing
			// period now: the transport is going away.
			run_tasks();
			polled = endpoints_;
			for (auto &endpoint : polled) {
				endpoint->service(now_ns());
			}
			endpoints_.clear();
			return;
		}

		polled = endpoints_;
		uint64_t deadline = UINT64_MAX;
		fds.clear();
		fds.push_back({wake_fd_, POLLIN, 0});
		for (auto &endpoint : polled) {
			deadline = std::min(deadline, endpoint->next_deadline());
			short events = POLLIN;
			if (endpoint->wants_write()) {
				events |= POLLOUT;
			}
			fds.push_back({endpoint->fd(), events, 0});
		}

		int rc = wait_for(fds, deadline, now_ns());
		if (rc < 0 && socket_error() != EINTR) {
			continue;
		}
		if (fds[0].revents & POLLIN) {
#if defined(_WIN32)
			char ignored[64];
			while (recv(wake_fd_, ignored, sizeof(ignored), 0) > 0) {}
#elif defined(__linux__)
			uint64_t ignored;
			ssize_t n = read(wake_fd_, &ignored, sizeof(ignored));
			(void)n;
#else
			char ignored[256];
			while (read(wake_fd_, ignored, sizeof(ignored)) > 0) {}
#endif
		}
		now = now_ns();
		for (size_t i = 0; i < polled.size(); i++) {
			// POLLOUT needs no handler of its own: service() retries the
			// blocked packet first thing.
			if (fds[i + 1].revents & (POLLIN | POLLERR | POLLHUP)) {
				polled[i]->on_readable(now);
			}
		}
	}
}

} // namespace gdp
