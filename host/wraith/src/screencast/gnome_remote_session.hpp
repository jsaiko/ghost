// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// GnomeRemoteSession: the RemoteSession (remote_session.hpp) for
// `Backend=screencast-gnome` (docs/design/capture-backends.md) -- a linked
// `org.gnome.Mutter.RemoteDesktop`/
// `ScreenCast` session pair. mutter ties video and input to the *same*
// session object: `RemoteDesktop.Session.Start()` starts the linked
// screencast stream too, and `ConnectToEIS`/`Start`/`Stop` are all scoped
// to the D-Bus connection that called `CreateSession` -- a different
// connection gets AccessDenied (docs/design/capture-backends.md#gnome).
// One class, one sd-bus connection, shared by both halves: the
// FrameSource it makes is a PipeWireCapture on the stream's node id, the
// ScreencastInput an EiInput on a ConnectToEIS fd.
#pragma once

#include "screencast/remote_session.hpp"

#include <cstdint>
#include <memory>

namespace wraith {

class GnomeRemoteSession : public RemoteSession {
public:
	GnomeRemoteSession();
	~GnomeRemoteSession() override;
	GnomeRemoteSession(const GnomeRemoteSession &) = delete;
	GnomeRemoteSession &operator=(const GnomeRemoteSession &) = delete;

	// Creates a `RemoteDesktop` session, links a `ScreenCast` session to
	// it (`remote-desktop-session-id`), requests `RecordVirtual` (cursor
	// mode `metadata`), subscribes to the stream's
	// `PipeWireStreamAdded` signal *before* calling `RemoteDesktop.
	// Session.Start()` (order-sensitive -- Start() starts the linked
	// stream synchronously, and a missed signal isn't redelivered), then
	// blocks briefly for that signal to report a
	// node id. False on any failure -- D-Bus errors, or no
	// `PipeWireStreamAdded` within the wait.
	bool open(const Config &config) override;
	void close() override;

	// --- RemoteSession ---
	std::unique_ptr<FrameSource> make_frame_source() override;
	std::unique_ptr<ScreencastInput> make_input_sink() override;
	// The clipboard methods on this same RemoteDesktop.Session object
	// (screencast/gnome_clipboard.hpp). Null if EnableClipboard fails.
	std::unique_ptr<ClipboardSink> make_clipboard_sink() override;
	// RecordVirtual's monitor takes the size wraith's PipeWire stream asks
	// for, so a new size needs a new screencast, not a new GNOME.
	// Relaunching the whole session can't work: the new
	// gnome-session@.target start is refused as destructive while the old
	// session's stop jobs are still queued.
	bool sizes_output_on_open() const override { return true; }
	const char *name() const override { return "mutter"; }

private:
	// Valid once open() has returned true.
	uint32_t pipewire_node_id() const;
	// Calls `ConnectToEIS({})` on this session (idempotent -- mutter
	// tears the EIS context down when its client disappears, so this is
	// meant to be called at most once per open() and reopened via a
	// fresh GnomeRemoteSession if the connection drops). `*out_fd` is a
	// dup'd, CLOEXEC fd the caller owns (the message's own fd closes when
	// the reply is unref'd). False on failure.
	bool connect_eis(int *out_fd);

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
