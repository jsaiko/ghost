// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/framing.hpp"

#include "gdp/error_codes.hpp"

#include "le_bytes.hpp"

#include <google/protobuf/message_lite.h>

#include <algorithm>

namespace gdp {

bool encode_frame(const google::protobuf::MessageLite &msg, std::vector<uint8_t> *out) {
	size_t size = msg.ByteSizeLong();
	if (size > kMaxFrameSize) {
		return false;
	}
	size_t start = out->size();
	out->resize(start + kLengthPrefixSize + size);
	le::put_u32(out->data() + start, static_cast<uint32_t>(size));
	if (size > 0 && !msg.SerializeToArray(out->data() + start + kLengthPrefixSize, static_cast<int>(size))) {
		out->resize(start); // roll back
		return false;
	}
	return true;
}

void FrameReader::feed(const uint8_t *data, size_t len) {
	buf_.insert(buf_.end(), data, data + len);
}

FrameReader::Result FrameReader::drain(google::protobuf::MessageLite *msg) {
	if (buf_.size() < kLengthPrefixSize) {
		return Result::kIncomplete;
	}
	uint32_t frame_len = le::get_u32(buf_.data());
	if (frame_len > max_frame_size_) {
		return Result::kTooLarge;
	}
	if (buf_.size() < kLengthPrefixSize + frame_len) {
		return Result::kIncomplete;
	}

	msg->Clear();
	bool ok =
		frame_len == 0 || msg->ParseFromArray(buf_.data() + kLengthPrefixSize, static_cast<int>(frame_len));
	buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(kLengthPrefixSize + frame_len));
	return ok ? Result::kOk : Result::kMalformed;
}

bool FrameReader::feed_and_drain(const uint8_t *data, size_t len, google::protobuf::MessageLite *msg,
	const std::function<bool()> &on_frame) {
	feed(data, len);
	for (;;) {
		last_result_ = drain(msg);
		switch (last_result_) {
		case Result::kIncomplete: return true;
		case Result::kTooLarge:
		case Result::kMalformed: return false;
		case Result::kOk:
			if (!on_frame()) {
				return true;
			}
			break;
		}
	}
}

uint64_t FrameReader::error_code() const {
	switch (last_result_) {
	case Result::kTooLarge: return static_cast<uint64_t>(ErrorCode::kFrameTooLarge);
	case Result::kMalformed: return static_cast<uint64_t>(ErrorCode::kMalformedFrame);
	case Result::kOk:
	case Result::kIncomplete: break;
	}
	return static_cast<uint64_t>(ErrorCode::kNone);
}

} // namespace gdp
