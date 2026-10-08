// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// libFuzzer target for what a session connection's control stream does
// with bytes from a peer that hasn't authenticated yet (gdp-spec.md §6.3
// "Before authentication"): FrameReader at the pre-auth size cap, then
// ControlEnvelope parsing -- the only code wraith runs for a stranger.
// Build with GDP_ENABLE_FUZZING=ON (needs clang).
//
// The first input byte picks how the rest is chunked, since QUIC
// delivers stream data in arbitrary pieces and FrameReader must give the
// same answer however a frame is split.
#include "gdp/framing.hpp"
#include "session.pb.h"

#include <algorithm>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 1) {
		return 0;
	}
	size_t chunk = static_cast<size_t>(data[0]) + 1;
	data++;
	size--;

	gdp::FrameReader reader;
	reader.set_max_frame_size(gdp::kMaxPreAuthFrameSize);
	gdp::session::ControlEnvelope env;
	for (size_t off = 0; off < size; off += chunk) {
		size_t n = std::min(chunk, size - off);
		bool ok = reader.feed_and_drain(data + off, n, &env, [&] {
			// What handle_hello() reads before the token check.
			if (env.has_hello()) {
				const auto &hello = env.hello();
				volatile size_t sink = hello.token().size() + hello.codecs_size() +
					hello.capabilities_size() + hello.displays_size();
				(void)sink;
			}
			return false; // wraith stops at the first frame, pass or fail
		});
		if (!ok || reader.last_result() == gdp::FrameReader::Result::kOk) {
			break;
		}
	}
	return 0;
}
