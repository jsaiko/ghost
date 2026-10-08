// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Reorders audio datagrams and hands them to the Opus decoder at the audio
// device's own pace.
//
// Two threads meet here: push() runs on the main loop (session_client.cpp's
// on_audio_frame, from dispatch()), pull() runs on SDL's audio thread
// (audio_player.cpp's stream callback). Playback is deliberately *not*
// paced from the main loop: that loop also runs video decode and a
// vsync-blocking present() (vulkan_presenter.cpp), so during active video
// it stalls for most of every ~16ms frame -- far too irregular to feed a
// device that needs a 10ms frame every 10ms. Letting the device thread pull
// exactly what it needs when it needs it sidesteps that entirely, and also
// ties pacing to the device clock rather than std::chrono, so sender/
// receiver clock drift shows up only as slow buffer growth/shrink handled
// by the overrun/underrun paths below.
//
// `seq` wraps at 2^16 (gdp-spec.md §10) -- comparisons use signed 16-bit
// deltas, the same wraparound-safe idiom gdp::pts_diff uses for the 32-bit
// frame_id/pts fields (see session_client.cpp's note_frame_seen).
#pragma once

#include "audio/opus_decode.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace spectre {

class JitterBuffer {
public:
	// Frames buffered before playback starts, and the ceiling before
	// overrun trimming kicks in. 6 frames (60ms @ 10ms/frame) covers the
	// main loop being blocked for a couple of vsyncs (datagrams are only
	// read from the network when it runs) plus wraith's bursty delivery
	// (PipeWire hands it ~21ms of audio at a time, so packets arrive 2-3 at
	// once). Not adaptive.
	static constexpr size_t kTargetDepthFrames = 6;
	static constexpr size_t kMaxBufferedFrames = 4 * kTargetDepthFrames;

	explicit JitterBuffer(OpusDecodeWrapper *decoder);

	// Main thread. Stale packets (already played) are dropped silently --
	// no retransmit on this channel, same philosophy as video's frame
	// reassembly.
	void push(uint16_t seq, uint32_t pts, const uint8_t *data, size_t len);

	// Audio thread. Fills `pcm_out` (samples_per_frame()*channels()
	// interleaved S16) with the next frame and returns true, or returns
	// false when there's nothing to play yet (still buffering after start
	// or after an underrun) -- the caller outputs silence for that frame.
	bool pull(int16_t *pcm_out);

private:
	static int16_t seq_delta(uint16_t a, uint16_t b) { return (int16_t)(a - b); }

	// The buffered seq with the smallest signed delta from expected_seq_
	// (i.e. the one closest to/behind what's next due). Never buffer_.empty().
	uint16_t oldest_buffered_seq_locked() const;

	OpusDecodeWrapper *decoder_;

	std::mutex mutex_;
	std::map<uint16_t, std::vector<uint8_t>> buffer_;
	bool started_ = false;
	uint16_t expected_seq_ = 0;
};

} // namespace spectre
