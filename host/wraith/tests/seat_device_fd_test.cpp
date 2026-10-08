// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// SeatClient::create_device()'s half of the SCM_RIGHTS fd handoff
// (session/seat_client.hpp, docs/design/audio-cursor-gamepad.md#gamepads):
// wraith cannot open /dev/uinput itself, so a device only ever reaches
// it as a descriptor on the control socket -- wraith's end of the socket
// pair ghostseat relays to the device. A descriptor that silently fails
// to cross leaves a session with no gamepads and no error, so the
// transfer is checked here against a stand-in ghostseat rather than
// only in a live session.
//
// The stand-in sends a descriptor for a file with known contents; the
// test reads it back through whatever create_device() returned, which is
// proof the descriptor is real and refers to the same open file rather
// than merely being a plausible-looking integer.
#include "session/seat_client.hpp"

#include "control.pb.h"
#include "gdp/framing.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

constexpr const char *kPayload = "ghost-device-fd";
constexpr const char *kNode = "/dev/input/event99";

std::string temp_path(const char *stem) {
	// $XDG_RUNTIME_DIR is short enough for sun_path; /tmp otherwise.
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (!dir || !*dir) {
		dir = "/tmp";
	}
	return std::string(dir) + "/wraith-" + stem + "-" + std::to_string(getpid());
}

// Writes one framed ControlEnvelope with `fd` attached as SCM_RIGHTS, the
// way ghostseat's devices.rs send_frame_with_fd does.
bool send_reply_with_fd(int sock, const ghost::control::ControlEnvelope &env, int fd) {
	std::vector<uint8_t> frame;
	if (!gdp::encode_frame(env, &frame)) {
		return false;
	}
	iovec iov{frame.data(), frame.size()};
	union {
		cmsghdr align;
		uint8_t bytes[CMSG_SPACE(sizeof(int))];
	} control{};
	msghdr msg{};
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	if (fd >= 0) {
		msg.msg_control = control.bytes;
		msg.msg_controllen = sizeof(control.bytes);
		cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
	}
	return sendmsg(sock, &msg, 0) == (ssize_t)frame.size();
}

// A stand-in ghostseat: accepts one connection, reads the CreateDevice, and
// replies as `mode` says. Runs in a forked child so create_device()'s
// blocking calls are exercised exactly as they are in wraith.
enum class Mode {
	kSendFd, // DeviceCreated + a real descriptor
	kError,  // ControlError, no descriptor
	kNoFd,   // DeviceCreated but no descriptor at all
	kHangUp, // close without replying
};

pid_t spawn_fake_ghostseat(const std::string &sock_path, Mode mode, const std::string &file_path) {
	int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listener < 0) {
		return -1;
	}
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
	unlink(sock_path.c_str());
	if (bind(listener, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
		close(listener);
		return -1;
	}

	pid_t pid = fork();
	if (pid != 0) {
		// Parent: the child owns the listener now. Closing it here would
		// race the connect(), so it stays open until the child is reaped.
		close(listener);
		return pid;
	}

	int conn = accept(listener, nullptr, nullptr);
	if (conn < 0) {
		_exit(1);
	}
	// Read the request, however it is chunked.
	gdp::FrameReader reader;
	ghost::control::ControlEnvelope request;
	bool got = false;
	while (!got) {
		uint8_t buf[512];
		ssize_t n = read(conn, buf, sizeof(buf));
		if (n <= 0) {
			_exit(2);
		}
		reader.feed(buf, (size_t)n);
		if (reader.drain(&request) == gdp::FrameReader::Result::kOk) {
			got = true;
		}
	}
	if (!request.has_create_device()) {
		_exit(3);
	}

	ghost::control::ControlEnvelope reply;
	int fd = -1;
	switch (mode) {
	case Mode::kSendFd: {
		fd = open(file_path.c_str(), O_RDONLY);
		if (fd < 0) {
			_exit(4);
		}
		reply.mutable_device_created()->set_node(kNode);
		break;
	}
	case Mode::kError: reply.mutable_error()->set_message("no uinput on this host"); break;
	case Mode::kNoFd: reply.mutable_device_created()->set_node(kNode); break;
	case Mode::kHangUp: close(conn); _exit(0);
	}
	if (!send_reply_with_fd(conn, reply, fd)) {
		_exit(5);
	}
	// Waits for the parent to finish reading before tearing the socket
	// down, so a close() never races the reply out of the buffer.
	uint8_t drain;
	ssize_t n = read(conn, &drain, 1);
	_exit(n < 0 ? 6 : 0);
}

bool reap(pid_t pid) {
	int status = 0;
	return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void test_fd_arrives_and_is_usable() {
	std::string file_path = temp_path("payload");
	FILE *f = fopen(file_path.c_str(), "w");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	fputs(kPayload, f);
	fclose(f);

	std::string sock_path = temp_path("sock");
	pid_t pid = spawn_fake_ghostseat(sock_path, Mode::kSendFd, file_path);
	CHECK(pid > 0);

	std::string node, error;
	int fd = wraith::SeatClient::create_device(sock_path, ghost::control::CreateDevice::KIND_GAMEPAD, 0,
		"Test Pad", &node, &error);
	CHECK(fd >= 0);
	CHECK(error.empty());
	CHECK(node == kNode);

	if (fd >= 0) {
		// The descriptor must refer to the file the stand-in opened --
		// the whole point of the handoff.
		char buf[64] = {};
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		CHECK(n == (ssize_t)strlen(kPayload));
		CHECK(std::string(buf) == kPayload);
		// And it must not leak into a child process: ghostseat's reply is
		// received with MSG_CMSG_CLOEXEC.
		int flags = fcntl(fd, F_GETFD);
		CHECK(flags >= 0 && (flags & FD_CLOEXEC));
		close(fd);
	}
	CHECK(reap(pid));
	unlink(sock_path.c_str());
	unlink(file_path.c_str());
}

void test_error_reply_has_no_fd() {
	std::string sock_path = temp_path("sock-err");
	pid_t pid = spawn_fake_ghostseat(sock_path, Mode::kError, "");
	CHECK(pid > 0);

	std::string node, error;
	int fd = wraith::SeatClient::create_device(sock_path, ghost::control::CreateDevice::KIND_GAMEPAD, 1,
		"Test Pad", &node, &error);
	CHECK(fd < 0);
	// ghostseat's own words reach the caller, so the log says which step failed.
	CHECK(error == "no uinput on this host");
	CHECK(reap(pid));
	unlink(sock_path.c_str());
}

void test_created_without_fd_is_an_error() {
	std::string sock_path = temp_path("sock-nofd");
	pid_t pid = spawn_fake_ghostseat(sock_path, Mode::kNoFd, "");
	CHECK(pid > 0);

	std::string node, error;
	int fd = wraith::SeatClient::create_device(sock_path, ghost::control::CreateDevice::KIND_GAMEPAD, 2,
		"Test Pad", &node, &error);
	// A DeviceCreated with no descriptor is a protocol violation, not a
	// usable device -- it must not read as success.
	CHECK(fd < 0);
	CHECK(!error.empty());
	CHECK(reap(pid));
	unlink(sock_path.c_str());
}

void test_hangup_is_an_error() {
	std::string sock_path = temp_path("sock-hup");
	pid_t pid = spawn_fake_ghostseat(sock_path, Mode::kHangUp, "");
	CHECK(pid > 0);

	std::string node, error;
	int fd = wraith::SeatClient::create_device(sock_path, ghost::control::CreateDevice::KIND_GAMEPAD, 3,
		"Test Pad", &node, &error);
	CHECK(fd < 0);
	CHECK(!error.empty());
	CHECK(reap(pid));
	unlink(sock_path.c_str());
}

void test_missing_socket_is_an_error() {
	std::string node, error;
	int fd = wraith::SeatClient::create_device(temp_path("sock-absent"),
		ghost::control::CreateDevice::KIND_GAMEPAD, 0, "Test Pad", &node, &error);
	CHECK(fd < 0);
	CHECK(!error.empty());
}

} // namespace

int main() {
	// The fake ghostseat below runs as this user, not root.
	wraith::SeatClient::set_require_root_peer(false);
	// A stand-in that dies mid-test must not take the test with it.
	signal(SIGPIPE, SIG_IGN);
	test_fd_arrives_and_is_usable();
	test_error_reply_has_no_fd();
	test_created_without_fd_is_an_error();
	test_hangup_is_an_error();
	test_missing_socket_is_an_error();
	if (g_failures) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
