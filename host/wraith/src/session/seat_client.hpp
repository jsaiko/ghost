// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// wraith's side of the control socket (host/proto/control.proto,
// docs/design/login-and-sessions.md#the-control-socket), whose peer is
// the session's ghostseat, the root process that opened the logind
// session. Blocking: the handshake happens once at startup, before
// wraith's own event loop (and GDP listener) exist, so there's nothing
// else here to service concurrently and no reason to wire it into
// wl_event_loop. The one mid-session request, create_device(), is its
// own short connection and is rare enough (a controller hotplug) that
// blocking the loop for it is fine -- create_hid_device()'s too, up to
// DeviceCreated; its later verdict is read from the event loop
// (session/raw_controllers.cpp).
#pragma once

#include "control.pb.h"
#include "gdp/error_codes.hpp"
#include "gdp/framing.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace wraith {

struct SeatSessionInit {
	std::vector<uint8_t> session_secret;
	uint16_t port_range_start;
	uint16_t port_range_end;
	std::string session_type;
	// SessionInit.uinput_available: whether create_device() can work on
	// this host, known before any client attaches.
	bool uinput_available = false;
	// SessionInit.uhid_available: the same for create_hid_device().
	bool uhid_available = false;
	// SessionInit.sessions_dir: searched for profiles ahead of the defaults.
	std::string sessions_dir;
};

class SeatClient {
public:
	~SeatClient();

	// Whether every connection must come from root (SO_PEERCRED), which
	// the session's ghostseat is and nothing else that could sit at the
	// control socket's path is. On by default; main.cpp keeps it on
	// under -G. Tests that stand in for ghostseat as a plain user turn
	// it off.
	static void set_require_root_peer(bool require);

	// Connects to `socket_path` and blocks for SessionInit (host/proto/control.
	// proto), the first and only thing ghostseat sends. On failure, `*error`
	// is set and the connection (if any) is already closed.
	bool connect_and_read_init(const std::string &socket_path, SeatSessionInit *out, std::string *error);

	// Reports the GDP port wraith bound, and the fingerprint of the
	// session certificate it presents there (SessionCert), and closes the
	// connection. Call at most once, and only after a successful
	// connect_and_read_init().
	bool send_ready(uint16_t port, const std::string &cert_sha256, std::string *error);

	// Best-effort: reports a startup failure to ghostseat (so the log says
	// something more useful than "timed out") and closes the connection.
	// A `code` other than kNone is one ghostd passes on to spectre in its
	// LobbyError (e.g. kHostFull); kNone is a generic start failure.
	// Safe to call after a failed connect_and_read_init() too, in which
	// case it's a no-op (there's no connection to report on).
	void send_error(const std::string &message, gdp::ErrorCode code = gdp::ErrorCode::kNone);

	// Connects to `socket_path` only -- no read, no handshake. For the
	// `--session-ended` mode (main.cpp, wraith.service's ExecStopPost), which
	// has nothing to wait for: it's a one-way report to the session's
	// ghostseat, which has the socket bound at this path.
	bool connect_only(const std::string &socket_path, std::string *error);

	// Reports that this uid's session ended, with systemd's own verdict
	// ($SERVICE_RESULT/$EXIT_CODE/$EXIT_STATUS, passed through verbatim by
	// wraith.service's ExecStopPost=), and closes the connection. Call at
	// most once, and only after a successful connect_only().
	bool send_session_ended(const std::string &result, const std::string &exit_code,
		const std::string &exit_status, std::string *error);

	// Asks ghostseat to create a virtual input device for this session
	// (host/proto/control.proto's CreateDevice,
	// docs/design/audio-cursor-gamepad.md#gamepads): a fresh connection to
	// `socket_path`, the request, and the DeviceCreated reply with an fd
	// as SCM_RIGHTS ancillary data. The fd is wraith's end of a
	// SOCK_SEQPACKET socketpair that ghostseat relays to the device, so
	// it reads and writes exactly as the device fd would: write() takes a
	// batch of input_events, and close() destroys the device. Returns
	// that fd -- the caller owns it -- or -1 with `*error` set
	// (ghostseat's ControlError text, or the local failure). `*node` gets
	// the /dev/input/eventN for logs.
	static int create_device(const std::string &socket_path, ghost::control::CreateDevice::Kind kind,
		uint32_t index, const std::string &name, std::string *node, std::string *error);

	// The same for a raw controller (KIND_HID, gdp-spec.md §8.6): `name`
	// and `identity` become the uhid device's, and the fd carries uhid
	// events both ways, one per read() or write(). ghostseat sends the
	// verdict on its driver later on the same connection (DeviceVerified,
	// or a ControlError once it has destroyed the device), so on success
	// the connection stays open: `*conn_fd` gets it, for the caller to
	// watch and close, and `*reader` holds whatever was read past
	// DeviceCreated -- the verdict itself, if ghostseat was quick.
	static int create_hid_device(const std::string &socket_path, uint32_t index, const std::string &name,
		const ghost::control::HidIdentity &identity, std::string *node, int *conn_fd,
		gdp::FrameReader *reader, std::string *error);

	// Reports that this session's viewer attached or detached
	// (ViewerAttached/ViewerDetached, host/proto/control.proto): a fresh
	// connection to `socket_path`, the message, and close -- ghostseat
	// sends no reply.
	static bool report_viewer(const std::string &socket_path, bool attached, std::string *error);

private:
	// The SessionInit read and validation behind connect_and_read_init(),
	// which closes the connection whenever this fails.
	bool read_init(SeatSessionInit *out, std::string *error);
	bool connect_raw(const std::string &socket_path, std::string *error);
	// Sends a CreateDevice and reads DeviceCreated and its fd, as
	// create_device() describes; `reader` keeps anything read past it.
	int request_device(const ghost::control::ControlEnvelope &request, gdp::FrameReader *reader,
		std::string *node, std::string *error);

	int fd_ = -1;
	static bool require_root_peer_;
};

} // namespace wraith
