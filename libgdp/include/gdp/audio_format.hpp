// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// The session audio format (gdp-spec.md §10): what SessionAccept's
// AudioConfig carries, what wraith's capture/encode pipeline is opened
// with, and what spectre's decoder/player are opened from. One struct so
// the three sides can't drift -- and so the Opus wrappers on each end
// derive samples_per_frame the same way.
#pragma once

#include <cstdint>
#include <string>

namespace gdp {

// Audio codec identifier as it appears on the wire (AudioConfig.codec).
// Same lowercase-token convention as the video codecs: a name says
// nothing about which implementation produced or consumes it.
inline constexpr const char *kAudioCodecOpus = "opus";

// The capability that turns on the client -> host microphone channel
// (gdp-spec.md §6.7, §10.1).
inline constexpr const char *kCapabilityMicrophone = "microphone";

struct AudioFormat {
	// Defaults (gdp-spec.md §10): Opus, 48 kHz stereo, 10 ms frames.
	// `codec` is the AudioConfig.codec wire token; the Opus wrappers on
	// each end only ever handle kAudioCodecOpus, so spectre checks it
	// before opening a decoder.
	std::string codec = kAudioCodecOpus;
	uint32_t sample_rate_hz = 48000;
	uint32_t channels = 2;
	uint32_t frame_ms = 10;

	// Samples per channel in one frame -- the count libopus's encode/decode
	// calls take. frame_ms must divide evenly into a valid Opus frame size
	// (10 by default).
	uint32_t samples_per_frame() const { return sample_rate_hz / 1000 * frame_ms; }
	// Interleaved S16LE sample count for one frame's PCM buffer.
	uint32_t samples_per_frame_all_channels() const { return samples_per_frame() * channels; }
};

// The microphone's format: one mono Opus stream, the host's side of
// SessionAccept.microphone. The client encodes to exactly this.
inline AudioFormat microphone_format() {
	AudioFormat f;
	f.channels = 1;
	return f;
}

} // namespace gdp
