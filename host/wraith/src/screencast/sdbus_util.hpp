// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The few sd-bus idioms the GNOME and KDE sessions repeat: error text for a
// log line, taking an fd out of a reply, and the one-entry a{sv} option
// dict mutter's methods take.
#pragma once

#include <systemd/sd-bus.h>

#include <cstring>
#include <fcntl.h>

namespace wraith {

// What to log for a failed call: the peer's error message when it sent one,
// else strerror of the negative return.
inline const char *sdbus_error_text(const sd_bus_error &error, int r) {
	return error.message ? error.message : strerror(-r);
}

// An fd read out of a reply ("h") is borrowed: it closes when the message
// is unref'd. This dups it (CLOEXEC) into one the caller owns; -1 if the
// reply carried no usable fd or the dup failed.
inline int sdbus_dup_fd(int borrowed_fd) {
	return borrowed_fd >= 0 ? fcntl(borrowed_fd, F_DUPFD_CLOEXEC, 0) : -1;
}

// Appends an a{sv} with exactly one entry: `key` -> a variant of signature
// `sig` whose contents `append` writes (returning sd-bus's own status).
// An empty dict is `sdbus_append_no_options`.
template <typename Append>
int sdbus_append_one_option(sd_bus_message *m, const char *key, const char *sig, Append append) {
	int r = sd_bus_message_open_container(m, 'a', "{sv}");
	r = r >= 0 ? sd_bus_message_open_container(m, 'e', "sv") : r;
	r = r >= 0 ? sd_bus_message_append(m, "s", key) : r;
	r = r >= 0 ? sd_bus_message_open_container(m, 'v', sig) : r;
	r = r >= 0 ? append(m) : r;
	r = r >= 0 ? sd_bus_message_close_container(m) : r; // v
	r = r >= 0 ? sd_bus_message_close_container(m) : r; // e
	r = r >= 0 ? sd_bus_message_close_container(m) : r; // a
	return r;
}

inline int sdbus_append_no_options(sd_bus_message *m) {
	int r = sd_bus_message_open_container(m, 'a', "{sv}");
	return r >= 0 ? sd_bus_message_close_container(m) : r;
}

} // namespace wraith
