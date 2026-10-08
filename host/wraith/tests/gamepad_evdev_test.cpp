// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// gamepad_evdev_diff (session/gamepad_evdev.hpp): the SDL-order snapshot
// (gdp/gamepad.hpp) to evdev mapping, the scaling, and the diff rule.
// This is the one place the wire's button/axis order meets evdev codes,
// so the specific codes are asserted, not just "something changed".
#include "session/gamepad_evdev.hpp"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

using gdp::GamepadAxis;
using gdp::GamepadButton;
using gdp::GamepadSnapshot;

size_t diff(const GamepadSnapshot &a, const GamepadSnapshot &b, input_event *out) {
	return wraith::gamepad_evdev_diff(a, b, out);
}

bool has(const input_event *ev, size_t n, uint16_t type, uint16_t code, int32_t value) {
	for (size_t i = 0; i < n; i++) {
		if (ev[i].type == type && ev[i].code == code && ev[i].value == value) {
			return true;
		}
	}
	return false;
}

void set_button(GamepadSnapshot *s, GamepadButton b, bool down) {
	s->buttons[static_cast<size_t>(b)] = down;
}

void set_axis(GamepadSnapshot *s, GamepadAxis a, float v) {
	s->axes[static_cast<size_t>(a)] = v;
}

void test_no_change_is_no_events() {
	GamepadSnapshot a, b;
	input_event ev[wraith::kMaxGamepadEvents];
	CHECK(diff(a, b, ev) == 0);
	set_axis(&a, GamepadAxis::kLeftX, 0.5f);
	set_axis(&b, GamepadAxis::kLeftX, 0.5f);
	CHECK(diff(a, b, ev) == 0);
}

void test_button_codes() {
	// SDL's positional names to the codes the xpad driver (and so SDL's
	// host-side mapping) uses: kWest is the X button and must be BTN_X,
	// even though the kernel aliases BTN_X to BTN_NORTH.
	struct {
		GamepadButton button;
		uint16_t code;
	} cases[] = {
		{GamepadButton::kSouth, BTN_A},
		{GamepadButton::kEast, BTN_B},
		{GamepadButton::kWest, BTN_X},
		{GamepadButton::kNorth, BTN_Y},
		{GamepadButton::kLeftShoulder, BTN_TL},
		{GamepadButton::kRightShoulder, BTN_TR},
		{GamepadButton::kBack, BTN_SELECT},
		{GamepadButton::kStart, BTN_START},
		{GamepadButton::kGuide, BTN_MODE},
		{GamepadButton::kLeftStick, BTN_THUMBL},
		{GamepadButton::kRightStick, BTN_THUMBR},
	};
	for (const auto &c : cases) {
		GamepadSnapshot a, b;
		set_button(&b, c.button, true);
		input_event ev[wraith::kMaxGamepadEvents];
		size_t n = diff(a, b, ev);
		CHECK(n == 2);
		CHECK(has(ev, n, EV_KEY, c.code, 1));
		CHECK(ev[n - 1].type == EV_SYN && ev[n - 1].code == SYN_REPORT);
		// And the release, diffed the other way.
		n = diff(b, a, ev);
		CHECK(n == 2);
		CHECK(has(ev, n, EV_KEY, c.code, 0));
	}
}

void test_unmapped_buttons_are_ignored() {
	GamepadSnapshot a, b;
	set_button(&b, GamepadButton::kTouchpad, true);
	set_button(&b, GamepadButton::kMisc1, true);
	set_button(&b, GamepadButton::kLeftPaddle1, true);
	input_event ev[wraith::kMaxGamepadEvents];
	CHECK(diff(a, b, ev) == 0);
}

void test_axis_scaling() {
	GamepadSnapshot a, b;
	set_axis(&b, GamepadAxis::kLeftX, 1.0f);
	set_axis(&b, GamepadAxis::kLeftY, -1.0f);
	set_axis(&b, GamepadAxis::kRightX, 0.5f);
	set_axis(&b, GamepadAxis::kRightY, 0.0f); // unchanged
	set_axis(&b, GamepadAxis::kLeftTrigger, 1.0f);
	set_axis(&b, GamepadAxis::kRightTrigger, 0.5f);
	input_event ev[wraith::kMaxGamepadEvents];
	size_t n = diff(a, b, ev);
	CHECK(n == 5 + 1);
	CHECK(has(ev, n, EV_ABS, ABS_X, 32767));
	CHECK(has(ev, n, EV_ABS, ABS_Y, -32767));
	CHECK(has(ev, n, EV_ABS, ABS_RX, 16384));
	CHECK(!has(ev, n, EV_ABS, ABS_RY, 0));
	CHECK(has(ev, n, EV_ABS, ABS_Z, 255));
	CHECK(has(ev, n, EV_ABS, ABS_RZ, 128));
}

void test_bad_floats_clamp() {
	GamepadSnapshot a, b;
	set_axis(&b, GamepadAxis::kLeftX, 40.0f);
	set_axis(&b, GamepadAxis::kLeftY, -40.0f);
	set_axis(&b, GamepadAxis::kRightX, NAN);
	set_axis(&b, GamepadAxis::kLeftTrigger, -3.0f);
	set_axis(&b, GamepadAxis::kRightTrigger, INFINITY);
	input_event ev[wraith::kMaxGamepadEvents];
	size_t n = diff(a, b, ev);
	CHECK(has(ev, n, EV_ABS, ABS_X, 32767));
	CHECK(has(ev, n, EV_ABS, ABS_Y, -32767));
	CHECK(!has(ev, n, EV_ABS, ABS_RX, 0) && !has(ev, n, EV_ABS, ABS_RX, 32767)); // NaN reads as 0: no change
	CHECK(!has(ev, n, EV_ABS, ABS_Z, 0));                                        // -3 clamps to 0: no change
	CHECK(!has(ev, n, EV_ABS, ABS_RZ, 255));                                     // inf reads as 0: no change
}

void test_dpad_becomes_hat() {
	GamepadSnapshot a, b;
	set_button(&b, GamepadButton::kDpadRight, true);
	set_button(&b, GamepadButton::kDpadUp, true);
	input_event ev[wraith::kMaxGamepadEvents];
	size_t n = diff(a, b, ev);
	CHECK(n == 3);
	CHECK(has(ev, n, EV_ABS, ABS_HAT0X, 1));
	CHECK(has(ev, n, EV_ABS, ABS_HAT0Y, -1));
	// Left+right together cancel to centre, and a change to centre is
	// still a change.
	GamepadSnapshot c = b;
	set_button(&c, GamepadButton::kDpadLeft, true);
	n = diff(b, c, ev);
	CHECK(n == 2);
	CHECK(has(ev, n, EV_ABS, ABS_HAT0X, 0));
	// D-pad buttons never appear as EV_KEY.
	n = diff(a, b, ev);
	for (size_t i = 0; i < n; i++) {
		CHECK(ev[i].type != EV_KEY);
	}
}

void test_everything_fits() {
	GamepadSnapshot a, b;
	for (size_t i = 0; i < gdp::kGamepadAxisCount; i++) {
		b.axes[i] = 1.0f;
	}
	for (size_t i = 0; i < gdp::kGamepadButtonCount; i++) {
		b.buttons[i] = true;
	}
	input_event ev[wraith::kMaxGamepadEvents];
	size_t n = diff(a, b, ev);
	// 6 axes + hat y (up+down cancel; left+right cancel -> hat x
	// unchanged) + 11 buttons + SYN.
	CHECK(n <= wraith::kMaxGamepadEvents);
	CHECK(n == 6 + 0 + 11 + 1);
}

} // namespace

int main() {
	test_no_change_is_no_events();
	test_button_codes();
	test_unmapped_buttons_are_ignored();
	test_axis_scaling();
	test_bad_floats_clamp();
	test_dpad_becomes_hat();
	test_everything_fits();
	if (g_failures) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
