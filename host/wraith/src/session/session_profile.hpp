// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Session-type profiles (docs/reference/session-profiles.md): small INI
// files under sessions.d/ describing one selectable desktop. Mirrors
// ghostd/src/profiles.rs's format exactly, but wraith is the side that
// actually resolves an id to a profile and launches its Exec -- ghostd
// only ever reads these to build the lobby's advertised list and to
// validate the id a client picks; the client never sends a raw command.
#pragma once

#include <string>
#include <vector>

namespace wraith {

struct SessionProfile {
	std::string id;
	std::string name;
	// Required. One of the screencast backends, where a top-level
	// compositor runs and wraith is its client
	// (docs/design/capture-backends.md): "screencast-gnome",
	// "screencast-kwin" or "screencast-ext".
	// Free-form here; main.cpp is what rejects an unknown value, so ghostd
	// (which only checks it is present) and this parser never disagree.
	std::string backend;
	std::string exec;
	// TryExec= is ghostd's (it hides a type whose binary isn't installed)
	// and is accepted but not stored here: wraith only ever launches a
	// type ghostd has already listed.
	// LogoutExec=: the command that asks
	// this desktop to log itself out gracefully (e.g. Plasma's own D-Bus
	// logout call) when a client requests it. Empty means SIGTERM the
	// session leader instead -- right for a plain terminal, not for a DE.
	std::string logout_exec;
	std::vector<std::pair<std::string, std::string>> environment;

	// Any "screencast-<compositor>" backend, and the <compositor> part
	// ("gnome", "kwin", "ext"; empty for a non-screencast backend). A bare
	// "screencast" with no compositor suffix is not accepted.
	bool is_screencast() const { return backend.rfind("screencast-", 0) == 0; }
	std::string screencast_compositor() const {
		return is_screencast() ? backend.substr(std::string("screencast-").size()) : std::string();
	}
};

// Parses one profile file's contents. Returns false (profile untouched;
// *error set) for a missing Name/Exec/Backend, an empty file (the documented way
// an override masks a shipped default) or Enabled=false (checked first, so
// a disabled override needs no other keys) -- all "not a usable session
// type", not a hard error, so the resolver below turns either into a
// clean not-found rather than a crash.
bool parse_session_profile(const std::string &id, const std::string &text, SessionProfile *out,
	std::string *error);

// Resolves `id` to a profile by searching `dirs` in order (first directory
// containing a `<id>.conf` wins, matching ghostd's override precedence).
// Rejects ids containing '/' or ".." before touching the filesystem --
// this id can come from a network client via SessionInit.session_type, by
// way of ghostd, so it must never be usable to escape sessions.d.
//
// With `strict`, the directory the profile was found in and the file
// itself must be owned by root and not writable by group or others, or
// the profile is refused: its Exec runs as the session user, and the
// directory list reaches wraith over the control socket. The -G path
// always sets it; a developer's `-p`/`-d` run doesn't.
bool resolve_session_profile(const std::string &id, const std::vector<std::string> &dirs, SessionProfile *out,
	std::string *error, bool strict = true);

// {sessions_dir if given (ghostd.toml's, as ghostseat vetted it, from SessionInit), "/etc/ghost/sessions.d",
// "<datadir_override or GHOST_DATADIR>/sessions.d"}, in override-wins-first order.
std::vector<std::string> default_session_dirs(const std::string &datadir_override,
	const std::string &sessions_dir = std::string());

} // namespace wraith
