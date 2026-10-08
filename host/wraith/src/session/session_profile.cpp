// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/session_profile.hpp"

#include <fstream>
#include <sstream>

#include <sys/stat.h>

namespace wraith {

namespace {

std::string trim(const std::string &s) {
	size_t start = s.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) {
		return "";
	}
	size_t end = s.find_last_not_of(" \t\r\n");
	return s.substr(start, end - start + 1);
}

bool parse_bool(const std::string &value) {
	return value == "true" || value == "1" || value == "yes";
}

// Root-owned and not writable by group or others: what a profile and the
// directory it sits in must be before wraith execs it (strict mode).
bool owned_by_root_only(const std::string &path, bool directory, std::string *error) {
	struct stat st;
	if (stat(path.c_str(), &st) != 0) {
		*error = path + ": cannot stat";
		return false;
	}
	if (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) {
		*error = path + (directory ? " is not a directory" : " is not a regular file");
		return false;
	}
	if (st.st_uid != 0) {
		*error = path + " is not owned by root (uid " + std::to_string(st.st_uid) + ")";
		return false;
	}
	if (st.st_mode & 022) {
		*error = path + " is writable by group or others";
		return false;
	}
	return true;
}

} // namespace

bool parse_session_profile(const std::string &id, const std::string &text, SessionProfile *out,
	std::string *error) {
	if (trim(text).empty()) {
		*error = "empty profile (masked)";
		return false;
	}

	SessionProfile profile;
	profile.id = id;
	bool in_session_section = false;
	bool have_name = false, have_exec = false;
	bool enabled = true;

	std::stringstream ss(text);
	std::string raw_line;
	while (std::getline(ss, raw_line)) {
		std::string line = trim(raw_line);
		if (line.empty() || line[0] == '#' || line[0] == ';') {
			continue;
		}
		if (line.front() == '[' && line.back() == ']') {
			in_session_section = trim(line.substr(1, line.size() - 2)) == "Session";
			continue;
		}
		if (!in_session_section) {
			continue;
		}
		size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string key = trim(line.substr(0, eq));
		std::string value = trim(line.substr(eq + 1));
		if (key == "Name") {
			profile.name = value;
			have_name = true;
		} else if (key == "Backend") {
			profile.backend = value;
		} else if (key == "Exec") {
			profile.exec = value;
			have_exec = true;
		} else if (key == "LogoutExec") {
			profile.logout_exec = value;
		} else if (key == "Enabled") {
			enabled = parse_bool(value);
		} else if (key == "Environment") {
			size_t sep = value.find('=');
			if (sep != std::string::npos) {
				profile.environment.emplace_back(value.substr(0, sep), value.substr(sep + 1));
			}
		}
	}

	if (!enabled) {
		*error = "profile disabled (Enabled=false)";
		return false;
	}
	if (!have_name || !have_exec || profile.backend.empty()) {
		*error = "profile missing a required Name, Exec or Backend key";
		return false;
	}

	*out = std::move(profile);
	return true;
}

bool resolve_session_profile(const std::string &id, const std::vector<std::string> &dirs, SessionProfile *out,
	std::string *error, bool strict) {
	if (id.empty() || id.find('/') != std::string::npos || id.find("..") != std::string::npos) {
		*error = "invalid session type id";
		return false;
	}

	for (const auto &dir : dirs) {
		std::string path = dir + "/" + id + ".conf";
		std::ifstream file(path);
		if (!file) {
			continue; // not present in this directory; try the next
		}
		// The first directory with the file decides, as ghostd's listing
		// did; a file that fails the check is refused, not skipped, so an
		// override can't be bypassed by making it unreadable.
		if (strict && (!owned_by_root_only(dir, true, error) || !owned_by_root_only(path, false, error))) {
			*error = "refusing profile " + path + ": " + *error;
			return false;
		}
		std::stringstream buffer;
		buffer << file.rdbuf();
		return parse_session_profile(id, buffer.str(), out, error);
	}

	*error = "no such session type: " + id;
	return false;
}

std::vector<std::string> default_session_dirs(const std::string &datadir_override,
	const std::string &sessions_dir) {
	std::string datadir = datadir_override;
	if (datadir.empty()) {
#ifdef GHOST_DATADIR
		datadir = GHOST_DATADIR;
#else
		datadir = "/usr/local/share/ghost";
#endif
	}
	std::vector<std::string> dirs;
	if (!sessions_dir.empty()) {
		dirs.push_back(sessions_dir);
	}
	dirs.push_back("/etc/ghost/sessions.d");
	dirs.push_back(datadir + "/sessions.d");
	return dirs;
}

} // namespace wraith
