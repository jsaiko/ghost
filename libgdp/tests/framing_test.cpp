// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/framing.hpp"

#include "lobby.pb.h"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

static void test_single_frame_roundtrip() {
	gdp::lobby::LobbyEnvelope env;
	env.mutable_hello()->set_protocol_version(1);
	env.mutable_hello()->set_client_id("test-client");

	std::vector<uint8_t> buf;
	bool encoded = gdp::encode_frame(env, &buf);
	assert(encoded);
	assert(buf.size() == gdp::kLengthPrefixSize + env.ByteSizeLong());

	gdp::FrameReader reader;
	reader.feed(buf.data(), buf.size());

	gdp::lobby::LobbyEnvelope out;
	auto result = reader.drain(&out);
	assert(result == gdp::FrameReader::Result::kOk);
	assert(out.has_hello());
	assert(out.hello().protocol_version() == 1);
	assert(out.hello().client_id() == "test-client");

	// Nothing left buffered.
	result = reader.drain(&out);
	assert(result == gdp::FrameReader::Result::kIncomplete);
}

static void test_partial_feed() {
	gdp::lobby::LobbyEnvelope env;
	env.mutable_error()->set_code(gdp::lobby::LOBBY_ERROR_AUTH_FAILED);
	env.mutable_error()->set_message("bad password");

	std::vector<uint8_t> buf;
	bool encoded = gdp::encode_frame(env, &buf);
	assert(encoded);

	gdp::FrameReader reader;
	gdp::lobby::LobbyEnvelope out;

	// Feed one byte at a time; should stay kIncomplete until the last byte.
	for (size_t i = 0; i + 1 < buf.size(); i++) {
		reader.feed(&buf[i], 1);
		auto result = reader.drain(&out);
		assert(result == gdp::FrameReader::Result::kIncomplete);
	}
	reader.feed(&buf[buf.size() - 1], 1);
	auto result = reader.drain(&out);
	assert(result == gdp::FrameReader::Result::kOk);
	assert(out.error().code() == gdp::lobby::LOBBY_ERROR_AUTH_FAILED);
	assert(out.error().message() == "bad password");
}

static void test_multiple_frames_in_one_feed() {
	gdp::lobby::LobbyEnvelope a, b;
	a.mutable_hello()->set_client_id("a");
	b.mutable_hello()->set_client_id("b");

	std::vector<uint8_t> buf;
	bool encoded = gdp::encode_frame(a, &buf);
	assert(encoded);
	encoded = gdp::encode_frame(b, &buf);
	assert(encoded);

	gdp::FrameReader reader;
	reader.feed(buf.data(), buf.size());

	gdp::lobby::LobbyEnvelope out;
	auto result = reader.drain(&out);
	assert(result == gdp::FrameReader::Result::kOk);
	assert(out.hello().client_id() == "a");
	result = reader.drain(&out);
	assert(result == gdp::FrameReader::Result::kOk);
	assert(out.hello().client_id() == "b");
	result = reader.drain(&out);
	assert(result == gdp::FrameReader::Result::kIncomplete);
}

static void test_too_large_length_prefix() {
	std::vector<uint8_t> buf(gdp::kLengthPrefixSize, 0);
	uint32_t huge = static_cast<uint32_t>(gdp::kMaxFrameSize) + 1;
	buf[0] = static_cast<uint8_t>(huge);
	buf[1] = static_cast<uint8_t>(huge >> 8);
	buf[2] = static_cast<uint8_t>(huge >> 16);
	buf[3] = static_cast<uint8_t>(huge >> 24);

	gdp::FrameReader reader;
	reader.feed(buf.data(), buf.size());
	gdp::lobby::LobbyEnvelope out;
	auto result = reader.drain(&out);
	assert(result == gdp::FrameReader::Result::kTooLarge);
}

static void test_lowered_max_frame_size() {
	gdp::lobby::LobbyEnvelope env;
	env.mutable_hello()->set_client_id(std::string(100, 'x'));
	std::vector<uint8_t> buf;
	bool encoded = gdp::encode_frame(env, &buf);
	assert(encoded);
	size_t payload = buf.size() - gdp::kLengthPrefixSize;

	// Exactly at the limit still parses; one byte under it doesn't.
	gdp::FrameReader at_limit;
	at_limit.set_max_frame_size(payload);
	at_limit.feed(buf.data(), buf.size());
	gdp::lobby::LobbyEnvelope out;
	assert(at_limit.drain(&out) == gdp::FrameReader::Result::kOk);

	gdp::FrameReader under_limit;
	under_limit.set_max_frame_size(payload - 1);
	under_limit.feed(buf.data(), buf.size());
	assert(under_limit.drain(&out) == gdp::FrameReader::Result::kTooLarge);

	// Raising it back is what a reader does once its peer authenticates;
	// nothing above kMaxFrameSize is ever accepted.
	under_limit.set_max_frame_size(gdp::kMaxFrameSize * 2);
	assert(under_limit.max_frame_size() == gdp::kMaxFrameSize);
	assert(under_limit.drain(&out) == gdp::FrameReader::Result::kOk);
}

static void test_malformed_payload() {
	// A length prefix claiming 5 garbage bytes that don't parse as a
	// LobbyEnvelope.
	std::vector<uint8_t> buf = {5, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff};
	gdp::FrameReader reader;
	reader.feed(buf.data(), buf.size());
	gdp::lobby::LobbyEnvelope out;
	auto result = reader.drain(&out);
	assert(result == gdp::FrameReader::Result::kMalformed);
}

int main() {
	test_single_frame_roundtrip();
	test_partial_feed();
	test_multiple_frames_in_one_feed();
	test_too_large_length_prefix();
	test_lowered_max_frame_size();
	test_malformed_payload();
	printf("framing_test: all tests passed\n");
	return 0;
}
