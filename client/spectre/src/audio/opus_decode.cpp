// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/opus_decode.hpp"

#include <opus/opus.h>

namespace spectre {

OpusDecodeWrapper::OpusDecodeWrapper() = default;

OpusDecodeWrapper::~OpusDecodeWrapper() {
	close();
}

bool OpusDecodeWrapper::open(const gdp::AudioFormat &format) {
	close();

	int err = 0;
	dec_ = opus_decoder_create((opus_int32)format.sample_rate_hz, (int)format.channels, &err);
	if (err != OPUS_OK || !dec_) {
		dec_ = nullptr;
		return false;
	}

	format_ = format;
	return true;
}

bool OpusDecodeWrapper::decode(const uint8_t *data, size_t len, int16_t *pcm_out) {
	if (!dec_) {
		return false;
	}
	int frame_samples = (int)format_.samples_per_frame();
	int n = opus_decode(dec_, data, (opus_int32)len, pcm_out, frame_samples, 0);
	return n == frame_samples;
}

void OpusDecodeWrapper::decode_plc(int16_t *pcm_out) {
	if (!dec_) {
		return;
	}
	// gdp-spec.md §10 / opus_decode(3): a null packet with the expected
	// frame size tells Opus's own concealment to synthesize this frame
	// from state left over by the last successfully decoded one.
	int n = opus_decode(dec_, nullptr, 0, pcm_out, (int)format_.samples_per_frame(), 0);
	(void)n; // GCC's warn_unused_result isn't silenced by a (void) cast on the call itself
}

void OpusDecodeWrapper::close() {
	if (dec_) {
		opus_decoder_destroy(dec_);
		dec_ = nullptr;
	}
}

} // namespace spectre
