// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Length-prefixed protobuf framing for the lobby/control/input streams
// (gdp-spec.md §3.1). Pure byte-buffer logic, no transport dependency
// -- the transport wrapper (transport.hpp) drives this by feeding it
// whatever bytes a stream delivers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace google::protobuf {
class MessageLite;
}

namespace gdp {

// gdp-spec.md §3.1: a frame's length prefix may not exceed this.
inline constexpr size_t kMaxFrameSize = 1 << 20;
// gdp-spec.md §7.4: the largest CursorShape side. 384 x 384 x 4 bytes is
// 576 KiB, so the largest shape fits one frame; 384 is also the cursor
// size mutter's screencast always offers.
inline constexpr uint32_t kMaxCursorDim = 384;
static_assert((size_t)kMaxCursorDim * kMaxCursorDim * 4 + 1024 <= kMaxFrameSize,
	"a CursorShape must fit one frame");
inline constexpr size_t kLengthPrefixSize = 4;
// gdp-spec.md §6.3: the cap on a session
// connection's control frames until its SessionHello's token has been
// checked -- one small message (a token and a few short lists), not
// kMaxFrameSize, for a peer nobody has vouched for yet.
inline constexpr size_t kMaxPreAuthFrameSize = 16 * 1024;

// Appends `msg`'s length-prefixed encoding to `out`. Returns false (and
// leaves `out` untouched) if `msg` is larger than kMaxFrameSize or fails to
// serialize.
bool encode_frame(const google::protobuf::MessageLite &msg, std::vector<uint8_t> *out);

// Accumulates bytes from one direction of a reliable stream and yields
// complete frames as they arrive. QUIC stream data can be delivered in
// arbitrary chunks unrelated to message boundaries, so this owns a small
// growing buffer rather than assuming feed() gets whole frames.
class FrameReader {
public:
	enum class Result {
		kOk,         // a frame was parsed into `msg`; call drain() again, more may be queued
		kIncomplete, // not enough bytes yet; wait for the next feed()
		kTooLarge,   // length prefix exceeded max_frame_size() -- caller should close per gdp-spec.md §12
		kMalformed,  // protobuf parse failed -- caller should close per gdp-spec.md §12
	};

	void feed(const uint8_t *data, size_t len);

	// The largest length prefix drain() accepts before reporting
	// kTooLarge: kMaxFrameSize unless lowered, e.g. to hold an
	// unauthenticated peer to one small SessionHello (gdp-spec.md §6.3).
	// Values above kMaxFrameSize are clamped to it.
	void set_max_frame_size(size_t max) { max_frame_size_ = max < kMaxFrameSize ? max : kMaxFrameSize; }
	size_t max_frame_size() const { return max_frame_size_; }

	// `msg` is Clear()ed and populated on kOk; left as-is otherwise.
	Result drain(google::protobuf::MessageLite *msg);

	// The usual on_data handler in one call: feed() `data`, then drain()
	// every complete frame into `msg`, calling `on_frame` after each parse.
	// Returns false on kTooLarge/kMalformed (gdp-spec.md §12: the caller
	// should close the connection), true otherwise. `on_frame` returning
	// false stops draining early -- for a handler that just closed or reset
	// the connection and must not see further frames; whatever remains
	// buffered is left in place.
	bool feed_and_drain(const uint8_t *data, size_t len, google::protobuf::MessageLite *msg,
		const std::function<bool()> &on_frame);

	// The Result of the last drain() feed_and_drain() performed -- after a
	// false return, which of kTooLarge/kMalformed it was, so the caller can
	// close with the matching gdp-spec.md §12 code (error_code() below).
	Result last_result() const { return last_result_; }
	// gdp-spec.md §12's application error code for last_result(): kFrameTooLarge
	// for kTooLarge, kMalformedFrame for kMalformed, 0 otherwise (matches error_codes.hpp).
	uint64_t error_code() const;

private:
	std::vector<uint8_t> buf_;
	size_t max_frame_size_ = kMaxFrameSize;
	Result last_result_ = Result::kIncomplete;
};

} // namespace gdp
