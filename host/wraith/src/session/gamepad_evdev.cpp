// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/gamepad_evdev.hpp"

#include <algorithm>
#include <cmath>

namespace wraith {

namespace {

using gdp::GamepadAxis;
using gdp::GamepadButton;

// SDL button -> evdev code, for the buttons the Xbox 360 layout has.
// BTN_X/BTN_Y rather than BTN_NORTH/BTN_WEST: the kernel aliases BTN_X
// (0x133) to BTN_NORTH and BTN_Y (0x134) to BTN_WEST -- geometrically
// backwards for an Xbox pad, but it is what the xpad driver reports and
// therefore what SDL's mapping on the host expects for X and Y. SDL's
// kWest *is* the X button, so kWest -> BTN_X, kNorth -> BTN_Y.
struct ButtonMap {
	GamepadButton button;
	uint16_t code;
};
constexpr ButtonMap kButtons[] = {
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
static_assert(sizeof(kButtons) / sizeof(kButtons[0]) == 11, "kMaxGamepadEvents counts 11 mapped buttons");

struct AxisMap {
	GamepadAxis axis;
	uint16_t code;
	bool trigger; // 0..1 -> 0..255 rather than -1..1 -> -32768..32767
};
constexpr AxisMap kAxes[] = {
	{GamepadAxis::kLeftX, ABS_X, false},
	{GamepadAxis::kLeftY, ABS_Y, false},
	{GamepadAxis::kRightX, ABS_RX, false},
	{GamepadAxis::kRightY, ABS_RY, false},
	{GamepadAxis::kLeftTrigger, ABS_Z, true},
	{GamepadAxis::kRightTrigger, ABS_RZ, true},
};

int32_t scale_axis(float value, bool trigger) {
	if (!std::isfinite(value)) {
		value = 0.0f;
	}
	if (trigger) {
		value = std::clamp(value, 0.0f, 1.0f);
		return static_cast<int32_t>(std::lround(value * 255.0f));
	}
	value = std::clamp(value, -1.0f, 1.0f);
	// 32767 both ways, so full deflection lands exactly on the max; the
	// device's minimum is -32768 but nothing needs that last count.
	return static_cast<int32_t>(std::lround(value * 32767.0f));
}

bool button(const gdp::GamepadSnapshot &s, GamepadButton b) {
	return s.buttons[static_cast<size_t>(b)];
}

int32_t hat_x(const gdp::GamepadSnapshot &s) {
	return (button(s, GamepadButton::kDpadRight) ? 1 : 0) - (button(s, GamepadButton::kDpadLeft) ? 1 : 0);
}

int32_t hat_y(const gdp::GamepadSnapshot &s) {
	return (button(s, GamepadButton::kDpadDown) ? 1 : 0) - (button(s, GamepadButton::kDpadUp) ? 1 : 0);
}

input_event make_event(uint16_t type, uint16_t code, int32_t value) {
	input_event ev{};
	ev.type = type;
	ev.code = code;
	ev.value = value;
	return ev;
}

} // namespace

size_t gamepad_evdev_diff(const gdp::GamepadSnapshot &prev, const gdp::GamepadSnapshot &next,
	input_event *out) {
	size_t n = 0;
	for (const AxisMap &a : kAxes) {
		int32_t before = scale_axis(prev.axes[static_cast<size_t>(a.axis)], a.trigger);
		int32_t after = scale_axis(next.axes[static_cast<size_t>(a.axis)], a.trigger);
		if (before != after) {
			out[n++] = make_event(EV_ABS, a.code, after);
		}
	}
	if (hat_x(prev) != hat_x(next)) {
		out[n++] = make_event(EV_ABS, ABS_HAT0X, hat_x(next));
	}
	if (hat_y(prev) != hat_y(next)) {
		out[n++] = make_event(EV_ABS, ABS_HAT0Y, hat_y(next));
	}
	for (const ButtonMap &b : kButtons) {
		bool before = button(prev, b.button);
		bool after = button(next, b.button);
		if (before != after) {
			out[n++] = make_event(EV_KEY, b.code, after ? 1 : 0);
		}
	}
	if (n == 0) {
		return 0;
	}
	out[n++] = make_event(EV_SYN, SYN_REPORT, 0);
	return n;
}

} // namespace wraith
