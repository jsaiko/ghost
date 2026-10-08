// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// libFuzzer target for the datagram header parsers (gdp-spec.md §3.2): untrusted network bytes
// reach hand-written buffer arithmetic here rather than in a generated protobuf parser
// (refine_fuzzer.cpp covers the other such place). Build with GDP_ENABLE_FUZZING=ON (needs clang).
#include "gdp/datagram.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	auto channel = gdp::peek_channel(data, size);
	if (!channel) {
		return 0;
	}

	const uint8_t *payload = nullptr;
	size_t payload_len = 0;

	if (*channel == gdp::DatagramChannel::kVideo) {
		gdp::VideoDatagramHeader hdr;
		if (gdp::VideoDatagramHeader::decode(data, size, &hdr, &payload, &payload_len)) {
			// Touch the reported payload bounds so a mislocated payload
			// (off-by-one in decode()) shows up under ASan.
			if (payload_len > 0) {
				volatile uint8_t sink = payload[0];
				sink ^= payload[payload_len - 1];
				(void)sink;
			}
		}
	} else if (*channel == gdp::DatagramChannel::kAudio) {
		gdp::AudioDatagramHeader hdr;
		if (gdp::AudioDatagramHeader::decode(data, size, &hdr, &payload, &payload_len)) {
			if (payload_len > 0) {
				volatile uint8_t sink = payload[0];
				sink ^= payload[payload_len - 1];
				(void)sink;
			}
		}
	}
	return 0;
}
