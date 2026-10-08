// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Host-scoped owner of the audio capture+encode path
// (docs/design/audio-cursor-gamepad.md), owned by SessionServices like the video encoder. The capture
// runs continuously once opened, so the session's sink stays put whether
// or not a GDP session is attached; the Opus encode only runs while
// set_encoding() says a viewer is there to hear it.
//
// Capture and Opus encode run on PipewireAudioCapture's own thread, but
// every GdpSession call has to happen on wraith's main thread: finished
// packets are queued, and an eventfd wakes the main loop to drain them
// with poll() -- the same notify_fd() shape GdpSession uses.
#pragma once

#include "audio/opus_encode.hpp"
#include "audio/pipewire_capture.hpp"

#include "gdp/audio_format.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace wraith {

// The wire-visible format (gdp::AudioFormat: what SessionAccept.audio
// advertises to spectre) plus wraith's own capture-side settings.
struct AudioPipelineConfig : gdp::AudioFormat {
	std::string node_name = "gdp-audio";
};

struct EncodedAudioPacket {
	std::vector<uint8_t> data; // one Opus packet
};

class AudioPipeline {
public:
	AudioPipeline();
	~AudioPipeline();

	AudioPipeline(const AudioPipeline &) = delete;
	AudioPipeline &operator=(const AudioPipeline &) = delete;

	bool open(const AudioPipelineConfig &config);

	// Register with wl_event_loop_add_fd(..., WL_EVENT_READABLE, ...) and
	// call poll() when it fires, same pattern as GdpSession::notify_fd().
	int notify_fd() const { return event_fd_; }

	// Drains and returns whatever Opus packets have finished encoding since
	// the last call. Safe to call only from the thread that owns wraith's
	// wl_event_loop (matches GdpSession::dispatch()'s contract).
	std::vector<EncodedAudioPacket> poll();

	const AudioPipelineConfig &config() const { return config_; }

	// Whether captured PCM is encoded and queued at all. Off (the default)
	// while no viewer is attached: the sink keeps taking the session's
	// audio, which is just dropped on the capture thread. Main thread.
	void set_encoding(bool encoding) { encoding_.store(encoding, std::memory_order_relaxed); }

	void close();

private:
	AudioPipelineConfig config_;
	PipewireAudioCapture capture_;
	OpusEncoderWrapper encoder_;
	std::atomic<bool> encoding_{false};

	std::mutex queue_mutex_;
	std::deque<EncodedAudioPacket> queue_;

	int event_fd_ = -1;
};

} // namespace wraith
