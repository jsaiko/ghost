// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Software H.264 fallback for the Encoder interface, used when no hardware
// backend (VA-API, NVENC) opens, or under -F -- see encoder_factory.cpp's
// backend table. x264 is GPL-2.0-or-later, compatible with wraith's
// GPL-3.0-only.
//
// wants_cpu_frame() is true: there's no device to import a dmabuf into, so
// the caller reads the frame back into host memory (ScreencastHost's
// DmabufReader, or a memfd frame) and calls push_cpu()
// with DRM_FORMAT_XRGB8888 pixels instead of push().
#pragma once

#include "encode/encoder.hpp"

#include <x264.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace wraith {

class X264Encoder : public Encoder {
public:
	~X264Encoder() override;

	bool open(const EncoderConfig &config) override;
	bool wants_cpu_frame() const override { return true; }
	bool push(const DmabufFrame &frame, int64_t pts_us) override; // unused; always returns false
	bool push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride, int64_t pts_us,
		const DamageRegion *damage) override;
	void request_keyframe() override;
	void set_bitrate(uint32_t bitrate_bps) override;
	void set_asynchronous(bool on) override { asynchronous_ = on; }
	int completion_fd() const override { return wake_fd_; }
	std::vector<EncodedPacket> poll() override;
	void close() override;

private:
	EncoderConfig config_;
	x264_t *encoder_ = nullptr;
	x264_param_t params_;

	// Synchronous mode's XRGB8888 -> I420 conversion buffer, sized once in
	// open() and pointed to by pic_.img.plane[0..2]. Empty in asynchronous
	// mode, which converts into slots_ instead.
	std::vector<uint8_t> i420_;
	x264_picture_t pic_;

	// x264_encoder_encode() both encodes and returns the bitstream
	// synchronously in this config (sliced rather than frame threading, no
	// B-frames, no lookahead -- so no internal reorder delay), so poll()
	// just drains what push_cpu() or the worker staged here.
	std::vector<EncodedPacket> ready_;

	bool force_idr_next_ = true;

	// Encodes `pic` and appends its packet to `out`; false on an x264
	// error.
	bool encode_picture(x264_picture_t *pic, int64_t pts_us, std::vector<EncodedPacket> *out);
	// Sets the VBV ceiling (and buffer, one second of it) to `bitrate_bps`.
	void apply_vbv(uint32_t bitrate_bps);

	// Asynchronous mode (set_asynchronous()): push_cpu() converts on the
	// caller's thread into one of two pictures while worker_ encodes the
	// other, then hands its picture over -- waiting only if the previous
	// one is still being encoded, so at most one frame is ever in flight.
	// The caller's thread is also the event loop's: conversion and x264
	// back to back on it don't fit a 60 fps frame at 4K, though each step
	// does on its own.
	struct Slot {
		std::vector<uint8_t> i420;
		x264_picture_t pic;
	};
	void worker_loop();
	bool asynchronous_ = false;
	Slot slots_[2];
	int next_slot_ = 0;
	std::thread worker_;
	std::mutex mutex_; // guards everything below, and ready_ in this mode
	std::condition_variable cv_;
	Slot *job_ = nullptr; // being (or about to be) encoded
	int64_t job_convert_us_ = 0;
	uint32_t pending_bitrate_bps_ = 0; // applied by the worker between frames
	bool stop_ = false;
	int wake_fd_ = -1; // eventfd: packets waiting in ready_

	// Per-frame cost, logged every kTimingWindowUs: where a software
	// session's frame time goes (colour conversion vs x264), to tell an
	// encoder that can't keep up from frames that simply aren't arriving.
	struct Timing {
		int64_t window_start_us = 0;
		uint32_t frames = 0;
		int64_t convert_sum_us = 0, convert_max_us = 0;
		int64_t encode_sum_us = 0, encode_max_us = 0;
	} timing_;
	void note_timing(int64_t convert_us, int64_t encode_us);
};

} // namespace wraith
