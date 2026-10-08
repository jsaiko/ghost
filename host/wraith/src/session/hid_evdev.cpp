// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-FileCopyrightText: 2000-2001 Vojtech Pavlik
// SPDX-FileCopyrightText: 2006-2010 Jiri Kosina
// SPDX-License-Identifier: GPL-3.0-only
//
// The HID-to-evdev table below is from the Linux kernel's
// drivers/hid/hid-input.c (GPL-2.0-or-later), carried here under GPL-3.

#include "session/hid_evdev.hpp"

namespace wraith {

namespace {

// USB HID usage ID (Keyboard/Keypad page, 0x07) -> Linux evdev keycode.
// Copied from the Linux kernel's drivers/hid/hid-input.c `hid_keyboard[256]`
// with its `unk` entries as 0 (no evdev equivalent: unassigned, or one of
// HID's keyboard-rollover / error-state pseudo-codes at indices 1-3).
// tests/hid_evdev_test.cpp checks the entries against KEY_* names.
// clang-format off: a table laid out by hand
constexpr uint32_t kHidToEvdev[256] = {
	  0,   0,   0,   0,  30,  48,  46,  32,  18,  33,  34,  35,  23,  36,  37,  38,
	 50,  49,  24,  25,  16,  19,  31,  20,  22,  47,  17,  45,  21,  44,   2,   3,
	  4,   5,   6,   7,   8,   9,  10,  11,  28,   1,  14,  15,  57,  12,  13,  26,
	 27,  43,  43,  39,  40,  41,  51,  52,  53,  58,  59,  60,  61,  62,  63,  64,
	 65,  66,  67,  68,  87,  88,  99,  70, 119, 110, 102, 104, 111, 107, 109, 106,
	105, 108, 103,  69,  98,  55,  74,  78,  96,  79,  80,  81,  75,  76,  77,  71,
	 72,  73,  82,  83,  86, 127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,
	191, 192, 193, 194, 134, 138, 130, 132, 128, 129, 131, 137, 133, 135, 136, 113,
	115, 114,   0,   0,   0, 121,   0,  89,  93, 124,  92,  94,  95,   0,   0,   0,
	122, 123,  90,  91,  85,   0,   0,   0,   0,   0,   0,   0, 111,   0,   0,   0,
	  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
	  0,   0,   0,   0,   0,   0, 179, 180,   0,   0,   0,   0,   0,   0,   0,   0,
	  0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
	  0,   0,   0,   0,   0,   0,   0,   0, 111,   0,   0,   0,   0,   0,   0,   0,
	 29,  42,  56, 125,  97,  54, 100, 126, 164, 166, 165, 163, 161, 115, 114, 113,
	150, 158, 159, 128, 136, 177, 178, 176, 142, 152, 173, 140,   0,   0,   0,   0,
};
// clang-format on

} // namespace

uint32_t hid_usage_to_evdev(uint32_t hid_usage) {
	if (hid_usage >= 256) {
		return 0;
	}
	return kHidToEvdev[hid_usage];
}

} // namespace wraith
