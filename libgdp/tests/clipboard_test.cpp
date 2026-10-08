// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/clipboard.hpp"

#include "gdp/framing.hpp"
#include "session.pb.h"

// Plain assert()s: make sure a Release build (-DNDEBUG) can't compile them
// away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

int main() {
	using Strings = std::vector<std::string>;

	// Mime preference: the order in clipboard_offer_mime_types(), not the
	// order the peer happened to list.
	assert(gdp::select_clipboard_mime({"STRING", "text/plain", "text/plain;charset=utf-8"}) ==
		"text/plain;charset=utf-8");
	assert(gdp::select_clipboard_mime({"STRING", "TEXT", "text/plain"}) == "text/plain");
	assert(gdp::select_clipboard_mime({"TEXT", "STRING"}) == "STRING");
	// The peer's own spelling comes back, since that's what has to be
	// handed to the data-control receive request -- but the *match* is
	// case- and whitespace-insensitive.
	assert(gdp::select_clipboard_mime({"text/plain; charset=UTF-8"}) == "text/plain; charset=UTF-8");
	assert(gdp::select_clipboard_mime({"utf8_string"}) == "utf8_string");
	// Nothing textual on offer, and nothing on offer at all.
	assert(gdp::select_clipboard_mime({"image/png", "text/html"}).empty());
	assert(gdp::select_clipboard_mime({}).empty());

	// Everything in the offer set is UTF-8 except STRING, which is
	// Latin-1: 0xe9 is U+00E9 and must come back as two bytes.
	assert(gdp::decode_clipboard_text("STRING", std::string("caf\xe9")) == "caf\xc3\xa9");
	assert(gdp::decode_clipboard_text("string", std::string("\x80")) == "\xc2\x80");
	// UTF-8 mimes pass through untouched, including bytes a Latin-1
	// transcode would have mangled.
	assert(gdp::decode_clipboard_text("text/plain;charset=utf-8", "caf\xc3\xa9") == "caf\xc3\xa9");
	assert(gdp::decode_clipboard_text("UTF8_STRING", "caf\xc3\xa9") == "caf\xc3\xa9");
	// Trailing NULs (X11 clients habitually send them) are stripped either
	// way, but an interior one is content.
	assert(gdp::decode_clipboard_text("text/plain", std::string("hi\0\0", 4)) == "hi");
	assert(gdp::decode_clipboard_text("STRING", std::string("hi\0", 3)) == "hi");
	assert(gdp::decode_clipboard_text("text/plain", std::string("a\0b", 3)) == std::string("a\0b", 3));

	// The offer set is what gets advertised when we own the selection --
	// all of it, or pastes into older X11/GTK apps silently fail.
	const Strings &offer = gdp::clipboard_offer_mime_types();
	assert(offer.size() == 5);
	assert(offer.front() == "text/plain;charset=utf-8");

	// Echo suppression. A fresh instance sends anything non-empty.
	gdp::ClipboardEcho echo;
	assert(echo.should_send("hello"));
	assert(echo.should_apply("hello"));

	// Peer content applied locally must not come straight back: every
	// mechanism reports a selection-owner change for our own write.
	echo.note_applied("from peer");
	assert(!echo.should_send("from peer"));
	assert(echo.should_send("something else"));

	// ...and our own send must not be applied again if the peer echoes it.
	echo.note_sent("from us");
	assert(!echo.should_apply("from us"));
	assert(!echo.should_send("from us"));
	assert(echo.should_apply("from peer")); // only our *sends* block applies

	// Empty is never "clear the peer's clipboard": an unfocused SDL window
	// and a text-less data-control offer both read as "".
	assert(!echo.should_send(""));
	assert(!echo.should_apply(""));

	// The size cap is enforced on both directions, at the boundary.
	assert(echo.should_send(std::string(gdp::kMaxClipboardBytes, 'x')));
	assert(!echo.should_send(std::string(gdp::kMaxClipboardBytes + 1, 'x')));
	assert(echo.should_apply(std::string(gdp::kMaxClipboardBytes, 'x')));
	assert(!echo.should_apply(std::string(gdp::kMaxClipboardBytes + 1, 'x')));

	// And text at the cap still fits one control frame once wrapped.
	{
		gdp::session::ControlEnvelope env;
		env.mutable_clipboard()->set_mime_type(gdp::kClipboardMimeText);
		env.mutable_clipboard()->set_data(std::string(gdp::kMaxClipboardBytes, 'x'));
		std::vector<uint8_t> buf;
		assert(gdp::encode_frame(env, &buf));
	}

	// A new connection starts with no history: the same text as last
	// session is a fresh copy as far as this one is concerned.
	echo.reset();
	assert(echo.should_send("from peer"));
	assert(echo.should_apply("from us"));

	printf("clipboard_test: all checks passed\n");
	return 0;
}
