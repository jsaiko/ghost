// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// GnomeClipboard: clipboard sync for `Backend=screencast-gnome`, over the
// clipboard methods on the very
// `org.gnome.Mutter.RemoteDesktop.Session` object GnomeRemoteSession
// already holds for ConnectToEIS -- the same API gnome-remote-desktop
// uses. Pure D-Bus: no WaylandClient, no Xwayland, no XCB.
//
//   EnableClipboard(a{sv}) / DisableClipboard()
//   SetSelection(a{sv})                       -- we take the selection
//   SelectionRead(s mime) -> h                -- read what someone else copied
//   SelectionWrite(u serial) -> h, SelectionWriteDone(u serial, b ok)
//   signals SelectionOwnerChanged(a{sv}), SelectionTransfer(s mime, u serial)
//
// mutter bridges this selection to Xwayland clients itself, so X11 apps
// in the session are covered without any extra work here. Mutter times
// out a SelectionTransfer it gets no answer to, so the write side never
// blocks: it answers from the event loop like every other transfer.
#pragma once

#include "session/clipboard_sync.hpp"

#include <memory>
#include <string>

struct sd_bus;
struct wl_event_loop;

namespace wraith {

class GnomeClipboard : public ClipboardSink {
public:
	// `bus` and `session_path` are GnomeRemoteSession's, and must outlive
	// this object. Null if EnableClipboard fails -- an older mutter, or
	// one that refuses -- in which case the session simply runs without
	// clipboard sync (session/gdp_session.cpp never offers the
	// capability).
	static std::unique_ptr<GnomeClipboard> create(struct sd_bus *bus, const std::string &session_path,
		struct wl_event_loop *loop);

	~GnomeClipboard() override;

	void set_text(const std::string &utf8) override;
	void close() override;

private:
	struct Impl;
	GnomeClipboard(struct wl_event_loop *loop, std::unique_ptr<Impl> impl);

	// Impl's D-Bus callbacks reach the ClipboardPipes pool through these
	// (it's protected on the base class).
	void read_pipe(int fd, const std::string &mime);
	void write_pipe(int fd, std::string payload, uint32_t serial);

	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
