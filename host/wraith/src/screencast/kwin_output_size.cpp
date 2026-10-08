// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "screencast/kwin_output_size.hpp"

#include "screencast/wayland_client.hpp"

#include <kde-output-device-v2-client-protocol.h>
#include <kde-output-management-v2-client-protocol.h>
#include <wayland-client-core.h>

#include <memory>
#include <unistd.h>
#include <vector>

namespace wraith {

namespace {

// set_custom_modes arrived in v18. kwin 6.6 advertises 19.
constexpr uint32_t kManagementVersion = 18;
// kwin 6.7 no longer advertises kde_output_device_v2 as a global per output;
// the outputs come from kde_output_device_registry_v2 (which needs v21).
constexpr uint32_t kRegistryVersion = 21;
// kwin answers apply with applied/failed once the new configuration is
// live; polled in short round trips for up to this long.
constexpr int kApplyWaitTicks = 100;
constexpr useconds_t kApplyWaitTickUs = 10'000;
// Used only if kwin reports no refresh for the current mode.
constexpr uint32_t kDefaultRefreshMilliHz = 60'000;

// Event opcodes, in protocol XML order. The objects below are dispatched
// through wl_proxy_add_dispatcher instead of listeners: a listener has to
// fill in every event of the bound version (kde_output_device_v2 has
// fourteen at v1 alone), where only these few matter.
enum : uint32_t { kDeviceCurrentMode = 1, kDeviceMode = 2, kDeviceDone = 3 };
enum : uint32_t { kRegistryOutput = 1 };
enum : uint32_t { kModeSize = 0, kModeRefresh = 1, kModeRemoved = 3 };
enum : uint32_t { kConfigApplied = 0, kConfigFailed = 1, kConfigFailureReason = 2 };

struct Mode {
	struct kde_output_device_mode_v2 *proxy = nullptr;
	int32_t width = 0, height = 0, refresh = 0;
	bool removed = false;
};

struct Sizer {
	WaylandClient &client;
	struct kde_output_device_registry_v2 *registry = nullptr;
	struct kde_output_device_v2 *device = nullptr;
	int device_version = 1;
	struct kde_output_management_v2 *management = nullptr;
	std::vector<std::unique_ptr<Mode>> modes;
	Mode *current = nullptr;
	bool device_done = false;
	enum class Apply { Pending, Applied, Failed } apply_state = Apply::Pending;
	std::string failure_reason;

	explicit Sizer(WaylandClient &c) : client(c) {}
	~Sizer() {
		for (auto &m : modes) {
			// The mode's own release request only exists from v26.
			wl_proxy_destroy(reinterpret_cast<struct wl_proxy *>(m->proxy));
		}
		if (device) {
			if (device_version >= 21) {
				kde_output_device_v2_release(device);
			} else {
				wl_proxy_destroy(reinterpret_cast<struct wl_proxy *>(device));
			}
		}
		if (registry) {
			kde_output_device_registry_v2_stop(registry);
			wl_proxy_destroy(reinterpret_cast<struct wl_proxy *>(registry));
		}
		if (management) {
			kde_output_management_v2_destroy(management);
		}
		client.flush();
	}

	static int on_mode_event(const void *, void *target, uint32_t opcode, const struct wl_message *,
		union wl_argument *args) {
		auto *mode = static_cast<Mode *>(wl_proxy_get_user_data(static_cast<struct wl_proxy *>(target)));
		switch (opcode) {
		case kModeSize:
			mode->width = args[0].i;
			mode->height = args[1].i;
			break;
		case kModeRefresh: mode->refresh = args[0].i; break;
		case kModeRemoved: mode->removed = true; break;
		}
		return 0;
	}

	static int on_registry_event(const void *, void *target, uint32_t opcode, const struct wl_message *,
		union wl_argument *args) {
		auto *self = static_cast<Sizer *>(wl_proxy_get_user_data(static_cast<struct wl_proxy *>(target)));
		if (opcode != kRegistryOutput) {
			return 0;
		}
		auto *proxy = reinterpret_cast<struct wl_proxy *>(args[0].o);
		if (self->device) {
			// A virtual kwin has the one output; any other is not ours to size.
			kde_output_device_v2_release(reinterpret_cast<struct kde_output_device_v2 *>(proxy));
			return 0;
		}
		self->device = reinterpret_cast<struct kde_output_device_v2 *>(proxy);
		self->device_version = (int)wl_proxy_get_version(proxy);
		wl_proxy_add_dispatcher(proxy, on_device_event, nullptr, self);
		return 0;
	}

	static int on_device_event(const void *, void *target, uint32_t opcode, const struct wl_message *,
		union wl_argument *args) {
		auto *self = static_cast<Sizer *>(wl_proxy_get_user_data(static_cast<struct wl_proxy *>(target)));
		switch (opcode) {
		case kDeviceMode: {
			auto mode = std::make_unique<Mode>();
			mode->proxy = reinterpret_cast<struct kde_output_device_mode_v2 *>(args[0].o);
			wl_proxy_add_dispatcher(reinterpret_cast<struct wl_proxy *>(mode->proxy), on_mode_event, nullptr,
				mode.get());
			self->modes.push_back(std::move(mode));
			break;
		}
		case kDeviceCurrentMode:
			self->current = nullptr;
			for (auto &m : self->modes) {
				if (reinterpret_cast<struct wl_object *>(m->proxy) == args[0].o) {
					self->current = m.get();
				}
			}
			break;
		case kDeviceDone: self->device_done = true; break;
		}
		return 0;
	}

	static int on_config_event(const void *, void *target, uint32_t opcode, const struct wl_message *,
		union wl_argument *args) {
		auto *self = static_cast<Sizer *>(wl_proxy_get_user_data(static_cast<struct wl_proxy *>(target)));
		switch (opcode) {
		case kConfigApplied: self->apply_state = Apply::Applied; break;
		case kConfigFailed: self->apply_state = Apply::Failed; break;
		case kConfigFailureReason: self->failure_reason = args[0].s ? args[0].s : ""; break;
		}
		return 0;
	}

	Mode *find(uint32_t width, uint32_t height) const {
		for (const auto &m : modes) {
			if (!m->removed && (uint32_t)m->width == width && (uint32_t)m->height == height) {
				return m.get();
			}
		}
		return nullptr;
	}

	bool current_is(uint32_t width, uint32_t height) const {
		return current && !current->removed && (uint32_t)current->width == width &&
			(uint32_t)current->height == height;
	}

	// Applies and destroys `cfg`; the device's own update (new modes, the
	// new current mode, done) arrives before kwin's applied event.
	bool apply(struct kde_output_configuration_v2 *cfg, const std::string &what, std::string *error) {
		apply_state = Apply::Pending;
		failure_reason.clear();
		wl_proxy_add_dispatcher(reinterpret_cast<struct wl_proxy *>(cfg), on_config_event, nullptr, this);
		kde_output_configuration_v2_apply(cfg);
		for (int i = 0; i < kApplyWaitTicks && apply_state == Apply::Pending; i++) {
			if (!client.roundtrip()) {
				break;
			}
			if (apply_state == Apply::Pending) {
				usleep(kApplyWaitTickUs);
			}
		}
		kde_output_configuration_v2_destroy(cfg);
		if (apply_state == Apply::Applied) {
			return true;
		}
		*error = "kwin " + std::string(apply_state == Apply::Failed ? "refused " : "never answered ") + what +
			(failure_reason.empty() ? "" : ": " + failure_reason);
		return false;
	}
};

std::string size_text(uint32_t width, uint32_t height) {
	return std::to_string(width) + "x" + std::to_string(height);
}

} // namespace

bool set_kwin_output_size(WaylandClient &client, uint32_t width, uint32_t height, std::string *error) {
	if (!client.has_global(kde_output_management_v2_interface.name)) {
		*error = "kwin does not advertise kde_output_management_v2 to wraith -- check the desktop-file trust "
				 "rule (packaging/desktop/wraith.desktop.in: X-KDE-Wayland-Interfaces= must list it)";
		return false;
	}
	Sizer s(client);
	if (client.has_global(kde_output_device_registry_v2_interface.name)) {
		s.registry = static_cast<struct kde_output_device_registry_v2 *>(
			client.bind(&kde_output_device_registry_v2_interface, kRegistryVersion));
		if (!s.registry) {
			*error = "kwin advertised kde_output_device_registry_v2 but it would not bind";
			return false;
		}
		wl_proxy_add_dispatcher(reinterpret_cast<struct wl_proxy *>(s.registry), Sizer::on_registry_event,
			nullptr, &s);
		if (!client.roundtrip() || !s.device) {
			*error = "kwin's kde_output_device_registry_v2 listed no output";
			return false;
		}
	} else {
		// kwin 6.6: a global per output. Only v1's current_mode/mode/done are
		// read; binding at v1 keeps kwin from sending anything newer.
		s.device =
			static_cast<struct kde_output_device_v2 *>(client.bind(&kde_output_device_v2_interface, 1));
		if (!s.device) {
			*error = "kwin advertised neither kde_output_device_registry_v2 nor kde_output_device_v2";
			return false;
		}
		wl_proxy_add_dispatcher(reinterpret_cast<struct wl_proxy *>(s.device), Sizer::on_device_event,
			nullptr, &s);
	}
	if (!client.roundtrip() || !s.device_done) {
		*error = "kwin sent no state for its output device";
		return false;
	}
	if (s.current_is(width, height)) {
		return true;
	}

	s.management = static_cast<struct kde_output_management_v2 *>(
		client.bind(&kde_output_management_v2_interface, kManagementVersion));
	uint32_t version =
		s.management ? wl_proxy_get_version(reinterpret_cast<struct wl_proxy *>(s.management)) : 0;
	if (version < kManagementVersion) {
		*error = "kwin's kde_output_management_v2 is version " + std::to_string(version) +
			", custom modes need " + std::to_string(kManagementVersion);
		return false;
	}

	Mode *target = s.find(width, height);
	if (!target) {
		// set_custom_modes replaces the output's whole custom list, so the
		// size from an earlier resize doesn't pile up beside this one.
		uint32_t refresh =
			s.current && s.current->refresh > 0 ? (uint32_t)s.current->refresh : kDefaultRefreshMilliHz;
		struct kde_mode_list_v2 *list = kde_output_management_v2_create_mode_list(s.management);
		kde_mode_list_v2_set_resolution(list, width, height);
		kde_mode_list_v2_set_refresh_rate(list, refresh);
		kde_mode_list_v2_set_reduced_blanking(list, 0);
		kde_mode_list_v2_add_mode(list);
		struct kde_output_configuration_v2 *cfg = kde_output_management_v2_create_configuration(s.management);
		kde_output_configuration_v2_set_custom_modes(cfg, s.device, list);
		bool added = s.apply(cfg, "a " + size_text(width, height) + " custom mode", error);
		kde_mode_list_v2_destroy(list);
		if (!added || !client.roundtrip()) {
			return false;
		}
		target = s.find(width, height);
		if (!target) {
			*error = "kwin accepted a " + size_text(width, height) + " custom mode but never listed it";
			return false;
		}
	}

	if (!s.current_is(width, height)) {
		struct kde_output_configuration_v2 *cfg = kde_output_management_v2_create_configuration(s.management);
		kde_output_configuration_v2_mode(cfg, s.device, target->proxy);
		if (!s.apply(cfg, "switching to " + size_text(width, height), error) || !client.roundtrip()) {
			return false;
		}
	}
	if (!s.current_is(width, height)) {
		*error = "kwin applied a " + size_text(width, height) + " mode but its output is still " +
			(s.current ? size_text((uint32_t)s.current->width, (uint32_t)s.current->height) : "unknown");
		return false;
	}
	return true;
}

} // namespace wraith
