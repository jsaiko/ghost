// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Gamepad forwarding (gdp-spec.md §8.5, §8.6): the "gamepad" and "hid"
// capabilities and the one thing both ends must agree on that the .proto
// can't express -- the *order* of GamepadState.axes and .buttons.
//
// The wire order is SDL3's SDL_GamepadAxis / SDL_GamepadButton order,
// copied here verbatim rather than referenced, because wraith doesn't
// link SDL at all and spectre's SDL may be a different release than the
// one this was written against. spectre static_asserts its SDL's enums
// against these; wraith maps them to evdev codes (wraith's
// session/gamepad_evdev.cpp). If SDL ever renumbers, the assert fires on
// the client side and the wire keeps *this* order.
#pragma once

#include <cstddef>
#include <cstdint>

namespace gdp {

inline constexpr const char *kCapabilityGamepad = "gamepad";
// Raw HID controllers (gdp-spec.md §8.6): HidConnect and the rest, only
// alongside "gamepad", whose slots they share.
inline constexpr const char *kCapabilityHid = "hid";
// HidConnect.report_descriptor's limit (the kernel's
// HID_MAX_DESCRIPTOR_SIZE) and a report's (UHID_DATA_MAX).
inline constexpr size_t kMaxHidDescriptor = 4096;
inline constexpr size_t kMaxHidReport = 4096;

// Slots a client may use in GamepadConnect/GamepadState.pad_index. Four
// matches what every platform's controller stack tops out at (XInput's
// hard limit, the four player LEDs on the pads themselves).
inline constexpr uint32_t kMaxGamepads = 4;

// SDL_GamepadAxis, SDL3 3.2+. Sticks are -1..1 (down and right positive,
// SDL's convention), triggers 0..1.
enum class GamepadAxis : uint8_t {
	kLeftX = 0,
	kLeftY,
	kRightX,
	kRightY,
	kLeftTrigger,
	kRightTrigger,
	kCount,
};
inline constexpr size_t kGamepadAxisCount = static_cast<size_t>(GamepadAxis::kCount);

// SDL_GamepadButton, SDL3 3.2+. Face buttons are by *position* (SDL's
// naming), so kSouth is Xbox A / PlayStation Cross regardless of label.
enum class GamepadButton : uint8_t {
	kSouth = 0,
	kEast,
	kWest,
	kNorth,
	kBack,
	kGuide,
	kStart,
	kLeftStick,
	kRightStick,
	kLeftShoulder,
	kRightShoulder,
	kDpadUp,
	kDpadDown,
	kDpadLeft,
	kDpadRight,
	kMisc1,
	kRightPaddle1,
	kLeftPaddle1,
	kRightPaddle2,
	kLeftPaddle2,
	kTouchpad,
	kMisc2,
	kMisc3,
	kMisc4,
	kMisc5,
	kMisc6,
	kCount,
};
inline constexpr size_t kGamepadButtonCount = static_cast<size_t>(GamepadButton::kCount);

// One controller's complete state as GamepadState carries it, in the
// order above. What spectre snapshots and what wraith diffs against.
struct GamepadSnapshot {
	float axes[kGamepadAxisCount] = {};
	bool buttons[kGamepadButtonCount] = {};
};

} // namespace gdp
