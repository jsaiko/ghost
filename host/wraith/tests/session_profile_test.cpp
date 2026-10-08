// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Plain-assert unit test (same convention as token_test.cpp) for the
// session-type hosting-profile parser. Mirrors
// ghostd/src/profiles.rs's own test suite closely: the two
// implementations must agree on the format even though nothing enforces
// that at compile time.
#include "session/session_profile.hpp"

#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace wraith;

namespace {

void test_parses_well_formed_profile() {
	SessionProfile profile;
	std::string error;
	bool ok = parse_session_profile("terminal",
		"[Session]\nName=Terminal\nBackend=screencast-ext\nExec=foot\nTryExec=foot\n"
		"Environment=FOO=bar\nLogoutExec=qdbus6 org.kde.Shutdown /Shutdown logout\n",
		&profile, &error);
	assert(ok);
	assert(profile.id == "terminal");
	assert(profile.name == "Terminal");
	assert(profile.backend == "screencast-ext");
	assert(profile.exec == "foot");
	assert(profile.logout_exec == "qdbus6 org.kde.Shutdown /Shutdown logout");
	assert(profile.environment.size() == 1);
	assert(profile.environment[0].first == "FOO");
	assert(profile.environment[0].second == "bar");
}

void test_defaults() {
	SessionProfile profile;
	std::string error;
	assert(
		parse_session_profile("x", "[Session]\nName=X\nBackend=screencast-ext\nExec=x\n", &profile, &error));
	assert(profile.logout_exec.empty());
	assert(profile.environment.empty());
}

// No default backend: a profile without one is not a session type.
void test_backend_is_required() {
	SessionProfile profile;
	std::string error;
	assert(!parse_session_profile("x", "[Session]\nName=X\nExec=x\n", &profile, &error));
	assert(!parse_session_profile("x", "[Session]\nName=X\nBackend=\nExec=x\n", &profile, &error));
}

// Backend=screencast-<compositor>:
// the compositor suffix is what main.cpp dispatches on, and a bare
// "screencast" with no suffix is not a screencast backend.
void test_screencast_backend_naming() {
	SessionProfile profile;
	std::string error;
	assert(parse_session_profile("gnome", "[Session]\nName=GNOME\nBackend=screencast-gnome\nExec=gnome\n",
		&profile, &error));
	assert(profile.is_screencast());
	assert(profile.screencast_compositor() == "gnome");

	assert(parse_session_profile("kde", "[Session]\nName=KDE\nBackend=screencast-kwin\nExec=kwin\n", &profile,
		&error));
	assert(profile.screencast_compositor() == "kwin");

	assert(
		parse_session_profile("old", "[Session]\nName=Old\nBackend=screencast\nExec=x\n", &profile, &error));
	assert(!profile.is_screencast());
	assert(profile.screencast_compositor().empty());

	// Parsed, not validated: main.cpp rejects a Backend it doesn't know.
	assert(parse_session_profile("native", "[Session]\nName=Native\nBackend=native\nExec=foot\n", &profile,
		&error));
	assert(!profile.is_screencast());
}

void test_empty_file_is_masked() {
	SessionProfile profile;
	std::string error;
	assert(!parse_session_profile("terminal", "", &profile, &error));
	assert(!parse_session_profile("terminal", "   \n\n", &profile, &error));
}

void test_enabled_defaults_true_and_false_disables() {
	SessionProfile profile;
	std::string error;
	assert(parse_session_profile("x", "[Session]\nName=X\nBackend=screencast-ext\nExec=x\nEnabled=true\n",
		&profile, &error));
	assert(!parse_session_profile("x", "[Session]\nName=X\nExec=x\nEnabled=false\n", &profile, &error));
	// A disabled override needs no Name/Exec.
	assert(!parse_session_profile("x", "[Session]\nEnabled=false\n", &profile, &error));
}

void test_missing_required_keys_is_invalid() {
	SessionProfile profile;
	std::string error;
	assert(!parse_session_profile("x", "[Session]\nName=X\n", &profile, &error)); // no Exec
	assert(!parse_session_profile("x", "[Session]\nExec=x\n", &profile, &error)); // no Name
}

std::string make_tempdir(const char *label) {
	std::string tmpl = std::string("/tmp/wraith-session-profile-test-") + label + "-XXXXXX";
	std::vector<char> buf(tmpl.begin(), tmpl.end());
	buf.push_back('\0');
	char *dir = mkdtemp(buf.data());
	assert(dir);
	return dir;
}

void write_file(const std::string &dir, const std::string &name, const std::string &contents) {
	std::ofstream f(dir + "/" + name);
	f << contents;
}

void test_resolve_honors_override_precedence_and_masking() {
	std::string etc = make_tempdir("etc");
	std::string datadir = make_tempdir("datadir");

	write_file(datadir, "terminal.conf", "[Session]\nName=Terminal\nBackend=screencast-ext\nExec=foot\n");
	write_file(datadir, "plasma.conf", "[Session]\nName=KDE Plasma\nBackend=screencast-kwin\nExec=plasma\n");
	write_file(etc, "terminal.conf",
		"[Session]\nName=Terminal (custom)\nBackend=screencast-ext\nExec=foot\n");
	write_file(etc, "plasma.conf", ""); // masks the shipped default
	write_file(datadir, "gnome.conf", "[Session]\nName=GNOME\nBackend=screencast-ext\nExec=gnome\n");
	write_file(etc, "gnome.conf", "[Session]\nEnabled=false\n"); // disables it

	// Not strict: these directories are the test user's.
	SessionProfile profile;
	std::string error;
	assert(resolve_session_profile("terminal", {etc, datadir}, &profile, &error, false));
	assert(profile.name == "Terminal (custom)");

	assert(!resolve_session_profile("plasma", {etc, datadir}, &profile, &error, false));
	assert(!resolve_session_profile("gnome", {etc, datadir}, &profile, &error, false));
	assert(!resolve_session_profile("nonexistent", {etc, datadir}, &profile, &error, false));
}

// Strict mode (the -G path) refuses a profile from a directory that isn't
// root's, even a well-formed one, and says so rather than skipping it.
void test_strict_refuses_a_user_owned_profile() {
	if (getuid() == 0) {
		return; // as root the directory would pass; nothing to test here
	}
	std::string dir = make_tempdir("strict");
	write_file(dir, "terminal.conf", "[Session]\nName=Terminal\nBackend=screencast-ext\nExec=foot\n");
	SessionProfile profile;
	std::string error;
	assert(!resolve_session_profile("terminal", {dir}, &profile, &error));
	assert(error.find("not owned by root") != std::string::npos);
	assert(resolve_session_profile("terminal", {dir}, &profile, &error, false));
}

void test_default_dirs_put_ghostds_sessions_dir_first() {
	std::vector<std::string> dirs = default_session_dirs("/share/ghost", "/srv/sessions");
	assert(dirs.size() == 3);
	assert(dirs[0] == "/srv/sessions");
	assert(dirs[1] == "/etc/ghost/sessions.d");
	assert(dirs[2] == "/share/ghost/sessions.d");

	dirs = default_session_dirs("/share/ghost");
	assert(dirs.size() == 2);
	assert(dirs[0] == "/etc/ghost/sessions.d");
}

void test_resolve_rejects_path_traversal() {
	SessionProfile profile;
	std::string error;
	assert(!resolve_session_profile("../etc/passwd", {"/tmp"}, &profile, &error));
	assert(!resolve_session_profile("a/b", {"/tmp"}, &profile, &error));
	assert(!resolve_session_profile("", {"/tmp"}, &profile, &error));
}

} // namespace

int main() {
	test_parses_well_formed_profile();
	test_defaults();
	test_backend_is_required();
	test_screencast_backend_naming();
	test_empty_file_is_masked();
	test_enabled_defaults_true_and_false_disables();
	test_missing_required_keys_is_invalid();
	test_resolve_honors_override_precedence_and_masking();
	test_strict_refuses_a_user_owned_profile();
	test_default_dirs_put_ghostds_sessions_dir_first();
	test_resolve_rejects_path_traversal();
	printf("session_profile_test: ok\n");
	return 0;
}
