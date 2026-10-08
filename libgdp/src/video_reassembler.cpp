// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

#include "gdp/video_reassembler.hpp"

namespace gdp {

bool VideoFrameReassembler::push(const VideoDatagramHeader &hdr, const uint8_t *payload, size_t payload_len,
	Frame *out) {
	Pending &pending = pending_[hdr.stream_id];
	if (!pending.active || pending.frame_id != hdr.frame_id) {
		// New frame (or the first one ever for this stream_id): whatever
		// was pending before is now an incomplete, undeliverable frame.
		pending = Pending{};
		pending.active = true;
		pending.frame_id = hdr.frame_id;
		pending.keyframe = hdr.keyframe();
		pending.pts = hdr.pts;
		pending.slices.resize(hdr.slice_count);
		pending.received.assign(hdr.slice_count, false);
	}

	if (hdr.slice_idx >= pending.slices.size() || pending.received[hdr.slice_idx]) {
		return false; // out-of-range or duplicate slice
	}
	pending.slices[hdr.slice_idx].assign(payload, payload + payload_len);
	pending.received[hdr.slice_idx] = true;
	pending.slices_received++;

	if (pending.slices_received < pending.slices.size()) {
		return false;
	}

	out->stream_id = hdr.stream_id;
	out->frame_id = pending.frame_id;
	out->keyframe = pending.keyframe;
	out->pts = pending.pts;
	out->data.clear();
	size_t total = 0;
	for (const auto &slice : pending.slices) {
		total += slice.size();
	}
	out->data.reserve(total);
	for (const auto &slice : pending.slices) {
		out->data.insert(out->data.end(), slice.begin(), slice.end());
	}
	pending.active = false;
	return true;
}

} // namespace gdp
