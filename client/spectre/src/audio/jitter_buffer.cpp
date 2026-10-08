// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/jitter_buffer.hpp"

#include <iterator>

namespace spectre {

JitterBuffer::JitterBuffer(OpusDecodeWrapper *decoder) : decoder_(decoder) {}

void JitterBuffer::push(uint16_t seq, uint32_t pts, const uint8_t *data, size_t len) {
	(void)pts; // audio isn't synced to video: the device clock paces playback

	std::lock_guard<std::mutex> lock(mutex_);

	if (started_ && seq_delta(seq, expected_seq_) < 0) {
		return; // already played: stale
	}

	if (!started_ && buffer_.empty()) {
		// First frame since session start (or since an underrun reset us in
		// pull()): definitionally the earliest one we have, by arrival
		// order. Anchoring on arrival order rather than "numerically
		// smallest buffered key" matters once a session has run past the
		// seq counter's 2^16 wrap (gdp-spec.md §10) -- past that point a
		// freshly-wrapped low seq value is *not* actually the oldest frame.
		expected_seq_ = seq;
	} else if (!started_ && seq_delta(seq, expected_seq_) < 0) {
		// Still buffering, and this one was sent before the frame we
		// anchored on (the network reordered them): it is the earlier
		// start. Buffered behind the anchor it could never be played --
		// and, never drained, it would keep the buffer from ever looking
		// empty, so an outage would go on concealing forever instead of
		// re-buffering. Far behind, it's a straggler from before the
		// underrun, not a reordering.
		if (seq_delta(expected_seq_, seq) > (int16_t)kMaxBufferedFrames) {
			return;
		}
		expected_seq_ = seq;
	}

	buffer_[seq].assign(data, data + len);

	if (!started_ && buffer_.size() >= kTargetDepthFrames) {
		started_ = true;
	}

	// Overrun (wraith's capture clock running faster than the playback
	// device drains it): walk latency back down by dropping the oldest
	// buffered frame rather than resetting everything. "Oldest" is judged
	// by signed delta from expected_seq_, not raw key order, for the same
	// wraparound reason as above.
	bool dropped_expected = false;
	while (buffer_.size() > kMaxBufferedFrames) {
		uint16_t oldest = oldest_buffered_seq_locked();
		if (oldest == expected_seq_) {
			dropped_expected = true;
		}
		buffer_.erase(oldest);
	}
	if (dropped_expected && !buffer_.empty()) {
		expected_seq_ = oldest_buffered_seq_locked();
	}
}

uint16_t JitterBuffer::oldest_buffered_seq_locked() const {
	auto it = buffer_.begin();
	uint16_t oldest = it->first;
	int16_t oldest_delta = seq_delta(oldest, expected_seq_);
	for (++it; it != buffer_.end(); ++it) {
		int16_t delta = seq_delta(it->first, expected_seq_);
		if (delta < oldest_delta) {
			oldest_delta = delta;
			oldest = it->first;
		}
	}
	return oldest;
}

bool JitterBuffer::pull(int16_t *pcm_out) {
	std::lock_guard<std::mutex> lock(mutex_);

	if (!started_) {
		return false;
	}

	// Anything behind what's next can never be played. push() keeps such
	// frames out, but one left here would make the buffer look non-empty
	// through an outage: every pull would conceal and advance
	// expected_seq_ 100 times a second past the real stream, and once
	// packets came back every one would be dropped as stale -- audio gone
	// for good. Without them, an outage is the underrun below.
	for (auto it = buffer_.begin(); it != buffer_.end();) {
		it = seq_delta(it->first, expected_seq_) < 0 ? buffer_.erase(it) : std::next(it);
	}

	if (buffer_.empty()) {
		// Underrun: nothing at all buffered, not just the next frame
		// missing. Either the source went quiet (wraith's seq counter is
		// then frozen -- guessing ahead with PLC would desync us from it,
		// and every real packet after the silence would look stale) or
		// the playback device is slowly outrunning wraith's capture clock.
		// Both want the same thing: stop, re-buffer to target depth, and
		// re-anchor expected_seq_ on whatever arrives next.
		started_ = false;
		return false;
	}

	auto it = buffer_.find(expected_seq_);
	if (it != buffer_.end()) {
		if (!decoder_->decode(it->second.data(), it->second.size(), pcm_out)) {
			decoder_->decode_plc(pcm_out);
		}
		buffer_.erase(it);
	} else {
		// Later frames are here but this one isn't: lost (or late enough
		// to count as lost). Opus's PLC synthesizes it from decoder state
		// instead of leaving a hard gap.
		decoder_->decode_plc(pcm_out);
	}

	expected_seq_ = (uint16_t)(expected_seq_ + 1);
	return true;
}

} // namespace spectre
