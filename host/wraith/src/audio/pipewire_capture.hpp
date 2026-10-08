// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// A PipeWire virtual sink: appears in the
// session's PipeWire graph as an ordinary Audio/Sink node, so setting it as
// the session's default sink routes the session's game/app audio into it,
// same trick as a `pactl`/`pw-loopback`-created null sink. Pure capture:
// hands fixed-size PCM chunks to a callback, no Opus, no GDP.
//
// Runs entirely on its own pw_thread_loop, not wraith's wl_event_loop:
// audio capture is a real-time 10ms-cadence domain, decoupled from the event
// loop's bursty, damage-gated dispatch. `on_pcm` fires from that thread -- callers
// must not touch wraith/libgdp state directly from it (see AudioPipeline,
// which bridges back to the main thread via an eventfd).
#pragma once

#include "gdp/audio_format.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace wraith {

class PipewireAudioCapture {
public:
	PipewireAudioCapture();
	~PipewireAudioCapture();

	PipewireAudioCapture(const PipewireAudioCapture &) = delete;
	PipewireAudioCapture &operator=(const PipewireAudioCapture &) = delete;

	// Fires on the PipeWire thread once per full frame_ms chunk: exactly
	// format.samples_per_frame_all_channels() interleaved S16LE samples.
	std::function<void(const int16_t *pcm, size_t n_samples)> on_pcm;

	// `node_name` is the sink's name and description as other PipeWire
	// clients (and `wpctl`/`pactl`) see it -- unique per session, so
	// sessions on a shared graph are told apart.
	bool open(const gdp::AudioFormat &format, const std::string &node_name);
	void close();

private:
	struct Impl;
	Impl *impl_ = nullptr;
};

} // namespace wraith
