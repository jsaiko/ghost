// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/opus_encode.hpp"

#include <opus/opus.h>

namespace wraith {

OpusEncoderWrapper::OpusEncoderWrapper() = default;

OpusEncoderWrapper::~OpusEncoderWrapper() {
	close();
}

bool OpusEncoderWrapper::open(const gdp::AudioFormat &format) {
	close();

	// RESTRICTED_LOWDELAY disables Opus's own look-ahead: this is a live
	// interactive stream, so zero algorithmic delay is worth a little
	// compression efficiency.
	int err = 0;
	enc_ = opus_encoder_create((opus_int32)format.sample_rate_hz, (int)format.channels,
		OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
	if (err != OPUS_OK || !enc_) {
		enc_ = nullptr;
		return false;
	}

	opus_encoder_ctl(enc_, OPUS_SET_BITRATE(64000 * (int)format.channels));

	format_ = format;
	return true;
}

std::vector<uint8_t> OpusEncoderWrapper::encode(const int16_t *pcm) {
	if (!enc_) {
		return {};
	}
	// Opus packets are always well under a QUIC datagram's payload budget
	// (gdp/datagram.hpp's kFallbackDatagramPayload) even at high complexity/max
	// bitrate for 10ms stereo frames, so a fixed buffer is fine -- no
	// slicing needed on send, unlike video.
	std::vector<uint8_t> out(4000);
	int n = opus_encode(enc_, pcm, (int)format_.samples_per_frame(), out.data(), (opus_int32)out.size());
	if (n < 0) {
		return {};
	}
	out.resize((size_t)n);
	return out;
}

void OpusEncoderWrapper::close() {
	if (enc_) {
		opus_encoder_destroy(enc_);
		enc_ = nullptr;
	}
}

} // namespace wraith
