// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Thin libopus wrapper. Pure encode: no
// PipeWire, no threading, no GDP. spectre's OpusDecodeWrapper is the
// mirror image; both are opened from the same gdp::AudioFormat so their
// frame sizes agree by construction.
#pragma once

#include "gdp/audio_format.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

struct OpusEncoder;

namespace wraith {

class OpusEncoderWrapper {
public:
	OpusEncoderWrapper();
	~OpusEncoderWrapper();

	OpusEncoderWrapper(const OpusEncoderWrapper &) = delete;
	OpusEncoderWrapper &operator=(const OpusEncoderWrapper &) = delete;

	// format.frame_ms must be a valid Opus frame duration (10 ms in GDP
	// v1, gdp-spec.md §6.8).
	bool open(const gdp::AudioFormat &format);

	// `pcm` must be exactly samples_per_frame() * channels() interleaved
	// S16LE samples. Returns the encoded Opus packet, or an empty vector on
	// failure.
	std::vector<uint8_t> encode(const int16_t *pcm);

	uint32_t samples_per_frame() const { return format_.samples_per_frame(); }
	uint32_t channels() const { return format_.channels; }

	void close();

private:
	::OpusEncoder *enc_ = nullptr;
	gdp::AudioFormat format_;
};

} // namespace wraith
