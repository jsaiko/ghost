// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// VirtualInput: ScreencastInput (remote_session.hpp) over the wlroots-
// origin zwlr_virtual_pointer_v1 and zwp_virtual_keyboard_v1 protocols
// (docs/design/capture-backends.md), the input half of
// screencast-ext. Less universal than the capture protocol -- wlroots,
// Smithay-based compositors and Hyprland implement both; a compositor
// without them gets no input.
//
// The virtual pointer's motion request is relative, so a game's pointer
// lock inside the compositor receives real deltas -- no nesting hop.
#pragma once

#include "screencast/remote_session.hpp"

#include <cstdint>
#include <memory>

struct wl_output;
struct wl_seat;
struct zwlr_virtual_pointer_manager_v1;
struct zwp_virtual_keyboard_manager_v1;

namespace wraith {

class WaylandClient;

class VirtualInput : public ScreencastInput {
public:
	struct Globals {
		WaylandClient *client = nullptr;
		struct wl_seat *seat = nullptr;
		struct wl_output *output = nullptr; // for create_virtual_pointer_with_output (v2); may be null
		struct zwlr_virtual_pointer_manager_v1 *pointer_manager = nullptr;
		struct zwp_virtual_keyboard_manager_v1 *keyboard_manager = nullptr;
		uint32_t pointer_manager_version = 1;
	};

	explicit VirtualInput(const Globals &globals);
	~VirtualInput() override;
	VirtualInput(const VirtualInput &) = delete;
	VirtualInput &operator=(const VirtualInput &) = delete;

	struct Config {
		// The output's pixel size: inject_motion_absolute's 0..1 input
		// is scaled into this and sent as motion_absolute's extent.
		uint32_t output_width = 0;
		uint32_t output_height = 0;
	};

	// Creates the virtual pointer and keyboard and uploads a default xkb
	// keymap (the protocol requires one before any key). False if either
	// object can't be created.
	bool open(const Config &config);
	void close() override;

	// --- InputSink ---
	void inject_key(uint32_t time_msec, uint32_t keycode, enum wl_keyboard_key_state state) override;
	void inject_motion(uint32_t time_msec, double dx, double dy) override;
	void inject_motion_absolute(uint32_t time_msec, double x, double y) override;
	void inject_button(uint32_t time_msec, uint32_t button, enum wl_pointer_button_state state) override;
	void inject_axis(uint32_t time_msec, enum wl_pointer_axis orientation, double delta) override;
	void resend_cursor_shape() override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
