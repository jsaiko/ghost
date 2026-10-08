// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/clipboard.hpp"

#include "gdp/framing.hpp"

#include <algorithm>
#include <cctype>

namespace gdp {

static_assert(kMaxClipboardBytes + 1024 <= kMaxFrameSize,
	"a capped ClipboardData must fit one control frame");

namespace {

// Read preference *and* offer order in one list: the first entry a peer
// offers is the one read from, and all of them are advertised when we own
// the selection. UTF8_STRING/STRING/TEXT are the X11 atom names Xwayland
// and older toolkits still ask for by name.
const std::vector<std::string> kTextMimeTypes = {
	"text/plain;charset=utf-8",
	"text/plain",
	"UTF8_STRING",
	"STRING",
	"TEXT",
};

// "text/plain; charset=UTF-8" and "text/plain;charset=utf-8" are the same
// mime type; compositors and toolkits disagree about the space and the
// case. Compared with both removed.
std::string canonical_mime(const std::string &mime) {
	std::string out;
	out.reserve(mime.size());
	for (char c : mime) {
		if (c == ' ' || c == '\t') {
			continue;
		}
		out.push_back((char)std::tolower((unsigned char)c));
	}
	return out;
}

} // namespace

const std::vector<std::string> &clipboard_offer_mime_types() {
	return kTextMimeTypes;
}

std::string select_clipboard_mime(const std::vector<std::string> &offered) {
	for (const std::string &preferred : kTextMimeTypes) {
		const std::string canonical_preferred = canonical_mime(preferred);
		for (const std::string &candidate : offered) {
			if (canonical_mime(candidate) == canonical_preferred) {
				// The peer's own spelling, not ours: it's what has to go
				// back over the data-control receive request.
				return candidate;
			}
		}
	}
	return "";
}

std::string decode_clipboard_text(const std::string &mime, const std::string &raw) {
	std::string text = raw;
	// X11 clients habitually include the terminating NUL in the transfer.
	while (!text.empty() && text.back() == '\0') {
		text.pop_back();
	}
	if (canonical_mime(mime) != "string") {
		return text;
	}
	// STRING is Latin-1 (ISO-8859-1): every byte is its own code point, so
	// the transcode is the two-byte UTF-8 encoding for anything >= 0x80.
	std::string utf8;
	utf8.reserve(text.size());
	for (unsigned char c : text) {
		if (c < 0x80) {
			utf8.push_back((char)c);
		} else {
			utf8.push_back((char)(0xc0 | (c >> 6)));
			utf8.push_back((char)(0x80 | (c & 0x3f)));
		}
	}
	return utf8;
}

bool ClipboardEcho::should_send(const std::string &text) const {
	if (text.empty() || text.size() > kMaxClipboardBytes) {
		return false;
	}
	return text != last_applied_ && text != last_sent_;
}

bool ClipboardEcho::should_apply(const std::string &text) const {
	if (text.empty() || text.size() > kMaxClipboardBytes) {
		return false;
	}
	return text != last_sent_;
}

void ClipboardEcho::reset() {
	last_sent_.clear();
	last_applied_.clear();
}

} // namespace gdp
