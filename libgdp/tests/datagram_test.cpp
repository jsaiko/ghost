// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Plain-assert unit tests, no external framework: run via ctest, exits
// non-zero (via assert()) on first failure.
#include "gdp/datagram.hpp"
#include "gdp/video_reassembler.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace gdp;

static void test_video_roundtrip() {
	VideoDatagramHeader hdr;
	hdr.stream_id = 3;
	hdr.frame_id = 0xdeadbeef;
	hdr.slice_idx = 1;
	hdr.slice_count = 4;
	hdr.flags = VideoDatagramHeader::kFlagKeyframe;
	hdr.pts = 123456789;
	hdr.host_latency = 1234;

	uint8_t payload[] = {0xaa, 0xbb, 0xcc};
	std::vector<uint8_t> buf(1 + VideoDatagramHeader::kSize + sizeof(payload));
	hdr.encode(buf.data());
	memcpy(buf.data() + 1 + VideoDatagramHeader::kSize, payload, sizeof(payload));

	VideoDatagramHeader out;
	const uint8_t *out_payload = nullptr;
	size_t out_payload_len = 0;
	bool ok = VideoDatagramHeader::decode(buf.data(), buf.size(), &out, &out_payload, &out_payload_len);
	assert(ok);
	assert(out.stream_id == hdr.stream_id);
	assert(out.frame_id == hdr.frame_id);
	assert(out.slice_idx == hdr.slice_idx);
	assert(out.slice_count == hdr.slice_count);
	assert(out.flags == hdr.flags);
	assert(out.keyframe());
	assert(out.pts == hdr.pts);
	assert(out.host_latency == 1234);
	assert(out.host_latency_us() == 12340);
	assert(out_payload_len == sizeof(payload));
	assert(memcmp(out_payload, payload, sizeof(payload)) == 0);

	assert(peek_channel(buf.data(), buf.size()) == DatagramChannel::kVideo);
}

static void test_host_latency_units() {
	assert(VideoDatagramHeader::host_latency_from_us(-5) == 0);
	assert(VideoDatagramHeader::host_latency_from_us(0) == 1); // reported, so never 0
	assert(VideoDatagramHeader::host_latency_from_us(14) == 1);
	assert(VideoDatagramHeader::host_latency_from_us(15) == 2);
	assert(VideoDatagramHeader::host_latency_from_us(10'900) == 1090);
	assert(VideoDatagramHeader::host_latency_from_us(10'000'000) == 0xFFFF);
	VideoDatagramHeader hdr;
	assert(hdr.host_latency_us() == 0);
}

static void test_video_rejects_short_and_bad_slice_index() {
	VideoDatagramHeader out;
	const uint8_t *payload;
	size_t payload_len;

	uint8_t too_short[10] = {};
	assert(!VideoDatagramHeader::decode(too_short, sizeof(too_short), &out, &payload, &payload_len));

	VideoDatagramHeader hdr;
	hdr.slice_idx = 5;
	hdr.slice_count = 3; // idx >= count: invalid
	uint8_t buf[1 + VideoDatagramHeader::kSize];
	hdr.encode(buf);
	assert(!VideoDatagramHeader::decode(buf, sizeof(buf), &out, &payload, &payload_len));

	VideoDatagramHeader zero_count;
	zero_count.slice_count = 0;
	uint8_t buf2[1 + VideoDatagramHeader::kSize];
	zero_count.encode(buf2);
	assert(!VideoDatagramHeader::decode(buf2, sizeof(buf2), &out, &payload, &payload_len));
}

static void test_audio_roundtrip() {
	AudioDatagramHeader hdr;
	hdr.seq = 0xfffe; // near wraparound
	hdr.pts = 42;

	uint8_t payload[] = {1, 2, 3, 4, 5};
	std::vector<uint8_t> buf(1 + AudioDatagramHeader::kSize + sizeof(payload));
	hdr.encode(buf.data());
	memcpy(buf.data() + 1 + AudioDatagramHeader::kSize, payload, sizeof(payload));

	AudioDatagramHeader out;
	const uint8_t *out_payload = nullptr;
	size_t out_payload_len = 0;
	bool ok = AudioDatagramHeader::decode(buf.data(), buf.size(), &out, &out_payload, &out_payload_len);
	assert(ok);
	assert(out.seq == hdr.seq);
	assert(out.pts == hdr.pts);
	assert(out_payload_len == sizeof(payload));
	assert(memcmp(out_payload, payload, sizeof(payload)) == 0);

	assert(peek_channel(buf.data(), buf.size()) == DatagramChannel::kAudio);
}

static void test_microphone_roundtrip() {
	AudioDatagramHeader hdr;
	hdr.seq = 7;
	hdr.pts = 99;
	uint8_t payload[] = {9, 8, 7};
	std::vector<uint8_t> buf(1 + AudioDatagramHeader::kSize + sizeof(payload));
	hdr.encode(buf.data(), DatagramChannel::kMicrophone);
	memcpy(buf.data() + 1 + AudioDatagramHeader::kSize, payload, sizeof(payload));

	assert(peek_channel(buf.data(), buf.size()) == DatagramChannel::kMicrophone);
	AudioDatagramHeader out;
	const uint8_t *out_payload = nullptr;
	size_t out_payload_len = 0;
	// Only decodes as the channel it was encoded for.
	assert(!AudioDatagramHeader::decode(buf.data(), buf.size(), &out, &out_payload, &out_payload_len));
	assert(AudioDatagramHeader::decode(buf.data(), buf.size(), &out, &out_payload, &out_payload_len,
		DatagramChannel::kMicrophone));
	assert(out.seq == 7 && out.pts == 99 && out_payload_len == sizeof(payload));
}

static void test_peek_channel_edge_cases() {
	assert(!peek_channel(nullptr, 0).has_value());
	uint8_t unknown[1] = {0xff};
	assert(!peek_channel(unknown, 1).has_value());
}

static void test_channel_cross_decode_rejected() {
	// A video header handed to the audio decoder (wrong channel byte) must
	// fail, not silently misparse.
	VideoDatagramHeader vhdr;
	uint8_t vbuf[1 + VideoDatagramHeader::kSize];
	vhdr.encode(vbuf);

	AudioDatagramHeader aout;
	const uint8_t *payload;
	size_t payload_len;
	assert(!AudioDatagramHeader::decode(vbuf, sizeof(vbuf), &aout, &payload, &payload_len));
}

// Feeds every datagram `slice_video_frame` emits for `frame` straight into
// `reassembler` (in the order `order` picks -- identity by default), and
// returns the completed frame, asserting exactly one completes.
static VideoFrameReassembler::Frame slice_and_reassemble(VideoFrameReassembler &reassembler,
	const std::vector<uint8_t> &frame, std::vector<size_t> order = {},
	size_t max_payload = kFallbackDatagramPayload) {
	std::vector<std::vector<uint8_t>> datagrams;
	VideoDatagramHeader proto;
	proto.stream_id = 7;
	proto.frame_id = 99;
	proto.flags = VideoDatagramHeader::kFlagKeyframe;
	proto.pts = 5000;
	bool ok =
		slice_video_frame(proto, frame.data(), frame.size(), max_payload, [&](const uint8_t *d, size_t len) {
			datagrams.emplace_back(d, d + len);
			return true;
		});
	assert(ok);
	if (order.empty()) {
		for (size_t i = 0; i < datagrams.size(); i++)
			order.push_back(i);
	}
	assert(order.size() == datagrams.size());

	VideoFrameReassembler::Frame out;
	int completed = 0;
	for (size_t i : order) {
		VideoDatagramHeader hdr;
		const uint8_t *payload;
		size_t payload_len;
		assert(VideoDatagramHeader::decode(datagrams[i].data(), datagrams[i].size(), &hdr, &payload,
			&payload_len));
		assert(hdr.slice_count == datagrams.size());
		assert(hdr.slice_idx == i);
		// Every slice but the last is full, at the clamped slice size.
		size_t slice = max_payload < kMaxSlicePayload ? max_payload : kMaxSlicePayload;
		assert(payload_len <= slice);
		assert(i + 1 == datagrams.size() || payload_len == slice);
		if (reassembler.push(hdr, payload, payload_len, &out)) completed++;
	}
	assert(completed == 1);
	assert(out.stream_id == 7 && out.frame_id == 99 && out.keyframe && out.pts == 5000);
	return out;
}

static void test_slice_and_reassemble() {
	VideoFrameReassembler reassembler;

	// Fits in one datagram.
	std::vector<uint8_t> small(100, 0x5a);
	assert(slice_and_reassemble(reassembler, small).data == small);

	// Exactly two full payloads plus a partial third, in order.
	std::vector<uint8_t> big(kFallbackDatagramPayload * 2 + 17);
	for (size_t i = 0; i < big.size(); i++)
		big[i] = (uint8_t)i;
	assert(slice_and_reassemble(reassembler, big).data == big);

	// Same frame, slices arriving out of order.
	assert(slice_and_reassemble(reassembler, big, {2, 0, 1}).data == big);

	// The same bytes at a larger slice size (a path MTU discovery raised)
	// are fewer slices, and still reassemble: slice size is the sender's
	// choice and the receiver needs no notice of it.
	assert(slice_and_reassemble(reassembler, big, {0, 1}, 1400).data == big);

	// A frame an exact multiple of the slice size has no short tail.
	std::vector<uint8_t> exact(1400 * 3, 0x33);
	assert(slice_and_reassemble(reassembler, exact, {}, 1400).data == exact);

	// Anything past kMaxSlicePayload is clamped to it.
	std::vector<uint8_t> huge(kMaxSlicePayload + 1, 0x44);
	assert(slice_and_reassemble(reassembler, huge, {0, 1}, 65535).data == huge);

	// An empty frame is still one (empty) datagram.
	std::vector<uint8_t> empty;
	assert(slice_and_reassemble(reassembler, empty).data.empty());
}

static void test_slice_payload_for() {
	// Unknown (0), or too small for the channel byte and header: fallback.
	assert(slice_payload_for(0) == kFallbackDatagramPayload);
	assert(slice_payload_for(1 + VideoDatagramHeader::kSize) == kFallbackDatagramPayload);
	// Otherwise the datagram less the channel byte and header...
	assert(slice_payload_for(1200) == 1200 - 1 - VideoDatagramHeader::kSize);
	assert(slice_payload_for(1452) == 1452 - 1 - VideoDatagramHeader::kSize);
	// ...capped at kMaxSlicePayload.
	assert(slice_payload_for(65535) == kMaxSlicePayload);
}

static void test_reassembler_discards_incomplete_and_duplicates() {
	VideoFrameReassembler reassembler;
	VideoFrameReassembler::Frame out;
	uint8_t payload[4] = {1, 2, 3, 4};

	// Slice 0 of frame 1 (a 2-slice frame) arrives, then frame 2 starts:
	// frame 1 is abandoned, and frame 2's remaining slice completes it.
	VideoDatagramHeader hdr;
	hdr.frame_id = 1;
	hdr.slice_idx = 0;
	hdr.slice_count = 2;
	assert(!reassembler.push(hdr, payload, 4, &out));
	hdr.frame_id = 2;
	assert(!reassembler.push(hdr, payload, 4, &out));
	// Frame 1's late slice 1 must not complete anything (it's a new frame_id
	// again as far as the reassembler is concerned, and incomplete).
	hdr.frame_id = 1;
	hdr.slice_idx = 1;
	assert(!reassembler.push(hdr, payload, 4, &out));
	// Back to frame 2: its slice 0 is gone now (frame 1 evicted it), so
	// slice 1 alone can't complete it either -- resend both.
	hdr.frame_id = 2;
	hdr.slice_idx = 1;
	assert(!reassembler.push(hdr, payload, 4, &out));
	hdr.slice_idx = 1; // duplicate: ignored, not double-counted
	assert(!reassembler.push(hdr, payload, 4, &out));
	hdr.slice_idx = 0;
	assert(reassembler.push(hdr, payload, 4, &out));
	assert(out.frame_id == 2 && out.data.size() == 8);

	// Streams are independent: a frame on stream 1 doesn't disturb stream 0.
	hdr.stream_id = 0;
	hdr.frame_id = 3;
	hdr.slice_idx = 0;
	assert(!reassembler.push(hdr, payload, 4, &out));
	hdr.stream_id = 1;
	hdr.slice_count = 1;
	assert(reassembler.push(hdr, payload, 4, &out));
	assert(out.stream_id == 1);
	hdr.stream_id = 0;
	hdr.slice_count = 2;
	hdr.slice_idx = 1;
	assert(reassembler.push(hdr, payload, 4, &out));
	assert(out.stream_id == 0 && out.frame_id == 3);
}

int main() {
	test_video_roundtrip();
	test_host_latency_units();
	test_slice_and_reassemble();
	test_slice_payload_for();
	test_reassembler_discards_incomplete_and_duplicates();
	test_video_rejects_short_and_bad_slice_index();
	test_audio_roundtrip();
	test_microphone_roundtrip();
	test_peek_channel_edge_cases();
	test_channel_cross_decode_rejected();
	printf("datagram_test: all tests passed\n");
	return 0;
}
