// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The bytes of linux/uhid.h's events, for a raw controller's uhid fd
// (session/raw_controllers.hpp). Every event is a u32 type and a packed
// union in host byte order; ghostseat wrote UHID_CREATE2 already, so this
// side only reads the driver's events and writes input reports and
// replies. Pure, so tests/uhid_codec_test.cpp covers the layout without
// a device.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wraith::uhid {

// sizeof(struct uhid_event): the type plus the union, whose largest
// member is uhid_create2_req (4372 bytes), padded to 4376 by the legacy
// uhid_create_req's pointer. A read returns at most this.
inline constexpr size_t kEventSize = 4 + 4376;
// UHID_DATA_MAX: the most report bytes any event carries.
inline constexpr size_t kDataMax = 4096;

// enum uhid_event_type, the ones this side sees or sends.
enum Type : uint32_t {
	kStart = 2,
	kStop = 3,
	kOpen = 4,
	kClose = 5,
	kOutput = 6,
	kGetReport = 9,
	kGetReportReply = 10,
	kInput2 = 12,
	kSetReport = 13,
	kSetReportReply = 14,
};

// enum uhid_report_type.
enum ReportType : uint8_t {
	kFeatureReport = 0,
	kOutputReport = 1,
	kInputReport = 2,
};

// One event read from the fd. `data`/`size` point into the read buffer
// (UHID_OUTPUT, UHID_SET_REPORT); `id`, `rnum` and `rtype` are set for
// the two requests and `rtype` for UHID_OUTPUT.
struct Event {
	uint32_t type = 0;
	uint32_t id = 0;
	uint8_t rnum = 0;
	uint8_t rtype = 0;
	const uint8_t *data = nullptr;
	size_t size = 0;
};

// False for a buffer too short for its type, or a report size past
// kDataMax (neither of which the kernel produces).
bool parse(const uint8_t *buf, size_t len, Event *out);

// UHID_INPUT2 with one input report; `len` is clamped to kDataMax.
std::vector<uint8_t> input2(const uint8_t *data, size_t len);
// UHID_GET_REPORT_REPLY: `err` 0 with the report, or an errno with none.
std::vector<uint8_t> get_report_reply(uint32_t id, uint16_t err, const uint8_t *data, size_t len);
// UHID_SET_REPORT_REPLY.
std::vector<uint8_t> set_report_reply(uint32_t id, uint16_t err);

} // namespace wraith::uhid
