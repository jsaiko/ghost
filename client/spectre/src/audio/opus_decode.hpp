// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Thin libopus decoder wrapper. Pure decode,
// no jitter/reorder logic (see jitter_buffer.hpp) and no SDL -- easy to
// exercise standalone against a captured Opus stream. wraith's
// OpusEncoderWrapper is the mirror image; both are opened from the same
// gdp::AudioFormat so their frame sizes agree by construction.
#pragma once

#include "gdp/audio_format.hpp"

#include <cstddef>
#include <cstdint>

struct OpusDecoder;

namespace spectre {

class OpusDecodeWrapper {
public:
	OpusDecodeWrapper();
	~OpusDecodeWrapper();

	OpusDecodeWrapper(const OpusDecodeWrapper &) = delete;
	OpusDecodeWrapper &operator=(const OpusDecodeWrapper &) = delete;

	// `format` is SessionAccept.audio as wraith sent it.
	bool open(const gdp::AudioFormat &format);

	uint32_t channels() const { return format_.channels; }
	uint32_t samples_per_frame() const { return format_.samples_per_frame(); }

	// Decodes one Opus packet into exactly samples_per_frame()*channels()
	// interleaved S16LE samples at `pcm_out`. Returns false on decode
	// failure (caller should fall back to decode_plc()).
	bool decode(const uint8_t *data, size_t len, int16_t *pcm_out);

	// Opus's built-in packet-loss concealment: synthesizes one frame's
	// worth of plausible audio in place of a packet known to be missing,
	// rather than a hard silence gap.
	void decode_plc(int16_t *pcm_out);

	void close();

private:
	::OpusDecoder *dec_ = nullptr;
	gdp::AudioFormat format_;
};

} // namespace spectre
