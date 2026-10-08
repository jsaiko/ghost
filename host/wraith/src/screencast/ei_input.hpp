// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// EiInput: ScreencastInput (remote_session.hpp) implemented over libei.
// The EIS fd comes from a RemoteSession (mutter's `RemoteDesktop.Session.
// ConnectToEIS`, kwin's `org.kde.KWin.EIS.RemoteDesktop.connectToEIS`).
// This class has no D-Bus knowledge of its own -- it just turns an
// already-open EIS fd into libei sender calls, so it works unchanged
// against any EIS-speaking compositor.
//
// Runs on the caller's wl_event_loop: libei's one fd (ei_get_fd) is added
// as a source, same pattern PipeWireCapture's PipeWire fd uses.
#pragma once

#include "screencast/remote_session.hpp"

#include <cstdint>
#include <memory>

struct wl_event_loop;

namespace wraith {

class EiInput : public ScreencastInput {
public:
	EiInput();
	~EiInput() override;
	EiInput(const EiInput &) = delete;
	EiInput &operator=(const EiInput &) = delete;

	struct Config {
		struct wl_event_loop *event_loop = nullptr;
		// The virtual output's pixel size (wraith's own -o size). Only a
		// fallback for inject_motion_absolute, which maps its 0..1 input
		// into the region the compositor configured on the absolute
		// device -- that region is in logical pixels, so it differs from
		// this whenever the remote desktop is scaled.
		uint32_t output_width = 0;
		uint32_t output_height = 0;
		// Already-open EIS fd -- ei_setup_backend_fd takes ownership of
		// it; the caller must not close it itself, on success or failure.
		int eis_fd = -1;
	};

	// Sets up an ei sender context on config.eis_fd. False on any
	// failure -- ei_setup_backend_fd itself, most likely. on_disconnected
	// and resend_cursor_shape_cb (ScreencastInput) are read when they
	// fire, so they may be set after this.
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
