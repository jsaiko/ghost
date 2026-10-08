// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "audio/audio_pipeline.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

namespace wraith {

AudioPipeline::AudioPipeline() = default;

AudioPipeline::~AudioPipeline() {
	close();
}

bool AudioPipeline::open(const AudioPipelineConfig &config) {
	close();
	config_ = config;

	if (!encoder_.open(config)) {
		return false;
	}

	event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (event_fd_ < 0) {
		encoder_.close();
		return false;
	}

	// Runs on PipewireAudioCapture's own thread -- only touches the
	// mutex-protected queue and the eventfd, never host or GdpSession state.
	capture_.on_pcm = [this](const int16_t *pcm, size_t n_samples) {
		(void)n_samples; // always samples_per_frame()*channels() by construction
		if (!encoding_.load(std::memory_order_relaxed)) {
			return;
		}
		std::vector<uint8_t> packet = encoder_.encode(pcm);
		if (packet.empty()) {
			return;
		}
		{
			std::lock_guard<std::mutex> lock(queue_mutex_);
			queue_.push_back(EncodedAudioPacket{std::move(packet)});
		}
		uint64_t one = 1;
		ssize_t written = write(event_fd_, &one, sizeof(one));
		(void)written; // EFD_NONBLOCK: a full counter just means the reader hasn't caught up yet
	};

	if (!capture_.open(config, config.node_name)) {
		encoder_.close();
		::close(event_fd_);
		event_fd_ = -1;
		return false;
	}

	return true;
}

std::vector<EncodedAudioPacket> AudioPipeline::poll() {
	uint64_t count = 0;
	ssize_t n = read(event_fd_, &count, sizeof(count));
	(void)n; // EAGAIN (nothing pending) is fine; we still drain the queue below

	std::vector<EncodedAudioPacket> out;
	std::lock_guard<std::mutex> lock(queue_mutex_);
	out.reserve(queue_.size());
	while (!queue_.empty()) {
		out.push_back(std::move(queue_.front()));
		queue_.pop_front();
	}
	return out;
}

void AudioPipeline::close() {
	capture_.close();
	encoder_.close();
	if (event_fd_ >= 0) {
		::close(event_fd_);
		event_fd_ = -1;
	}
	std::lock_guard<std::mutex> lock(queue_mutex_);
	queue_.clear();
}

} // namespace wraith
