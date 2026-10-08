// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Plays session audio on the default SDL3 output device, pull-model: SDL's
// audio thread asks for data whenever the device needs it, and the stream
// callback fills it one frame at a time from `pull` (JitterBuffer::pull).
// Playback pacing therefore follows the device clock and never depends on
// the main loop being responsive -- jitter_buffer.hpp says why that
// matters.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

struct SDL_AudioStream;

namespace spectre {

class AudioPlayer {
public:
	// Fills `pcm_out` with one frame (frame_samples interleaved S16) and
	// returns true, or returns false to play silence for that frame. Called
	// from SDL's audio thread.
	using PullFn = std::function<bool(int16_t *pcm_out)>;

	AudioPlayer();
	~AudioPlayer();

	AudioPlayer(const AudioPlayer &) = delete;
	AudioPlayer &operator=(const AudioPlayer &) = delete;

	// Requires SDL_INIT_AUDIO to already have been done (stream_session.cpp).
	// `frame_samples` is samples per frame *including* channels (i.e.
	// samples_per_frame()*channels() in OpusDecodeWrapper's terms).
	bool open(uint32_t sample_rate_hz, uint32_t channels, size_t frame_samples, PullFn pull);

	void close();
	bool is_open() const { return stream_ != nullptr; }

	// While muted, frames are still pulled (the jitter buffer keeps
	// draining, so unmuting plays what's current) but silence is played.
	void set_muted(bool muted) { muted_.store(muted, std::memory_order_relaxed); }
	bool muted() const { return muted_.load(std::memory_order_relaxed); }

private:
	static void stream_callback(void *userdata, SDL_AudioStream *stream, int additional_amount,
		int total_amount);

	SDL_AudioStream *stream_ = nullptr;
	PullFn pull_;
	std::vector<int16_t> frame_;
	std::atomic<bool> muted_{false};
};

} // namespace spectre
