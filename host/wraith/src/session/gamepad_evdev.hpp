// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Translates a GamepadState snapshot (gdp/gamepad.hpp's SDL order, docs/spec/
// gdp-spec.md §8.5) into the evdev events that move ghostd's
// virtual Xbox-360-layout pad (host/ghostseat/src/devices.rs's
// GAMEPAD_KEYS/GAMEPAD_AXES -- the two tables must agree) from its
// previous snapshot to this one. Pure: no fd, no uinput, so
// tests/gamepad_evdev_test.cpp covers the mapping without a device.
#pragma once

#include "gdp/gamepad.hpp"

#include <linux/input.h>

#include <cstddef>

namespace wraith {

// The most events one diff can produce: every axis, every mapped button,
// both hat axes, and the trailing SYN_REPORT.
inline constexpr size_t kMaxGamepadEvents = gdp::kGamepadAxisCount + 11 + 2 + 1;

// Appends to `out` (at least kMaxGamepadEvents long) one EV_ABS/EV_KEY
// per control whose device-side value differs between `prev` and `next`,
// then an EV_SYN/SYN_REPORT. Returns how many were written: 0 when
// nothing changed (no SYN either -- an empty report is noise), otherwise
// at least 2. Timestamps are left zero; the kernel stamps uinput writes
// itself.
//
// Sticks scale -1..1 to the device's -32768..32767, triggers 0..1 to
// 0..255, and the four d-pad buttons become the hat's two axes (the
// Xbox layout has no d-pad *buttons*). Non-finite or out-of-range floats
// clamp, so a bad client can move a stick to its limit and no further.
// SDL buttons the layout has no code for (paddles, touchpad, misc) are
// ignored.
size_t gamepad_evdev_diff(const gdp::GamepadSnapshot &prev, const gdp::GamepadSnapshot &next,
	input_event *out);

} // namespace wraith
