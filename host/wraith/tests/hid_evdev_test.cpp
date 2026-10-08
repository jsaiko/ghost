// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Plain-assert unit test (same convention as libgdp/tests): checks
// hid_usage_to_evdev() against the KEY_* names in linux/input-event-codes.h
// for every usage the table maps, so a transposed or off-by-one entry
// fails by name rather than by an opaque number.
#include "session/hid_evdev.hpp"

#include <linux/input-event-codes.h>

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>

using namespace wraith;

namespace {

struct Expect {
	uint32_t hid;
	uint32_t evdev;
};

// HID Usage Tables §10 (Keyboard/Keypad page) -> evdev.
// clang-format off: a table laid out by hand
constexpr Expect kExpected[] = {
	{0x04, KEY_A}, {0x05, KEY_B}, {0x06, KEY_C}, {0x07, KEY_D}, {0x08, KEY_E}, {0x09, KEY_F},
	{0x0a, KEY_G}, {0x0b, KEY_H}, {0x0c, KEY_I}, {0x0d, KEY_J}, {0x0e, KEY_K}, {0x0f, KEY_L},
	{0x10, KEY_M}, {0x11, KEY_N}, {0x12, KEY_O}, {0x13, KEY_P}, {0x14, KEY_Q}, {0x15, KEY_R},
	{0x16, KEY_S}, {0x17, KEY_T}, {0x18, KEY_U}, {0x19, KEY_V}, {0x1a, KEY_W}, {0x1b, KEY_X},
	{0x1c, KEY_Y}, {0x1d, KEY_Z},
	{0x1e, KEY_1}, {0x1f, KEY_2}, {0x20, KEY_3}, {0x21, KEY_4}, {0x22, KEY_5}, {0x23, KEY_6},
	{0x24, KEY_7}, {0x25, KEY_8}, {0x26, KEY_9}, {0x27, KEY_0},
	{0x28, KEY_ENTER}, {0x29, KEY_ESC}, {0x2a, KEY_BACKSPACE}, {0x2b, KEY_TAB}, {0x2c, KEY_SPACE},
	{0x2d, KEY_MINUS}, {0x2e, KEY_EQUAL}, {0x2f, KEY_LEFTBRACE}, {0x30, KEY_RIGHTBRACE},
	{0x31, KEY_BACKSLASH}, {0x32, KEY_BACKSLASH} /* "Non-US # and ~": kernel maps it to KEY_BACKSLASH too */,
	{0x33, KEY_SEMICOLON}, {0x34, KEY_APOSTROPHE}, {0x35, KEY_GRAVE}, {0x36, KEY_COMMA},
	{0x37, KEY_DOT}, {0x38, KEY_SLASH}, {0x39, KEY_CAPSLOCK},
	{0x3a, KEY_F1}, {0x3b, KEY_F2}, {0x3c, KEY_F3}, {0x3d, KEY_F4}, {0x3e, KEY_F5}, {0x3f, KEY_F6},
	{0x40, KEY_F7}, {0x41, KEY_F8}, {0x42, KEY_F9}, {0x43, KEY_F10}, {0x44, KEY_F11}, {0x45, KEY_F12},
	{0x46, KEY_SYSRQ}, {0x47, KEY_SCROLLLOCK}, {0x48, KEY_PAUSE}, {0x49, KEY_INSERT},
	{0x4a, KEY_HOME}, {0x4b, KEY_PAGEUP}, {0x4c, KEY_DELETE}, {0x4d, KEY_END}, {0x4e, KEY_PAGEDOWN},
	{0x4f, KEY_RIGHT}, {0x50, KEY_LEFT}, {0x51, KEY_DOWN}, {0x52, KEY_UP},
	{0x53, KEY_NUMLOCK}, {0x54, KEY_KPSLASH}, {0x55, KEY_KPASTERISK}, {0x56, KEY_KPMINUS},
	{0x57, KEY_KPPLUS}, {0x58, KEY_KPENTER},
	{0x59, KEY_KP1}, {0x5a, KEY_KP2}, {0x5b, KEY_KP3}, {0x5c, KEY_KP4}, {0x5d, KEY_KP5},
	{0x5e, KEY_KP6}, {0x5f, KEY_KP7}, {0x60, KEY_KP8}, {0x61, KEY_KP9}, {0x62, KEY_KP0},
	{0x63, KEY_KPDOT}, {0x64, KEY_102ND}, {0x65, KEY_COMPOSE}, {0x66, KEY_POWER}, {0x67, KEY_KPEQUAL},
	{0x68, KEY_F13}, {0x69, KEY_F14}, {0x6a, KEY_F15}, {0x6b, KEY_F16}, {0x6c, KEY_F17},
	{0x6d, KEY_F18}, {0x6e, KEY_F19}, {0x6f, KEY_F20}, {0x70, KEY_F21}, {0x71, KEY_F22},
	{0x72, KEY_F23}, {0x73, KEY_F24},
	{0x74, KEY_OPEN}, {0x75, KEY_HELP}, {0x76, KEY_PROPS}, {0x77, KEY_FRONT}, {0x78, KEY_STOP},
	{0x79, KEY_AGAIN}, {0x7a, KEY_UNDO}, {0x7b, KEY_CUT}, {0x7c, KEY_COPY}, {0x7d, KEY_PASTE},
	{0x7e, KEY_FIND}, {0x7f, KEY_MUTE}, {0x80, KEY_VOLUMEUP}, {0x81, KEY_VOLUMEDOWN},
	{0x85, KEY_KPCOMMA},
	{0x87, KEY_RO}, {0x88, KEY_KATAKANAHIRAGANA}, {0x89, KEY_YEN}, {0x8a, KEY_HENKAN},
	{0x8b, KEY_MUHENKAN}, {0x8c, KEY_KPJPCOMMA},
	{0x90, KEY_HANGEUL}, {0x91, KEY_HANJA}, {0x92, KEY_KATAKANA}, {0x93, KEY_HIRAGANA},
	{0x94, KEY_ZENKAKUHANKAKU},
	{0x9c, KEY_DELETE} /* Keyboard Clear */, {0xb6, KEY_KPLEFTPAREN}, {0xb7, KEY_KPRIGHTPAREN},
	{0xd8, KEY_DELETE} /* Keypad Clear */,
	{0xe0, KEY_LEFTCTRL}, {0xe1, KEY_LEFTSHIFT}, {0xe2, KEY_LEFTALT}, {0xe3, KEY_LEFTMETA},
	{0xe4, KEY_RIGHTCTRL}, {0xe5, KEY_RIGHTSHIFT}, {0xe6, KEY_RIGHTALT}, {0xe7, KEY_RIGHTMETA},
	{0xe8, KEY_PLAYPAUSE}, {0xe9, KEY_STOPCD}, {0xea, KEY_PREVIOUSSONG}, {0xeb, KEY_NEXTSONG},
	{0xec, KEY_EJECTCD}, {0xed, KEY_VOLUMEUP}, {0xee, KEY_VOLUMEDOWN}, {0xef, KEY_MUTE},
	{0xf0, KEY_WWW}, {0xf1, KEY_BACK}, {0xf2, KEY_FORWARD}, {0xf3, KEY_STOP}, {0xf4, KEY_FIND},
	{0xf5, KEY_SCROLLUP}, {0xf6, KEY_SCROLLDOWN}, {0xf7, KEY_EDIT}, {0xf8, KEY_SLEEP},
	{0xf9, KEY_COFFEE}, {0xfa, KEY_REFRESH}, {0xfb, KEY_CALC},
};
// clang-format on

void test_mapped_entries() {
	for (const Expect &e : kExpected) {
		uint32_t got = hid_usage_to_evdev(e.hid);
		if (got != e.evdev) {
			fprintf(stderr, "hid 0x%02x: expected evdev %u, got %u\n", e.hid, e.evdev, got);
			assert(false);
		}
	}
}

void test_unmapped_entries() {
	// Everything not listed above maps to 0 -- includes the rollover /
	// error pseudo-codes (1-3), the "locking" caps/num/scroll keys
	// (0x82-0x84) that have no evdev equivalent, and the whole 0x100+
	// range outside the table.
	bool mapped[256] = {};
	for (const Expect &e : kExpected) {
		mapped[e.hid] = true;
	}
	for (uint32_t hid = 0; hid < 256; hid++) {
		if (!mapped[hid]) {
			uint32_t got = hid_usage_to_evdev(hid);
			if (got != 0) {
				fprintf(stderr, "hid 0x%02x: expected no mapping, got %u\n", hid, got);
				assert(false);
			}
		}
	}
	assert(hid_usage_to_evdev(256) == 0);
	assert(hid_usage_to_evdev(0xffffffffu) == 0);
}

} // namespace

int main() {
	test_mapped_entries();
	test_unmapped_entries();
	printf("hid_evdev_test: ok\n");
	return 0;
}
