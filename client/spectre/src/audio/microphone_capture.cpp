// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/microphone_capture.hpp"

#include <SDL3/SDL.h>
#include <opus/opus.h>

namespace spectre {

MicrophoneCapture::MicrophoneCapture() = default;

MicrophoneCapture::~MicrophoneCapture() {
	close();
}

bool MicrophoneCapture::open(const gdp::AudioFormat &format, SendFn send) {
	close();
	if (format.samples_per_frame() == 0) {
		return false;
	}

	int err = 0;
	enc_ = opus_encoder_create((opus_int32)format.sample_rate_hz, (int)format.channels, OPUS_APPLICATION_VOIP,
		&err);
	if (err != OPUS_OK || !enc_) {
		enc_ = nullptr;
		return false;
	}
	opus_encoder_ctl(enc_, OPUS_SET_BITRATE(32000 * (int)format.channels));
	opus_encoder_ctl(enc_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

	format_ = format;
	send_ = std::move(send);
	frame_.assign(format.samples_per_frame_all_channels(), 0);
	packet_.assign(1500, 0);

	SDL_AudioSpec spec{};
	spec.format = SDL_AUDIO_S16;
	spec.channels = (int)format.channels;
	spec.freq = (int)format.sample_rate_hz;

	stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, stream_callback, this);
	if (!stream_) {
		close();
		return false;
	}
	SDL_ResumeAudioStreamDevice(stream_); // opened paused, like playback
	return true;
}

// SDL's audio thread, stream locked. SDL converts the device's native
// format to `spec`, so what comes out of the stream is already ours.
void MicrophoneCapture::stream_callback(void *userdata, SDL_AudioStream *stream, int additional_amount,
	int total_amount) {
	(void)additional_amount;
	(void)total_amount;
	auto *self = static_cast<MicrophoneCapture *>(userdata);
	const int frame_bytes = (int)(self->frame_.size() * sizeof(int16_t));
	while (SDL_GetAudioStreamAvailable(stream) >= frame_bytes) {
		if (SDL_GetAudioStreamData(stream, self->frame_.data(), frame_bytes) != frame_bytes) {
			break;
		}
		if (self->muted_.load(std::memory_order_relaxed)) {
			continue;
		}
		int n = opus_encode(self->enc_, self->frame_.data(), (int)self->format_.samples_per_frame(),
			self->packet_.data(), (opus_int32)self->packet_.size());
		if (n > 0 && self->send_) {
			self->send_(self->packet_.data(), (size_t)n);
		}
	}
}

void MicrophoneCapture::close() {
	if (stream_) {
		SDL_DestroyAudioStream(stream_); // closes the device and joins the callback
		stream_ = nullptr;
	}
	if (enc_) {
		opus_encoder_destroy(enc_);
		enc_ = nullptr;
	}
	send_ = nullptr;
}

} // namespace spectre
