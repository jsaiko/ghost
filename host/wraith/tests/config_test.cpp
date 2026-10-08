// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// wraith.toml (util/config.hpp): the shipped packaging/config/wraith.toml, every
// key uncommented, must come out exactly at the built-in defaults -- so
// the file and the code can't drift apart, as ghostd's own test does for
// ghostd.toml -- and a file wraith can't use is rejected whole.
#include "util/config.hpp"

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace {

// Not <cassert>: the tree's default build type is RelWithDebInfo, which
// defines NDEBUG and would compile every check -- calls included -- away.
int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

std::string g_dir;

std::string write_file(const std::string &name, const std::string &contents) {
	std::string path = g_dir + "/" + name;
	std::ofstream(path) << contents;
	return path;
}

void test_shipped_file_is_the_defaults() {
	std::ifstream in(WRAITH_SHIPPED_CONFIG);
	CHECK(in.good());
	std::stringstream all;
	all << in.rdbuf();
	// "#key = value" lines are the settings; prose comments have a space
	// after the '#'.
	std::string uncommented =
		std::regex_replace(all.str(), std::regex("^#([a-z0-9_]+ = )", std::regex::multiline), "$1");
	CHECK(uncommented != all.str());

	std::string expected = wraith::describe_config(wraith::WraithConfig{});
	wraith::ConfigLoad load = wraith::load_config(write_file("shipped.toml", uncommented));
	CHECK(load.ok);
	CHECK(!load.missing);
	CHECK(load.warnings.empty());
	CHECK(wraith::describe_config(wraith::config()) == expected);
	if (wraith::describe_config(wraith::config()) != expected) {
		fprintf(stderr, "got:\n%s\nexpected:\n%s\n", wraith::describe_config(wraith::config()).c_str(),
			expected.c_str());
	}
}

void test_values_apply() {
	wraith::ConfigLoad load = wraith::load_config(write_file("set.toml",
		"[log]\nlevel = \"info\"\n[network]\ncongestion_control = \"bbr\"\nrate_trace = true\n"
		"pacing_multiplier = 4\npacing_floor_mbps = 900\n"
		"[encode]\ngop = 0\nmax_bitrate_mbps = 45\nforce_software = true\nnvenc_zero_copy = false\n"
		"pyrowave_bpp = 0.75\n"
		"[encode.codecs]\nav1 = true\nh264 = false\npyrowave = true\n"
		"[refine]\nburst_ms = 200\nsettle_ms = 120\n"));
	CHECK(load.ok);
	const wraith::WraithConfig &c = wraith::config();
	CHECK(c.log.level == wraith::LogLevel::Info);
	CHECK(c.network.congestion_control == "bbr");
	CHECK(c.network.rate_trace);
	CHECK(c.network.pacing_multiplier == 4);
	CHECK(c.network.pacing_floor_mbps == 900);
	CHECK(c.refine.burst_ms == 200);
	CHECK(c.refine.settle_ms == 120);
	CHECK(c.refine.bandwidth_percent == 50); // untouched keys keep their default
	CHECK(c.encode.gop == 0);
	CHECK(c.encode.max_bitrate_mbps == 45);
	CHECK(c.encode.force_software);
	CHECK(!c.encode.nvenc_zero_copy);
	CHECK(c.encode.pyrowave_bpp == 0.75);
	CHECK(c.encode.codec_enabled("av1"));
	CHECK(c.encode.codec_enabled("pyrowave"));
	CHECK(c.encode.codec_enabled("h265")); // untouched keys keep their default
	CHECK(!c.encode.codec_enabled("h264"));
	CHECK(!c.encode.codec_enabled("vp9"));
}

// A bad file changes nothing: the previous load's settings stand.
void test_bad_files_are_rejected_whole() {
	wraith::load_config(write_file("base.toml", "[refine]\nsettle_ms = 100\n"));
	const char *bad[] = {
		"[refine]\nsettle_ms = 50\nburst_ms = \"two\"\n", // wrong type
		"[refine]\nsettle_ms = 50\nburst_ms = 1\n",       // out of range
		"[network]\ncongestion_control = \"reno\"\n",     // not a choice
		"[network]\npacing_floor_mbps = 0\n",             // out of range
		"[encode]\ngop = -1\n",                           // out of range
		"[encode]\nmax_bitrate_mbps = 0\n",               // out of range
		"[refine\nsettle_ms = 50\n",                      // doesn't parse
		"refine = 3\n",                                   // section not a table
		"[encode.codecs]\nav1 = 1\n",                     // wrong type
		"[encode]\npyrowave_bpp = 0\n",                   // out of range
		"[encode]\npyrowave_bpp = \"1\"\n",               // wrong type
	};
	for (const char *contents : bad) {
		wraith::ConfigLoad load = wraith::load_config(write_file("bad.toml", contents));
		CHECK(!load.ok);
		CHECK(load.error.rfind("line ", 0) == 0);
		CHECK(wraith::config().refine.settle_ms == 100);
	}
}

void test_unknown_keys_warn() {
	wraith::ConfigLoad load = wraith::load_config(
		write_file("unknown.toml", "[refine]\nsettle = 90\nsettle_ms = 90\n[video]\nx = 1\n"));
	CHECK(load.ok);
	CHECK(load.warnings.size() == 2);
	CHECK(wraith::config().refine.settle_ms == 90);
}

// [refine.<profile>] keys replace [refine]'s for that profile only, and
// merge over the built-in overrides rather than dropping them -- which
// also means a built-in override beats a [refine] key: internet and
// mobile keep their own settle times.
void test_refine_profiles() {
	wraith::ConfigLoad load = wraith::load_config(write_file("profiles.toml",
		"[refine]\nsettle_ms = 100\n[refine.lan]\nsettle_ms = 60\n[refine.mobile]\nbandwidth_percent = 20\n"
		"burst_ms = 300\n"));
	CHECK(load.ok);
	CHECK(load.warnings.empty());
	const wraith::WraithConfig &c = wraith::config();

	wraith::RefineSettings lan = c.refine_for(wraith::LinkProfile::kLan);
	CHECK(lan.settle_ms == 60);
	CHECK(lan.bandwidth_percent == 50);

	wraith::RefineSettings internet = c.refine_for(wraith::LinkProfile::kInternet);
	CHECK(internet.settle_ms == 150); // built in
	CHECK(internet.burst_ms == 500);

	wraith::RefineSettings mobile = c.refine_for(wraith::LinkProfile::kMobile);
	CHECK(mobile.settle_ms == 250); // built in
	CHECK(mobile.burst_ms == 300);
	CHECK(mobile.bandwidth_percent == 20);

	// Unknown keys in a profile section warn; bad values reject the file.
	load = wraith::load_config(write_file("profile-unknown.toml", "[refine.lan]\nsetle_ms = 60\n"));
	CHECK(load.ok);
	CHECK(load.warnings.size() == 1);
	load = wraith::load_config(write_file("profile-bad.toml", "[refine.mobile]\nburst_ms = 1\n"));
	CHECK(!load.ok);
}

void test_missing_file_is_defaults() {
	wraith::ConfigLoad load = wraith::load_config(g_dir + "/does-not-exist.toml");
	CHECK(load.ok);
	CHECK(load.missing);
}

} // namespace

int main() {
	char tmpl[] = "/tmp/wraith-config-test-XXXXXX";
	if (!mkdtemp(tmpl)) {
		perror("mkdtemp");
		return 1;
	}
	g_dir = tmpl;

	test_shipped_file_is_the_defaults();
	test_values_apply();
	test_bad_files_are_rejected_whole();
	test_unknown_keys_warn();
	test_refine_profiles();
	test_missing_file_is_defaults();

	std::string cleanup = "rm -rf '" + g_dir + "'";
	if (system(cleanup.c_str()) != 0) {
		fprintf(stderr, "config_test: could not remove %s\n", g_dir.c_str());
	}
	if (g_failures > 0) {
		fprintf(stderr, "config_test: %d check(s) failed\n", g_failures);
		return 1;
	}
	printf("config_test: ok\n");
	return 0;
}
