// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Plain-assert unit tests for forward error correction (gdp/fec.hpp,
// gdp-spec.md §9.7): the erasure code, the parity datagram header, and
// frames sliced, protected, partly dropped and reassembled.
#include "gdp/datagram.hpp"
#include "gdp/fec.hpp"
#include "gdp/video_reassembler.hpp"

// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

using namespace gdp;

namespace {

std::mt19937 rng(12345);

std::vector<uint8_t> random_bytes(size_t len) {
	std::vector<uint8_t> out(len);
	for (auto &b : out) {
		b = (uint8_t)rng();
	}
	return out;
}

// Encodes k random shards (the last `short_by` bytes short) with m parity,
// drops the data shards in `lost_data` and the parity in `lost_parity`, and
// checks recovery succeeds exactly when it should, with the right bytes.
void round_trip(size_t k, size_t m, size_t shard_len, const std::vector<bool> &lost_data,
	const std::vector<bool> &lost_parity, size_t short_by = 0) {
	std::vector<std::vector<uint8_t>> data(k);
	std::vector<const uint8_t *> data_ptrs(k);
	std::vector<size_t> data_lens(k);
	for (size_t i = 0; i < k; i++) {
		data[i] = random_bytes(i == k - 1 ? shard_len - short_by : shard_len);
		data_ptrs[i] = data[i].data();
		data_lens[i] = data[i].size();
	}
	std::vector<std::vector<uint8_t>> parity(m, std::vector<uint8_t>(shard_len));
	std::vector<uint8_t *> parity_ptrs(m);
	for (size_t j = 0; j < m; j++) {
		parity_ptrs[j] = parity[j].data();
	}
	fec_encode(data_ptrs.data(), data_lens.data(), k, parity_ptrs.data(), m, shard_len);

	std::vector<std::vector<uint8_t>> work(k);
	std::vector<uint8_t *> work_ptrs(k);
	std::unique_ptr<bool[]> have_data(new bool[k]);
	std::unique_ptr<bool[]> have_parity(new bool[m ? m : 1]);
	size_t missing = 0, available = 0;
	for (size_t i = 0; i < k; i++) {
		have_data[i] = !lost_data[i];
		work[i] = have_data[i] ? data[i] : std::vector<uint8_t>(shard_len, 0xee);
		work[i].resize(shard_len); // zero-pads the short one
		work_ptrs[i] = work[i].data();
		missing += lost_data[i] ? 1 : 0;
	}
	std::vector<const uint8_t *> parity_in(m);
	for (size_t j = 0; j < m; j++) {
		have_parity[j] = !lost_parity[j];
		parity_in[j] = parity[j].data();
		available += have_parity[j] ? 1 : 0;
	}
	bool ok =
		fec_recover(work_ptrs.data(), have_data.get(), k, parity_in.data(), have_parity.get(), m, shard_len);
	assert(ok == (available >= missing));
	if (!ok) {
		return;
	}
	for (size_t i = 0; i < k; i++) {
		assert(memcmp(work[i].data(), data[i].data(), data[i].size()) == 0);
		for (size_t b = data[i].size(); b < shard_len; b++) {
			assert(work[i][b] == 0); // the padding comes back as zeros
		}
	}
}

void test_every_erasure_pattern_small() {
	// Every pattern of lost data and parity shards for small blocks.
	for (size_t k = 1; k <= 5; k++) {
		for (size_t m = 1; m <= 4; m++) {
			for (unsigned mask = 0; mask < (1u << (k + m)); mask++) {
				std::vector<bool> lost_data(k), lost_parity(m);
				for (size_t i = 0; i < k; i++) {
					lost_data[i] = (mask >> i) & 1;
				}
				for (size_t j = 0; j < m; j++) {
					lost_parity[j] = (mask >> (k + j)) & 1;
				}
				// Odd lengths exercise every SIMD tail.
				round_trip(k, m, 37, lost_data, lost_parity, k > 1 ? 11 : 0);
			}
		}
	}
}

void test_random_patterns_large() {
	// Full-size blocks: kFecMaxBlockData data shards and the most parity
	// the block can carry, and random losses up to and past what's
	// recoverable.
	const size_t sizes[][2] = {{kFecMaxBlockData, 16},
		{kFecMaxBlockData, kFecMaxBlockShards - kFecMaxBlockData}, {200, 55}, {17, 3}};
	for (const auto &size : sizes) {
		size_t k = size[0], m = size[1];
		for (int trial = 0; trial < 20; trial++) {
			std::vector<bool> lost_data(k), lost_parity(m);
			size_t losses = rng() % (m + 3);
			for (size_t n = 0; n < losses; n++) {
				size_t at = rng() % (k + m);
				if (at < k) {
					lost_data[at] = true;
				} else {
					lost_parity[at - k] = true;
				}
			}
			round_trip(k, m, 1400, lost_data, lost_parity, rng() % 1400);
		}
	}
}

void test_nothing_missing_needs_no_parity() {
	round_trip(4, 2, 100, {false, false, false, false}, {true, true});
}

void test_parity_header() {
	VideoParityHeader hdr;
	hdr.stream_id = 2;
	hdr.frame_id = 0xfeedface;
	hdr.block_idx = 3;
	hdr.block_count = 5;
	hdr.data_shards = 60;
	hdr.parity_idx = 7;
	hdr.parity_count = 12;
	hdr.flags = VideoDatagramHeader::kFlagKeyframe;
	hdr.pts = 987654321;
	hdr.frame_len = 412345;
	uint8_t buf[1 + VideoParityHeader::kSize + 3] = {};
	hdr.encode(buf);
	buf[1 + VideoParityHeader::kSize] = 0xab;
	assert(peek_channel(buf, sizeof(buf)) == DatagramChannel::kVideoParity);

	VideoParityHeader out;
	const uint8_t *payload = nullptr;
	size_t payload_len = 0;
	assert(VideoParityHeader::decode(buf, sizeof(buf), &out, &payload, &payload_len));
	assert(out.stream_id == 2 && out.frame_id == 0xfeedface && out.block_idx == 3 && out.block_count == 5);
	assert(out.data_shards == 60 && out.parity_idx == 7 && out.parity_count == 12);
	assert(
		out.flags == VideoDatagramHeader::kFlagKeyframe && out.pts == 987654321 && out.frame_len == 412345);
	assert(payload == buf + 1 + VideoParityHeader::kSize && payload_len == 3 && payload[0] == 0xab);

	// No payload, a short header, indices outside their counts, and too
	// many shards for one block are all refused.
	assert(!VideoParityHeader::decode(buf, 1 + VideoParityHeader::kSize, &out, &payload, &payload_len));
	auto refused = [&](auto change) {
		VideoParityHeader bad = hdr;
		change(bad);
		uint8_t b[1 + VideoParityHeader::kSize + 1] = {};
		bad.encode(b);
		return !VideoParityHeader::decode(b, sizeof(b), &out, &payload, &payload_len);
	};
	assert(refused([](VideoParityHeader &h) { h.block_idx = h.block_count; }));
	assert(refused([](VideoParityHeader &h) { h.block_count = 0; }));
	assert(refused([](VideoParityHeader &h) { h.data_shards = 0; }));
	assert(refused([](VideoParityHeader &h) { h.parity_idx = h.parity_count; }));
	assert(refused([](VideoParityHeader &h) { h.parity_count = 0; }));
	assert(refused([](VideoParityHeader &h) {
		h.data_shards = 200;
		h.parity_count = 56;
	}));
	// A video decoder doesn't take it, nor it a video datagram.
	VideoDatagramHeader vout;
	assert(!VideoDatagramHeader::decode(buf, sizeof(buf), &vout, &payload, &payload_len));
	uint8_t vbuf[1 + VideoDatagramHeader::kSize + 8] = {};
	VideoDatagramHeader{}.encode(vbuf);
	assert(!VideoParityHeader::decode(vbuf, sizeof(vbuf), &out, &payload, &payload_len));
}

void test_layout() {
	assert(fec_block_count(0) == 0);
	assert(fec_block_count(1) == 1);
	assert(fec_block_count(kFecMaxBlockData) == 1);
	assert(fec_block_count(kFecMaxBlockData + 1) == 2);
	assert(fec_block_count(kFecMaxBlocks * kFecMaxBlockData) == kFecMaxBlocks);
	assert(fec_block_count(kFecMaxBlocks * kFecMaxBlockData + 1) == 0); // too big to protect
	// The blocks' data shards add up, none over the limit.
	for (size_t n : {1, 2, 63, 64, 65, 700, 16320}) {
		size_t blocks = fec_block_count(n), total = 0;
		for (size_t b = 0; b < blocks; b++) {
			size_t k = fec_block_data(n, blocks, b);
			assert(k >= 1 && k <= kFecMaxBlockData);
			total += k;
		}
		assert(total == n);
	}
	assert(fec_slice_payload_for(0) == kFallbackDatagramPayload);
	assert(fec_slice_payload_for(1452) == 1452 - 1 - VideoParityHeader::kSize);
	assert(fec_slice_payload_for(65535) == kMaxSlicePayload);
	// A shard always fits where the slice of the same length does.
	assert(fec_slice_payload_for(1452) < slice_payload_for(1452));
}

struct Datagram {
	bool parity;
	std::vector<uint8_t> bytes;
};

// Slices `frame` and adds `parity_for` parity as wraith does: data, then
// parity, in send order.
std::vector<Datagram> send_frame(const std::vector<uint8_t> &frame, uint32_t frame_id, size_t max_payload,
	const std::function<size_t(size_t)> &parity_for, bool keyframe = true) {
	std::vector<Datagram> out;
	VideoDatagramHeader hdr;
	hdr.stream_id = 0;
	hdr.frame_id = frame_id;
	hdr.flags = keyframe ? VideoDatagramHeader::kFlagKeyframe : 0;
	hdr.pts = 4242;
	slice_video_frame(hdr, frame.data(), frame.size(), max_payload, [&](const uint8_t *d, size_t len) {
		out.push_back({false, std::vector<uint8_t>(d, d + len)});
		return true;
	});
	std::vector<uint8_t> scratch;
	send_video_parity(
		hdr, frame.data(), frame.size(), max_payload, parity_for,
		[&](const uint8_t *d, size_t len) {
			out.push_back({true, std::vector<uint8_t>(d, d + len)});
			return true;
		},
		&scratch);
	return out;
}

// Feeds `datagrams` but those `drop` selects; returns how many frames
// completed, the last into *out.
int receive(VideoFrameReassembler &reassembler, const std::vector<Datagram> &datagrams,
	const std::function<bool(size_t)> &drop, VideoFrameReassembler::Frame *out) {
	int completed = 0;
	for (size_t i = 0; i < datagrams.size(); i++) {
		if (drop(i)) {
			continue;
		}
		const auto &d = datagrams[i].bytes;
		const uint8_t *payload;
		size_t payload_len;
		if (datagrams[i].parity) {
			VideoParityHeader hdr;
			assert(VideoParityHeader::decode(d.data(), d.size(), &hdr, &payload, &payload_len));
			completed += reassembler.push_parity(hdr, payload, payload_len, out) ? 1 : 0;
		} else {
			VideoDatagramHeader hdr;
			assert(VideoDatagramHeader::decode(d.data(), d.size(), &hdr, &payload, &payload_len));
			completed += reassembler.push(hdr, payload, payload_len, out) ? 1 : 0;
		}
	}
	return completed;
}

void test_frames_recovered_through_the_reassembler() {
	const size_t max_payload = fec_slice_payload_for(1452);
	auto ten_percent = [](size_t k) { return (k + 9) / 10; };
	uint32_t frame_id = 0;

	// Each frame: its size, and which datagrams to drop.
	struct Case {
		size_t len;
		std::function<bool(size_t)> drop;
		bool recovered;
	};
	std::vector<Case> cases = {
		{100, [](size_t i) { return i == 0; }, true}, // a one-slice frame from its parity alone
		{max_payload * 5 + 9, [](size_t i) { return i == 5; }, true}, // the short last slice
		{max_payload * 3, [](size_t) { return false; }, false},       // nothing lost: done before the parity
		{max_payload * 700, [](size_t i) { return i >= 100 && i < 111; }, true}, // a burst over 11 blocks
		{max_payload * 64, [](size_t i) { return i >= 3 && i < 11; }, false},    // 8 lost, 7 parity: lost
	};
	for (const Case &c : cases) {
		VideoFrameReassembler reassembler;
		std::vector<uint8_t> frame = random_bytes(c.len);
		auto datagrams = send_frame(frame, frame_id++, max_payload, ten_percent);
		for (const Datagram &d : datagrams) {
			assert(d.bytes.size() <= 1452); // parity fits the datagram limit too
		}
		VideoFrameReassembler::Frame out;
		int completed = receive(reassembler, datagrams, c.drop, &out);
		bool lost = c.len == max_payload * 64;
		assert(completed == (lost ? 0 : 1));
		if (!lost) {
			assert(out.data == frame && out.keyframe && out.pts == 4242 && out.recovered == c.recovered);
			VideoFrameReassembler::Stats stats = reassembler.take_stats();
			assert(stats.frames_recovered == (c.recovered ? 1u : 0u));
			assert(stats.slices_expected == (c.len + max_payload - 1) / max_payload);
		}
	}

	// Parity arriving after the next frame's first slice still counts.
	{
		VideoFrameReassembler reassembler;
		std::vector<uint8_t> a = random_bytes(max_payload * 4), b = random_bytes(max_payload * 2);
		auto first = send_frame(a, 1, max_payload, ten_percent);
		auto second = send_frame(b, 2, max_payload, ten_percent);
		std::vector<Datagram> order;
		order.push_back(first[0]);
		order.push_back(first[1]);
		order.push_back(first[3]); // slice 2 lost
		order.push_back(second[0]);
		order.push_back(first[4]); // frame 1's parity, late
		order.push_back(second[1]);
		VideoFrameReassembler::Frame out;
		std::vector<uint32_t> ids;
		for (const Datagram &d : order) {
			if (receive(reassembler, {d}, [](size_t) { return false; }, &out)) {
				ids.push_back(out.frame_id);
			}
		}
		assert(ids.size() == 2 && ids[0] == 1 && ids[1] == 2);
	}

	// Parity that disagrees with the frame's slices is ignored, and can't
	// corrupt it: here, parity claiming a different frame length.
	{
		VideoFrameReassembler reassembler;
		std::vector<uint8_t> frame = random_bytes(max_payload * 3);
		auto datagrams = send_frame(frame, 7, max_payload, ten_percent);
		VideoParityHeader hdr;
		const uint8_t *payload;
		size_t payload_len;
		auto &bytes = datagrams.back().bytes;
		assert(VideoParityHeader::decode(bytes.data(), bytes.size(), &hdr, &payload, &payload_len));
		hdr.frame_len += (uint32_t)max_payload; // a fourth slice
		VideoFrameReassembler::Frame out;
		assert(!reassembler.push_parity(hdr, payload, payload_len, &out));
		hdr.frame_len -= (uint32_t)max_payload;
		hdr.data_shards = 2; // the wrong block size
		assert(!reassembler.push_parity(hdr, payload, payload_len, &out));
		assert(receive(reassembler, datagrams, [](size_t i) { return i == 1; }, &out) == 1);
		assert(out.data == frame && out.recovered);
	}

	// Parity of a frame larger than FEC protects is never sent.
	{
		size_t too_big = kFecMaxBlocks * kFecMaxBlockData + 1;
		std::vector<uint8_t> frame(too_big * 16, 1);
		auto datagrams = send_frame(frame, 9, 16, ten_percent);
		assert(datagrams.size() == too_big);
	}
}

void benchmark() {
	// A 4K keyframe's worth (about 1 MB) at 20%: what wraith pays per
	// frame, worst case.
	const size_t shard = 1431;
	std::vector<uint8_t> frame = random_bytes(1'000'000);
	VideoDatagramHeader hdr;
	std::vector<uint8_t> scratch;
	auto start = std::chrono::steady_clock::now();
	size_t parity = 0;
	const int rounds = 20;
	for (int i = 0; i < rounds; i++) {
		parity += send_video_parity(
			hdr, frame.data(), frame.size(), shard, [](size_t k) { return (k + 4) / 5; },
			[](const uint8_t *, size_t) { return true; }, &scratch);
	}
	double ms =
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / rounds;
	printf("fec_test: %s, 1 MB frame at 20%% parity (%zu bytes) in %.2f ms\n", fec_implementation(),
		parity / rounds, ms);
}

} // namespace

int main() {
	test_every_erasure_pattern_small();
	test_random_patterns_large();
	test_nothing_missing_needs_no_parity();
	test_parity_header();
	test_layout();
	test_frames_recovered_through_the_reassembler();
	benchmark();
	printf("fec_test: all tests passed\n");
	return 0;
}
