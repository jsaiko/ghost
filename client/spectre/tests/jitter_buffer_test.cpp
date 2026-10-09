// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// JitterBuffer unit tests plus the two-thread stress harness. Plain assert(), as in libgdp/tests.
//
// Packets are real Opus frames (a tone through libopus's encoder) so the
// decoder path is the production one; what the tests observe is only
// pull()'s true/false, since that's the whole contract with the audio
// thread -- decoded samples are libopus's business.
//
// The stress section runs the push side (main-loop stand-in) and pull
// side (audio-thread stand-in) on real threads with time scaled down
// (one 10 ms frame period = one tick of kTick), which the buffer can't
// tell apart from real time: it only sees call order. Bursty 2-3 packet
// arrivals, ~43 ms main-loop stalls, a "wire gap" where the source goes
// silent with its seq counter frozen, 2% loss, and the 2^16 seq wrap are
// all in there. Its assertions are deliberately loose (thread scheduling
// is not deterministic); the exact behaviours are pinned by the
// single-threaded tests above it.
#include "audio/jitter_buffer.hpp"
#include "audio/opus_decode.hpp"

#include "gdp/audio_format.hpp"

#include <opus/opus.h>

#include <atomic>
// These tests are plain assert()s: make sure a Release build (-DNDEBUG)
// can't compile them away into a vacuous pass.
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using namespace spectre;

namespace {

const gdp::AudioFormat kFormat{}; // defaults: 48 kHz stereo 10 ms

// Encodes a continuous 440 Hz tone one frame at a time.
class ToneSource {
public:
	ToneSource() {
		int err = 0;
		enc_ = opus_encoder_create((opus_int32)kFormat.sample_rate_hz, (int)kFormat.channels,
			OPUS_APPLICATION_AUDIO, &err);
		assert(err == OPUS_OK && enc_);
		pcm_.resize(kFormat.samples_per_frame_all_channels());
	}
	~ToneSource() { opus_encoder_destroy(enc_); }

	std::vector<uint8_t> next() {
		constexpr double kPi = 3.14159265358979323846;
		const double step = 2.0 * kPi * 440.0 / kFormat.sample_rate_hz;
		for (uint32_t i = 0; i < kFormat.samples_per_frame(); i++) {
			int16_t s = (int16_t)(std::sin(phase_) * 8000.0);
			phase_ += step;
			for (uint32_t c = 0; c < kFormat.channels; c++) {
				pcm_[i * kFormat.channels + c] = s;
			}
		}
		std::vector<uint8_t> out(1500);
		int len =
			opus_encode(enc_, pcm_.data(), (int)kFormat.samples_per_frame(), out.data(), (int)out.size());
		assert(len > 0);
		out.resize((size_t)len);
		return out;
	}

private:
	OpusEncoder *enc_ = nullptr;
	std::vector<int16_t> pcm_;
	double phase_ = 0.0;
};

struct Fixture {
	OpusDecodeWrapper decoder;
	JitterBuffer jb;
	ToneSource source;
	std::vector<int16_t> pcm;

	Fixture() : jb(&decoder), pcm(kFormat.samples_per_frame_all_channels()) {
		bool ok = decoder.open(kFormat);
		assert(ok);
		(void)ok;
	}

	void push(uint16_t seq) {
		std::vector<uint8_t> pkt = source.next();
		jb.push(seq, (uint32_t)seq * 10, pkt.data(), pkt.size());
	}
	bool pull() { return jb.pull(pcm.data()); }

	// Pulls until the buffer reports nothing to play; returns how many
	// frames came out.
	size_t drain() {
		size_t n = 0;
		while (pull()) {
			n++;
			assert(n < 100000); // runaway PLC would loop forever
		}
		return n;
	}
};

void test_buffers_to_target_depth_before_playing() {
	Fixture f;
	assert(!f.pull());
	for (size_t seq = 0; seq + 1 < JitterBuffer::kTargetDepthFrames; seq++) {
		f.push(seq);
		assert(!f.pull()); // still filling
	}
	f.push((uint16_t)(JitterBuffer::kTargetDepthFrames - 1));
	assert(f.drain() == JitterBuffer::kTargetDepthFrames);
	assert(!f.pull()); // empty again: underrun, back to buffering
}

void test_missing_frame_is_concealed_not_skipped() {
	Fixture f;
	// 0 1 2 _ 4 5 6: seq 3 is lost. Its slot is still played (PLC), so
	// seven pulls succeed, not six.
	for (uint16_t seq = 0; seq <= 6; seq++) {
		if (seq != 3) {
			f.push(seq);
		}
	}
	assert(f.drain() == 7);
}

void test_stale_packet_is_dropped() {
	Fixture f;
	for (uint16_t seq = 0; seq < 6; seq++) {
		f.push(seq);
	}
	assert(f.pull() && f.pull() && f.pull()); // played 0 1 2, expecting 3
	f.push(1);                                // late duplicate of an already-played seq
	// Only 3 4 5 remain. If 1 had been buffered it could never be
	// reached (it's behind expected_seq_) and every later pull would
	// PLC forever.
	assert(f.drain() == 3);
}

void test_source_silence_reanchors_on_resume() {
	Fixture f;
	for (uint16_t seq = 0; seq < 6; seq++) {
		f.push(seq);
	}
	assert(f.drain() == 6); // then the source goes quiet...

	// ...and its seq counter freezes while quiet, so the next packet is
	// 6, not 6 + (silence / 10 ms). If the buffer had guessed ahead
	// during the silence these would all look stale.
	for (uint16_t seq = 6; seq < 12; seq++) {
		f.push(seq);
	}
	assert(f.drain() == 6);

	// A source that instead restarts far ahead (reconnect) is fine too:
	// the first arrival after an underrun is the new anchor.
	for (uint16_t seq = 5000; seq < 5006; seq++) {
		f.push(seq);
	}
	assert(f.drain() == 6);
}

// The network reorders the first packets after an underrun: 5 arrives
// before 3 and 4. They are the earlier start, not stragglers to strand
// behind the anchor.
void test_reordered_start_plays_everything() {
	Fixture f;
	const uint16_t seqs[] = {5, 3, 4, 6, 7, 8};
	for (uint16_t seq : seqs) {
		f.push(seq);
	}
	assert(f.drain() == 6); // 3 4 5 6 7 8
}

// A stalling Wi-Fi link: a reordered start, then an outage. The
// outage must end in an underrun and re-buffer, not conceal on, running
// the expected seq ahead of the stream until every packet after it is
// "stale" and audio never comes back.
void test_outage_after_reordered_start_recovers() {
	Fixture f;
	const uint16_t seqs[] = {5, 3, 4, 6, 7, 8};
	for (uint16_t seq : seqs) {
		f.push(seq);
	}
	f.drain(); // plays out, then underruns: the outage
	for (int i = 0; i < 50; i++) {
		assert(!f.pull()); // silent while nothing arrives, not concealing
	}
	for (uint16_t seq = 9; seq < 15; seq++) {
		f.push(seq);
	}
	assert(f.drain() == 6); // audio is back
}

// A straggler from well before an underrun doesn't drag the anchor back.
void test_far_straggler_is_dropped_while_buffering() {
	Fixture f;
	f.push(500);
	f.push(100);
	for (uint16_t seq = 501; seq < 506; seq++) {
		f.push(seq);
	}
	assert(f.drain() == 6); // 500..505, not 100 and 400 frames of concealment
}

void test_seq_wrap_plays_through() {
	Fixture f;
	const uint16_t seqs[] = {65533, 65534, 65535, 0, 1, 2};
	for (uint16_t seq : seqs) {
		f.push(seq);
	}
	assert(f.drain() == 6);
}

void test_overrun_drops_oldest_across_wrap() {
	Fixture f;
	// Fill past kMaxBufferedFrames with the wrap inside the window. The
	// frame that gets trimmed must be 65533 (oldest by arrival), not 0
	// (smallest key): with the right frame dropped, exactly
	// kMaxBufferedFrames play; with the wrong one, 0's slot is concealed
	// and one extra frame comes out.
	uint16_t seq = 65533;
	for (size_t i = 0; i < JitterBuffer::kMaxBufferedFrames + 1; i++) {
		f.push(seq++);
	}
	assert(f.drain() == JitterBuffer::kMaxBufferedFrames);
}

// --- stress ---

constexpr auto kTick = std::chrono::microseconds(100); // stands in for one 10 ms frame period
constexpr int kStressFrames = 4000;

void stress_two_threads() {
	Fixture f;
	std::atomic<bool> producer_done{false};

	std::thread producer([&]() {
		std::mt19937 rng(12345);
		std::uniform_int_distribution<int> burst_len(2, 3);
		std::uniform_int_distribution<int> percent(0, 99);
		uint16_t seq = 65000; // wraps at frame 536
		int sent = 0;
		bool gapped = false;
		while (sent < kStressFrames) {
			int burst = burst_len(rng);
			for (int i = 0; i < burst && sent < kStressFrames; i++, sent++) {
				if (percent(rng) < 2) {
					seq++; // lost on the wire: seq consumed, nothing pushed
					continue;
				}
				f.push(seq++);
			}
			// PipeWire hands wraith 2-3 frames at a time: the burst
			// arrives together, then the wire is quiet for that long.
			std::this_thread::sleep_for(kTick * burst);
			if (sent % 50 < burst) {
				std::this_thread::sleep_for(kTick * 4); // ~43 ms main-loop stall
			}
			if (!gapped && sent >= kStressFrames / 2) {
				// 5 s wire gap: the source stops, seq counter frozen.
				gapped = true;
				std::this_thread::sleep_for(kTick * 500);
			}
		}
		producer_done = true;
	});

	size_t played = 0;
	std::thread consumer([&]() {
		int idle_after_done = 0;
		while (idle_after_done < 3) {
			if (f.pull()) {
				played++;
			} else if (producer_done) {
				idle_after_done++;
			}
			std::this_thread::sleep_for(kTick);
		}
	});

	producer.join();
	consumer.join();

	// Every seq the producer consumed either played (real or PLC), was
	// still buffered at the end, or was trimmed by an overrun during a
	// stall. Loss is concealed, so it doesn't reduce the count. Overrun
	// trimming is bounded by scheduling jitter; 10% is generous.
	// stderr: an abort loses buffered stdout, and the count is what a
	// failure needs to show.
	fprintf(stderr, "stress: %zu of %d frames played\n", played, kStressFrames);
	assert(played <= (size_t)kStressFrames);
	assert(played >= (size_t)(kStressFrames * 9 / 10));
}

} // namespace

int main() {
	test_buffers_to_target_depth_before_playing();
	test_missing_frame_is_concealed_not_skipped();
	test_stale_packet_is_dropped();
	test_source_silence_reanchors_on_resume();
	test_reordered_start_plays_everything();
	test_outage_after_reordered_start_recovers();
	test_far_straggler_is_dropped_while_buffering();
	test_seq_wrap_plays_through();
	test_overrun_drops_oldest_across_wrap();
	stress_two_threads();
	printf("jitter_buffer_test: ok\n");
	return 0;
}
