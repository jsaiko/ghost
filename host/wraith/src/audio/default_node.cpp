// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/default_node.hpp"

#include <cstring>

#include <spa/utils/json.h>

namespace wraith {

void DefaultNodeClaim::open(struct pw_core *core, const char *key, const std::string &node_name) {
	key_ = key;
	node_name_ = node_name;
	static const struct pw_registry_events registry_events = [] {
		struct pw_registry_events ev{};
		ev.version = PW_VERSION_REGISTRY_EVENTS;
		ev.global = on_registry_global;
		return ev;
	}();
	registry_ = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	if (registry_) {
		pw_registry_add_listener(registry_, &registry_listener_, &registry_events, this);
	}
}

void DefaultNodeClaim::close() {
	if (metadata_) {
		spa_hook_remove(&metadata_listener_);
		pw_proxy_destroy((struct pw_proxy *)metadata_);
		metadata_ = nullptr;
	}
	if (registry_) {
		spa_hook_remove(&registry_listener_);
		pw_proxy_destroy((struct pw_proxy *)registry_);
		registry_ = nullptr;
	}
}

void DefaultNodeClaim::on_registry_global(void *data, uint32_t id, uint32_t permissions, const char *type,
	uint32_t version, const struct spa_dict *props) {
	(void)permissions;
	(void)version;
	auto *self = static_cast<DefaultNodeClaim *>(data);
	if (self->metadata_ || !props || strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0) {
		return;
	}
	const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
	if (!name || strcmp(name, "default") != 0) {
		return;
	}
	self->metadata_ =
		(struct pw_metadata *)pw_registry_bind(self->registry_, id, type, PW_VERSION_METADATA, 0);
	if (!self->metadata_) {
		return;
	}
	static const struct pw_metadata_events metadata_events = [] {
		struct pw_metadata_events ev{};
		ev.version = PW_VERSION_METADATA_EVENTS;
		ev.property = on_metadata_property;
		return ev;
	}();
	pw_metadata_add_listener(self->metadata_, &self->metadata_listener_, &metadata_events, self);
	self->claimed_at_ = std::chrono::steady_clock::now();
	self->write();
}

void DefaultNodeClaim::write() {
	std::string value = "{\"name\":\"" + node_name_ + "\"}";
	pw_metadata_set_property(metadata_, 0, key_, "Spa:String:JSON", value.c_str());
}

int DefaultNodeClaim::on_metadata_property(void *data, uint32_t subject, const char *key, const char *type,
	const char *value) {
	(void)type;
	auto *self = static_cast<DefaultNodeClaim *>(data);
	if (subject != 0 || !key || strcmp(key, self->key_) != 0 ||
		std::chrono::steady_clock::now() - self->claimed_at_ > kReassertWindow) {
		return 0;
	}
	// Ours echoed back (WirePlumber may re-serialise it with spaces), or
	// someone else's: the name decides.
	char name[256] = "";
	if (value) {
		struct spa_json it[1];
		spa_json_init(&it[0], value, strlen(value));
		struct spa_json obj;
		if (spa_json_enter_object(&it[0], &obj) > 0) {
			char k[64];
			while (spa_json_get_string(&obj, k, sizeof(k)) > 0) {
				if (strcmp(k, "name") == 0) {
					spa_json_get_string(&obj, name, sizeof(name));
					break;
				}
				const char *skip;
				if (spa_json_next(&obj, &skip) <= 0) {
					break;
				}
			}
		}
	}
	if (self->node_name_ != name) {
		self->write();
	}
	return 0;
}

} // namespace wraith
