// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/kwin_remote_session.hpp"

#include "screencast/data_control_clipboard.hpp"
#include "screencast/ei_input.hpp"
#include "screencast/kwin_output_size.hpp"
#include "screencast/pipewire_capture.hpp"
#include "screencast/sdbus_util.hpp"
#include "screencast/wayland_client.hpp"

#include <systemd/sd-bus.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>
#include <zkde-screencast-unstable-v1-client-protocol.h>

#include "util/log.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace wraith {

namespace {
constexpr int kKeyboardCap = 1, kPointerCap = 2; // org.kde.KWin.EIS.RemoteDesktop's own bit values
constexpr uint32_t kScreencastMaxVersion = 6;
// created(node) follows stream_output once kwin's own PipeWire stream is
// up -- its core connect is asynchronous, so a single round-trip can come
// back before it. Polled in short round-trips for up to this long.
constexpr int kStreamWaitTicks = 100;
constexpr useconds_t kStreamWaitTickUs = 10'000;
} // namespace

struct KwinRemoteSession::Impl {
	Config config;
	WaylandClient client;
	struct wl_output *output = nullptr;
	struct zkde_screencast_unstable_v1 *manager = nullptr;
	struct zkde_screencast_stream_unstable_v1 *stream = nullptr;
	enum class StreamState { Pending, Created, Failed, Closed } stream_state = StreamState::Pending;
	uint32_t node_id = 0;
	std::string stream_error;
	sd_bus *bus = nullptr;
	int eis_cookie = -1;
	bool opened = false;
	bool closed_reported = false;
	bool trust_warned = false;
	bool size_warned = false;
	std::function<void()> *on_closed = nullptr;

	// --- zkde_screencast_stream_unstable_v1 ---

	static void on_stream_closed(void *data, struct zkde_screencast_stream_unstable_v1 *) {
		auto *impl = static_cast<Impl *>(data);
		impl->stream_state = StreamState::Closed;
		impl->report_closed("kwin closed the screencast stream");
	}

	static void on_stream_created(void *data, struct zkde_screencast_stream_unstable_v1 *, uint32_t node) {
		auto *impl = static_cast<Impl *>(data);
		impl->node_id = node;
		impl->stream_state = StreamState::Created;
	}

	static void on_stream_failed(void *data, struct zkde_screencast_stream_unstable_v1 *, const char *error) {
		auto *impl = static_cast<Impl *>(data);
		impl->stream_error = error ? error : "stream_output failed";
		impl->stream_state = StreamState::Failed;
		impl->report_closed("kwin: " + impl->stream_error);
	}

	static void on_stream_serial(void *, struct zkde_screencast_stream_unstable_v1 *, uint32_t, uint32_t) {
		// v6 sends this before `created`; the node id from `created` is
		// still what pw_stream_connect takes, so nothing to act on here.
	}

	void report_closed(const std::string &reason) {
		// Only a session that has been handed out counts as "closed out
		// from under us"; failures during open() are its return value.
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

	void teardown_wayland() {
		if (stream) {
			zkde_screencast_stream_unstable_v1_close(stream);
			stream = nullptr;
		}
		if (manager) {
			zkde_screencast_unstable_v1_destroy(manager);
			manager = nullptr;
		}
		if (output) {
			wl_output_destroy(output);
			output = nullptr;
		}
		client.flush();
		client.disconnect();
		stream_state = StreamState::Pending;
		node_id = 0;
	}

	bool connect_wayland() {
		std::string name = WaylandClient::display_from_user_manager();
		if (!name.empty() && client.connect(config.event_loop, name.c_str())) {
			return true;
		}
		// kwin_wayland_wrapper always creates wayland-0 in XDG_RUNTIME_DIR
		// whatever the manager environment says.
		return client.connect(config.event_loop, "wayland-0");
	}

	// --- EIS over D-Bus ---

	bool connect_to_eis(int *out_fd) {
		if (!bus && sd_bus_open_user(&bus) < 0) {
			WLOG_ERROR("screencast: sd_bus_open_user failed");
			return false;
		}
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = nullptr;
		int r = sd_bus_call_method(bus, "org.kde.KWin", "/org/kde/KWin/EIS/RemoteDesktop",
			"org.kde.KWin.EIS.RemoteDesktop", "connectToEIS", &error, &reply, "i",
			kKeyboardCap | kPointerCap);
		if (r < 0) {
			WLOG_ERROR("screencast: connectToEIS failed: %s", sdbus_error_text(error, r));
			sd_bus_error_free(&error);
			return false;
		}
		sd_bus_error_free(&error);
		int fd = -1, cookie = -1;
		r = sd_bus_message_read(reply, "hi", &fd, &cookie);
		// EiInput takes ownership of what it is handed, so it gets its own
		// copy of the message's borrowed fd.
		int dup_fd = r >= 0 ? sdbus_dup_fd(fd) : -1;
		sd_bus_message_unref(reply);
		if (dup_fd < 0) {
			WLOG_ERROR("screencast: connectToEIS reply had no usable fd");
			return false;
		}
		*out_fd = dup_fd;
		eis_cookie = cookie;
		return true;
	}

	void disconnect_eis() {
		if (!bus || eis_cookie < 0) {
			return;
		}
		sd_bus_error error = SD_BUS_ERROR_NULL;
		// Best effort: kwin also drops the context when this bus
		// connection goes away.
		sd_bus_call_method(bus, "org.kde.KWin", "/org/kde/KWin/EIS/RemoteDesktop",
			"org.kde.KWin.EIS.RemoteDesktop", "disconnect", &error, nullptr, "i", eis_cookie);
		sd_bus_error_free(&error);
		eis_cookie = -1;
	}
};

KwinRemoteSession::KwinRemoteSession() : impl_(std::make_unique<Impl>()) {
	impl_->on_closed = &on_closed;
}

KwinRemoteSession::~KwinRemoteSession() {
	close();
}

bool KwinRemoteSession::open(const Config &config) {
	impl_->config = config;
	impl_->closed_reported = false;

	if (!impl_->connect_wayland()) {
		return false; // kwin's socket isn't there yet
	}
	impl_->client.on_disconnected = [this](const std::string &reason) { impl_->report_closed(reason); };

	if (!impl_->client.has_global(zkde_screencast_unstable_v1_interface.name)) {
		if (!impl_->trust_warned) {
			impl_->trust_warned = true;
			WLOG_ERROR(
				"screencast: kwin is up but does not advertise zkde_screencast_unstable_v1 to wraith -- "
				"check the desktop-file trust rule (packaging/desktop/wraith.desktop.in: Exec= must resolve to this "
				"binary and X-KDE-Wayland-Interfaces= must list it), or KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 for dev");
		}
		impl_->teardown_wayland();
		return false;
	}
	// Before stream_output: the stream is negotiated at the output's size
	// at that moment, and PipeWireCapture offers exactly config's.
	std::string size_error;
	if (!set_kwin_output_size(impl_->client, config.width, config.height, &size_error)) {
		// Retried every tick like everything else here; said once at ERROR.
		if (!impl_->size_warned) {
			impl_->size_warned = true;
			WLOG_ERROR("screencast: can't size kwin's output to %ux%u: %s", config.width, config.height,
				size_error.c_str());
		} else {
			WLOG_DEBUG("screencast: can't size kwin's output to %ux%u: %s", config.width, config.height,
				size_error.c_str());
		}
		impl_->teardown_wayland();
		return false;
	}
	WLOG_INFO("screencast: kwin's output is %ux%u", config.width, config.height);

	impl_->output = static_cast<struct wl_output *>(impl_->client.bind(&wl_output_interface, 1));
	impl_->manager = static_cast<struct zkde_screencast_unstable_v1 *>(
		impl_->client.bind(&zkde_screencast_unstable_v1_interface, kScreencastMaxVersion));
	if (!impl_->output || !impl_->manager) {
		WLOG_ERROR("screencast: kwin advertised no wl_output yet");
		impl_->teardown_wayland();
		return false;
	}

	impl_->stream_state = Impl::StreamState::Pending;
	impl_->stream = zkde_screencast_unstable_v1_stream_output(impl_->manager, impl_->output,
		ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_METADATA);
	static const struct zkde_screencast_stream_unstable_v1_listener stream_listener = {
		.closed = Impl::on_stream_closed,
		.created = Impl::on_stream_created,
		.failed = Impl::on_stream_failed,
		.serial = Impl::on_stream_serial,
	};
	zkde_screencast_stream_unstable_v1_add_listener(impl_->stream, &stream_listener, impl_.get());

	for (int i = 0; i < kStreamWaitTicks && impl_->stream_state == Impl::StreamState::Pending; i++) {
		if (!impl_->client.roundtrip()) {
			break;
		}
		if (impl_->stream_state == Impl::StreamState::Pending) {
			usleep(kStreamWaitTickUs);
		}
	}
	if (impl_->stream_state != Impl::StreamState::Created) {
		// A timeout right after kwin comes up is normal (its PipeWire core
		// isn't connected yet) -- the host retries on its next tick.
		if (impl_->stream_state == Impl::StreamState::Failed) {
			WLOG_ERROR("screencast: kwin refused stream_output: %s", impl_->stream_error.c_str());
		} else {
			WLOG_INFO("screencast: kwin's stream_output produced no PipeWire node yet, retrying");
		}
		impl_->teardown_wayland();
		return false;
	}
	WLOG_INFO("screencast: kwin screencast stream up (PipeWire node %u)", impl_->node_id);
	impl_->opened = true;
	return true;
}

void KwinRemoteSession::close() {
	impl_->disconnect_eis();
	impl_->teardown_wayland();
	if (impl_->bus) {
		sd_bus_unref(impl_->bus);
		impl_->bus = nullptr;
	}
	impl_->opened = false;
}

std::unique_ptr<FrameSource> KwinRemoteSession::make_frame_source() {
	return std::make_unique<PipeWireCapture>(impl_->node_id);
}

std::unique_ptr<ClipboardSink> KwinRemoteSession::make_clipboard_sink() {
	return DataControlClipboard::create(impl_->client, impl_->config.event_loop);
}

std::unique_ptr<ScreencastInput> KwinRemoteSession::make_input_sink() {
	int eis_fd = -1;
	if (!impl_->connect_to_eis(&eis_fd)) {
		return nullptr;
	}
	auto input = std::make_unique<EiInput>();
	EiInput::Config config;
	config.event_loop = impl_->config.event_loop;
	config.output_width = impl_->config.width;
	config.output_height = impl_->config.height;
	config.eis_fd = eis_fd;
	if (!input->open(config)) {
		impl_->disconnect_eis();
		return nullptr;
	}
	WLOG_INFO("screencast: kwin EIS input connected (cookie %d)", impl_->eis_cookie);
	return input;
}

} // namespace wraith
