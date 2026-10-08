// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/pipewire_capture.hpp"
#include "audio/default_node.hpp"
#include "util/pipewire_init.hpp"

#include <pipewire/pipewire.h>
#include <pipewire/thread-loop.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cstring>

namespace wraith {

struct PipewireAudioCapture::Impl {
	std::function<void(const int16_t *, size_t)> *on_pcm = nullptr;

	struct pw_thread_loop *loop = nullptr;
	struct pw_context *context = nullptr;
	struct pw_core *core = nullptr;
	struct pw_stream *stream = nullptr;
	struct spa_hook stream_listener{};
	// Without it a host whose sound card the session can see plays apps
	// out of that card, not into this sink.
	DefaultNodeClaim default_claim;

	// PipeWire's own buffer quanta aren't guaranteed to line up with a
	// 10ms Opus frame boundary, so process() accumulates into this ring
	// and only fires on_pcm once a full frame's worth is ready.
	std::vector<int16_t> accum;
	size_t accum_filled = 0; // in samples (interleaved, i.e. frames*channels)

	static void on_process(void *data) {
		auto *impl = static_cast<Impl *>(data);
		struct pw_buffer *b = pw_stream_dequeue_buffer(impl->stream);
		if (!b) {
			return;
		}
		struct spa_buffer *buf = b->buffer;
		if (buf->datas[0].data) {
			const int16_t *src = (const int16_t *)buf->datas[0].data;
			uint32_t n_bytes = buf->datas[0].chunk->size;
			size_t n_samples = n_bytes / sizeof(int16_t);
			size_t src_off = 0;
			while (src_off < n_samples) {
				size_t want = impl->accum.size() - impl->accum_filled;
				size_t take = std::min(want, n_samples - src_off);
				std::memcpy(impl->accum.data() + impl->accum_filled, src + src_off, take * sizeof(int16_t));
				impl->accum_filled += take;
				src_off += take;
				if (impl->accum_filled == impl->accum.size()) {
					if (impl->on_pcm && *impl->on_pcm) {
						(*impl->on_pcm)(impl->accum.data(), impl->accum.size());
					}
					impl->accum_filled = 0;
				}
			}
		}
		pw_stream_queue_buffer(impl->stream, b);
	}

	static void on_stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
		const char *error) {
		(void)data;
		(void)old;
		if (state == PW_STREAM_STATE_ERROR) {
			pw_log_error("wraith audio capture stream error: %s", error ? error : "(unknown)");
		}
	}
};

PipewireAudioCapture::PipewireAudioCapture() = default;

PipewireAudioCapture::~PipewireAudioCapture() {
	close();
}

bool PipewireAudioCapture::open(const gdp::AudioFormat &format, const std::string &node_name) {
	close();
	ensure_pw_init();

	impl_ = new Impl();
	impl_->on_pcm = &on_pcm;
	impl_->accum.resize(format.samples_per_frame_all_channels());

	impl_->loop = pw_thread_loop_new("wraith-audio-capture", nullptr);
	if (!impl_->loop) {
		close();
		return false;
	}

	if (pw_thread_loop_start(impl_->loop) < 0) {
		close();
		return false;
	}

	pw_thread_loop_lock(impl_->loop);

	impl_->context = pw_context_new(pw_thread_loop_get_loop(impl_->loop), nullptr, 0);
	if (!impl_->context) {
		pw_thread_loop_unlock(impl_->loop);
		close();
		return false;
	}

	impl_->core = pw_context_connect(impl_->context, nullptr, 0);
	if (!impl_->core) {
		pw_thread_loop_unlock(impl_->loop);
		close();
		return false;
	}

	// Audio/Sink + a non-monitor input stream is the standard "virtual
	// sink" shape: the session manager (WirePlumber/pipewire-media-session)
	// exposes this node as a selectable playback target for other clients
	// in the same graph, exactly like a hardware sink. We are the
	// *consumer* of that audio (direction INPUT), not a source, so the
	// stream's own process() callback receives what other clients played
	// into it.
	//
	// The priority outranks every hardware sink (ALSA's top out near
	// 1000), so the session manager falls back to this one whenever the
	// configured default is cleared or gone: Steam's Big Picture clears
	// it, and the host's own speakers would otherwise win. One session
	// per user, so the name needs no pid, and the session manager's
	// saved defaults don't collect a dead name per session.
	struct pw_properties *props =
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS,
			"Audio/Sink", PW_KEY_NODE_NAME, node_name.c_str(), PW_KEY_NODE_DESCRIPTION, "Ghost remote audio",
			PW_KEY_PRIORITY_SESSION, "10000", PW_KEY_PRIORITY_DRIVER, "10000", nullptr);

	// Value-initialise then assign so every unused callback slot is nullptr
	// without tripping -Wmissing-field-initializers on the designated form.
	static const struct pw_stream_events stream_events = [] {
		struct pw_stream_events ev{};
		ev.version = PW_VERSION_STREAM_EVENTS;
		ev.state_changed = Impl::on_stream_state_changed;
		ev.process = Impl::on_process;
		return ev;
	}();

	impl_->stream = pw_stream_new(impl_->core, "wraith-audio-capture", props);
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
	if (format.channels == 2) {
		info.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.position[1] = SPA_AUDIO_CHANNEL_FR;
	}

	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

	int res = pw_stream_connect(impl_->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
		(enum pw_stream_flags)(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);

	if (res >= 0) {
		impl_->default_claim.open(impl_->core, "default.configured.audio.sink", node_name);
	}

	pw_thread_loop_unlock(impl_->loop);

	if (res < 0) {
		pw_log_error("wraith: pw_stream_connect failed: %s", spa_strerror(res));
		close();
		return false;
	}

	return true;
}

void PipewireAudioCapture::close() {
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
	delete impl_;
	impl_ = nullptr;
}

} // namespace wraith
