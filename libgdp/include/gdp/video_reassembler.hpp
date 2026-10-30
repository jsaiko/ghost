// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Puts sliced video datagrams (gdp-spec.md §9.2-§9.3, the receiving end
// of datagram.hpp's slice_video_frame) back together into whole coded
// frames, rebuilding lost slices from parity (§9.7) when "fec" is in
// effect. Kept out of datagram.hpp because it allocates: that header is
// the alloc-free, fuzzed wire-format layer, this is the buffering policy
// on top of it.
#pragma once

#include "gdp/datagram.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace gdp {

class VideoFrameReassembler {
public:
	struct Frame {
		uint8_t stream_id = 0;
		uint32_t frame_id = 0;
		bool keyframe = false;
		uint32_t pts = 0;
		// Some of its slices were rebuilt from parity.
		bool recovered = false;
		std::vector<uint8_t> data;
	};

	// Data slices of the frames this reassembler finished with -- delivered
	// or given up on -- since the last take_stats(), before any recovery:
	// StreamStats' slices_expected and slices_lost (gdp-spec.md §7.5).
	// Frames none of whose datagrams arrived aren't counted.
	struct Stats {
		uint64_t slices_expected = 0;
		uint64_t slices_lost = 0;
		uint32_t frames_recovered = 0;
	};

	// Feeds one already-decoded video datagram (VideoDatagramHeader::decode
	// output). Returns true exactly when this slice completed a frame,
	// which is moved into `*out`; false otherwise (`*out` untouched).
	//
	// Up to kMaxFramesInProgress frames per stream_id are kept, so a slice
	// or parity shard of one frame arriving after the next frame's first
	// datagram still counts. Frames are delivered in frame_id order: once
	// one is delivered, every older one still incomplete is given up as
	// lost (nothing is retransmitted on a datagram channel), and anything
	// arriving for them later is ignored. Out-of-range, inconsistent and
	// duplicate slices are ignored.
	bool push(const VideoDatagramHeader &hdr, const uint8_t *payload, size_t payload_len, Frame *out);

	// Feeds one parity datagram (VideoParityHeader::decode output), with
	// the same contract as push(): true when it let a frame be rebuilt.
	bool push_parity(const VideoParityHeader &hdr, const uint8_t *payload, size_t payload_len, Frame *out);

	Stats take_stats();

	static constexpr size_t kMaxFramesInProgress = 3;
	// A datagram this many frames or fewer older than the last frame
	// delivered is late, and ignored; one further back restarts the stream.
	static constexpr uint32_t kMaxFrameAge = 64;

private:
	struct Pending {
		uint32_t frame_id = 0;
		bool keyframe = false;
		uint32_t pts = 0;
		// 0 until a data slice or parity shard says.
		uint16_t slice_count = 0;
		uint32_t slices_received = 0;
		std::vector<std::vector<uint8_t>> slices;
		// Tracked separately from slices[i].empty(): a zero-length slice
		// is legal on the wire and must still count as received exactly
		// once.
		std::vector<bool> received;

		// The FEC layout, from the first parity shard: shard_len 0 until then.
		size_t shard_len = 0;
		uint32_t frame_len = 0;
		size_t block_count = 0;
		// Per block: its parity shards by index (empty until one arrives),
		// which arrived, and how many.
		std::vector<std::vector<std::vector<uint8_t>>> parity;
		std::vector<std::vector<bool>> parity_received;
		size_t parity_total = 0;
	};
	struct Stream {
		std::vector<Pending> frames; // in progress, any order
		bool delivered_any = false;
		uint32_t last_delivered = 0;
	};

	// The frame `frame_id` is for, made if new; null when it's no longer
	// wanted (as old as the last one delivered).
	Pending *frame_for(Stream &stream, uint32_t frame_id, bool keyframe, uint32_t pts);
	void size_slices(Pending &frame, uint16_t slice_count);
	// Rebuilds the frame's missing slices if every block has enough.
	bool try_recover(Pending &frame);
	// Hands the frame over and gives up on every older one.
	void deliver(Stream &stream, uint8_t stream_id, size_t index, bool recovered, Frame *out);
	void count_finished(const Pending &frame);

	std::map<uint8_t, Stream> streams_;
	Stats stats_;
};

} // namespace gdp
