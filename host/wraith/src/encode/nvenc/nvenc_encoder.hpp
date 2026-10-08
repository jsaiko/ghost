// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// NVENC backend for the Encoder interface: H.264, H.265
// and AV1 on NVIDIA GPUs, one class for all three -- NVENC takes the codec
// as a GUID and writes every header itself, so there is no per-codec
// parameter-buffer or bitstream code to split out the way VA-API needs.
//
// Input is packed 32-bit RGB in CUDA memory, and NVENC does the RGB -> YUV
// conversion; it gets there by one of two paths, picked in open():
//
// - dmabuf (push()): CudaDmabufImporter copies the frame into one of its
//   CUDA buffers on the GPU. Used whenever the importer comes up.
// - CPU (push_cpu()): a host-to-device copy into the same kind of buffer.
//   The fallback when the importer can't come up -- or is switched off
//   with encode.nvenc_zero_copy = false in wraith.toml -- and, as with
//   VaapiEncoderBase, always available regardless: lossless refinement
//   feeds its base encoder host pixels wherever it can't hash the frame
//   on the GPU (refine/refine_encoder.hpp).
//
// Colour: NVENC converts RGB with the matrix and range the stream's own
// colour description names, so every codec states BT.601 studio range,
// wraith's convention everywhere (see the matrix note in
// vaapi_encoder_base.cpp): a VUI for H.264 and H.265, the sequence
// header for AV1, as VaapiAv1Encoder does.
//
// Asynchronous (set_asynchronous(), which the encoder factory always
// asks for): two frames in flight, each with its own input and bitstream
// buffer, so the next frame's copy -- which waits for the compositor to
// finish drawing it -- overlaps the GPU encoding the last one. Serially the
// two add up: 23 ms a 4K frame on an RTX 3070 Ti once the driver had
// clocked it down, 40 fps. A worker thread blocks in nvEncLockBitstream()
// (the NVENC guide's pattern for its synchronous mode, the only one Linux
// has) and wakes the session through completion_fd(); everything else,
// mapping and unmapping included, stays on the caller's thread.
#pragma once

#include "encode/encoder.hpp"
#include "encode/nvenc/cuda_dmabuf_importer.hpp"
#include "encode/nvenc/nvenc_runtime.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace wraith {

class NvencEncoder : public Encoder {
public:
	~NvencEncoder() override;

	bool open(const EncoderConfig &config) override;
	// False while the zero-copy importer is up. It can turn true after
	// open() -- once, never back -- if an import fails at runtime: the
	// host asks per frame and switches to read-back, which beats
	// dropping every frame on a driver that advertised a modifier it can't
	// actually take (see encoder.hpp).
	bool wants_cpu_frame() const override { return !importer_ || import_failed_; }
	bool push(const DmabufFrame &frame, int64_t pts_us) override;
	bool push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride, int64_t pts_us,
		const DamageRegion *damage) override;
	std::vector<uint64_t> supported_import_modifiers(uint32_t drm_format) const override;
	void request_keyframe() override;
	void set_bitrate(uint32_t bitrate_bps) override;
	std::vector<EncodedPacket> poll() override;
	void close() override;
	void set_asynchronous(bool on) override { asynchronous_ = on; }
	int completion_fd() const override { return asynchronous_ ? done_fd_ : -1; }
	bool ready_for_frame() const override;

	// The wire codecs the NVIDIA GPU behind render node `drm_fd` can
	// encode, for hardware_encodable_codecs() (encoder_factory.hpp).
	// Opens and closes a throwaway NVENC session to ask, so it costs a
	// CUDA context; empty for a non-NVIDIA node without loading anything.
	static std::vector<gdp::VideoCodec> encodable_codecs(int drm_fd);

private:
	static constexpr int kSlots = 2;
	struct Slot {
		CUdeviceptr input = 0; // the importer's buffer, or own_input
		CUdeviceptr own_input = 0;
		NV_ENC_REGISTERED_PTR registered = nullptr;
		NV_ENC_BUFFER_FORMAT registered_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
		NV_ENC_OUTPUT_PTR bitstream = nullptr;
		// Free -> Encoding (encode(), main thread) -> Done (lock_slot(),
		// the worker when asynchronous) -> Free (collect_done(), main).
		// Guarded by mutex_.
		enum class State { Free, Encoding, Done } state = State::Free;
		NV_ENC_INPUT_PTR mapped = nullptr;
		int64_t pts_us = 0, copy_us = 0, submit_us = 0;
		// Written by lock_slot(), read once Done.
		bool locked = false;
		int64_t encode_us = 0;
		EncodedPacket packet;
	};

	bool init_session();
	bool init_input();
	// (Re)registers a slot's input buffer with NVENC as `format`: dmabufs
	// can arrive as either RGB byte order, and NVENC takes the order at
	// registration.
	bool ensure_registered(Slot &slot, NV_ENC_BUFFER_FORMAT format);
	void unregister_input(Slot &slot);
	// A free slot to fill, waiting for the oldest frame if there is none.
	int acquire_slot();
	// Encodes whatever is in slot `index`'s input buffer.
	bool encode(int index, NV_ENC_BUFFER_FORMAT format, int64_t pts_us, int64_t copy_us);
	void lock_slot(int index);
	void collect_done();
	void worker_loop();
	void note_timing(int64_t copy_us, int64_t encode_us, size_t bytes);
	std::string last_error() const;

	EncoderConfig config_;
	const NvencRuntime *runtime_ = nullptr;
	CUdevice cu_device_ = 0;
	CUcontext cu_context_ = nullptr; // the device's primary context, retained
	void *session_ = nullptr;

	GUID codec_guid_ = {};
	NV_ENC_INITIALIZE_PARAMS init_params_ = {};
	NV_ENC_CONFIG encode_config_ = {};

	// The zero-copy path's importer, which owns the slots' input buffers
	// when it exists. Otherwise each slot cuMemAllocPitch's its own.
	std::unique_ptr<CudaDmabufImporter> importer_;
	bool import_failed_ = false;
	uint32_t input_pitch_ = 0;

	Slot slots_[kSlots];
	std::deque<int> in_flight_; // slots, oldest first (main thread)
	std::vector<EncodedPacket> ready_;
	bool force_idr_next_ = true;

	bool asynchronous_ = false;
	std::thread worker_;
	mutable std::mutex mutex_;
	std::condition_variable work_cv_; // to_lock_ grew, or stop_
	std::condition_variable done_cv_; // a slot turned Done
	std::deque<int> to_lock_;         // the worker's queue
	bool stop_ = false;
	int done_fd_ = -1; // eventfd, written per finished frame

	struct Timing {
		int64_t window_start_us = 0;
		uint32_t frames = 0;
		int64_t copy_sum_us = 0, copy_max_us = 0;
		int64_t encode_sum_us = 0, encode_max_us = 0;
		uint64_t bytes = 0;
	} timing_;
};

} // namespace wraith
