// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/wayland_client.hpp"

#include <systemd/sd-bus.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-server-core.h>

#include "util/log.hpp"

#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

namespace wraith {

struct WaylandClient::Impl {
	struct Global {
		uint32_t name;
		std::string interface;
		uint32_t version;
	};

	struct wl_display *display = nullptr;
	struct wl_registry *registry = nullptr;
	struct wl_event_source *source = nullptr;
	std::vector<Global> globals;
	std::set<uint32_t> bound_names;
	bool disconnected_reported = false;
	std::function<void(const std::string &)> *on_disconnected = nullptr;

	static void on_global(void *data, struct wl_registry *, uint32_t name, const char *interface,
		uint32_t version) {
		auto *impl = static_cast<Impl *>(data);
		impl->globals.push_back({name, interface, version});
	}

	static void on_global_remove(void *data, struct wl_registry *, uint32_t name) {
		auto *impl = static_cast<Impl *>(data);
		bool was_bound = impl->bound_names.count(name) != 0;
		std::string iface;
		for (auto it = impl->globals.begin(); it != impl->globals.end(); ++it) {
			if (it->name == name) {
				iface = it->interface;
				impl->globals.erase(it);
				break;
			}
		}
		if (was_bound) {
			impl->report_disconnected("compositor removed the " + iface + " global");
		}
	}

	void report_disconnected(const std::string &reason) {
		if (disconnected_reported) {
			return;
		}
		disconnected_reported = true;
		if (on_disconnected && *on_disconnected) {
			auto cb = *on_disconnected;
			cb(reason);
		}
	}
};

WaylandClient::WaylandClient() : impl_(std::make_unique<Impl>()) {
	impl_->on_disconnected = &on_disconnected;
}

WaylandClient::~WaylandClient() {
	disconnect();
}

bool WaylandClient::connect(struct wl_event_loop *loop, const char *display_name) {
	disconnect();
	impl_->disconnected_reported = false;
	impl_->display = wl_display_connect(display_name);
	if (!impl_->display) {
		return false;
	}
	impl_->registry = wl_display_get_registry(impl_->display);
	static const struct wl_registry_listener registry_listener = {
		.global = Impl::on_global,
		.global_remove = Impl::on_global_remove,
	};
	wl_registry_add_listener(impl_->registry, &registry_listener, impl_.get());
	// One round-trip is enough for a compositor's initial global burst.
	if (wl_display_roundtrip(impl_->display) < 0) {
		WLOG_ERROR("screencast: initial Wayland round-trip to %s failed",
			display_name ? display_name : "(default)");
		disconnect();
		return false;
	}
	impl_->source = wl_event_loop_add_fd(
		loop, wl_display_get_fd(impl_->display), WL_EVENT_READABLE,
		[](int, uint32_t, void *data) {
			auto *impl = static_cast<Impl *>(data);
			if (wl_display_dispatch(impl->display) < 0) {
				impl->report_disconnected("lost the compositor's Wayland connection");
				return 0;
			}
			wl_display_flush(impl->display);
			return 0;
		},
		impl_.get());
	return true;
}

void WaylandClient::disconnect() {
	if (impl_->source) {
		wl_event_source_remove(impl_->source);
		impl_->source = nullptr;
	}
	if (impl_->registry) {
		wl_registry_destroy(impl_->registry);
		impl_->registry = nullptr;
	}
	if (impl_->display) {
		wl_display_disconnect(impl_->display);
		impl_->display = nullptr;
	}
	impl_->globals.clear();
	impl_->bound_names.clear();
}

bool WaylandClient::connected() const {
	return impl_->display != nullptr;
}

struct wl_display *WaylandClient::display() const {
	return impl_->display;
}

bool WaylandClient::has_global(const char *interface) const {
	for (const auto &g : impl_->globals) {
		if (g.interface == interface) {
			return true;
		}
	}
	return false;
}

void *WaylandClient::bind(const struct wl_interface *iface, uint32_t max_version) {
	if (!impl_->registry) {
		return nullptr;
	}
	for (const auto &g : impl_->globals) {
		if (g.interface == iface->name) {
			uint32_t version = g.version < max_version ? g.version : max_version;
			void *proxy = wl_registry_bind(impl_->registry, g.name, iface, version);
			if (proxy) {
				impl_->bound_names.insert(g.name);
			}
			return proxy;
		}
	}
	return nullptr;
}

bool WaylandClient::roundtrip() {
	if (!impl_->display) {
		return false;
	}
	if (wl_display_roundtrip(impl_->display) < 0) {
		impl_->report_disconnected("lost the compositor's Wayland connection");
		return false;
	}
	return true;
}

void WaylandClient::flush() {
	if (impl_->display) {
		wl_display_flush(impl_->display);
	}
}

std::string WaylandClient::display_from_user_manager() {
	sd_bus *bus = nullptr;
	if (sd_bus_open_user(&bus) < 0) {
		return {};
	}
	sd_bus_error error = SD_BUS_ERROR_NULL;
	char **envp = nullptr;
	int r = sd_bus_get_property_strv(bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
		"org.freedesktop.systemd1.Manager", "Environment", &error, &envp);
	sd_bus_error_free(&error);
	sd_bus_unref(bus);

	std::string result;
	if (r >= 0 && envp) {
		for (char **e = envp; *e; e++) {
			if (std::strncmp(*e, "WAYLAND_DISPLAY=", 16) == 0) {
				result = *e + 16;
			}
			free(*e);
		}
		free(envp);
	}
	return result;
}

} // namespace wraith
