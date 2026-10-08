// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/seat_client.hpp"

#include "control.pb.h"
#include "gdp/framing.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace wraith {

namespace {

constexpr int kReadTimeoutMs = 10'000;

void close_if_valid(int fd) {
	if (fd >= 0) {
		close(fd);
	}
}

bool write_all(int fd, const uint8_t *data, size_t len) {
	size_t sent = 0;
	while (sent < len) {
		ssize_t n = write(fd, data + sent, len - sent);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		sent += static_cast<size_t>(n);
	}
	return true;
}

bool send_envelope(int fd, const ghost::control::ControlEnvelope &env, std::string *error) {
	std::vector<uint8_t> out;
	if (!gdp::encode_frame(env, &out)) {
		if (error) {
			*error = "failed to encode ControlEnvelope";
		}
		return false;
	}
	if (!write_all(fd, out.data(), out.size())) {
		if (error) {
			*error = std::string("write to ghostseat failed: ") + strerror(errno);
		}
		return false;
	}
	return true;
}

// The peer's uid, or -1 when it can't be read.
uid_t peer_uid(int fd) {
	ucred cred{};
	socklen_t len = sizeof(cred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
		return (uid_t)-1;
	}
	return cred.uid;
}

} // namespace

bool SeatClient::require_root_peer_ = true;

void SeatClient::set_require_root_peer(bool require) {
	require_root_peer_ = require;
}

SeatClient::~SeatClient() {
	if (fd_ >= 0) {
		close(fd_);
	}
}

bool SeatClient::connect_raw(const std::string &socket_path, std::string *error) {
	fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd_ < 0) {
		if (error) {
			*error = std::string("socket() failed: ") + strerror(errno);
		}
		return false;
	}

	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	if (socket_path.size() >= sizeof(addr.sun_path)) {
		if (error) {
			*error = "control socket path too long";
		}
		close(fd_);
		fd_ = -1;
		return false;
	}
	std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

	if (connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		if (error) {
			*error = std::string("connect(") + socket_path + ") failed: " + strerror(errno);
		}
		// Closed here, not left for the destructor: fd_ < 0 is what
		// send_error() checks to decide there's no connection to report on.
		close(fd_);
		fd_ = -1;
		return false;
	}
	// Whoever answers at this path gets to name the session secret and
	// the sessions directory, so it has to be ghostseat: root. Anything
	// else that could bind here runs as a user.
	if (require_root_peer_) {
		uid_t uid = peer_uid(fd_);
		if (uid != 0) {
			if (error) {
				*error = socket_path + " is not served by root (peer uid " + std::to_string((long)uid) +
					"); refusing to talk to it";
			}
			close(fd_);
			fd_ = -1;
			return false;
		}
	}
	return true;
}

bool SeatClient::connect_only(const std::string &socket_path, std::string *error) {
	return connect_raw(socket_path, error);
}

bool SeatClient::connect_and_read_init(const std::string &socket_path, SeatSessionInit *out,
	std::string *error) {
	if (!connect_raw(socket_path, error)) {
		return false;
	}
	if (!read_init(out, error)) {
		close(fd_);
		fd_ = -1;
		return false;
	}
	return true;
}

bool SeatClient::read_init(SeatSessionInit *out, std::string *error) {
	gdp::FrameReader reader;
	ghost::control::ControlEnvelope env;
	for (;;) {
		pollfd pfd{fd_, POLLIN, 0};
		int n = poll(&pfd, 1, kReadTimeoutMs);
		if (n <= 0) {
			if (error) {
				*error = n == 0 ? "timed out waiting for SessionInit"
								: (std::string("poll() failed: ") + strerror(errno));
			}
			return false;
		}
		uint8_t buf[4096];
		ssize_t got = read(fd_, buf, sizeof(buf));
		if (got <= 0) {
			if (error) {
				*error = got == 0 ? "ghostseat closed the connection before sending SessionInit"
								  : (std::string("read() failed: ") + strerror(errno));
			}
			return false;
		}
		reader.feed(buf, static_cast<size_t>(got));
		gdp::FrameReader::Result result = reader.drain(&env);
		if (result == gdp::FrameReader::Result::kOk) {
			break;
		}
		if (result == gdp::FrameReader::Result::kTooLarge || result == gdp::FrameReader::Result::kMalformed) {
			if (error) {
				*error = "malformed SessionInit frame from ghostseat";
			}
			return false;
		}
		// kIncomplete: loop and read more.
	}

	if (!env.has_init()) {
		if (error) {
			*error = "expected SessionInit from ghostseat, got something else";
		}
		return false;
	}
	const ghost::control::SessionInit &init = env.init();
	if (init.port_range_start() > 0xFFFF || init.port_range_end() > 0xFFFF ||
		init.port_range_start() > init.port_range_end()) {
		if (error) {
			*error = "invalid port range in SessionInit";
		}
		return false;
	}
	out->session_secret.assign(init.session_secret().begin(), init.session_secret().end());
	out->port_range_start = static_cast<uint16_t>(init.port_range_start());
	out->port_range_end = static_cast<uint16_t>(init.port_range_end());
	out->session_type = init.session_type();
	out->uinput_available = init.uinput_available();
	out->uhid_available = init.uhid_available();
	out->sessions_dir = init.sessions_dir();
	return true;
}

bool SeatClient::report_viewer(const std::string &socket_path, bool attached, std::string *error) {
	SeatClient client;
	if (!client.connect_raw(socket_path, error)) {
		return false;
	}
	ghost::control::ControlEnvelope env;
	if (attached) {
		env.mutable_viewer_attached();
	} else {
		env.mutable_viewer_detached();
	}
	return send_envelope(client.fd_, env, error);
}

int SeatClient::create_device(const std::string &socket_path, ghost::control::CreateDevice::Kind kind,
	uint32_t index, const std::string &name, std::string *node, std::string *error) {
	SeatClient client;
	if (!client.connect_raw(socket_path, error)) {
		return -1;
	}
	ghost::control::ControlEnvelope request;
	auto *create = request.mutable_create_device();
	create->set_kind(kind);
	create->set_index(index);
	create->set_name(name);
	gdp::FrameReader reader;
	return client.request_device(request, &reader, node, error);
}

int SeatClient::create_hid_device(const std::string &socket_path, uint32_t index, const std::string &name,
	const ghost::control::HidIdentity &identity, std::string *node, int *conn_fd, gdp::FrameReader *reader,
	std::string *error) {
	SeatClient client;
	if (!client.connect_raw(socket_path, error)) {
		return -1;
	}
	ghost::control::ControlEnvelope request;
	auto *create = request.mutable_create_device();
	create->set_kind(ghost::control::CreateDevice::KIND_HID);
	create->set_index(index);
	create->set_name(name);
	*create->mutable_hid() = identity;
	int fd = client.request_device(request, reader, node, error);
	if (fd >= 0) {
		// The verdict follows on this connection; the caller reads it.
		*conn_fd = client.fd_;
		client.fd_ = -1;
	}
	return fd;
}

int SeatClient::request_device(const ghost::control::ControlEnvelope &request, gdp::FrameReader *reader,
	std::string *node, std::string *error) {
	if (!send_envelope(fd_, request, error)) {
		return -1;
	}

	// ghostseat attaches the fd to the first byte of the reply frame
	// (devices.rs's send_frame_with_fd), so every read here has to
	// be a recvmsg with room for one SCM_RIGHTS cmsg -- a plain read()
	// would discard the fd. Which read carries it isn't guaranteed: the
	// kernel delivers ancillary data with whichever byte it was queued
	// against, so the fd is kept as it arrives and matched up with the
	// message once a whole frame has been parsed.
	int received_fd = -1;
	ghost::control::ControlEnvelope reply;
	for (;;) {
		pollfd pfd{fd_, POLLIN, 0};
		int n = poll(&pfd, 1, kReadTimeoutMs);
		if (n <= 0) {
			if (error) {
				*error = n == 0 ? "timed out waiting for DeviceCreated"
								: (std::string("poll() failed: ") + strerror(errno));
			}
			close_if_valid(received_fd);
			return -1;
		}

		uint8_t buf[4096];
		iovec iov{buf, sizeof(buf)};
		// One fd is all any reply carries; anything more is a protocol
		// error on ghostseat's side, handled below rather than trusted.
		union {
			cmsghdr align;
			uint8_t bytes[CMSG_SPACE(sizeof(int))];
		} control{};
		msghdr msg{};
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control.bytes;
		msg.msg_controllen = sizeof(control.bytes);

		// MSG_CMSG_CLOEXEC: the fd must not survive into the session
		// leader wraith forks, or a logged-out session's gamepad would
		// stay alive in a child process.
		ssize_t got = recvmsg(fd_, &msg, MSG_CMSG_CLOEXEC);
		if (got < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (error) {
				*error = std::string("recvmsg() failed: ") + strerror(errno);
			}
			close_if_valid(received_fd);
			return -1;
		}
		if (got == 0) {
			if (error) {
				*error = "ghostseat closed the connection before replying to CreateDevice";
			}
			close_if_valid(received_fd);
			return -1;
		}

		for (cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
				continue;
			}
			// Trust the header's own length rather than assuming one fd:
			// a truncated or oversized cmsg must not leak descriptors.
			// The control buffer only has room for one, so `count` can't
			// exceed that -- the loop is what keeps an unexpected extra
			// from being silently dropped rather than closed.
			size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			for (size_t i = 0; i < count; i++) {
				int fd = -1;
				std::memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(fd));
				if (received_fd < 0) {
					received_fd = fd;
				} else {
					close(fd); // a second fd is never expected
				}
			}
		}

		reader->feed(buf, static_cast<size_t>(got));
		gdp::FrameReader::Result result = reader->drain(&reply);
		if (result == gdp::FrameReader::Result::kOk) {
			break;
		}
		if (result == gdp::FrameReader::Result::kTooLarge || result == gdp::FrameReader::Result::kMalformed) {
			if (error) {
				*error = "malformed DeviceCreated frame from ghostseat";
			}
			close_if_valid(received_fd);
			return -1;
		}
		// kIncomplete: loop and read more.
	}

	if (reply.has_error()) {
		// ghostseat's own words -- it knows which step failed (no uinput,
		// no udev rule, a chown that didn't take).
		if (error) {
			*error = reply.error().message();
		}
		close_if_valid(received_fd);
		return -1;
	}
	if (!reply.has_device_created()) {
		if (error) {
			*error = "expected DeviceCreated from ghostseat, got something else";
		}
		close_if_valid(received_fd);
		return -1;
	}
	if (received_fd < 0) {
		if (error) {
			*error = "ghostseat reported the device was created but sent no descriptor";
		}
		return -1;
	}
	if (node) {
		*node = reply.device_created().node();
	}
	return received_fd;
}

bool SeatClient::send_ready(uint16_t port, const std::string &cert_sha256, std::string *error) {
	ghost::control::ControlEnvelope env;
	env.mutable_ready()->set_port(port);
	env.mutable_ready()->set_cert_sha256(cert_sha256);
	bool ok = send_envelope(fd_, env, error);
	close(fd_);
	fd_ = -1;
	return ok;
}

void SeatClient::send_error(const std::string &message, gdp::ErrorCode code) {
	if (fd_ < 0) {
		return;
	}
	ghost::control::ControlEnvelope env;
	env.mutable_error()->set_message(message);
	env.mutable_error()->set_code(static_cast<uint32_t>(code));
	send_envelope(fd_, env, nullptr);
	close(fd_);
	fd_ = -1;
}

bool SeatClient::send_session_ended(const std::string &result, const std::string &exit_code,
	const std::string &exit_status, std::string *error) {
	ghost::control::ControlEnvelope env;
	ghost::control::SessionEnded *ended = env.mutable_session_ended();
	ended->set_result(result);
	ended->set_exit_code(exit_code);
	ended->set_exit_status(exit_status);
	bool ok = send_envelope(fd_, env, error);
	close(fd_);
	fd_ = -1;
	return ok;
}

} // namespace wraith
