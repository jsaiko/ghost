// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// The network thread behind one Transport. It owns every socket and every
// ngtcp2_conn the transport has -- an ngtcp2_conn isn't thread-safe, so
// nothing else ever touches one -- and wakes on socket readiness, a timer
// (the soonest of every endpoint's deadlines) or a posted task. Anything
// the application should see goes out through the transport's EventQueue
// instead, to run from dispatch() on the host's thread.
#pragma once

#include "socket_platform.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gdp {

// Nanoseconds on the monotonic clock: ngtcp2's ngtcp2_tstamp.
uint64_t now_ns();

class NetLoop {
public:
	// A socket and whatever lives behind it: one client connection, or a
	// listener and its server connections. All calls come from the network
	// thread.
	class Endpoint {
	public:
		virtual ~Endpoint() = default;
		virtual socket_t fd() const = 0;
		// A send hit EAGAIN and a packet is waiting: poll for POLLOUT too.
		virtual bool wants_write() const = 0;
		// Readable (or an error is pending on the socket).
		virtual void on_readable(uint64_t now) = 0;
		// Runs every timer that is due and writes whatever can be written.
		// Called on every pass of the loop, so it must be cheap when idle.
		virtual void service(uint64_t now) = 0;
		// When service() next has something to do, ns; UINT64_MAX for never.
		virtual uint64_t next_deadline() const = 0;
	};

	NetLoop();
	~NetLoop(); // stop()s if still running
	NetLoop(const NetLoop &) = delete;
	NetLoop &operator=(const NetLoop &) = delete;

	// Any thread. Tasks run in order on the network thread. Once stop()
	// has begun they're dropped unrun.
	void post(std::function<void()> fn);
	// Any thread: just wake the loop (a datagram was queued, the pacing
	// rate changed). Cheap to call often -- wakes coalesce.
	void wake();

	// Network thread only.
	void add(std::shared_ptr<Endpoint> endpoint);
	void remove(Endpoint *endpoint);

	// Any thread but the network thread. Runs the tasks already posted (a
	// connection's final CONNECTION_CLOSE among them) and one last service()
	// pass, then joins the thread and drops every endpoint.
	void stop();

private:
	void run();
	void run_tasks();

	// What wake() signals: an eventfd on Linux, a self-pipe on other POSIX
	// systems (wake_write_fd_ its write end), and on Windows a loopback UDP
	// socket connected to itself, since WSAPoll only waits on sockets.
	socket_t wake_fd_ = kInvalidSocket;
	int wake_write_fd_ = -1;
	std::atomic<bool> wake_pending_{false};

	std::mutex tasks_mutex_;
	std::vector<std::function<void()>> tasks_;
	bool stopping_ = false; // under tasks_mutex_

	std::vector<std::shared_ptr<Endpoint>> endpoints_; // network thread only
	std::thread thread_;
};

} // namespace gdp
