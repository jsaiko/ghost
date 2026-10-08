// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The session's microphone (gdp-spec.md §10.1): Opus packets the
// client captured arrive on datagram channel 0x03 and come out as a
// PipeWire virtual source -- an ordinary Audio/Source node, so the
// session's apps (voice chat, browsers) record from it like a hardware
// mic. The mirror of PipewireAudioCapture's virtual sink.
//
// Two threads meet here, like AudioPipeline's: on_packet()/reset() run on
// wraith's main thread (where datagrams are dispatched), and the PipeWire
// thread's process callback pulls PCM from a small ring the main thread
// fills. The ring is the jitter buffer: playback waits for kPrimeFrames
// before starting, and an underrun goes silent and re-primes rather than
// stuttering.
#pragma once

#include "gdp/audio_format.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace wraith {

class MicrophoneSource {
public:
	MicrophoneSource();
	~MicrophoneSource();

	MicrophoneSource(const MicrophoneSource &) = delete;
	MicrophoneSource &operator=(const MicrophoneSource &) = delete;

	// `node_name` is the source's name and description as other PipeWire
	// clients see it -- unique per session.
	bool open(const gdp::AudioFormat &format, const std::string &node_name);
	void close();

	const gdp::AudioFormat &format() const { return format_; }

	// One Opus packet off the wire. Main thread. A late or duplicate
	// packet is dropped; a gap is concealed with Opus PLC.
	void on_packet(uint16_t seq, const uint8_t *data, size_t len);

	// A new client: its seq restarts at 0 and nothing buffered is its.
	void reset();

private:
	struct Impl;
	Impl *impl_ = nullptr;
	gdp::AudioFormat format_;
};

} // namespace wraith
