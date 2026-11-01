// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/video_reassembler.hpp"

#include "gdp/fec.hpp"

#include <array>

namespace gdp {

namespace {

// a is older than b, modulo 2^32 (frame_ids wrap, gdp-spec.md §9.1).
bool older(uint32_t a, uint32_t b) {
	return (int32_t)(a - b) < 0;
}

} // namespace

VideoFrameReassembler::Pending *VideoFrameReassembler::frame_for(Stream &stream, uint32_t frame_id,
	bool keyframe, uint32_t pts) {
	if (stream.delivered_any && !older(stream.last_delivered, frame_id)) {
		if (stream.last_delivered - frame_id <= kMaxFrameAge) {
			return nullptr; // a late datagram of a frame delivered or given up on
		}
		// Far behind: not a late datagram but a sender whose frame_ids
		// started over, which §9.1 forbids; start over with it rather than
		// ignore it for good.
		stream = Stream{};
	}
	for (Pending &frame : stream.frames) {
		if (frame.frame_id == frame_id) {
			return &frame;
		}
	}
	if (stream.frames.size() >= kMaxFramesInProgress) {
		size_t oldest = 0;
		for (size_t i = 1; i < stream.frames.size(); i++) {
			if (older(stream.frames[i].frame_id, stream.frames[oldest].frame_id)) {
				oldest = i;
			}
		}
		if (older(frame_id, stream.frames[oldest].frame_id)) {
			return nullptr; // older than every frame kept: not worth evicting one for
		}
		count_finished(stream.frames[oldest]);
		stream.frames.erase(stream.frames.begin() + (ptrdiff_t)oldest);
	}
	Pending frame;
	frame.frame_id = frame_id;
	frame.keyframe = keyframe;
	frame.pts = pts;
	stream.frames.push_back(std::move(frame));
	return &stream.frames.back();
}

void VideoFrameReassembler::size_slices(Pending &frame, uint16_t slice_count) {
	frame.slice_count = slice_count;
	frame.slices.resize(slice_count);
	frame.received.assign(slice_count, false);
}

bool VideoFrameReassembler::push(const VideoDatagramHeader &hdr, const uint8_t *payload, size_t payload_len,
	Frame *out) {
	Stream &stream = streams_[hdr.stream_id];
	Pending *frame = frame_for(stream, hdr.frame_id, hdr.keyframe(), hdr.pts);
	if (!frame) {
		return false;
	}
	if (frame->slice_count == 0) {
		size_slices(*frame, hdr.slice_count);
	} else if (frame->slice_count != hdr.slice_count) {
		return false; // every slice of one frame carries the same count (§9.1)
	}
	if (hdr.slice_idx >= frame->slice_count || frame->received[hdr.slice_idx]) {
		return false; // out-of-range or duplicate slice
	}
	frame->slices[hdr.slice_idx].assign(payload, payload + payload_len);
	frame->received[hdr.slice_idx] = true;
	frame->slices_received++;

	size_t index = (size_t)(frame - stream.frames.data());
	if (frame->slices_received == frame->slice_count) {
		deliver(stream, hdr.stream_id, index, false, out);
		return true;
	}
	if (try_recover(*frame)) {
		deliver(stream, hdr.stream_id, index, true, out);
		return true;
	}
	return false;
}

bool VideoFrameReassembler::push_parity(const VideoParityHeader &hdr, const uint8_t *payload,
	size_t payload_len, Frame *out) {
	// The layout the shard implies (§9.7): it must be the one its frame was
	// sliced with, which every other datagram of the frame implies too.
	const size_t shard_len = payload_len;
	if (shard_len == 0 || shard_len > kMaxSlicePayload) {
		return false;
	}
	uint64_t slices = hdr.frame_len == 0 ? 1 : ((uint64_t)hdr.frame_len + shard_len - 1) / shard_len;
	if (slices > UINT16_MAX) {
		return false;
	}
	size_t blocks = fec_block_count((size_t)slices);
	if (blocks != hdr.block_count ||
		fec_block_data((size_t)slices, blocks, hdr.block_idx) != hdr.data_shards) {
		return false;
	}

	Stream &stream = streams_[hdr.stream_id];
	Pending *frame =
		frame_for(stream, hdr.frame_id, (hdr.flags & VideoDatagramHeader::kFlagKeyframe) != 0, hdr.pts);
	if (!frame) {
		return false;
	}
	if (frame->slice_count != 0 && frame->slice_count != slices) {
		return false;
	}
	if (frame->shard_len == 0) {
		frame->shard_len = shard_len;
		frame->frame_len = hdr.frame_len;
		frame->block_count = blocks;
		frame->parity.resize(blocks);
		frame->parity_received.resize(blocks);
	} else if (frame->shard_len != shard_len || frame->frame_len != hdr.frame_len) {
		return false;
	}
	if (frame->slice_count == 0) {
		size_slices(*frame, (uint16_t)slices);
	}

	auto &shards = frame->parity[hdr.block_idx];
	auto &received = frame->parity_received[hdr.block_idx];
	if (shards.empty()) {
		shards.resize(hdr.parity_count);
		received.assign(hdr.parity_count, false);
	} else if (shards.size() != hdr.parity_count) {
		return false;
	}
	if (received[hdr.parity_idx]) {
		return false;
	}
	shards[hdr.parity_idx].assign(payload, payload + payload_len);
	received[hdr.parity_idx] = true;
	frame->parity_total++;

	if (try_recover(*frame)) {
		deliver(stream, hdr.stream_id, (size_t)(frame - stream.frames.data()), true, out);
		return true;
	}
	return false;
}

bool VideoFrameReassembler::try_recover(Pending &frame) {
	if (frame.shard_len == 0 || frame.slice_count == 0 ||
		frame.slices_received + frame.parity_total < frame.slice_count) {
		return false;
	}
	const size_t slices = frame.slice_count;
	const size_t blocks = frame.block_count;
	const size_t shard_len = frame.shard_len;
	const size_t last_len = frame.frame_len - (slices - 1) * shard_len;

	std::array<size_t, kFecMaxBlocks> have{};
	std::array<size_t, kFecMaxBlocks> parity_have{};
	for (size_t i = 0; i < slices; i++) {
		if (!frame.received[i]) {
			continue;
		}
		// A slice that doesn't fit the layout can't take part: the frame is
		// inconsistent, and only its data alone could still complete it.
		if (frame.slices[i].size() != (i == slices - 1 ? last_len : shard_len)) {
			return false;
		}
		have[i % blocks]++;
	}
	for (size_t b = 0; b < blocks; b++) {
		for (bool got : frame.parity_received[b]) {
			parity_have[b] += got ? 1 : 0;
		}
		if (have[b] + parity_have[b] < fec_block_data(slices, blocks, b)) {
			return false;
		}
	}

	bool ok = true;
	for (size_t b = 0; b < blocks && ok; b++) {
		size_t k = fec_block_data(slices, blocks, b);
		if (have[b] == k) {
			continue;
		}
		std::array<uint8_t *, kFecMaxBlockData> data;
		std::array<bool, kFecMaxBlockData> have_data;
		for (size_t j = 0; j < k; j++) {
			size_t i = b + j * blocks;
			frame.slices[i].resize(shard_len); // zero-pads the short last slice
			data[j] = frame.slices[i].data();
			have_data[j] = frame.received[i];
		}
		const auto &shards = frame.parity[b];
		std::array<const uint8_t *, kFecMaxBlockShards> parity{};
		std::array<bool, kFecMaxBlockShards> have_parity{};
		for (size_t q = 0; q < shards.size(); q++) {
			have_parity[q] = frame.parity_received[b][q];
			parity[q] = shards[q].data();
		}
		ok = fec_recover(data.data(), have_data.data(), k, parity.data(), have_parity.data(), shards.size(),
			shard_len);
		if (ok) {
			for (size_t j = 0; j < k; j++) {
				frame.received[b + j * blocks] = true;
			}
		}
	}
	frame.slices[slices - 1].resize(last_len);
	return ok;
}

void VideoFrameReassembler::deliver(Stream &stream, uint8_t stream_id, size_t index, bool recovered,
	Frame *out) {
	Pending &frame = stream.frames[index];
	count_finished(frame);
	if (recovered) {
		stats_.frames_recovered++;
	}

	out->stream_id = stream_id;
	out->frame_id = frame.frame_id;
	out->keyframe = frame.keyframe;
	out->pts = frame.pts;
	out->recovered = recovered;
	out->data.clear();
	size_t total = 0;
	for (const auto &slice : frame.slices) {
		total += slice.size();
	}
	out->data.reserve(total);
	for (const auto &slice : frame.slices) {
		out->data.insert(out->data.end(), slice.begin(), slice.end());
	}

	// Frames are decoded in order, so an older one still incomplete can no
	// longer be used.
	const uint32_t delivered = frame.frame_id;
	stream.frames.erase(stream.frames.begin() + (ptrdiff_t)index);
	for (size_t i = 0; i < stream.frames.size();) {
		if (older(stream.frames[i].frame_id, delivered)) {
			count_finished(stream.frames[i]);
			stream.frames.erase(stream.frames.begin() + (ptrdiff_t)i);
		} else {
			i++;
		}
	}
	stream.delivered_any = true;
	stream.last_delivered = delivered;
}

void VideoFrameReassembler::count_finished(const Pending &frame) {
	stats_.slices_expected += frame.slice_count;
	stats_.slices_lost += frame.slice_count - frame.slices_received;
}

VideoFrameReassembler::Stats VideoFrameReassembler::take_stats() {
	Stats stats = stats_;
	stats_ = Stats{};
	return stats;
}

} // namespace gdp
