// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/ext_remote_session.hpp"

#include "screencast/data_control_clipboard.hpp"
#include "screencast/ext_image_copy_capture.hpp"
#include "screencast/virtual_input.hpp"
#include "screencast/wayland_client.hpp"

#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <linux-dmabuf-v1-client-protocol.h>
#include <virtual-keyboard-unstable-v1-client-protocol.h>
#include <wayland-client-protocol.h>
#include <wlr-output-management-unstable-v1-client-protocol.h>
#include <wlr-virtual-pointer-unstable-v1-client-protocol.h>

#include "util/log.hpp"

#include <string>

namespace wraith {

struct ExtRemoteSession::Impl {
	Config config;
	WaylandClient client;
	struct wl_output *output = nullptr;
	struct wl_seat *seat = nullptr;
	uint32_t seat_caps = 0;
	struct ext_output_image_capture_source_manager_v1 *source_manager = nullptr;
	struct ext_image_copy_capture_manager_v1 *capture_manager = nullptr;
	struct zwp_linux_dmabuf_v1 *dmabuf = nullptr;
	struct wl_shm *shm = nullptr;
	struct zwlr_virtual_pointer_manager_v1 *pointer_manager = nullptr;
	uint32_t pointer_manager_version = 1;
	struct zwp_virtual_keyboard_manager_v1 *keyboard_manager = nullptr;
	bool opened = false;
	bool closed_reported = false;
	bool missing_warned = false;
	std::function<void()> *on_closed = nullptr;

	// Best-effort output resize (resize_output_to_config()): not one of
	// the required globals, since a compositor without it just keeps
	// whatever size it started with (logged once below).
	struct zwlr_output_manager_v1 *output_manager = nullptr;
	struct zwlr_output_head_v1 *pending_head = nullptr;
	uint32_t pending_serial = 0;
	bool got_done = false;
	bool resize_result_known = false;
	bool resize_succeeded = false;
	// Set once an open() finds the compositor can't take a new size;
	// survives teardown(), so sizes_output_on_open() stops promising one.
	bool resize_unsupported = false;

	static void on_seat_capabilities(void *data, struct wl_seat *, uint32_t caps) {
		static_cast<Impl *>(data)->seat_caps = caps;
	}
	static void on_seat_name(void *, struct wl_seat *, const char *) {}

	// Only the first head is tracked: screencast-ext's whole model is one
	// output (the FrameSource binds "the first wl_output"), so a
	// multi-head compositor here would already be an unsupported shape.
	static void on_manager_head(void *data, struct zwlr_output_manager_v1 *,
		struct zwlr_output_head_v1 *head) {
		auto *impl = static_cast<Impl *>(data);
		if (!impl->pending_head) {
			impl->pending_head = head;
		}
	}
	static void on_manager_done(void *data, struct zwlr_output_manager_v1 *, uint32_t serial) {
		auto *impl = static_cast<Impl *>(data);
		impl->pending_serial = serial;
		impl->got_done = true;
	}
	static void on_manager_finished(void *data, struct zwlr_output_manager_v1 *) {
		static_cast<Impl *>(data)->output_manager = nullptr;
	}
	static void on_config_succeeded(void *data, struct zwlr_output_configuration_v1 *) {
		auto *impl = static_cast<Impl *>(data);
		impl->resize_result_known = true;
		impl->resize_succeeded = true;
	}
	static void on_config_failed(void *data, struct zwlr_output_configuration_v1 *) {
		auto *impl = static_cast<Impl *>(data);
		impl->resize_result_known = true;
		impl->resize_succeeded = false;
	}
	static void on_config_cancelled(void *data, struct zwlr_output_configuration_v1 *) {
		auto *impl = static_cast<Impl *>(data);
		impl->resize_result_known = true;
		impl->resize_succeeded = false;
	}

	void report_closed(const std::string &reason) {
		if (!opened || closed_reported) {
			return;
		}
		closed_reported = true;
		WLOG_ERROR("screencast: %s", reason.c_str());
		if (on_closed && *on_closed) {
			auto cb = *on_closed;
			cb();
		}
	}

	void teardown() {
		if (seat) {
			wl_seat_destroy(seat);
			seat = nullptr;
		}
		if (output) {
			wl_output_destroy(output);
			output = nullptr;
		}
		if (source_manager) {
			ext_output_image_capture_source_manager_v1_destroy(source_manager);
			source_manager = nullptr;
		}
		if (capture_manager) {
			ext_image_copy_capture_manager_v1_destroy(capture_manager);
			capture_manager = nullptr;
		}
		if (dmabuf) {
			zwp_linux_dmabuf_v1_destroy(dmabuf);
			dmabuf = nullptr;
		}
		if (shm) {
			wl_shm_destroy(shm);
			shm = nullptr;
		}
		if (pointer_manager) {
			zwlr_virtual_pointer_manager_v1_destroy(pointer_manager);
			pointer_manager = nullptr;
		}
		if (keyboard_manager) {
			zwp_virtual_keyboard_manager_v1_destroy(keyboard_manager);
			keyboard_manager = nullptr;
		}
		if (output_manager) {
			zwlr_output_manager_v1_destroy(output_manager);
			output_manager = nullptr;
		}
		client.flush();
		client.disconnect();
		seat_caps = 0;
		pending_head = nullptr;
		pending_serial = 0;
		got_done = false;
		resize_result_known = false;
		resize_succeeded = false;
	}

	// Best-effort: resizes the compositor's first output to config.width x
	// config.height via zwlr_output_manager_v1 (wlr-output-management-
	// unstable-v1), the same mechanism wayvnc uses to size a wlroots
	// headless/virtual output to match a connecting client. No-op if the
	// compositor doesn't export the protocol, or advertises no heads --
	// the caller proceeds with whatever size the compositor already has.
	void resize_output_to_config() {
		output_manager =
			static_cast<struct zwlr_output_manager_v1 *>(client.bind(&zwlr_output_manager_v1_interface, 4));
		if (!output_manager) {
			WLOG_INFO("screencast: ext: compositor has no zwlr_output_manager_v1, "
					  "output size (not GHOST_OUTPUT_WIDTH/HEIGHT) is whatever it already is");
			resize_unsupported = true;
			return;
		}
		static const struct zwlr_output_manager_v1_listener manager_listener = {
			.head = Impl::on_manager_head,
			.done = Impl::on_manager_done,
			.finished = Impl::on_manager_finished,
		};
		zwlr_output_manager_v1_add_listener(output_manager, &manager_listener, this);
		if (!client.roundtrip() || !output_manager) {
			return; // connection died, or the manager was already torn down
		}
		if (!got_done || !pending_head) {
			WLOG_ERROR("screencast: ext: zwlr_output_manager_v1 advertised no heads, "
					   "keeping the compositor's own output size");
			resize_unsupported = true;
			return;
		}

		struct zwlr_output_configuration_v1 *cfg =
			zwlr_output_manager_v1_create_configuration(output_manager, pending_serial);
		struct zwlr_output_configuration_head_v1 *cfg_head =
			zwlr_output_configuration_v1_enable_head(cfg, pending_head);
		zwlr_output_configuration_head_v1_set_custom_mode(cfg_head, (int32_t)config.width,
			(int32_t)config.height, 0);
		static const struct zwlr_output_configuration_v1_listener config_listener = {
			.succeeded = Impl::on_config_succeeded,
			.failed = Impl::on_config_failed,
			.cancelled = Impl::on_config_cancelled,
		};
		zwlr_output_configuration_v1_add_listener(cfg, &config_listener, this);
		zwlr_output_configuration_v1_apply(cfg);
		if (!client.roundtrip() || !resize_result_known || !resize_succeeded) {
			WLOG_ERROR("screencast: ext: compositor rejected a %ux%u output resize, "
					   "keeping its own output size",
				config.width, config.height);
			resize_unsupported = true;
		} else {
			WLOG_INFO("screencast: ext: resized the compositor's output to %ux%u", config.width,
				config.height);
		}
		// The head has no destructor request; its proxy is freed client-side.
		zwlr_output_configuration_head_v1_destroy(cfg_head);
		zwlr_output_configuration_v1_destroy(cfg);
	}

	bool connect_wayland() {
		std::string name = WaylandClient::display_from_user_manager();
		if (!name.empty() && client.connect(config.event_loop, name.c_str())) {
			return true;
		}
		return client.connect(config.event_loop, "wayland-0");
	}
};

ExtRemoteSession::ExtRemoteSession() : impl_(std::make_unique<Impl>()) {
	impl_->on_closed = &on_closed;
}

ExtRemoteSession::~ExtRemoteSession() {
	close();
}

bool ExtRemoteSession::open(const Config &config) {
	Impl &i = *impl_;
	i.config = config;
	i.closed_reported = false;

	if (!i.connect_wayland()) {
		return false; // the leader's socket isn't there yet
	}
	i.client.on_disconnected = [this](const std::string &reason) { impl_->report_closed(reason); };

	// Required globals, named so a compositor that lacks one is diagnosed
	// in one log line rather than by a null proxy later.
	const char *required[] = {
		wl_output_interface.name,
		wl_seat_interface.name,
		ext_output_image_capture_source_manager_v1_interface.name,
		ext_image_copy_capture_manager_v1_interface.name,
		zwlr_virtual_pointer_manager_v1_interface.name,
		zwp_virtual_keyboard_manager_v1_interface.name,
	};
	std::string missing;
	for (const char *iface : required) {
		if (!i.client.has_global(iface)) {
			missing += missing.empty() ? iface : std::string(", ") + iface;
		}
	}
	if (!i.client.has_global(zwp_linux_dmabuf_v1_interface.name) &&
		!i.client.has_global(wl_shm_interface.name)) {
		missing += missing.empty() ? "zwp_linux_dmabuf_v1 or wl_shm" : ", zwp_linux_dmabuf_v1 or wl_shm";
	}
	if (!missing.empty()) {
		if (!i.missing_warned) {
			i.missing_warned = true;
			WLOG_ERROR("screencast: the compositor does not export what screencast-ext needs: %s",
				missing.c_str());
		}
		i.teardown();
		return false;
	}

	i.output = static_cast<struct wl_output *>(i.client.bind(&wl_output_interface, 2));
	i.seat = static_cast<struct wl_seat *>(i.client.bind(&wl_seat_interface, 5));
	i.source_manager = static_cast<struct ext_output_image_capture_source_manager_v1 *>(
		i.client.bind(&ext_output_image_capture_source_manager_v1_interface, 1));
	i.capture_manager = static_cast<struct ext_image_copy_capture_manager_v1 *>(
		i.client.bind(&ext_image_copy_capture_manager_v1_interface, 1));
	i.dmabuf = static_cast<struct zwp_linux_dmabuf_v1 *>(i.client.bind(&zwp_linux_dmabuf_v1_interface, 4));
	i.shm = static_cast<struct wl_shm *>(i.client.bind(&wl_shm_interface, 1));
	i.pointer_manager = static_cast<struct zwlr_virtual_pointer_manager_v1 *>(
		i.client.bind(&zwlr_virtual_pointer_manager_v1_interface, 2));
	i.pointer_manager_version =
		i.pointer_manager ? wl_proxy_get_version((struct wl_proxy *)i.pointer_manager) : 1;
	i.keyboard_manager = static_cast<struct zwp_virtual_keyboard_manager_v1 *>(
		i.client.bind(&zwp_virtual_keyboard_manager_v1_interface, 1));

	static const struct wl_seat_listener seat_listener = {
		.capabilities = Impl::on_seat_capabilities,
		.name = Impl::on_seat_name,
	};
	wl_seat_add_listener(i.seat, &seat_listener, impl_.get());
	if (!i.client.roundtrip()) {
		i.teardown();
		return false;
	}
	if (!(i.seat_caps & WL_SEAT_CAPABILITY_POINTER)) {
		// Normal for a headless compositor: the pointer appears once
		// make_input_sink() creates wraith's virtual one, and the frame
		// source picks it up then (it watches the seat itself).
		WLOG_INFO("screencast: ext: the seat has no pointer yet, cursor session deferred");
	}

	i.resize_output_to_config();

	WLOG_INFO("screencast: ext session bound");
	i.opened = true;
	return true;
}

bool ExtRemoteSession::sizes_output_on_open() const {
	return !impl_->resize_unsupported;
}

void ExtRemoteSession::close() {
	impl_->teardown();
	impl_->opened = false;
}

std::unique_ptr<FrameSource> ExtRemoteSession::make_frame_source() {
	ExtImageCopyCapture::Globals g;
	g.client = &impl_->client;
	g.output = impl_->output;
	g.source_manager = impl_->source_manager;
	g.capture_manager = impl_->capture_manager;
	g.dmabuf = impl_->dmabuf;
	g.shm = impl_->shm;
	// A seat proxy of the capture's own, so it can listen for the pointer
	// capability without sharing this session's listener; the capture
	// destroys it in close().
	g.seat = static_cast<struct wl_seat *>(impl_->client.bind(&wl_seat_interface, 5));
	return std::make_unique<ExtImageCopyCapture>(g);
}

std::unique_ptr<ClipboardSink> ExtRemoteSession::make_clipboard_sink() {
	return DataControlClipboard::create(impl_->client, impl_->config.event_loop);
}

std::unique_ptr<ScreencastInput> ExtRemoteSession::make_input_sink() {
	VirtualInput::Globals g;
	g.client = &impl_->client;
	g.seat = impl_->seat;
	g.output = impl_->output;
	g.pointer_manager = impl_->pointer_manager;
	g.pointer_manager_version = impl_->pointer_manager_version;
	g.keyboard_manager = impl_->keyboard_manager;
	auto input = std::make_unique<VirtualInput>(g);
	VirtualInput::Config config;
	config.output_width = impl_->config.width;
	config.output_height = impl_->config.height;
	if (!input->open(config)) {
		return nullptr;
	}
	return input;
}

} // namespace wraith
