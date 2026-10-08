// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// session/uhid_codec.cpp against linux/uhid.h's own structs: the offsets
// it writes and reads are the kernel's.

#include "session/uhid_codec.hpp"

#include <linux/uhid.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

using namespace wraith;

namespace {

int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

} // namespace

static_assert(uhid::kEventSize == sizeof(uhid_event));
static_assert(uhid::kDataMax == UHID_DATA_MAX);

int main() {
	// Input reports land where uhid_input2_req has them.
	const uint8_t report[] = {0x01, 0x7f, 0x80, 0x10};
	auto in = uhid::input2(report, sizeof(report));
	uhid_event ev{};
	std::memcpy(&ev, in.data(), in.size());
	CHECK(ev.type == UHID_INPUT2);
	CHECK(ev.u.input2.size == sizeof(report));
	CHECK(std::memcmp(ev.u.input2.data, report, sizeof(report)) == 0);

	// Replies.
	const uint8_t feature[] = {0x05, 1, 2, 3};
	auto get = uhid::get_report_reply(42, 0, feature, sizeof(feature));
	ev = uhid_event{};
	std::memcpy(&ev, get.data(), get.size());
	CHECK(ev.type == UHID_GET_REPORT_REPLY);
	CHECK(ev.u.get_report_reply.id == 42 && ev.u.get_report_reply.err == 0);
	CHECK(ev.u.get_report_reply.size == sizeof(feature));
	CHECK(std::memcmp(ev.u.get_report_reply.data, feature, sizeof(feature)) == 0);
	auto failed = uhid::get_report_reply(7, 5, feature, sizeof(feature));
	ev = uhid_event{};
	std::memcpy(&ev, failed.data(), failed.size());
	CHECK(ev.u.get_report_reply.err == 5 && ev.u.get_report_reply.size == 0);
	auto set = uhid::set_report_reply(9, 0);
	ev = uhid_event{};
	std::memcpy(&ev, set.data(), set.size());
	CHECK(ev.type == UHID_SET_REPORT_REPLY && ev.u.set_report_reply.id == 9);

	// The driver's events, as the kernel lays them out.
	uhid_event out{};
	out.type = UHID_OUTPUT;
	out.u.output.size = 3;
	out.u.output.rtype = UHID_OUTPUT_REPORT;
	out.u.output.data[0] = 0x11;
	uhid::Event parsed;
	CHECK(uhid::parse(reinterpret_cast<const uint8_t *>(&out), sizeof(out), &parsed));
	CHECK(parsed.type == uhid::kOutput && parsed.size == 3 && parsed.data[0] == 0x11);
	CHECK(parsed.rtype == uhid::kOutputReport);

	uhid_event req{};
	req.type = UHID_GET_REPORT;
	req.u.get_report.id = 3;
	req.u.get_report.rnum = 0xa3;
	req.u.get_report.rtype = UHID_FEATURE_REPORT;
	CHECK(uhid::parse(reinterpret_cast<const uint8_t *>(&req), sizeof(req), &parsed));
	CHECK(parsed.type == uhid::kGetReport && parsed.id == 3 && parsed.rnum == 0xa3);
	CHECK(parsed.rtype == uhid::kFeatureReport);

	req = uhid_event{};
	req.type = UHID_SET_REPORT;
	req.u.set_report.id = 4;
	req.u.set_report.rnum = 0x08;
	req.u.set_report.rtype = UHID_FEATURE_REPORT;
	req.u.set_report.size = 2;
	req.u.set_report.data[0] = 0x08;
	req.u.set_report.data[1] = 0x07;
	CHECK(uhid::parse(reinterpret_cast<const uint8_t *>(&req), sizeof(req), &parsed));
	CHECK(parsed.type == uhid::kSetReport && parsed.id == 4 && parsed.size == 2 && parsed.data[1] == 0x07);

	// Too short for what the type promises.
	CHECK(!uhid::parse(reinterpret_cast<const uint8_t *>(&req), 8, &parsed));

	if (g_failures) {
		fprintf(stderr, "%d failure(s)\n", g_failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
