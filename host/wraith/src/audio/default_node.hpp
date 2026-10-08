// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

#include <chrono>
#include <string>

namespace wraith {

// Makes a node the session's default sink or source by writing `key`
// ("default.configured.audio.sink" or "...source") into PipeWire's
// "default" metadata -- the user's choice, what `wpctl set-default`
// writes, which WirePlumber copies to the effective default. Left to
// itself WirePlumber prefers a hardware device the session can see over
// wraith's virtual nodes, and its pick of default source is the sink's
// monitor.
//
// WirePlumber often starts with the session, and restores its saved
// defaults just after wraith's write. So for kReassertWindow after the
// first write, the claim writes again whenever someone else's value
// lands; after that, a user's or app's choice stands.
//
// Both calls with the core's thread loop locked; the object must stay put
// in between (the registry listener points at it).
class DefaultNodeClaim {
public:
	DefaultNodeClaim() = default;
	DefaultNodeClaim(const DefaultNodeClaim &) = delete;
	DefaultNodeClaim &operator=(const DefaultNodeClaim &) = delete;

	void open(struct pw_core *core, const char *key, const std::string &node_name);
	void close();

private:
	static constexpr std::chrono::seconds kReassertWindow{5};

	static void on_registry_global(void *data, uint32_t id, uint32_t permissions, const char *type,
		uint32_t version, const struct spa_dict *props);
	static int on_metadata_property(void *data, uint32_t subject, const char *key, const char *type,
		const char *value);
	void write();

	const char *key_ = nullptr;
	std::string node_name_;
	struct pw_registry *registry_ = nullptr;
	struct spa_hook registry_listener_{};
	struct pw_metadata *metadata_ = nullptr;
	struct spa_hook metadata_listener_{};
	std::chrono::steady_clock::time_point claimed_at_{};
};

} // namespace wraith
