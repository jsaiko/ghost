// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Captures the local default microphone through SDL3 and hands Opus
// packets to `send` (gdp-spec.md §10.1). The mirror of
// AudioPlayer + OpusDecodeWrapper: SDL's audio thread pushes captured PCM
// into the stream callback, which cuts it into frame_ms chunks and encodes
// each. `send` therefore runs on SDL's audio thread; SessionClient's
// send_microphone_packet() is built for that.
#pragma once

#include "gdp/audio_format.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

struct SDL_AudioStream;
struct OpusEncoder;

namespace spectre {

class MicrophoneCapture {
public:
	using SendFn = std::function<void(const uint8_t *packet, size_t len)>;

	MicrophoneCapture();
	~MicrophoneCapture();

	MicrophoneCapture(const MicrophoneCapture &) = delete;
	MicrophoneCapture &operator=(const MicrophoneCapture &) = delete;

	// Requires SDL_INIT_AUDIO. Fails (nothing opened) when there is no
	// recording device or the OS denies access.
	bool open(const gdp::AudioFormat &format, SendFn send);
	void close();
	bool is_open() const { return stream_ != nullptr; }

	// While muted the device stays open but nothing is encoded or sent.
	void set_muted(bool muted) { muted_.store(muted, std::memory_order_relaxed); }
	bool muted() const { return muted_.load(std::memory_order_relaxed); }

private:
	static void stream_callback(void *userdata, SDL_AudioStream *stream, int additional_amount,
		int total_amount);

	SDL_AudioStream *stream_ = nullptr;
	OpusEncoder *enc_ = nullptr;
	gdp::AudioFormat format_;
	SendFn send_;
	std::atomic<bool> muted_{false};
	std::vector<int16_t> frame_;
	std::vector<uint8_t> packet_;
};

} // namespace spectre
