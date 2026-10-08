// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// WaylandClient: wraith as a plain Wayland *client* of the compositor it
// captures (docs/design/capture-backends.md) -- the connection,
// its fd on the host's wl_event_loop, and the registry, shared by
// KwinRemoteSession (zkde_screencast_unstable_v1) and ExtRemoteSession
// (ext_image_copy_capture_v1). Nothing protocol-specific lives here; a
// user binds whatever globals it needs through bind() with the generated
// client interface tables.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct wl_display;
struct wl_interface;
struct wl_event_loop;

namespace wraith {

class WaylandClient {
public:
	WaylandClient();
	~WaylandClient();
	WaylandClient(const WaylandClient &) = delete;
	WaylandClient &operator=(const WaylandClient &) = delete;

	// Connects to `display_name` (null: libwayland's own default, i.e.
	// $WAYLAND_DISPLAY or wayland-0), adds the display fd to `loop`, and
	// does one round-trip so the compositor's initial global burst is
	// known before this returns. False if the socket isn't there (yet) --
	// cheap to retry -- or the connection fails; the object is left
	// disconnected either way.
	bool connect(struct wl_event_loop *loop, const char *display_name);
	void disconnect();
	bool connected() const;
	struct wl_display *display() const;

	// Whether the compositor advertised a global with this interface name.
	bool has_global(const char *interface) const;
	// Binds the first advertised global of `iface` at min(advertised,
	// max_version). Null if not advertised. The returned proxy is the
	// caller's to destroy (before disconnect()). Its removal by the
	// compositor later fires on_disconnected: a bound global going away
	// means the thing being captured is gone.
	void *bind(const struct wl_interface *iface, uint32_t max_version);

	// Blocking round trip (requests flushed, all replies dispatched).
	// False if the connection died.
	bool roundtrip();
	// Push queued requests out now. Needed after a burst of requests made
	// outside a round trip, since the fd source only reads.
	void flush();

	// Fires at most once: dispatch failed (the compositor went away), or
	// a global bound through bind() was removed.
	std::function<void(const std::string &reason)> on_disconnected;

	// The systemd --user manager's WAYLAND_DISPLAY, for a compositor that
	// was started as a user unit and imported its socket name there (kwin
	// under startplasma-wayland does). wraith's own environ is no use: a
	// screencast host runs with WAYLAND_DISPLAY deliberately unset. Empty
	// if the manager has none.
	static std::string display_from_user_manager();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
