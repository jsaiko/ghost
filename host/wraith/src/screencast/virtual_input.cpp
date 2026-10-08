// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/virtual_input.hpp"

#include "screencast/wayland_client.hpp"

#include <cmath>
#include <virtual-keyboard-unstable-v1-client-protocol.h>
#include <wayland-client-protocol.h>
#include <wlr-virtual-pointer-unstable-v1-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "util/log.hpp"

#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace wraith {

struct VirtualInput::Impl {
	Globals g;
	Config config;
	struct zwlr_virtual_pointer_v1 *pointer = nullptr;
	struct zwp_virtual_keyboard_v1 *keyboard = nullptr;

	// The protocol requires a keymap before any key; the compositor keeps
	// its own xkb state for a virtual keyboard, so a plain default map is
	// enough (spectre sends evdev keycodes, not symbols).
	bool upload_default_keymap() {
		struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		if (!ctx) {
			return false;
		}
		struct xkb_rule_names names{};
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (!keymap) {
			xkb_context_unref(ctx);
			return false;
		}
		char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
		bool ok = false;
		if (text) {
			size_t size = std::strlen(text) + 1;
			int fd = memfd_create("wraith-keymap", MFD_CLOEXEC);
			if (fd >= 0 && ftruncate(fd, (off_t)size) == 0) {
				void *map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
				if (map != MAP_FAILED) {
					std::memcpy(map, text, size);
					munmap(map, size);
					zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
						(uint32_t)size);
					ok = true;
				}
			}
			if (fd >= 0) {
				::close(fd);
			}
			free(text);
		}
		xkb_keymap_unref(keymap);
		xkb_context_unref(ctx);
		return ok;
	}
};

VirtualInput::VirtualInput(const Globals &globals) : impl_(std::make_unique<Impl>()) {
	impl_->g = globals;
}

VirtualInput::~VirtualInput() {
	close();
}

bool VirtualInput::open(const Config &config) {
	impl_->config = config;
	Impl &i = *impl_;
	if (!i.g.client || !i.g.seat || !i.g.pointer_manager || !i.g.keyboard_manager) {
		WLOG_ERROR("screencast: virtual input: missing globals");
		return false;
	}
	if (i.g.pointer_manager_version >= 2 && i.g.output) {
		i.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(i.g.pointer_manager,
			i.g.seat, i.g.output);
	} else {
		i.pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(i.g.pointer_manager, i.g.seat);
	}
	i.keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(i.g.keyboard_manager, i.g.seat);
	if (!i.pointer || !i.keyboard) {
		close();
		return false;
	}
	if (!i.upload_default_keymap()) {
		WLOG_ERROR("screencast: virtual input: building the default xkb keymap failed");
		close();
		return false;
	}
	i.g.client->flush();
	WLOG_INFO("screencast: virtual pointer + keyboard created (screencast-ext input)");
	return true;
}

void VirtualInput::close() {
	if (impl_->keyboard) {
		zwp_virtual_keyboard_v1_destroy(impl_->keyboard);
		impl_->keyboard = nullptr;
	}
	if (impl_->pointer) {
		zwlr_virtual_pointer_v1_destroy(impl_->pointer);
		impl_->pointer = nullptr;
	}
	if (impl_->g.client) {
		impl_->g.client->flush();
	}
}

void VirtualInput::inject_key(uint32_t time_msec, uint32_t keycode, enum wl_keyboard_key_state state) {
	if (!impl_->keyboard) {
		return;
	}
	zwp_virtual_keyboard_v1_key(impl_->keyboard, time_msec, keycode, (uint32_t)state);
	impl_->g.client->flush();
}

void VirtualInput::inject_motion(uint32_t time_msec, double dx, double dy) {
	if (!impl_->pointer) {
		return;
	}
	zwlr_virtual_pointer_v1_motion(impl_->pointer, time_msec, wl_fixed_from_double(dx),
		wl_fixed_from_double(dy));
	zwlr_virtual_pointer_v1_frame(impl_->pointer);
	impl_->g.client->flush();
}

void VirtualInput::inject_motion_absolute(uint32_t time_msec, double x, double y) {
	if (!impl_->pointer) {
		return;
	}
	uint32_t w = impl_->config.output_width, h = impl_->config.output_height;
	zwlr_virtual_pointer_v1_motion_absolute(impl_->pointer, time_msec, (uint32_t)(x * w), (uint32_t)(y * h),
		w, h);
	zwlr_virtual_pointer_v1_frame(impl_->pointer);
	impl_->g.client->flush();
}

void VirtualInput::inject_button(uint32_t time_msec, uint32_t button, enum wl_pointer_button_state state) {
	if (!impl_->pointer) {
		return;
	}
	zwlr_virtual_pointer_v1_button(impl_->pointer, time_msec, button, (uint32_t)state);
	zwlr_virtual_pointer_v1_frame(impl_->pointer);
	impl_->g.client->flush();
}

void VirtualInput::inject_axis(uint32_t time_msec, enum wl_pointer_axis orientation, double delta) {
	if (!impl_->pointer) {
		return;
	}
	// `delta` is in wheel notches (gdp-spec.md §8.2). A Wayland
	// axis value is in pixels, and a wheel is told apart from a touchpad by
	// its source and its discrete step: libinput reports a notch as 15 px
	// plus one step, which is what clients (terminals above all) turn into
	// "three lines". A bare 1.0 is one pixel, which scrolls nothing.
	constexpr double kPixelsPerNotch = 15.0;
	const wl_fixed_t value = wl_fixed_from_double(delta * kPixelsPerNotch);
	const int32_t steps = static_cast<int32_t>(std::lround(delta));
	zwlr_virtual_pointer_v1_axis_source(impl_->pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
	if (steps != 0) {
		zwlr_virtual_pointer_v1_axis_discrete(impl_->pointer, time_msec, (uint32_t)orientation, value, steps);
	} else {
		// Less than half a notch: smooth scrolling, pixels only.
		zwlr_virtual_pointer_v1_axis(impl_->pointer, time_msec, (uint32_t)orientation, value);
	}
	zwlr_virtual_pointer_v1_frame(impl_->pointer);
	impl_->g.client->flush();
}

void VirtualInput::resend_cursor_shape() {
	if (resend_cursor_shape_cb) {
		resend_cursor_shape_cb();
	}
}

} // namespace wraith
