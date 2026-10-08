// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The running session's control socket, which `wraith --report`
// (report_cli.hpp) talks to: a Unix socket in the user's runtime directory,
// named for wraith rather than for the one command so others can join it.
// The messages are host/proto/wraith.proto's, framed like GDP's streams: a
// connection sends one WraithRequest and gets one WraithResponse, and the
// server then closes. The only command so far is ReportRequest, answered
// with what wraith knows of the session (GdpSession::status_text()) and the
// attached client's own report (gdp-spec.md §7.11), or a sentence saying
// why there isn't one. A request with no command this wraith knows gets a
// WraithError.
//
// One report at a time; a second connection while one waits is told so.
#pragma once

#include "util/event_source.hpp"

#include <string>

struct wl_event_loop;

namespace wraith {

class GdpSession;

// $XDG_RUNTIME_DIR/wraith.sock, or empty if that is unset.
std::string control_socket_path();

class ReportServer {
public:
	ReportServer() = default;
	~ReportServer();

	ReportServer(const ReportServer &) = delete;
	ReportServer &operator=(const ReportServer &) = delete;

	// `gdp` may be null (no GDP session in this run); it must outlive this.
	// False, with the reason logged, leaves the session without reports.
	bool start(struct wl_event_loop *loop, GdpSession *gdp);

private:
	void handle_accept();
	void finish(const std::string &client_text);

	struct wl_event_loop *loop_ = nullptr;
	GdpSession *gdp_ = nullptr;
	std::string path_;
	int listen_fd_ = -1;
	EventSource listen_source_;
	// The waiting request: its connection, the host's half of the answer
	// (taken when it came in), and the timeout on the client's.
	int pending_fd_ = -1;
	std::string pending_host_text_;
	EventSource timeout_source_;
};

} // namespace wraith
