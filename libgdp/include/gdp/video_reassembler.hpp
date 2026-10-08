// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Puts sliced video datagrams (gdp-spec.md §9.2-§9.3, the receiving end
// of datagram.hpp's slice_video_frame) back together into whole coded
// frames. Kept out of datagram.hpp because it allocates: that header is
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
		std::vector<uint8_t> data;
	};

	// Feeds one already-decoded video datagram (VideoDatagramHeader::decode
	// output). Returns true exactly when this slice completed a frame,
	// which is moved into `*out`; false otherwise (`*out` untouched).
	//
	// One in-progress frame per stream_id. A new frame_id for the same
	// stream_id discards whatever was pending: nothing is retransmitted on
	// a datagram channel, so a still-incomplete older frame is simply a
	// lost one. Out-of-range and duplicate slices are ignored.
	bool push(const VideoDatagramHeader &hdr, const uint8_t *payload, size_t payload_len, Frame *out);

private:
	struct Pending {
		bool active = false;
		uint32_t frame_id = 0;
		uint16_t slices_received = 0;
		bool keyframe = false;
		uint32_t pts = 0;
		std::vector<std::vector<uint8_t>> slices;
		// Tracked separately from slices[i].empty(): a zero-length slice
		// is legal on the wire and must still count as received exactly
		// once.
		std::vector<bool> received;
	};
	std::map<uint8_t, Pending> pending_;
};

} // namespace gdp
