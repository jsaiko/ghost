// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Translates USB HID keyboard usage IDs (page 0x07, as sent by spectre's
// KeyEvent.hid_usage -- gdp-spec.md §8.2) to Linux evdev keycodes (as
// input-event-codes.h, what libei and zwp_virtual_keyboard_v1 take).
#pragma once

#include <cstdint>

namespace wraith {

// Returns 0 for a usage ID with no evdev equivalent (unassigned, or one of
// HID's keyboard-rollover/error-state pseudo-codes at 0x01-0x03) -- callers
// should drop the event rather than injecting keycode 0 (KEY_RESERVED).
uint32_t hid_usage_to_evdev(uint32_t hid_usage);

} // namespace wraith
