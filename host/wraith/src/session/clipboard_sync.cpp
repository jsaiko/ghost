// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/clipboard_sync.hpp"

#include "gdp/clipboard.hpp"

#include "util/log.hpp"

#include <wayland-server-core.h>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <strings.h>
#include <unistd.h>

namespace wraith {

namespace {

// One read()/write() per event-loop wakeup. A pipe's own buffer is 64 KiB
// by default, so this drains a typical selection in a couple of ticks and
// one at the ~1 MiB cap without ever blocking.
constexpr size_t kChunkBytes = 64 * 1024;

void set_nonblocking(int fd) {
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
}

std::string utf8_to_latin1(const std::string &utf8) {
	std::string latin1;
	latin1.reserve(utf8.size());
	for (size_t i = 0; i < utf8.size();) {
		unsigned char c = (unsigned char)utf8[i];
		if (c < 0x80) {
			latin1.push_back((char)c);
			i++;
			continue;
		}
		if ((c & 0xe0) == 0xc0 && i + 1 < utf8.size()) {
			unsigned code = ((c & 0x1fu) << 6) | ((unsigned char)utf8[i + 1] & 0x3fu);
			latin1.push_back(code <= 0xff ? (char)code : '?');
			i += 2;
			continue;
		}
		latin1.push_back('?');
		// Skip the whole sequence, not just its lead byte.
		i++;
		while (i < utf8.size() && ((unsigned char)utf8[i] & 0xc0) == 0x80) {
			i++;
		}
	}
	return latin1;
}

} // namespace

std::string clipboard_payload_for_mime(const std::string &utf8, const char *mime) {
	if (mime && strcasecmp(mime, "STRING") == 0) {
		return utf8_to_latin1(utf8);
	}
	return utf8;
}

struct ClipboardPipes::Transfer {
	ClipboardPipes *owner = nullptr;
	struct wl_event_source *source = nullptr;
	int fd = -1;
	std::string buffer;   // reading: what has arrived; writing: what's left
	size_t write_pos = 0; // writing: how much of buffer has gone out
	std::function<void(std::string)> done;
	std::function<void(bool ok)> write_done;

	~Transfer() {
		if (source) {
			wl_event_source_remove(source);
		}
		if (fd >= 0) {
			close(fd);
		}
	}
};

ClipboardPipes::ClipboardPipes(struct wl_event_loop *loop) : loop_(loop) {}

ClipboardPipes::~ClipboardPipes() {
	// Transfers hold event sources into a loop that may outlive this pool
	// (the compositor's) and callbacks into the sink that owns it (which
	// does not). Both go away here, and finish() must not touch the
	// vector while it drains.
	draining_ = true;
	transfers_.clear();
}

void ClipboardPipes::finish(Transfer *transfer) {
	if (draining_) {
		return;
	}
	for (auto it = transfers_.begin(); it != transfers_.end(); ++it) {
		if (it->get() == transfer) {
			// Moved out first: the completion callback can start another
			// transfer, which would reallocate the vector under us.
			std::unique_ptr<Transfer> owned = std::move(*it);
			transfers_.erase(it);
			auto done = std::move(owned->done);
			auto write_done = std::move(owned->write_done);
			std::string text = std::move(owned->buffer);
			bool complete = owned->write_pos == text.size();
			owned.reset(); // closes the fd before the callback runs
			if (done) {
				done(std::move(text));
			} else if (write_done) {
				write_done(complete);
			}
			return;
		}
	}
}

void ClipboardPipes::read(int fd, std::function<void(std::string)> done) {
	if (fd < 0) {
		return;
	}
	set_nonblocking(fd);
	auto transfer = std::make_unique<Transfer>();
	transfer->owner = this;
	transfer->fd = fd;
	transfer->done = std::move(done);
	Transfer *raw = transfer.get();
	raw->source = wl_event_loop_add_fd(
		loop_, fd, WL_EVENT_READABLE,
		[](int fd, uint32_t mask, void *data) {
			auto *t = static_cast<Transfer *>(data);
			char chunk[kChunkBytes];
			for (;;) {
				ssize_t n = ::read(fd, chunk, sizeof(chunk));
				if (n > 0) {
					if (t->buffer.size() + (size_t)n > gdp::kMaxClipboardBytes) {
						// Over the wire cap: nothing to gain by reading
						// the rest, and the result is dropped either way.
						WLOG_INFO("clipboard: dropping a selection larger than the %zu-byte cap",
							gdp::kMaxClipboardBytes);
						t->buffer.clear();
						t->owner->finish(t);
						return 0;
					}
					t->buffer.append(chunk, (size_t)n);
					continue;
				}
				if (n == 0) {
					t->owner->finish(t); // writer closed: transfer complete
					return 0;
				}
				if (errno == EINTR) {
					continue;
				}
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					// More may come; the source fires again. A hangup
					// with no data left arrives as n == 0 above.
					if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
						t->owner->finish(t);
					}
					return 0;
				}
				t->buffer.clear();
				t->owner->finish(t);
				return 0;
			}
		},
		raw);
	transfers_.push_back(std::move(transfer));
}

void ClipboardPipes::write(int fd, std::string text, std::function<void(bool ok)> done) {
	if (fd < 0) {
		return;
	}
	set_nonblocking(fd);
	auto transfer = std::make_unique<Transfer>();
	transfer->owner = this;
	transfer->fd = fd;
	transfer->buffer = std::move(text);
	transfer->write_done = std::move(done);
	Transfer *raw = transfer.get();
	raw->source = wl_event_loop_add_fd(
		loop_, fd, WL_EVENT_WRITABLE,
		[](int fd, uint32_t mask, void *data) {
			auto *t = static_cast<Transfer *>(data);
			if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
				// The reader went away mid-transfer -- routine when a
				// client asks for the selection and then exits.
				t->owner->finish(t);
				return 0;
			}
			while (t->write_pos < t->buffer.size()) {
				ssize_t n = ::write(fd, t->buffer.data() + t->write_pos, t->buffer.size() - t->write_pos);
				if (n > 0) {
					t->write_pos += (size_t)n;
					continue;
				}
				if (n < 0 && errno == EINTR) {
					continue;
				}
				if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
					return 0; // the source fires again when there's room
				}
				break; // EPIPE and friends: the reader is gone
			}
			t->owner->finish(t);
			return 0;
		},
		raw);
	transfers_.push_back(std::move(transfer));
}

ClipboardSink::ClipboardSink(struct wl_event_loop *loop) : pipes_(loop) {}
ClipboardSink::~ClipboardSink() = default;

void ClipboardSink::note_local_text(std::string utf8) {
	if (utf8.empty() || utf8.size() > gdp::kMaxClipboardBytes) {
		// An empty read is "nothing textual here", never a request to
		// clear the peer's clipboard (gdp-spec.md §7.9).
		return;
	}
	cached_ = std::move(utf8);
	if (on_local_change) {
		on_local_change(cached_);
	}
}

} // namespace wraith
