// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/gnome_remote_session.hpp"

#include "screencast/ei_input.hpp"
#include "screencast/gnome_clipboard.hpp"
#include "screencast/pipewire_capture.hpp"
#include "screencast/sdbus_util.hpp"
#include "util/clock.hpp"

#include <systemd/sd-bus.h>

#include "util/log.hpp"

#include <wayland-server-core.h>

#include <cerrno>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <unistd.h>

namespace wraith {

namespace {
constexpr const char *kRdService = "org.gnome.Mutter.RemoteDesktop";
constexpr const char *kRdPath = "/org/gnome/Mutter/RemoteDesktop";
constexpr const char *kRdIface = "org.gnome.Mutter.RemoteDesktop";
constexpr const char *kRdSessionIface = "org.gnome.Mutter.RemoteDesktop.Session";
constexpr const char *kScService = "org.gnome.Mutter.ScreenCast";
constexpr const char *kScPath = "/org/gnome/Mutter/ScreenCast";
constexpr const char *kScIface = "org.gnome.Mutter.ScreenCast";
constexpr const char *kScSessionIface = "org.gnome.Mutter.ScreenCast.Session";
constexpr const char *kScStreamIface = "org.gnome.Mutter.ScreenCast.Stream";

// The linked stream's PipeWireStreamAdded signal fires synchronously
// inside RemoteDesktop.Session.Start() in practice, well under 100 ms;
// 3 s is a generous ceiling, not a tuned value.
constexpr int64_t kNodeIdWaitUs = 3'000'000;

} // namespace

struct GnomeRemoteSession::Impl {
	Config config;
	sd_bus *bus = nullptr;
	std::string rd_path;
	std::string sc_path;
	std::string stream_path;
	uint32_t node_id = 0;
	bool node_id_ready = false;
	bool eis_connected = false;

	sd_bus_slot *node_added_slot = nullptr;
	sd_bus_slot *closed_slot = nullptr;
	struct wl_event_source *bus_source = nullptr;

	std::function<void()> on_closed_cb;

	static int on_pipewire_stream_added(sd_bus_message *m, void *data, sd_bus_error *) {
		auto *impl = static_cast<Impl *>(data);
		uint32_t node_id = 0;
		if (sd_bus_message_read(m, "u", &node_id) >= 0) {
			impl->node_id = node_id;
			impl->node_id_ready = true;
		}
		return 0;
	}

	static int on_session_closed(sd_bus_message *, void *data, sd_bus_error *) {
		auto *impl = static_cast<Impl *>(data);
		if (impl->on_closed_cb) {
			impl->on_closed_cb();
		}
		return 0;
	}

	// CreateSession on org.gnome.Mutter.RemoteDesktop -- no arguments,
	// returns the new session's object path.
	bool create_remote_desktop_session() {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		int r = sd_bus_call_method(bus, kRdService, kRdPath, kRdIface, "CreateSession", &error, &reply, "");
		if (r < 0) {
			WLOG_ERROR("screencast: RemoteDesktop.CreateSession failed: %s", sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			return false;
		}
		const char *path = nullptr;
		r = sd_bus_message_read(reply, "o", &path);
		if (r >= 0 && path) {
			rd_path = path;
		}
		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);
		return r >= 0 && !rd_path.empty();
	}

	bool get_session_id(std::string *out) {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		char *session_id = nullptr;
		int r = sd_bus_get_property_string(bus, kRdService, rd_path.c_str(), kRdSessionIface, "SessionId",
			&error, &session_id);
		if (r < 0) {
			WLOG_ERROR("screencast: reading RemoteDesktop.Session.SessionId failed: %s",
				sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			return false;
		}
		sd_bus_error_free(&error);
		*out = session_id ? session_id : "";
		free(session_id);
		return !out->empty();
	}

	// ScreenCast.CreateSession({'remote-desktop-session-id': session_id})
	// -- the a{sv} dict with one string-valued entry links the two
	// sessions (docs/design/capture-backends.md#gnome).
	bool create_linked_screencast_session(const std::string &session_id) {
		sd_bus_message *m = nullptr;
		int r = sd_bus_message_new_method_call(bus, &m, kScService, kScPath, kScIface, "CreateSession");
		if (r < 0) {
			return false;
		}
		r = sdbus_append_one_option(m, "remote-desktop-session-id", "s",
			[&](sd_bus_message *msg) { return sd_bus_message_append(msg, "s", session_id.c_str()); });
		if (r < 0) {
			sd_bus_message_unref(m);
			return false;
		}

		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		r = sd_bus_call(bus, m, 0, &error, &reply);
		sd_bus_message_unref(m);
		if (r < 0) {
			WLOG_ERROR("screencast: ScreenCast.CreateSession(linked) failed: %s", sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			return false;
		}
		const char *path = nullptr;
		r = sd_bus_message_read(reply, "o", &path);
		if (r >= 0 && path) {
			sc_path = path;
		}
		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);
		return r >= 0 && !sc_path.empty();
	}

	// ScreenCast.Session.RecordVirtual({'cursor-mode': uint32(2)}) --
	// cursor mode 2 = metadata: embed SPA_META_Cursor in each frame rather
	// than painting the cursor into the pixels.
	bool record_virtual() {
		sd_bus_message *m = nullptr;
		int r = sd_bus_message_new_method_call(bus, &m, kScService, sc_path.c_str(), kScSessionIface,
			"RecordVirtual");
		if (r < 0) {
			return false;
		}
		r = sdbus_append_one_option(m, "cursor-mode", "u",
			[](sd_bus_message *msg) { return sd_bus_message_append(msg, "u", (uint32_t)2); });
		if (r < 0) {
			sd_bus_message_unref(m);
			return false;
		}

		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		r = sd_bus_call(bus, m, 0, &error, &reply);
		sd_bus_message_unref(m);
		if (r < 0) {
			WLOG_ERROR("screencast: ScreenCast.Session.RecordVirtual failed: %s", sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			return false;
		}
		const char *path = nullptr;
		r = sd_bus_message_read(reply, "o", &path);
		if (r >= 0 && path) {
			stream_path = path;
		}
		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);
		return r >= 0 && !stream_path.empty();
	}

	bool start_session() {
		sd_bus_error error = SD_BUS_ERROR_NULL;
		int r = sd_bus_call_method(bus, kRdService, rd_path.c_str(), kRdSessionIface, "Start", &error,
			nullptr, "");
		if (r < 0) {
			WLOG_ERROR("screencast: RemoteDesktop.Session.Start failed: %s", sdbus_error_text(error, r));
		}
		sd_bus_error_free(&error);
		return r >= 0;
	}

	// Blocks (processing the bus) until node_id_ready or the deadline.
	// The match has to be installed before Start() (see open()); a
	// signal that arrives between Start() and this wait is still queued
	// by sd-bus and dispatched here.
	bool wait_for_node_id() {
		int64_t deadline = monotonic_now_us() + kNodeIdWaitUs;
		while (!node_id_ready) {
			int r = sd_bus_process(bus, nullptr);
			if (r < 0) {
				return false;
			}
			if (r > 0) {
				continue;
			}
			int64_t remaining = deadline - monotonic_now_us();
			if (remaining <= 0) {
				return false;
			}
			sd_bus_wait(bus, (uint64_t)remaining);
		}
		return true;
	}

	static void bus_readable(void *data) {
		auto *impl = static_cast<Impl *>(data);
		while (sd_bus_process(impl->bus, nullptr) > 0) {}
	}
};

GnomeRemoteSession::GnomeRemoteSession() : impl_(std::make_unique<Impl>()) {}
GnomeRemoteSession::~GnomeRemoteSession() {
	close();
}

bool GnomeRemoteSession::open(const Config &config) {
	impl_->config = config;
	impl_->on_closed_cb = on_closed;

	if (sd_bus_open_user(&impl_->bus) < 0) {
		WLOG_ERROR("screencast: sd_bus_open_user failed");
		return false;
	}

	if (!impl_->create_remote_desktop_session()) {
		return false;
	}
	std::string session_id;
	if (!impl_->get_session_id(&session_id)) {
		return false;
	}
	if (!impl_->create_linked_screencast_session(session_id)) {
		return false;
	}
	if (!impl_->record_virtual()) {
		return false;
	}

	// Subscribed before Start() -- Start() starts the linked stream
	// synchronously and a missed signal is not redelivered
	// (docs/design/capture-backends.md#gnome).
	sd_bus_match_signal(impl_->bus, &impl_->node_added_slot, kScService, impl_->stream_path.c_str(),
		kScStreamIface, "PipeWireStreamAdded", Impl::on_pipewire_stream_added, impl_.get());
	sd_bus_match_signal(impl_->bus, &impl_->closed_slot, kRdService, impl_->rd_path.c_str(), kRdSessionIface,
		"Closed", Impl::on_session_closed, impl_.get());

	if (!impl_->start_session()) {
		return false;
	}
	if (!impl_->wait_for_node_id()) {
		WLOG_ERROR("screencast: PipeWireStreamAdded never arrived");
		return false;
	}

	impl_->bus_source = wl_event_loop_add_fd(
		config.event_loop, sd_bus_get_fd(impl_->bus), WL_EVENT_READABLE,
		[](int, uint32_t, void *data) {
			Impl::bus_readable(data);
			return 0;
		},
		impl_.get());
	return true;
}

void GnomeRemoteSession::close() {
	if (impl_->bus_source) {
		wl_event_source_remove(impl_->bus_source);
		impl_->bus_source = nullptr;
	}
	if (impl_->bus && !impl_->rd_path.empty()) {
		sd_bus_call_method(impl_->bus, kRdService, impl_->rd_path.c_str(), kRdSessionIface, "Stop", nullptr,
			nullptr, "");
	}
	if (impl_->node_added_slot) {
		sd_bus_slot_unref(impl_->node_added_slot);
		impl_->node_added_slot = nullptr;
	}
	if (impl_->closed_slot) {
		sd_bus_slot_unref(impl_->closed_slot);
		impl_->closed_slot = nullptr;
	}
	if (impl_->bus) {
		sd_bus_unref(impl_->bus);
		impl_->bus = nullptr;
	}
	impl_->rd_path.clear();
	impl_->sc_path.clear();
	impl_->stream_path.clear();
	impl_->node_id_ready = false;
	impl_->eis_connected = false;
}

uint32_t GnomeRemoteSession::pipewire_node_id() const {
	return impl_->node_id;
}

bool GnomeRemoteSession::connect_eis(int *out_fd) {
	sd_bus_message *m = nullptr;
	int r = sd_bus_message_new_method_call(impl_->bus, &m, kRdService, impl_->rd_path.c_str(),
		kRdSessionIface, "ConnectToEIS");
	if (r < 0) {
		return false;
	}
	r = sdbus_append_no_options(m);
	if (r < 0) {
		sd_bus_message_unref(m);
		return false;
	}

	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = nullptr;
	r = sd_bus_call(impl_->bus, m, 0, &error, &reply);
	sd_bus_message_unref(m);
	if (r < 0) {
		WLOG_ERROR("screencast: RemoteDesktop.Session.ConnectToEIS failed: %s", sdbus_error_text(error, r));
		sd_bus_error_free(&error);
		return false;
	}
	sd_bus_error_free(&error);

	int fd = -1;
	r = sd_bus_message_read(reply, "h", &fd);
	// ei_setup_backend_fd takes ownership of whatever it is handed, so it
	// gets its own copy of the message's borrowed fd.
	int dup_fd = r >= 0 ? sdbus_dup_fd(fd) : -1;
	sd_bus_message_unref(reply);
	if (dup_fd < 0) {
		WLOG_ERROR("screencast: ConnectToEIS reply had no usable fd");
		return false;
	}
	*out_fd = dup_fd;
	impl_->eis_connected = true;
	return true;
}

std::unique_ptr<FrameSource> GnomeRemoteSession::make_frame_source() {
	return std::make_unique<PipeWireCapture>(pipewire_node_id());
}

std::unique_ptr<ClipboardSink> GnomeRemoteSession::make_clipboard_sink() {
	return GnomeClipboard::create(impl_->bus, impl_->rd_path, impl_->config.event_loop);
}

std::unique_ptr<ScreencastInput> GnomeRemoteSession::make_input_sink() {
	// ConnectToEIS is cheap to retry (it works immediately once the linked
	// session is up, docs/design/capture-backends.md), so a null here just means
	// "ask again next tick".
	int eis_fd = -1;
	if (!connect_eis(&eis_fd)) {
		return nullptr;
	}
	auto input = std::make_unique<EiInput>();
	EiInput::Config config;
	config.event_loop = impl_->config.event_loop;
	config.output_width = impl_->config.width;
	config.output_height = impl_->config.height;
	config.eis_fd = eis_fd;
	if (!input->open(config)) {
		return nullptr;
	}
	return input;
}

} // namespace wraith
