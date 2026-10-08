// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/microphone_source.hpp"
#include "audio/default_node.hpp"
#include "util/pipewire_init.hpp"

#include <opus/opus.h>
#include <pipewire/pipewire.h>
#include <pipewire/thread-loop.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace wraith {

namespace {

// Frames buffered before playback starts, and the ring's ceiling. A
// datagram path jitters by a few ms on a LAN; 3 frames (30 ms at the
// default 10 ms) rides that out without adding noticeable mouth-to-ear
// delay. The ceiling bounds the damage if the PipeWire graph stalls.
constexpr size_t kPrimeFrames = 3;
constexpr size_t kMaxFrames = 20;
// A gap longer than this is a restart or a long outage, not loss worth
// concealing frame by frame.
constexpr int kMaxConcealFrames = 5;

} // namespace

struct MicrophoneSource::Impl {
	gdp::AudioFormat format;
	size_t frame_samples = 0; // per frame, all channels

	// Main thread only.
	OpusDecoder *decoder = nullptr;
	bool have_seq = false;
	uint16_t expected_seq = 0;

	// Shared with the PipeWire thread.
	std::mutex mutex;
	std::vector<int16_t> ring; // samples, oldest first
	bool primed = false;

	struct pw_thread_loop *loop = nullptr;
	struct pw_context *context = nullptr;
	struct pw_core *core = nullptr;
	struct pw_stream *stream = nullptr;
	struct spa_hook stream_listener{};

	// Without it WirePlumber keeps the sink's monitor as the default
	// source -- the only source there was when the session started -- and
	// apps would record the session's own audio instead of the microphone.
	DefaultNodeClaim default_claim;

	void push_pcm(const int16_t *pcm) {
		std::lock_guard<std::mutex> lock(mutex);
		ring.insert(ring.end(), pcm, pcm + frame_samples);
		size_t cap = kMaxFrames * frame_samples;
		if (ring.size() > cap) {
			ring.erase(ring.begin(), ring.begin() + (ring.size() - cap));
		}
		if (!primed && ring.size() >= kPrimeFrames * frame_samples) {
			primed = true;
		}
	}

	static void on_process(void *data) {
		auto *impl = static_cast<Impl *>(data);
		struct pw_buffer *b = pw_stream_dequeue_buffer(impl->stream);
		if (!b) {
			return;
		}
		struct spa_data &d = b->buffer->datas[0];
		if (d.data) {
			const size_t stride = sizeof(int16_t) * impl->format.channels;
			size_t frames = d.maxsize / stride;
			if (b->requested > 0) {
				frames = std::min<size_t>(frames, b->requested);
			}
			size_t want = frames * impl->format.channels;
			auto *out = static_cast<int16_t *>(d.data);
			size_t have = 0;
			{
				std::lock_guard<std::mutex> lock(impl->mutex);
				if (impl->primed) {
					have = std::min(want, impl->ring.size());
					std::memcpy(out, impl->ring.data(), have * sizeof(int16_t));
					impl->ring.erase(impl->ring.begin(), impl->ring.begin() + have);
					if (impl->ring.empty() && have < want) {
						impl->primed = false; // underrun: rebuffer
					}
				}
			}
			if (have < want) {
				std::memset(out + have, 0, (want - have) * sizeof(int16_t));
			}
			d.chunk->offset = 0;
			d.chunk->stride = (int32_t)stride;
			d.chunk->size = (uint32_t)(frames * stride);
		}
		pw_stream_queue_buffer(impl->stream, b);
	}

	static void on_stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
		const char *error) {
		(void)data;
		(void)old;
		if (state == PW_STREAM_STATE_ERROR) {
			pw_log_error("wraith microphone source stream error: %s", error ? error : "(unknown)");
		}
	}
};

MicrophoneSource::MicrophoneSource() = default;

MicrophoneSource::~MicrophoneSource() {
	close();
}

bool MicrophoneSource::open(const gdp::AudioFormat &format, const std::string &node_name) {
	close();
	ensure_pw_init();

	impl_ = new Impl();
	format_ = format;
	impl_->format = format;
	impl_->frame_samples = format.samples_per_frame_all_channels();

	int err = 0;
	impl_->decoder = opus_decoder_create((opus_int32)format.sample_rate_hz, (int)format.channels, &err);
	if (err != OPUS_OK || !impl_->decoder) {
		impl_->decoder = nullptr;
		close();
		return false;
	}

	impl_->loop = pw_thread_loop_new("wraith-mic-source", nullptr);
	if (!impl_->loop || pw_thread_loop_start(impl_->loop) < 0) {
		close();
		return false;
	}

	pw_thread_loop_lock(impl_->loop);

	impl_->context = pw_context_new(pw_thread_loop_get_loop(impl_->loop), nullptr, 0);
	if (impl_->context) {
		impl_->core = pw_context_connect(impl_->context, nullptr, 0);
	}
	if (!impl_->core) {
		pw_thread_loop_unlock(impl_->loop);
		close();
		return false;
	}

	// Audio/Source with an OUTPUT stream is the virtual-microphone shape:
	// the session manager lists the node as a recording device, and we
	// *produce* the samples apps record. Outranks every hardware source,
	// as the capture sink does (pipewire_capture.cpp), and the sink too:
	// WirePlumber weighs a sink's monitor as a source at the sink's
	// priority, and on a tie it picked the monitor.
	struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
		"Playback", PW_KEY_MEDIA_CLASS, "Audio/Source", PW_KEY_NODE_NAME, node_name.c_str(),
		PW_KEY_NODE_DESCRIPTION, "Ghost remote microphone", PW_KEY_PRIORITY_SESSION, "11000",
		PW_KEY_PRIORITY_DRIVER, "11000", nullptr);

	static const struct pw_stream_events stream_events = [] {
		struct pw_stream_events ev{};
		ev.version = PW_VERSION_STREAM_EVENTS;
		ev.state_changed = Impl::on_stream_state_changed;
		ev.process = Impl::on_process;
		return ev;
	}();

	impl_->stream = pw_stream_new(impl_->core, "wraith-mic-source", props);
	if (!impl_->stream) {
		pw_thread_loop_unlock(impl_->loop);
		close();
		return false;
	}
	pw_stream_add_listener(impl_->stream, &impl_->stream_listener, &stream_events, impl_);

	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	struct spa_audio_info_raw info{};
	info.format = SPA_AUDIO_FORMAT_S16;
	info.rate = format.sample_rate_hz;
	info.channels = format.channels;
	if (format.channels == 1) {
		info.position[0] = SPA_AUDIO_CHANNEL_MONO;
	} else if (format.channels == 2) {
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
	}

	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

	int res = pw_stream_connect(impl_->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
		(enum pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);

	if (res >= 0) {
		impl_->default_claim.open(impl_->core, "default.configured.audio.source", node_name);
	}

	pw_thread_loop_unlock(impl_->loop);

	if (res < 0) {
		pw_log_error("wraith: microphone pw_stream_connect failed: %s", spa_strerror(res));
		close();
		return false;
	}
	return true;
}

void MicrophoneSource::close() {
	if (!impl_) {
		return;
	}
	if (impl_->loop) {
		pw_thread_loop_lock(impl_->loop);
		impl_->default_claim.close();
		if (impl_->stream) {
			pw_stream_destroy(impl_->stream);
			impl_->stream = nullptr;
		}
		if (impl_->core) {
			pw_core_disconnect(impl_->core);
			impl_->core = nullptr;
		}
		if (impl_->context) {
			pw_context_destroy(impl_->context);
			impl_->context = nullptr;
		}
		pw_thread_loop_unlock(impl_->loop);
		pw_thread_loop_stop(impl_->loop);
		pw_thread_loop_destroy(impl_->loop);
		impl_->loop = nullptr;
	}
	if (impl_->decoder) {
		opus_decoder_destroy(impl_->decoder);
	}
	delete impl_;
	impl_ = nullptr;
}

void MicrophoneSource::reset() {
	if (!impl_) {
		return;
	}
	opus_decoder_ctl(impl_->decoder, OPUS_RESET_STATE);
	impl_->have_seq = false;
	std::lock_guard<std::mutex> lock(impl_->mutex);
	impl_->ring.clear();
	impl_->primed = false;
}

void MicrophoneSource::on_packet(uint16_t seq, const uint8_t *data, size_t len) {
	if (!impl_) {
		return;
	}
	Impl &i = *impl_;
	const int frame = (int)format_.samples_per_frame();
	std::vector<int16_t> pcm(i.frame_samples);

	if (i.have_seq) {
		int16_t delta = (int16_t)(seq - i.expected_seq);
		if (delta < 0) {
			return; // late or duplicate: its slot has already played as concealment
		}
		if (delta > kMaxConcealFrames) {
			// Too long a gap to paper over: start the stream afresh.
			opus_decoder_ctl(i.decoder, OPUS_RESET_STATE);
		} else {
			for (int16_t k = 0; k < delta; k++) {
				if (opus_decode(i.decoder, nullptr, 0, pcm.data(), frame, 0) == frame) {
					i.push_pcm(pcm.data());
				}
			}
		}
	}
	int n = opus_decode(i.decoder, data, (opus_int32)len, pcm.data(), frame, 0);
	if (n != frame) {
		return; // malformed packet: drop it (gdp-spec.md §3.2)
	}
	i.have_seq = true;
	i.expected_seq = (uint16_t)(seq + 1);
	i.push_pcm(pcm.data());
}

} // namespace wraith
