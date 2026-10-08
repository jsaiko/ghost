// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "session/uhid_codec.hpp"

#include <algorithm>
#include <cstring>

namespace wraith::uhid {

namespace {

// Offsets into an event, past the u32 type at 0 (linux/uhid.h, packed):
//   uhid_output_req:      data[4096] @4, size:u16 @4100, rtype:u8 @4102
//   uhid_get_report_req:  id:u32 @4, rnum:u8 @8, rtype:u8 @9
//   uhid_set_report_req:  id:u32 @4, rnum:u8 @8, rtype:u8 @9, size:u16 @10, data @12
//   uhid_input2_req:      size:u16 @4, data @6
//   uhid_get_report_reply_req: id:u32 @4, err:u16 @8, size:u16 @10, data @12
//   uhid_set_report_reply_req: id:u32 @4, err:u16 @8
constexpr size_t kOutputData = 4;
constexpr size_t kOutputSize = 4 + kDataMax;
constexpr size_t kOutputRtype = kOutputSize + 2;
constexpr size_t kReqId = 4;
constexpr size_t kReqRnum = 8;
constexpr size_t kReqRtype = 9;
constexpr size_t kSetSize = 10;
constexpr size_t kSetData = 12;

template <typename T> T read_at(const uint8_t *buf, size_t at) {
	T v;
	std::memcpy(&v, buf + at, sizeof(v));
	return v;
}

template <typename T> void write_at(std::vector<uint8_t> &buf, size_t at, T v) {
	std::memcpy(buf.data() + at, &v, sizeof(v));
}

std::vector<uint8_t> event(uint32_t type, size_t len) {
	std::vector<uint8_t> ev(len, 0);
	write_at(ev, 0, type);
	return ev;
}

} // namespace

bool parse(const uint8_t *buf, size_t len, Event *out) {
	if (len < 4) {
		return false;
	}
	*out = Event{};
	out->type = read_at<uint32_t>(buf, 0);
	switch (out->type) {
	case kOutput:
		if (len < kOutputRtype + 1) {
			return false;
		}
		out->size = read_at<uint16_t>(buf, kOutputSize);
		out->rtype = buf[kOutputRtype];
		out->data = buf + kOutputData;
		return out->size <= kDataMax;
	case kGetReport:
	case kSetReport:
		if (len < kReqRtype + 1) {
			return false;
		}
		out->id = read_at<uint32_t>(buf, kReqId);
		out->rnum = buf[kReqRnum];
		out->rtype = buf[kReqRtype];
		if (out->type == kSetReport) {
			if (len < kSetData) {
				return false;
			}
			out->size = read_at<uint16_t>(buf, kSetSize);
			out->data = buf + kSetData;
			return out->size <= kDataMax && kSetData + out->size <= len;
		}
		return true;
	default: return true; // START, STOP, OPEN, CLOSE: the type is all there is
	}
}

std::vector<uint8_t> input2(const uint8_t *data, size_t len) {
	len = std::min(len, kDataMax);
	std::vector<uint8_t> ev = event(kInput2, 6 + len);
	write_at(ev, 4, (uint16_t)len);
	if (len) {
		std::memcpy(ev.data() + 6, data, len);
	}
	return ev;
}

std::vector<uint8_t> get_report_reply(uint32_t id, uint16_t err, const uint8_t *data, size_t len) {
	len = err ? 0 : std::min(len, kDataMax);
	std::vector<uint8_t> ev = event(kGetReportReply, 12 + len);
	write_at(ev, 4, id);
	write_at(ev, 8, err);
	write_at(ev, 10, (uint16_t)len);
	if (len) {
		std::memcpy(ev.data() + 12, data, len);
	}
	return ev;
}

std::vector<uint8_t> set_report_reply(uint32_t id, uint16_t err) {
	std::vector<uint8_t> ev = event(kSetReportReply, 10);
	write_at(ev, 4, id);
	write_at(ev, 8, err);
	return ev;
}

} // namespace wraith::uhid
