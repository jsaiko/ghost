// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The virtual gamepads an attached client's controllers become
// (docs/design/audio-cursor-gamepad.md#gamepads): one uinput device per connected slot,
// created by ghostseat on request (seat_client.hpp's create_device)
// since wraith can neither open /dev/uinput nor reach the device node it
// produces from a seatless session. wraith's whole job with the fd it
// gets back (ghostseat's relay to the device) is write()ing
// input_events; closing it destroys the device.
//
// Below the compositor entirely -- the devices are kernel evdev nodes
// the session's games open through SDL/udev like any physical pad -- so
// unlike keyboard and pointer injection (session_host.hpp's InputSink)
// this is the same code on every backend.
//
// Owned by whoever owns the GdpSession and handed to it; a GdpSession
// with none offers no "gamepad" capability. Per-client state: the
// session disconnects every slot when the client goes away.
#pragma once

#include "gdp/gamepad.hpp"

#include <cstdint>
#include <string>

namespace wraith {

class GamepadDevices {
public:
	// `control_socket` is the -G control socket path: each create is a
	// fresh connection to it.
	explicit GamepadDevices(std::string control_socket);
	~GamepadDevices();
	GamepadDevices(const GamepadDevices &) = delete;
	GamepadDevices &operator=(const GamepadDevices &) = delete;

	// Creates the device for `index` (replacing one already there).
	// `name` is the client's name for its controller, for the log only.
	// False -- with the slot left empty -- if ghostseat refused or the
	// index is out of range; a session keeps working without that pad.
	bool connect(uint32_t index, const std::string &name);
	// Destroys the device for `index`; no-op for an empty slot.
	void disconnect(uint32_t index);
	void disconnect_all();
	bool connected(uint32_t index) const;

	// Moves the device to `state`, writing only what changed since the
	// last update (gamepad_evdev.hpp). No-op for an empty slot.
	void update(uint32_t index, const gdp::GamepadSnapshot &state);

private:
	struct Slot {
		int fd = -1;
		gdp::GamepadSnapshot last;
		bool write_failed = false; // logged once per device, not per event
	};

	std::string control_socket_;
	Slot slots_[gdp::kMaxGamepads];
};

} // namespace wraith
