// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/audio_player.hpp"

#include <SDL3/SDL.h>

#include <cstring>

namespace spectre {

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() {
	close();
}

bool AudioPlayer::open(uint32_t sample_rate_hz, uint32_t channels, size_t frame_samples, PullFn pull) {
	close();
	if (frame_samples == 0) {
		return false; // stream_callback() tops up in whole frames
	}

	pull_ = std::move(pull);
	frame_.assign(frame_samples, 0);

	SDL_AudioSpec spec{};
	spec.format = SDL_AUDIO_S16;
	spec.channels = (int)channels;
	spec.freq = (int)sample_rate_hz;

	stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, stream_callback, this);
	if (!stream_) {
		return false;
	}

	// SDL_OpenAudioDeviceStream() opens the device paused (SDL3 docs) --
	// nothing plays until this is called.
	SDL_ResumeAudioStreamDevice(stream_);
	return true;
}

// SDL's audio thread, with the stream locked for the duration (SDL3 docs);
// SDL_PutAudioStreamData() from inside the callback is explicitly allowed.
// `additional_amount` is how many bytes the device needs beyond what's
// already queued -- top up in whole frames until it's covered.
void AudioPlayer::stream_callback(void *userdata, SDL_AudioStream *stream, int additional_amount,
	int total_amount) {
	(void)total_amount;
	auto *self = static_cast<AudioPlayer *>(userdata);
	const int frame_bytes = (int)(self->frame_.size() * sizeof(int16_t));
	while (additional_amount > 0) {
		if (!self->pull_(self->frame_.data()) || self->muted_.load(std::memory_order_relaxed)) {
			std::memset(self->frame_.data(), 0, (size_t)frame_bytes);
		}
		SDL_PutAudioStreamData(stream, self->frame_.data(), frame_bytes);
		additional_amount -= frame_bytes;
	}
}

void AudioPlayer::close() {
	if (stream_) {
		SDL_DestroyAudioStream(stream_); // also closes the device (SDL3 docs)
		stream_ = nullptr;
	}
	pull_ = nullptr;
}

} // namespace spectre
