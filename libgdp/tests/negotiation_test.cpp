// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/error_codes.hpp"
#include "gdp/negotiation.hpp"

// Plain assert()s: make sure a Release build (-DNDEBUG) can't compile them
// away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
	using Strings = std::vector<std::string>;

	// Codec tokens map to and from the enum; anything else is Unknown.
	assert(gdp::video_codec_from_token("h264") == gdp::VideoCodec::H264);
	assert(gdp::video_codec_from_token("h265") == gdp::VideoCodec::H265);
	assert(gdp::video_codec_from_token("av1") == gdp::VideoCodec::AV1);
	assert(gdp::video_codec_from_token("vp9") == gdp::VideoCodec::Unknown);
	assert(strcmp(gdp::video_codec_token(gdp::VideoCodec::H265), "h265") == 0);
	assert(strcmp(gdp::video_codec_token(gdp::VideoCodec::Unknown), "") == 0);

	// Codec selection follows the client's preference order, not the
	// server's, and keeps the whole intersection so the server can fall
	// past a codec whose encoder won't open.
	assert(gdp::common_video_codecs({"av1", "h264"}, {"h264", "av1"}) == (Strings{"av1", "h264"}));
	assert(gdp::common_video_codecs({"h264", "av1"}, {"h264", "av1"}) == (Strings{"h264", "av1"}));
	assert(gdp::common_video_codecs({"av1", "vp9", "h264"}, {"h264", "av1"}) == (Strings{"av1", "h264"}));
	assert(gdp::common_video_codecs({"h264", "h264"}, {"h264"}) == (Strings{"h264"}));
	// No overlap, or nothing offered at all: no codec, never a guess.
	assert(gdp::common_video_codecs({"vp9"}, {"h264"}).empty());
	assert(gdp::common_video_codecs({}, {"h264"}).empty());
	assert(gdp::common_video_codecs({"h264"}, {}).empty());

	// Capabilities: the intersection, in offered order, deduplicated.
	Strings negotiated = gdp::negotiate_capabilities({"b", "a", "b", "zzz"}, {"a", "b", "c"});
	assert((negotiated == Strings{"b", "a"}));
	assert(gdp::has_capability(negotiated, "a"));
	assert(!gdp::has_capability(negotiated, "c"));   // server-only: not negotiated
	assert(!gdp::has_capability(negotiated, "zzz")); // client-only: not negotiated
	assert(gdp::negotiate_capabilities({}, {"a"}).empty());
	assert(gdp::negotiate_capabilities({"a"}, {}).empty());

	// Error-code text: known codes get words, unknown ones stay numeric so
	// a newer peer's code is still visible in the log.
	assert(gdp::describe_error_code(static_cast<uint64_t>(gdp::ErrorCode::kAuthFailed)) ==
		"authentication failed");
	assert(gdp::describe_error_code(999) == "application error 999");

	printf("negotiation_test: all checks passed\n");
	return 0;
}
