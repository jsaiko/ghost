// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "util/config.hpp"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <sys/stat.h>

namespace wraith {

namespace {

WraithConfig g_config;

// Walks one parsed file into a WraithConfig, collecting the first error
// and every unknown key.
class Reader {
public:
	explicit Reader(ConfigLoad *out) : out_(out) {}

	bool failed() const { return !out_->ok; }

	// The table `name` in `parent` -- [name], or [path] for a nested one --
	// or null when the file has none. Keys not in `keys` are warned about.
	const toml::table *section(const toml::table &parent, const char *name,
		const std::vector<std::string> &keys, const std::string &path = "") {
		const std::string full = path.empty() ? name : path;
		const toml::node *node = parent.get(name);
		if (!node) {
			return nullptr;
		}
		const toml::table *table = node->as_table();
		if (!table) {
			fail(*node, full + " must be a table ([" + full + "])");
			return nullptr;
		}
		for (const auto &[key, value] : *table) {
			if (std::find(keys.begin(), keys.end(), key.str()) == keys.end()) {
				out_->warnings.push_back(where(value) + "unknown key " + full + "." + std::string(key.str()));
			}
		}
		return table;
	}

	void boolean(const toml::table *table, const char *section, const char *key, bool *out) {
		const toml::node *node = find(table, key);
		if (!node) {
			return;
		}
		if (const auto *value = node->as_boolean()) {
			*out = value->get();
		} else {
			fail(*node, std::string(section) + "." + key + " must be true or false");
		}
	}

	// Returns true when the key was there and valid, i.e. `*out` was set.
	bool integer(const toml::table *table, const std::string &section, const char *key, uint32_t min,
		uint32_t max, uint32_t *out) {
		const toml::node *node = find(table, key);
		if (!node) {
			return false;
		}
		const auto *value = node->as_integer();
		if (!value || value->get() < (int64_t)min || value->get() > (int64_t)max) {
			fail(*node,
				section + "." + key + " must be an integer from " + std::to_string(min) + " to " +
					std::to_string(max));
			return false;
		}
		*out = (uint32_t)value->get();
		return true;
	}

	// A number (integer or float) from `min` to `max`.
	void number(const toml::table *table, const char *section, const char *key, double min, double max,
		double *out) {
		const toml::node *node = find(table, key);
		if (!node) {
			return;
		}
		std::optional<double> value;
		if (const auto *f = node->as_floating_point()) {
			value = f->get();
		} else if (const auto *i = node->as_integer()) {
			value = (double)i->get();
		}
		if (!value || *value < min || *value > max) {
			std::ostringstream range;
			range << min << " to " << max;
			fail(*node, std::string(section) + "." + key + " must be a number from " + range.str());
			return;
		}
		*out = *value;
	}

	// A string that must be one of `choices`.
	void choice(const toml::table *table, const char *section, const char *key,
		std::initializer_list<const char *> choices, std::string *out) {
		const toml::node *node = find(table, key);
		if (!node) {
			return;
		}
		const auto *value = node->as_string();
		if (value &&
			std::any_of(choices.begin(), choices.end(), [&](const char *c) { return value->get() == c; })) {
			*out = value->get();
			return;
		}
		std::string list;
		for (const char *c : choices) {
			list += std::string(list.empty() ? "" : ", ") + "\"" + c + "\"";
		}
		fail(*node, std::string(section) + "." + key + " must be one of " + list);
	}

	void unknown_sections(const toml::table &root, std::initializer_list<const char *> sections) {
		for (const auto &[key, value] : root) {
			if (std::none_of(sections.begin(), sections.end(),
					[&](const char *s) { return key.str() == s; })) {
				out_->warnings.push_back(where(value) + "unknown section or key " + std::string(key.str()));
			}
		}
	}

private:
	const toml::node *find(const toml::table *table, const char *key) {
		return (table && !failed()) ? table->get(key) : nullptr;
	}

	static std::string where(const toml::node &node) {
		return "line " + std::to_string(node.source().begin.line) + ": ";
	}

	void fail(const toml::node &node, const std::string &message) {
		if (!failed()) {
			out_->ok = false;
			out_->error = where(node) + message;
		}
	}

	ConfigLoad *out_;
};

// Every [refine] key, in the order describe_config() prints them: its
// field in RefineSettings, its override in RefineOverrides, its range.
struct RefineField {
	const char *key;
	uint32_t RefineSettings::*setting;
	std::optional<uint32_t> RefineOverrides::*override;
	uint32_t min;
	uint32_t max;
};
constexpr RefineField kRefineFields[] = {
	{"settle_ms", &RefineSettings::settle_ms, &RefineOverrides::settle_ms, 1, 10000},
	{"bandwidth_percent", &RefineSettings::bandwidth_percent, &RefineOverrides::bandwidth_percent, 0, 1000},
	{"burst_ms", &RefineSettings::burst_ms, &RefineOverrides::burst_ms, 16, 10000},
};

std::vector<std::string> refine_keys() {
	std::vector<std::string> keys;
	for (const RefineField &field : kRefineFields) {
		keys.push_back(field.key);
	}
	return keys;
}

const char *level_name(LogLevel level) {
	switch (level) {
	case LogLevel::Error: return "error";
	case LogLevel::Info: return "info";
	case LogLevel::Debug: return "debug";
	}
	return "debug";
}

} // namespace

const char *link_profile_name(LinkProfile profile) {
	switch (profile) {
	case LinkProfile::kLan: return "lan";
	case LinkProfile::kInternet: return "internet";
	case LinkProfile::kMobile: return "mobile";
	}
	return "lan";
}

RefineSettings WraithConfig::refine_for(LinkProfile profile) const {
	RefineSettings settings = refine;
	const RefineOverrides &overrides = refine_profiles[(int)profile];
	for (const RefineField &field : kRefineFields) {
		if (const std::optional<uint32_t> &value = overrides.*field.override) {
			settings.*field.setting = *value;
		}
	}
	return settings;
}

bool WraithConfig::Encode::codec_enabled(const std::string &token) const {
	if (token == "pyrowave") {
		return codecs.pyrowave;
	}
	if (token == "av1") {
		return codecs.av1;
	}
	if (token == "h265") {
		return codecs.h265;
	}
	if (token == "h264") {
		return codecs.h264;
	}
	return false;
}

const WraithConfig &config() {
	return g_config;
}

ConfigLoad load_config(const std::string &path) {
	ConfigLoad result;
	struct stat st;
	if (stat(path.c_str(), &st) != 0 && errno == ENOENT) {
		result.missing = true;
		return result;
	}

	toml::table root;
	try {
		root = toml::parse_file(path);
	} catch (const toml::parse_error &e) {
		result.ok = false;
		result.error = "line " + std::to_string(e.source().begin.line) + ": " + std::string(e.description());
		return result;
	}

	WraithConfig next;
	Reader reader(&result);
	reader.unknown_sections(root, {"log", "network", "encode", "refine"});

	const toml::table *log = reader.section(root, "log", {"level"});
	std::string level = level_name(next.log.level);
	reader.choice(log, "log", "level", {"error", "info", "debug"}, &level);
	next.log.level = level == "error" ? LogLevel::Error : level == "info" ? LogLevel::Info : LogLevel::Debug;

	const toml::table *network = reader.section(root, "network",
		{"congestion_control", "rate_trace", "pacing_multiplier", "pacing_floor_mbps"});
	reader.choice(network, "network", "congestion_control", {"cubic", "bbr"},
		&next.network.congestion_control);
	reader.boolean(network, "network", "rate_trace", &next.network.rate_trace);
	reader.integer(network, "network", "pacing_multiplier", 0, 100, &next.network.pacing_multiplier);
	reader.integer(network, "network", "pacing_floor_mbps", 1, 100000, &next.network.pacing_floor_mbps);

	const toml::table *encode = reader.section(root, "encode",
		{"gop", "max_bitrate_mbps", "force_software", "nvenc_zero_copy", "pyrowave_bpp", "codecs"});
	reader.integer(encode, "encode", "gop", 0, 1000000, &next.encode.gop);
	reader.integer(encode, "encode", "max_bitrate_mbps", 1, 4000, &next.encode.max_bitrate_mbps);
	reader.boolean(encode, "encode", "force_software", &next.encode.force_software);
	reader.boolean(encode, "encode", "nvenc_zero_copy", &next.encode.nvenc_zero_copy);
	reader.number(encode, "encode", "pyrowave_bpp", 0.1, 8.0, &next.encode.pyrowave_bpp);
	const toml::table *codecs = encode
		? reader.section(*encode, "codecs", {"pyrowave", "av1", "h265", "h264"}, "encode.codecs")
		: nullptr;
	reader.boolean(codecs, "encode.codecs", "pyrowave", &next.encode.codecs.pyrowave);
	reader.boolean(codecs, "encode.codecs", "av1", &next.encode.codecs.av1);
	reader.boolean(codecs, "encode.codecs", "h265", &next.encode.codecs.h265);
	reader.boolean(codecs, "encode.codecs", "h264", &next.encode.codecs.h264);

	std::vector<std::string> refine_section_keys = refine_keys();
	for (LinkProfile profile : kLinkProfiles) {
		refine_section_keys.push_back(link_profile_name(profile));
	}
	const toml::table *refine = reader.section(root, "refine", refine_section_keys);
	for (const RefineField &field : kRefineFields) {
		reader.integer(refine, "refine", field.key, field.min, field.max, &(next.refine.*field.setting));
	}
	// A profile section's keys merge over the built-in overrides
	// (internet's and mobile's settle_ms), key by key, like everything else
	// in the file.
	for (LinkProfile profile : kLinkProfiles) {
		const std::string path = std::string("refine.") + link_profile_name(profile);
		const toml::table *section =
			refine ? reader.section(*refine, link_profile_name(profile), refine_keys(), path) : nullptr;
		if (!section) {
			continue;
		}
		RefineOverrides &overrides = next.refine_profiles[(int)profile];
		for (const RefineField &field : kRefineFields) {
			uint32_t value = 0;
			if (reader.integer(section, path, field.key, field.min, field.max, &value)) {
				overrides.*field.override = value;
			}
		}
	}

	if (result.ok) {
		g_config = next;
	}
	return result;
}

std::string describe_config(const WraithConfig &config) {
	std::ostringstream out;
	auto flag = [](bool b) { return b ? "true" : "false"; };
	out << "[log]\n"
		<< "level = \"" << level_name(config.log.level) << "\"\n"
		<< "\n[network]\n"
		<< "congestion_control = \"" << config.network.congestion_control << "\"\n"
		<< "rate_trace = " << flag(config.network.rate_trace) << "\n"
		<< "pacing_multiplier = " << config.network.pacing_multiplier << "\n"
		<< "pacing_floor_mbps = " << config.network.pacing_floor_mbps << "\n"
		<< "\n[encode]\n"
		<< "gop = " << config.encode.gop << "\n"
		<< "max_bitrate_mbps = " << config.encode.max_bitrate_mbps << "\n"
		<< "force_software = " << flag(config.encode.force_software) << "\n"
		<< "nvenc_zero_copy = " << flag(config.encode.nvenc_zero_copy) << "\n"
		<< "pyrowave_bpp = " << config.encode.pyrowave_bpp << "\n"
		<< "\n[encode.codecs]\n"
		<< "pyrowave = " << flag(config.encode.codecs.pyrowave) << "\n"
		<< "av1 = " << flag(config.encode.codecs.av1) << "\n"
		<< "h265 = " << flag(config.encode.codecs.h265) << "\n"
		<< "h264 = " << flag(config.encode.codecs.h264) << "\n"
		<< "\n[refine]\n";
	for (const RefineField &field : kRefineFields) {
		out << field.key << " = " << config.refine.*field.setting << "\n";
	}
	for (LinkProfile profile : kLinkProfiles) {
		out << "\n[refine." << link_profile_name(profile) << "]\n";
		for (const RefineField &field : kRefineFields) {
			if (const std::optional<uint32_t> &value = config.refine_profiles[(int)profile].*field.override) {
				out << field.key << " = " << *value << "\n";
			}
		}
	}
	return out.str();
}

} // namespace wraith
