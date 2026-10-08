// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The host half of clipboard sync (gdp-spec.md §7.9):
// what GdpSession talks to, whichever backend is underneath.
//
// Implementations: DataControlClipboard (screencast/data_control_clipboard.hpp --
// ext-data-control-v1 against the captured compositor, shared by
// screencast-ext and screencast-kwin) and GnomeClipboard
// (screencast/gnome_clipboard.hpp -- the clipboard methods on the
// org.gnome.Mutter.RemoteDesktop.Session GnomeRemoteSession already
// holds).
//
// A backend whose compositor offers no mechanism simply has no sink; the
// session then never negotiates the "clipboard" capability and neither
// end sends ClipboardData (host/wraith/src/session/gdp_session.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct wl_event_loop;
struct wl_event_source;

namespace wraith {

// Reading a selection and serving one both mean moving bytes through a
// pipe whose far end belongs to another process. Neither may block the
// event loop: a client that asks for the selection and then stalls would
// otherwise freeze the capture and input with it. Every transfer
// therefore runs as a non-blocking event source
// here, and this pool owns them so a sink going away takes its in-flight
// transfers off the loop with it.
class ClipboardPipes {
public:
	// Both out of line: Transfer is only defined in the .cpp, and an
	// inline body here would need its full definition to instantiate the
	// vector's own teardown.
	explicit ClipboardPipes(struct wl_event_loop *loop);
	~ClipboardPipes();

	ClipboardPipes(const ClipboardPipes &) = delete;
	ClipboardPipes &operator=(const ClipboardPipes &) = delete;

	// Takes ownership of `fd` (a pipe's read end), drains it up to
	// gdp::kMaxClipboardBytes and calls `done` with what arrived. An
	// oversize transfer is abandoned and `done` gets "" -- the cap is a
	// wire limit, so there is nothing to be gained by reading the rest.
	// `done` never fires after this pool is destroyed.
	void read(int fd, std::function<void(std::string)> done);

	// Takes ownership of `fd` (a pipe's write end) and writes `text` to it,
	// closing it when done. `done` (optional) reports whether the whole
	// text got out -- mutter's SelectionWriteDone needs that answer; the
	// Wayland mechanisms don't, and a reader that hung up early is
	// routine there. `done` never fires after this pool is destroyed.
	void write(int fd, std::string text, std::function<void(bool ok)> done = {});

private:
	struct Transfer;
	void finish(Transfer *transfer);

	struct wl_event_loop *loop_;
	std::vector<std::unique_ptr<Transfer>> transfers_;
	// Set while ~ClipboardPipes() is tearing transfers down, so finish()
	// doesn't mutate the vector being drained.
	bool draining_ = false;
};

// The bytes to serve when a local client pastes `utf8` as `mime`: the
// text itself for every mime in gdp::clipboard_offer_mime_types() except
// STRING, which is Latin-1 -- anything outside U+00FF is unrepresentable
// there and goes out as '?' rather than as mojibake claiming to be
// Latin-1. Every ClipboardSink serves its selection through this.
std::string clipboard_payload_for_mime(const std::string &utf8, const char *mime);

class ClipboardSink {
public:
	explicit ClipboardSink(struct wl_event_loop *loop);
	virtual ~ClipboardSink();

	ClipboardSink(const ClipboardSink &) = delete;
	ClipboardSink &operator=(const ClipboardSink &) = delete;

	// Push text that arrived from the network onto the local clipboard,
	// taking the selection if that's what the mechanism requires. The
	// selection-owner notification this provokes must be recognised as
	// our own write and swallowed (see gdp::ClipboardEcho); every
	// implementation does that by comparing against what it last set.
	virtual void set_text(const std::string &utf8) = 0;

	// Releases the mechanism (drops the selection, unbinds the protocol
	// object, disables mutter's clipboard). The destructor does this too;
	// the explicit call is for a host tearing a backend down in a
	// particular order.
	virtual void close() {}

	// The local clipboard text as last seen, for the push a freshly
	// authenticated session makes (gdp-spec.md §7.9: the
	// compositor's clipboard can change while no client is attached, and
	// without that push the first paste after connecting is stale). Empty
	// if nothing textual has been seen.
	const std::string &current_text() const { return cached_; }

	// The local clipboard changed to `utf8` -- forward it to the peer.
	// Set by GdpSession for the life of an authenticated session and
	// cleared when it ends, so implementations must read it at fire time
	// rather than copying it at construction.
	std::function<void(const std::string &utf8)> on_local_change;

protected:
	// What an implementation calls once it has text for a local change:
	// caches it for current_text() and fires on_local_change.
	void note_local_text(std::string utf8);

	ClipboardPipes pipes_;

private:
	std::string cached_;
};

} // namespace wraith
