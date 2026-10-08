// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/gamepad_devices.hpp"

#include "control.pb.h"
#include "session/gamepad_evdev.hpp"
#include "session/seat_client.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "util/log.hpp"

namespace wraith {

GamepadDevices::GamepadDevices(std::string control_socket) : control_socket_(std::move(control_socket)) {}

GamepadDevices::~GamepadDevices() {
	disconnect_all();
}

bool GamepadDevices::connect(uint32_t index, const std::string &name) {
	if (index >= gdp::kMaxGamepads) {
		WLOG_ERROR("gamepad: client used slot %u, max is %u", index, gdp::kMaxGamepads - 1);
		return false;
	}
	disconnect(index);
	std::string node, error;
	int fd = SeatClient::create_device(control_socket_, ghost::control::CreateDevice::KIND_GAMEPAD, index,
		name, &node, &error);
	if (fd < 0) {
		WLOG_ERROR("gamepad: slot %u (\"%s\"): ghostseat could not create a device: %s", index, name.c_str(),
			error.c_str());
		return false;
	}
	Slot &slot = slots_[index];
	slot.fd = fd;
	slot.last = gdp::GamepadSnapshot{};
	slot.write_failed = false;
	WLOG_INFO("gamepad: slot %u (\"%s\") is %s", index, name.c_str(), node.c_str());
	return true;
}

void GamepadDevices::disconnect(uint32_t index) {
	if (index >= gdp::kMaxGamepads || slots_[index].fd < 0) {
		return;
	}
	// Closing the fd ends ghostseat's relay, which drops the uinput fd:
	// UI_DEV_DESTROY, the node disappears and every game that had it
	// open sees an unplug.
	close(slots_[index].fd);
	slots_[index].fd = -1;
	WLOG_INFO("gamepad: slot %u removed", index);
}

void GamepadDevices::disconnect_all() {
	for (uint32_t i = 0; i < gdp::kMaxGamepads; i++) {
		disconnect(i);
	}
}

bool GamepadDevices::connected(uint32_t index) const {
	return index < gdp::kMaxGamepads && slots_[index].fd >= 0;
}

void GamepadDevices::update(uint32_t index, const gdp::GamepadSnapshot &state) {
	if (!connected(index)) {
		return;
	}
	Slot &slot = slots_[index];
	input_event events[kMaxGamepadEvents];
	size_t n = gamepad_evdev_diff(slot.last, state, events);
	slot.last = state;
	if (n == 0) {
		return;
	}
	// One write per report: the batch crosses to ghostseat as one packet
	// and uinput takes the whole array atomically up to the SYN; the fd
	// is non-blocking with a queue far deeper than one report, so a short
	// write here means something is really wrong (the device was
	// destroyed under us), not back-pressure.
	ssize_t want = static_cast<ssize_t>(n * sizeof(input_event));
	if (write(slot.fd, events, static_cast<size_t>(want)) != want && !slot.write_failed) {
		slot.write_failed = true;
		WLOG_ERROR("gamepad: slot %u: write failed: %s", index, strerror(errno));
	}
}

} // namespace wraith
