// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Clipboard sync rules (gdp-spec.md §7.9), shared by
// wraith (which talks to a compositor's selection) and spectre (which
// talks to SDL's), so the two can't disagree about what crosses the wire
// or about which local change is really just an echo of the peer's.
//
// v1 is text only: ClipboardData.mime_type is literally "text/plain" and
// data is UTF-8. The compositors, though, see a zoo of text mime names,
// so the read-preference order and the offer set live here too -- wraith
// is the only caller of those (SDL handles the mime set internally), but
// they belong with the rest of the rules rather than buried in a backend.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gdp {

// SessionHello/SessionAccept.capabilities name for clipboard sync
// (gdp-spec.md §6.7). Neither end sends or acts on
// ClipboardData unless this was negotiated.
inline constexpr const char *kCapabilityClipboard = "clipboard";

// The only ClipboardData.mime_type v1 defines. Anything else is dropped
// rather than forwarded, which leaves room for images later behind a new
// mime_type value with no wire change.
inline constexpr const char *kClipboardMimeText = "text/plain";

// Maximum ClipboardData.data length, enforced on send and on receive by
// both ends: a stray copy of a huge file listing must not stall the
// control stream. 1 KiB under kMaxFrameSize (framing.hpp), which leaves
// the ControlEnvelope around it room to fit one frame.
inline constexpr size_t kMaxClipboardBytes = (1 << 20) - 1024;

// What wraith advertises when it owns a compositor-side selection, best
// first. The whole list is offered, not just "text/plain": pastes into
// older GTK/X11/Xwayland apps silently fail otherwise.
const std::vector<std::string> &clipboard_offer_mime_types();

// Picks which of a peer's offered mime types to read text from, by the
// preference order above. Empty if none of them is text. Matching is
// case-insensitive and tolerates the charset parameter's spacing
// ("text/plain;charset=utf-8" vs "text/plain; charset=UTF-8").
std::string select_clipboard_mime(const std::vector<std::string> &offered);

// Turns bytes read for `mime` into the UTF-8 the wire carries: everything
// in the offer set is UTF-8 already except STRING, which is Latin-1 and
// gets transcoded. Trailing NULs (which X11 apps sometimes include) are
// stripped either way.
std::string decode_clipboard_text(const std::string &mime, const std::string &raw);

// One direction-pair's worth of echo suppression, one instance per peer
// connection (wraith: per GdpSession; spectre: per SessionClient).
//
// Every mechanism on both sides reports a selection-owner change when we
// are the one who took the selection -- SDL raises
// SDL_EVENT_CLIPBOARD_UPDATE after its own SDL_SetClipboardText,
// data-control sends us back an offer for our own source, mutter emits SelectionOwnerChanged
// with session-is-owner. All of those are "content I just wrote", not a
// fresh local change, and must be swallowed rather than sent back.
//
// Usage: note_applied() *before* handing the peer's text to the local
// clipboard (the self-triggered notification can arrive synchronously
// inside that call), and should_send() on every local change.
class ClipboardEcho {
public:
	// False for content that must not go on the wire: empty (an empty
	// local read is "nothing readable", never a request to clear the
	// peer's clipboard), over the size cap, or the exact text this peer
	// just sent us or we just sent it.
	bool should_send(const std::string &text) const;
	// False for peer content not worth applying locally: empty, over the
	// size cap, or text we ourselves just sent -- which is the peer
	// echoing our own copy back at us.
	bool should_apply(const std::string &text) const;

	void note_sent(const std::string &text) { last_sent_ = text; }
	void note_applied(const std::string &text) { last_applied_ = text; }
	// A new connection starts with no history: the same text as last
	// session is a fresh copy as far as this one is concerned.
	void reset();

private:
	std::string last_sent_;
	std::string last_applied_;
};

} // namespace gdp
