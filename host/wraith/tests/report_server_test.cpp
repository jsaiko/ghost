// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// wraith.sock's wire (session/report_server.hpp, host/proto/wraith.proto):
// a forked client speaks to a real ReportServer on a real socket in a
// private runtime directory, and the answers are read back as the
// `wraith --report` command reads them. Covers the framing, the report
// answer and the error for a request with no command this wraith knows.
//
// No GdpSession here (its dependencies are most of wraith): the server is
// started without one, which is what a run with no GDP session looks like,
// and the two GdpSession members it names are stood in for below. The
// round trip through an attached client is checked live.
#include "session/report_server.hpp"

#include "gdp/framing.hpp"
#include "session/gdp_session.hpp"
#include "wraith.pb.h"

#include <wayland-server-core.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace wraith {
std::string GdpSession::status_text() const {
	return "";
}
bool GdpSession::request_client_diagnostics(std::function<void(std::string)>) {
	return false;
}
} // namespace wraith

namespace {

int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

// The client half, in a child: connects, sends `request`, reads one
// response frame. Exit status 0 if `check` accepts it.
int ask(const std::string &path, const ghost::wraith::WraithRequest &request,
	bool (*check)(const ghost::wraith::WraithResponse &)) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
	if (fd < 0 || connect(fd, (const sockaddr *)&addr, sizeof(addr)) < 0) {
		return 2;
	}
	std::vector<uint8_t> frame;
	if (!gdp::encode_frame(request, &frame) ||
		write(fd, frame.data(), frame.size()) != (ssize_t)frame.size()) {
		return 3;
	}
	gdp::FrameReader reader;
	ghost::wraith::WraithResponse response;
	for (;;) {
		gdp::FrameReader::Result result = reader.drain(&response);
		if (result == gdp::FrameReader::Result::kOk) {
			break;
		}
		if (result != gdp::FrameReader::Result::kIncomplete) {
			return 4;
		}
		uint8_t buf[4096];
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n <= 0) {
			return 5;
		}
		reader.feed(buf, (size_t)n);
	}
	return check(response) ? 0 : 6;
}

// Runs the server's event loop until the child has answered; returns the
// child's exit status.
int run_client(struct wl_event_loop *loop, const std::string &path,
	const ghost::wraith::WraithRequest &request, bool (*check)(const ghost::wraith::WraithResponse &)) {
	pid_t pid = fork();
	if (pid == 0) {
		_exit(ask(path, request, check));
	}
	int status = 0;
	for (int i = 0; i < 100; i++) {
		wl_event_loop_dispatch(loop, 50);
		if (waitpid(pid, &status, WNOHANG) == pid) {
			return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		}
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return -2;
}

} // namespace

int main() {
	char dir[] = "/tmp/wraith-report-test-XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		return 1;
	}
	setenv("XDG_RUNTIME_DIR", dir, 1);

	struct wl_event_loop *loop = wl_event_loop_create();
	{
		wraith::ReportServer server;
		CHECK(server.start(loop, nullptr));
		std::string path = wraith::control_socket_path();
		CHECK(path == std::string(dir) + "/wraith.sock");

		// A report with no GDP session: both halves say why they're empty.
		ghost::wraith::WraithRequest report;
		report.mutable_report();
		CHECK(run_client(loop, path, report, [](const ghost::wraith::WraithResponse &r) {
			return r.has_report() && r.report().host_status().find("no GDP session") != std::string::npos &&
				!r.report().client_text().empty();
		}) == 0);

		// A request with no command this wraith knows: an error, not a hang.
		ghost::wraith::WraithRequest nothing;
		CHECK(run_client(loop, path, nothing, [](const ghost::wraith::WraithResponse &r) {
			return r.has_error() && !r.error().message().empty();
		}) == 0);
	}
	wl_event_loop_destroy(loop);
	CHECK(access((std::string(dir) + "/wraith.sock").c_str(), F_OK) != 0); // removed on exit
	rmdir(dir);

	if (g_failures) {
		fprintf(stderr, "%d failures\n", g_failures);
		return 1;
	}
	return 0;
}
