// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/ei_input.hpp"

#include <libei.h>

#include "util/log.hpp"

#include <wayland-server-core.h>

#include <algorithm>
#include <cmath>

#include <unistd.h>

namespace wraith {

struct EiInput::Impl {
	Config config;
	struct ei *ei = nullptr;
	struct wl_event_source *source = nullptr;

	// mutter hands out *separate* ei_devices per pointer capability --
	// one relative-only "mouse", one absolute-only "tablet" -- rather
	// than a single device with both, so the two are tracked separately
	// and each motion goes to the device that has its capability.
	struct ei_device *pointer_rel_device = nullptr;
	struct ei_device *pointer_abs_device = nullptr;
	struct ei_device *keyboard_device = nullptr;
	uint32_t emulating_seq = 0;

	// Button/scroll capability isn't guaranteed to land on either
	// specific device -- check whichever of the two actually has it
	// rather than assuming.
	struct ei_device *device_with_capability(enum ei_device_capability cap) const {
		if (pointer_rel_device && ei_device_has_capability(pointer_rel_device, cap)) {
			return pointer_rel_device;
		}
		if (pointer_abs_device && ei_device_has_capability(pointer_abs_device, cap)) {
			return pointer_abs_device;
		}
		return nullptr;
	}

	// Back-pointer for the ScreencastInput callbacks, read when they fire
	// (the host sets them after open(), see remote_session.hpp).
	EiInput *self = nullptr;

	static void on_readable(void *data) {
		auto *impl = static_cast<Impl *>(data);
		ei_dispatch(impl->ei);
		bool disconnected = false;
		struct ei_event *event;
		while ((event = ei_get_event(impl->ei)) != nullptr) {
			// DISCONNECT is deliberately not handled inline: the
			// callback's owner (ScreencastHost::on_input_disconnected)
			// close()s this object, which ei_unref()s and nulls impl->ei
			// while this loop still needs it for the next ei_get_event.
			if (ei_event_get_type(event) == EI_EVENT_DISCONNECT) {
				disconnected = true;
			} else {
				impl->handle_event(event);
			}
			ei_event_unref(event);
		}
		if (disconnected && impl->self->on_disconnected) {
			// Last thing this function touches, through a copy: the
			// callback may close() or even destroy the EiInput, and
			// `impl` is not valid afterwards.
			auto cb = impl->self->on_disconnected;
			cb();
		}
	}

	void handle_event(struct ei_event *event) {
		switch (ei_event_get_type(event)) {
		case EI_EVENT_SEAT_ADDED:
			ei_seat_bind_capabilities(ei_event_get_seat(event), EI_DEVICE_CAP_POINTER,
				EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL,
				EI_DEVICE_CAP_KEYBOARD, nullptr);
			break;
		case EI_EVENT_DEVICE_ADDED:
		case EI_EVENT_DEVICE_RESUMED: {
			struct ei_device *device = ei_event_get_device(event);
			if (!pointer_rel_device && ei_device_has_capability(device, EI_DEVICE_CAP_POINTER)) {
				pointer_rel_device = device;
			}
			if (!pointer_abs_device && ei_device_has_capability(device, EI_DEVICE_CAP_POINTER_ABSOLUTE)) {
				pointer_abs_device = device;
				// The region is what absolute positions are mapped into
				// (see inject_motion_absolute) -- worth a line, since a
				// scaled remote desktop makes it differ from the output
				// size.
				if (struct ei_region *region = ei_device_get_region(device, 0)) {
					WLOG_INFO("screencast: ei absolute region %ux%u+%u+%u (output %ux%u)",
						ei_region_get_width(region), ei_region_get_height(region), ei_region_get_x(region),
						ei_region_get_y(region), config.output_width, config.output_height);
				} else {
					WLOG_INFO("screencast: ei absolute device has no region; mapping to output %ux%u",
						config.output_width, config.output_height);
				}
			}
			if (!keyboard_device && ei_device_has_capability(device, EI_DEVICE_CAP_KEYBOARD)) {
				keyboard_device = device;
			}
			ei_device_start_emulating(device, ++emulating_seq);
			break;
		}
		case EI_EVENT_DEVICE_PAUSED: ei_device_stop_emulating(ei_event_get_device(event)); break;
		case EI_EVENT_DEVICE_REMOVED: {
			struct ei_device *device = ei_event_get_device(event);
			if (device == pointer_rel_device) {
				pointer_rel_device = nullptr;
			}
			if (device == pointer_abs_device) {
				pointer_abs_device = nullptr;
			}
			if (device == keyboard_device) {
				keyboard_device = nullptr;
			}
			break;
		}
		case EI_EVENT_DISCONNECT:
			// Handled by on_readable after the drain loop, see there.
			break;
		default: break;
		}
	}
};

EiInput::EiInput() : impl_(std::make_unique<Impl>()) {}
EiInput::~EiInput() {
	close();
}

bool EiInput::open(const Config &config) {
	impl_->config = config;
	impl_->self = this;

	if (config.eis_fd < 0) {
		WLOG_ERROR("screencast: EiInput::open() needs an already-open EIS fd");
		return false;
	}

	impl_->ei = ei_new_sender(impl_.get());
	ei_configure_name(impl_->ei, "wraith");
	if (ei_setup_backend_fd(impl_->ei, config.eis_fd) < 0) {
		WLOG_ERROR("screencast: ei_setup_backend_fd failed");
		return false;
	}

	impl_->source = wl_event_loop_add_fd(
		config.event_loop, ei_get_fd(impl_->ei), WL_EVENT_READABLE,
		[](int, uint32_t, void *data) {
			Impl::on_readable(data);
			return 0;
		},
		impl_.get());
	return true;
}

void EiInput::close() {
	if (impl_->source) {
		wl_event_source_remove(impl_->source);
		impl_->source = nullptr;
	}
	if (impl_->ei) {
		ei_unref(impl_->ei);
		impl_->ei = nullptr;
	}
	impl_->pointer_rel_device = nullptr;
	impl_->pointer_abs_device = nullptr;
	impl_->keyboard_device = nullptr;
}

void EiInput::inject_key(uint32_t, uint32_t keycode, enum wl_keyboard_key_state state) {
	if (!impl_->keyboard_device) {
		return;
	}
	ei_device_keyboard_key(impl_->keyboard_device, keycode, state == WL_KEYBOARD_KEY_STATE_PRESSED);
	ei_device_frame(impl_->keyboard_device, ei_now(impl_->ei));
}

void EiInput::inject_motion(uint32_t, double dx, double dy) {
	if (!impl_->pointer_rel_device) {
		return;
	}
	ei_device_pointer_motion(impl_->pointer_rel_device, dx, dy);
	ei_device_frame(impl_->pointer_rel_device, ei_now(impl_->ei));
}

void EiInput::inject_motion_absolute(uint32_t, double x, double y) {
	if (!impl_->pointer_abs_device) {
		return;
	}
	// libei's absolute coordinates are desktop-wide *logical* pixels, not
	// the output's pixel size: on a remote desktop at 200% a 3840x2160
	// output is a 1920x1080 region, and scaling the 0..1 position by the
	// output size instead would land at twice the intended point (clamped
	// to the right/bottom edge for anything past halfway). Map into the
	// region the compositor actually configured on the device -- which
	// also picks up its origin, for a region that isn't at 0,0.
	double rx = 0.0, ry = 0.0;
	double rw = impl_->config.output_width, rh = impl_->config.output_height;
	if (struct ei_region *region = ei_device_get_region(impl_->pointer_abs_device, 0)) {
		rx = ei_region_get_x(region);
		ry = ei_region_get_y(region);
		rw = ei_region_get_width(region);
		rh = ei_region_get_height(region);
	}
	if (rw <= 0.0 || rh <= 0.0) {
		return;
	}
	// The far edge is outside the region (a region is half-open), and an
	// out-of-region position is dropped outright by some EIS
	// implementations rather than clamped -- keep it just inside.
	double ax = std::clamp(rx + x * rw, rx, std::nextafter(rx + rw, rx));
	double ay = std::clamp(ry + y * rh, ry, std::nextafter(ry + rh, ry));
	ei_device_pointer_motion_absolute(impl_->pointer_abs_device, ax, ay);
	ei_device_frame(impl_->pointer_abs_device, ei_now(impl_->ei));
}

void EiInput::inject_button(uint32_t, uint32_t button, enum wl_pointer_button_state state) {
	struct ei_device *device = impl_->device_with_capability(EI_DEVICE_CAP_BUTTON);
	if (!device) {
		return;
	}
	ei_device_button_button(device, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
	ei_device_frame(device, ei_now(impl_->ei));
}

void EiInput::inject_axis(uint32_t, enum wl_pointer_axis orientation, double delta) {
	struct ei_device *device = impl_->device_with_capability(EI_DEVICE_CAP_SCROLL);
	if (!device) {
		return;
	}
	// `delta` is in wheel notches (gdp-spec.md §8.2); libei's
	// discrete scroll counts 120 to a notch and takes fractions, so a
	// touchpad's small steps stay small. Never rounded to nothing: a step
	// too small to be 1/120 of a notch still moves.
	int32_t discrete = static_cast<int32_t>(std::lround(delta * 120.0));
	if (discrete == 0) {
		discrete = delta > 0 ? 1 : -1;
	}
	if (orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
		ei_device_scroll_discrete(device, discrete, 0);
	} else {
		ei_device_scroll_discrete(device, 0, discrete);
	}
	ei_device_frame(device, ei_now(impl_->ei));
}

void EiInput::resend_cursor_shape() {
	if (resend_cursor_shape_cb) {
		resend_cursor_shape_cb();
	}
}

} // namespace wraith
