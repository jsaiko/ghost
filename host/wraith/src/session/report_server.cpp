// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/report_server.hpp"

#include "gdp/framing.hpp"
#include "session/gdp_session.hpp"
#include "util/log.hpp"
#include "wraith.pb.h"

#include <wayland-server-core.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace wraith {

namespace {

// How long to wait for the client's half. A client that is gone, or
// rate-limiting its answers, never replies; the report then goes ahead
// without it.
constexpr int kClientTimeoutMs = 5000;

// How long to wait for a new connection's request.
constexpr int kRequestWaitMs = 100;

// The accepted socket is blocking, with a send timeout, so a reader that has
// gone away costs a moment, not the loop.
bool write_all(int fd, const uint8_t *data, size_t len) {
	while (len > 0) {
		ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		data += n;
		len -= (size_t)n;
	}
	return true;
}

bool send_response(int fd, const ghost::wraith::WraithResponse &response) {
	std::vector<uint8_t> frame;
	if (!gdp::encode_frame(response, &frame)) {
		// Over the frame cap: say so rather than send nothing.
		ghost::wraith::WraithResponse error;
		error.mutable_error()->set_message("the answer is too large to send");
		frame.clear();
		if (!gdp::encode_frame(error, &frame)) {
			return false;
		}
	}
	return write_all(fd, frame.data(), frame.size());
}

void send_error(int fd, const std::string &message) {
	ghost::wraith::WraithResponse response;
	response.mutable_error()->set_message(message);
	send_response(fd, response);
}

} // namespace

std::string control_socket_path() {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir || !*dir) {
		return "";
	}
	return std::string(dir) + "/wraith.sock";
}

ReportServer::~ReportServer() {
	if (gdp_) {
		gdp_->cancel_client_diagnostics();
	}
	timeout_source_.reset();
	listen_source_.reset();
	if (pending_fd_ >= 0) {
		close(pending_fd_);
	}
	if (listen_fd_ >= 0) {
		close(listen_fd_);
		unlink(path_.c_str());
	}
}

bool ReportServer::start(struct wl_event_loop *loop, GdpSession *gdp) {
	path_ = control_socket_path();
	if (path_.empty()) {
		WLOG_INFO("control: no XDG_RUNTIME_DIR, `wraith --report` will not reach this session");
		return false;
	}
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (path_.size() >= sizeof(addr.sun_path)) {
		WLOG_ERROR("control: %s is too long for a socket path", path_.c_str());
		return false;
	}
	memcpy(addr.sun_path, path_.c_str(), path_.size() + 1);

	listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (listen_fd_ < 0) {
		WLOG_ERROR("control: socket: %s", strerror(errno));
		return false;
	}
	// A leftover from a wraith that died: this one is the session now.
	unlink(path_.c_str());
	mode_t old_umask = umask(0177);
	int bound = bind(listen_fd_, (const sockaddr *)&addr, sizeof(addr));
	umask(old_umask);
	if (bound < 0 || listen(listen_fd_, 2) < 0) {
		WLOG_ERROR("control: %s: %s", path_.c_str(), strerror(errno));
		close(listen_fd_);
		listen_fd_ = -1;
		return false;
	}

	loop_ = loop;
	gdp_ = gdp;
	listen_source_.reset(wl_event_loop_add_fd(
		loop_, listen_fd_, WL_EVENT_READABLE,
		[](int, uint32_t, void *data) {
			static_cast<ReportServer *>(data)->handle_accept();
			return 0;
		},
		this));
	WLOG_INFO("control: listening on %s", path_.c_str());
	return true;
}

void ReportServer::handle_accept() {
	int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
	if (fd < 0) {
		return;
	}
	timeval send_timeout{2, 0};
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

	// The request is one small frame. A client writes it straight after
	// connecting, so it is normally here already; the short wait covers the
	// moment between connect and write and bounds what a silent connection
	// can cost the loop.
	uint8_t buffer[512];
	pollfd waiting{fd, POLLIN, 0};
	ssize_t got = poll(&waiting, 1, kRequestWaitMs) > 0 ? recv(fd, buffer, sizeof(buffer), 0) : -1;
	ghost::wraith::WraithRequest request;
	gdp::FrameReader reader;
	reader.set_max_frame_size(sizeof(buffer));
	if (got > 0) {
		reader.feed(buffer, (size_t)got);
	}
	if (got <= 0 || reader.drain(&request) != gdp::FrameReader::Result::kOk) {
		send_error(fd, "no request received");
		close(fd);
		return;
	}
	if (!request.has_report()) {
		send_error(fd, "this wraith knows no such command");
		close(fd);
		return;
	}

	if (pending_fd_ >= 0) {
		send_error(fd, "another report is already being collected");
		close(fd);
		return;
	}
	pending_fd_ = fd;
	if (!gdp_) {
		pending_host_text_ = "this wraith has no GDP session\n";
		finish("not asked: no GDP session\n");
		return;
	}
	pending_host_text_ = gdp_->status_text();
	bool asked = gdp_->request_client_diagnostics([this](std::string text) { finish(text); });
	if (!asked) {
		finish("not asked: no client is attached (or an earlier request is still waiting)\n");
		return;
	}
	timeout_source_.reset(wl_event_loop_add_timer(
		loop_,
		[](void *data) {
			auto *self = static_cast<ReportServer *>(data);
			self->gdp_->cancel_client_diagnostics();
			self->finish("the client did not answer within 5 s (a spectre older than this request?)\n");
			return 0;
		},
		this));
	wl_event_source_timer_update(timeout_source_.get(), kClientTimeoutMs);
}

void ReportServer::finish(const std::string &client_text) {
	timeout_source_.reset();
	int fd = pending_fd_;
	pending_fd_ = -1;
	if (fd < 0) {
		return;
	}
	ghost::wraith::WraithResponse response;
	auto *report = response.mutable_report();
	report->set_host_status(pending_host_text_);
	report->set_client_text(client_text);
	if (!send_response(fd, response)) {
		WLOG_INFO("control: the reader went away before the answer was written");
	}
	pending_host_text_.clear();
	close(fd);
}

} // namespace wraith
