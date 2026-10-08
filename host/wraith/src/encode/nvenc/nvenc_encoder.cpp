// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/nvenc/nvenc_encoder.hpp"

#include "util/clock.hpp"
#include "util/config.hpp"
#include "util/log.hpp"

#include <libdrm/drm_fourcc.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace wraith {

namespace {

bool same_guid(const GUID &a, const GUID &b) {
	return memcmp(&a, &b, sizeof(GUID)) == 0;
}

struct CodecGuids {
	GUID codec;
	GUID profile;
};

bool guids_for(gdp::VideoCodec codec, CodecGuids *out) {
	switch (codec) {
	case gdp::VideoCodec::H264:
		// High, not Main: NVENC's 8x8 transform is High-only, and every
		// decoder spectre uses takes High.
		*out = {NV_ENC_CODEC_H264_GUID, NV_ENC_H264_PROFILE_HIGH_GUID};
		return true;
	case gdp::VideoCodec::H265: *out = {NV_ENC_CODEC_HEVC_GUID, NV_ENC_HEVC_PROFILE_MAIN_GUID}; return true;
	case gdp::VideoCodec::AV1: *out = {NV_ENC_CODEC_AV1_GUID, NV_ENC_AV1_PROFILE_MAIN_GUID}; return true;
	default: return false;
	}
}

// The codec GUIDs `session` can encode.
std::vector<GUID> session_codec_guids(const NvencRuntime *runtime, void *session) {
	uint32_t count = 0;
	if (runtime->api.nvEncGetEncodeGUIDCount(session, &count) != NV_ENC_SUCCESS || count == 0) {
		return {};
	}
	std::vector<GUID> guids(count);
	uint32_t written = 0;
	if (runtime->api.nvEncGetEncodeGUIDs(session, guids.data(), count, &written) != NV_ENC_SUCCESS) {
		return {};
	}
	guids.resize(written);
	return guids;
}

// NVENC's word-ordered names for the DRM formats' byte orders: DRM
// XRGB8888 is B,G,R,X in memory, which NVENC calls ARGB ("B in the lowest
// 8 bits"); XBGR8888 is R,G,B,X, NVENC's ABGR.
// NVENC converts the RGB input to YCbCr itself, with the matrix and range
// the VUI names (BT.709 when it names none). spectre and the lossless tiles
// are BT.601 studio range, so say so, or every colour shifts (see
// VaapiEncoderBase::convert_to_nv12). AV1 states the same below.
void set_bt601_vui(NV_ENC_CONFIG_H264_VUI_PARAMETERS &vui) {
	vui.videoSignalTypePresentFlag = 1;
	vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED;
	vui.videoFullRangeFlag = 0;
	vui.colourDescriptionPresentFlag = 1;
	vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_SMPTE170M;
	vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE170M;
	vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_SMPTE170M;
}

NV_ENC_BUFFER_FORMAT nvenc_format_from_drm(uint32_t drm_format) {
	switch (drm_format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888: return NV_ENC_BUFFER_FORMAT_ARGB;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888: return NV_ENC_BUFFER_FORMAT_ABGR;
	default: return NV_ENC_BUFFER_FORMAT_UNDEFINED;
	}
}

} // namespace

NvencEncoder::~NvencEncoder() {
	close();
}

std::string NvencEncoder::last_error() const {
	const char *text = session_ ? runtime_->api.nvEncGetLastErrorString(session_) : nullptr;
	return text && *text ? text : "no detail";
}

bool NvencEncoder::open(const EncoderConfig &config) {
	config_ = config;
	if (config_.width == 0 || config_.height == 0) {
		WLOG_ERROR("nvenc: zero-sized frame");
		return false;
	}
	if ((config_.width % 2) != 0 || (config_.height % 2) != 0) {
		// 4:2:0 chroma, as in VaapiEncoderBase::open().
		WLOG_ERROR("nvenc: width/height must be even (got %ux%u)", config_.width, config_.height);
		return false;
	}

	std::string why;
	if (!cuda_device_for_render_node(config_.drm_fd, &cu_device_, &why)) {
		WLOG_INFO("nvenc: not available: %s", why.c_str());
		return false;
	}
	runtime_ = nvenc_runtime();

	CUresult cr = runtime_->cu->cuDevicePrimaryCtxRetain(&cu_context_, cu_device_);
	if (cr != CUDA_SUCCESS) {
		WLOG_ERROR("nvenc: cuDevicePrimaryCtxRetain failed: %s", cuda_error_string(cr).c_str());
		cu_context_ = nullptr;
		return false;
	}

	bool ok;
	{
		CudaContextScope scope(cu_context_);
		ok = init_session() && init_input();
	}
	if (ok && asynchronous_) {
		done_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (done_fd_ < 0) {
			WLOG_ERROR("nvenc: eventfd failed, encoding synchronously");
			asynchronous_ = false;
		} else {
			worker_ = std::thread([this] { worker_loop(); });
		}
	}
	if (!ok) {
		close();
		return false;
	}

	char device_name[128] = {};
	runtime_->cu->cuDeviceGetName(device_name, sizeof(device_name), cu_device_);
	WLOG_INFO("nvenc: %s on %s, %s input", gdp::video_codec_token(config_.codec), device_name,
		importer_ ? "zero-copy dmabuf" : "CPU upload");
	force_idr_next_ = true;
	return true;
}

bool NvencEncoder::init_session() {
	CodecGuids guids;
	if (!guids_for(config_.codec, &guids)) {
		WLOG_ERROR("nvenc: no NVENC codec for \"%s\"", gdp::video_codec_token(config_.codec));
		return false;
	}
	codec_guid_ = guids.codec;

	session_ = open_nvenc_session(cu_context_, "nvenc");
	if (!session_) {
		return false;
	}

	bool codec_supported = false;
	for (const GUID &guid : session_codec_guids(runtime_, session_)) {
		codec_supported = codec_supported || same_guid(guid, codec_guid_);
	}
	if (!codec_supported) {
		// The expected way for an older GPU to decline AV1 (Ada and up
		// only) or H.265: the session renegotiates down.
		WLOG_INFO("nvenc: this GPU has no %s encoder", gdp::video_codec_token(config_.codec));
		return false;
	}

	// P4 is the middle of NVENC's seven speed/quality presets and still
	// single-pass; the ultra-low-latency tuning is what matters for a
	// remote desktop (no lookahead, no B-frames, no reordering).
	NV_ENC_PRESET_CONFIG preset = {};
	preset.version = NV_ENC_PRESET_CONFIG_VER;
	preset.presetCfg.version = NV_ENC_CONFIG_VER;
	NVENCSTATUS st = runtime_->api.nvEncGetEncodePresetConfigEx(session_, codec_guid_, NV_ENC_PRESET_P4_GUID,
		NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
	if (st != NV_ENC_SUCCESS) {
		WLOG_ERROR("nvenc: nvEncGetEncodePresetConfigEx failed (%d): %s", (int)st, last_error().c_str());
		return false;
	}
	encode_config_ = preset.presetCfg;
	encode_config_.version = NV_ENC_CONFIG_VER;
	encode_config_.profileGUID = guids.profile;
	// No periodic IDRs at all for gop_size 0: NVENC's own "infinite".
	const uint32_t idr_period = config_.gop_size != 0 ? config_.gop_size : NVENC_INFINITE_GOPLENGTH;
	encode_config_.gopLength = idr_period;
	encode_config_.frameIntervalP = 1; // I and P only

	// VBR under a ceiling, like the VA-API encoders (see the long note in
	// VaapiEncoderBase::init_encode_pipeline): a static desktop should
	// cost next to nothing, and set_bitrate() is a cap, not a quota. The
	// one-second VBV matches VA-API's rate-control window.
	NV_ENC_RC_PARAMS &rc = encode_config_.rcParams;
	rc.rateControlMode = NV_ENC_PARAMS_RC_VBR;
	rc.averageBitRate = config_.bitrate_bps;
	rc.maxBitRate = config_.bitrate_bps;
	rc.vbvBufferSize = config_.bitrate_bps;
	rc.vbvInitialDelay = config_.bitrate_bps;
	rc.multiPass = NV_ENC_MULTI_PASS_DISABLED;
	rc.enableLookahead = 0;
	rc.zeroReorderDelay = 1;

	// Headers ride on every IDR, so a keyframe requested for a newly
	// joined or recovering client is decodable on its own -- the VA-API
	// encoders do the same.
	switch (config_.codec) {
	case gdp::VideoCodec::H264: {
		NV_ENC_CONFIG_H264 &h264 = encode_config_.encodeCodecConfig.h264Config;
		h264.idrPeriod = idr_period;
		h264.repeatSPSPPS = 1;
		h264.chromaFormatIDC = 1;
		// VUI bitstream_restriction, so the stream says it never reorders
		// (max_num_reorder_frames 0 with frameIntervalP 1). Without it a
		// decoder may hold a full DPB before output -- Chrome's hardware
		// decoders do, as with VaapiH264Encoder's SPS (h264_bitstream.cpp).
		h264.h264VUIParameters.bitstreamRestrictionFlag = 1;
		set_bt601_vui(h264.h264VUIParameters);
		break;
	}
	case gdp::VideoCodec::H265: {
		NV_ENC_CONFIG_HEVC &hevc = encode_config_.encodeCodecConfig.hevcConfig;
		hevc.idrPeriod = idr_period;
		hevc.repeatSPSPPS = 1;
		hevc.chromaFormatIDC = 1;
		hevc.pixelBitDepthMinus8 = 0;
		set_bt601_vui(hevc.hevcVUIParameters);
		break;
	}
	case gdp::VideoCodec::AV1: {
		NV_ENC_CONFIG_AV1 &av1 = encode_config_.encodeCodecConfig.av1Config;
		av1.idrPeriod = idr_period;
		av1.repeatSeqHdr = 1;
		av1.chromaFormatIDC = 1;
		av1.inputPixelBitDepthMinus8 = 0;
		av1.pixelBitDepthMinus8 = 0;
		av1.outputAnnexBFormat = 0; // low-overhead OBUs, as VaapiAv1Encoder
		av1.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_SMPTE170M;
		av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SMPTE170M;
		av1.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_SMPTE170M;
		av1.colorRange = 0;
		break;
	}
	default: return false;
	}

	init_params_ = {};
	init_params_.version = NV_ENC_INITIALIZE_PARAMS_VER;
	init_params_.encodeGUID = codec_guid_;
	init_params_.presetGUID = NV_ENC_PRESET_P4_GUID;
	init_params_.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
	init_params_.encodeWidth = config_.width;
	init_params_.encodeHeight = config_.height;
	init_params_.darWidth = config_.width; // display aspect ratio: square pixels
	init_params_.darHeight = config_.height;
	init_params_.maxEncodeWidth = config_.width;
	init_params_.maxEncodeHeight = config_.height;
	init_params_.frameRateNum = config_.framerate_num;
	init_params_.frameRateDen = config_.framerate_den;
	init_params_.enablePTD = 1;
	init_params_.enableEncodeAsync = 0; // Linux has no async mode
	init_params_.encodeConfig = &encode_config_;

	st = runtime_->api.nvEncInitializeEncoder(session_, &init_params_);
	if (st != NV_ENC_SUCCESS) {
		WLOG_ERROR("nvenc: nvEncInitializeEncoder(%s %ux%u) failed (%d): %s",
			gdp::video_codec_token(config_.codec), config_.width, config_.height, (int)st,
			last_error().c_str());
		return false;
	}

	for (Slot &slot : slots_) {
		NV_ENC_CREATE_BITSTREAM_BUFFER bitstream = {};
		bitstream.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
		st = runtime_->api.nvEncCreateBitstreamBuffer(session_, &bitstream);
		if (st != NV_ENC_SUCCESS) {
			WLOG_ERROR("nvenc: nvEncCreateBitstreamBuffer failed (%d): %s", (int)st, last_error().c_str());
			return false;
		}
		slot.bitstream = bitstream.bitstreamBuffer;
	}
	return true;
}

bool NvencEncoder::init_input() {
	if (!config().encode.nvenc_zero_copy) {
		WLOG_INFO("nvenc: encode.nvenc_zero_copy = false in wraith.toml, using CPU upload");
	} else {
		importer_ = std::make_unique<CudaDmabufImporter>();
		if (!importer_->init(cu_device_, config_.width, config_.height, kSlots)) {
			importer_.reset();
		}
	}
	if (importer_) {
		input_pitch_ = importer_->pitch();
		for (int i = 0; i < kSlots; i++) {
			slots_[i].input = importer_->device_ptr(i);
		}
		return true;
	}

	for (Slot &slot : slots_) {
		size_t pitch = 0;
		CUresult cr = runtime_->cu->cuMemAllocPitch(&slot.own_input, &pitch, (size_t)config_.width * 4,
			config_.height, 16);
		if (cr != CUDA_SUCCESS) {
			WLOG_ERROR("nvenc: cuMemAllocPitch failed: %s", cuda_error_string(cr).c_str());
			slot.own_input = 0;
			return false;
		}
		slot.input = slot.own_input;
		input_pitch_ = (uint32_t)pitch; // the same for every slot: same size
	}
	return true;
}

bool NvencEncoder::ensure_registered(Slot &slot, NV_ENC_BUFFER_FORMAT format) {
	if (slot.registered && slot.registered_format == format) {
		return true;
	}
	unregister_input(slot);

	NV_ENC_REGISTER_RESOURCE reg = {};
	reg.version = NV_ENC_REGISTER_RESOURCE_VER;
	reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
	reg.width = config_.width;
	reg.height = config_.height;
	reg.pitch = input_pitch_;
	reg.resourceToRegister = (void *)slot.input;
	reg.bufferFormat = format;
	reg.bufferUsage = NV_ENC_INPUT_IMAGE;
	NVENCSTATUS st = runtime_->api.nvEncRegisterResource(session_, &reg);
	if (st != NV_ENC_SUCCESS) {
		WLOG_ERROR("nvenc: nvEncRegisterResource failed (%d): %s", (int)st, last_error().c_str());
		return false;
	}
	slot.registered = reg.registeredResource;
	slot.registered_format = format;
	return true;
}

void NvencEncoder::unregister_input(Slot &slot) {
	if (slot.registered) {
		runtime_->api.nvEncUnregisterResource(session_, slot.registered);
		slot.registered = nullptr;
		slot.registered_format = NV_ENC_BUFFER_FORMAT_UNDEFINED;
	}
}

std::vector<uint64_t> NvencEncoder::supported_import_modifiers(uint32_t drm_format) const {
	if (!importer_ || import_failed_) {
		return {};
	}
	return importer_->import_modifiers(drm_format);
}

bool NvencEncoder::ready_for_frame() const {
	if (!asynchronous_) {
		return true;
	}
	std::lock_guard<std::mutex> lock(mutex_);
	for (const Slot &slot : slots_) {
		if (slot.state == Slot::State::Free) {
			return true;
		}
	}
	return false;
}

bool NvencEncoder::push(const DmabufFrame &frame, int64_t pts_us) {
	if (!session_ || !importer_) {
		return false;
	}
	NV_ENC_BUFFER_FORMAT format = nvenc_format_from_drm(frame.format);
	if (format == NV_ENC_BUFFER_FORMAT_UNDEFINED) {
		WLOG_ERROR("nvenc: unsupported dmabuf format 0x%08x", frame.format);
		return false;
	}

	CudaContextScope scope(cu_context_);
	int index = acquire_slot();
	int64_t t0 = monotonic_now_us();
	bool copied = importer_->copy(frame, index);
	int64_t copy_us = monotonic_now_us() - t0;
	if (!copied) {
		if (!import_failed_) {
			import_failed_ = true;
			WLOG_ERROR("nvenc: dmabuf import failed (format 0x%08x, modifier 0x%016llx); callers that can "
					   "will switch to CPU frames",
				frame.format, (unsigned long long)frame.modifier);
		}
		return false;
	}
	return encode(index, format, pts_us, copy_us);
}

bool NvencEncoder::push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride,
	int64_t pts_us, const DamageRegion *) {
	if (!session_) {
		return false;
	}
	if (width != config_.width || height != config_.height) {
		WLOG_ERROR("nvenc: push_cpu() frame %ux%u doesn't match the %ux%u this encoder was opened with",
			width, height, config_.width, config_.height);
		return false;
	}

	CudaContextScope scope(cu_context_);
	int index = acquire_slot();
	int64_t t0 = monotonic_now_us();
	CUDA_MEMCPY2D copy = {};
	copy.srcMemoryType = CU_MEMORYTYPE_HOST;
	copy.srcHost = data;
	copy.srcPitch = stride;
	copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
	copy.dstDevice = slots_[index].input;
	copy.dstPitch = input_pitch_;
	copy.WidthInBytes = (size_t)width * 4;
	copy.Height = height;
	CUresult cr = runtime_->cu->cuMemcpy2D(&copy);
	if (cr != CUDA_SUCCESS) {
		WLOG_ERROR("nvenc: cuMemcpy2D (upload) failed: %s", cuda_error_string(cr).c_str());
		return false;
	}
	// push_cpu() pixels are always DRM_FORMAT_XRGB8888 (encoder.hpp).
	return encode(index, NV_ENC_BUFFER_FORMAT_ARGB, pts_us, monotonic_now_us() - t0);
}

int NvencEncoder::acquire_slot() {
	for (;;) {
		collect_done();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (int i = 0; i < kSlots; i++) {
				if (slots_[i].state == Slot::State::Free) {
					return i;
				}
			}
		}
		// Every slot is in flight: a caller that ignored ready_for_frame(),
		// or a synchronous one. Wait for the oldest.
		int oldest = in_flight_.front();
		if (asynchronous_) {
			std::unique_lock<std::mutex> lock(mutex_);
			done_cv_.wait(lock, [&] { return slots_[oldest].state == Slot::State::Done; });
		} else {
			lock_slot(oldest);
		}
	}
}

bool NvencEncoder::encode(int index, NV_ENC_BUFFER_FORMAT format, int64_t pts_us, int64_t copy_us) {
	Slot &slot = slots_[index];
	if (!ensure_registered(slot, format)) {
		return false;
	}

	NV_ENC_MAP_INPUT_RESOURCE map = {};
	map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
	map.registeredResource = slot.registered;
	NVENCSTATUS st = runtime_->api.nvEncMapInputResource(session_, &map);
	if (st != NV_ENC_SUCCESS) {
		WLOG_ERROR("nvenc: nvEncMapInputResource failed (%d): %s", (int)st, last_error().c_str());
		return false;
	}

	NV_ENC_PIC_PARAMS pic = {};
	pic.version = NV_ENC_PIC_PARAMS_VER;
	pic.inputWidth = config_.width;
	pic.inputHeight = config_.height;
	pic.inputPitch = input_pitch_;
	pic.inputBuffer = map.mappedResource;
	pic.bufferFmt = map.mappedBufferFmt;
	pic.outputBitstream = slot.bitstream;
	pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
	pic.inputTimeStamp = (uint64_t)pts_us;
	if (force_idr_next_) {
		pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
	}
	st = runtime_->api.nvEncEncodePicture(session_, &pic);
	if (st != NV_ENC_SUCCESS) {
		// NV_ENC_ERR_NEED_MORE_INPUT only happens with B-frames, which
		// this configuration has none of; anything is a failure.
		WLOG_ERROR("nvenc: nvEncEncodePicture failed (%d): %s", (int)st, last_error().c_str());
		runtime_->api.nvEncUnmapInputResource(session_, map.mappedResource);
		return false;
	}
	force_idr_next_ = false;
	slot.mapped = map.mappedResource;
	slot.pts_us = pts_us;
	slot.copy_us = copy_us;
	slot.submit_us = monotonic_now_us();
	in_flight_.push_back(index);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		slot.state = Slot::State::Encoding;
		if (asynchronous_) {
			to_lock_.push_back(index);
		}
	}
	if (asynchronous_) {
		work_cv_.notify_one();
	}
	return true;
}

// Blocks until slot `index`'s picture is coded and copies its bitstream
// out. The worker thread's job when asynchronous, poll()'s otherwise.
// Touches only that slot's bitstream; the input stays mapped until
// collect_done() on the main thread.
void NvencEncoder::lock_slot(int index) {
	Slot &slot = slots_[index];
	CudaContextScope scope(cu_context_);
	NV_ENC_LOCK_BITSTREAM lock = {};
	lock.version = NV_ENC_LOCK_BITSTREAM_VER;
	lock.outputBitstream = slot.bitstream;
	lock.doNotWait = 0;
	NVENCSTATUS st = runtime_->api.nvEncLockBitstream(session_, &lock);
	slot.locked = st == NV_ENC_SUCCESS;
	slot.packet = EncodedPacket{};
	if (slot.locked) {
		slot.encode_us = monotonic_now_us() - slot.submit_us;
		slot.packet.pts_us = slot.pts_us;
		slot.packet.keyframe =
			lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
		const auto *bytes = static_cast<const uint8_t *>(lock.bitstreamBufferPtr);
		// Every AV1 temporal unit starts with a temporal delimiter (see
		// VaapiAv1Encoder); add one if NVENC's output doesn't. 0x12 0x00
		// is the whole OBU: type 2, has_size_field, zero-length payload.
		if (config_.codec == gdp::VideoCodec::AV1 &&
			(lock.bitstreamSizeInBytes < 1 || ((bytes[0] >> 3) & 0xf) != 2)) {
			slot.packet.data = {0x12, 0x00};
		}
		slot.packet.data.insert(slot.packet.data.end(), bytes, bytes + lock.bitstreamSizeInBytes);
		runtime_->api.nvEncUnlockBitstream(session_, slot.bitstream);
	} else {
		WLOG_ERROR("nvenc: nvEncLockBitstream failed (%d): %s", (int)st, last_error().c_str());
	}
	{
		std::lock_guard<std::mutex> guard(mutex_);
		slot.state = Slot::State::Done;
	}
	done_cv_.notify_all();
}

// Main thread: hands on the finished frames, oldest first, and frees their
// slots. Stops at the first still encoding, so packets stay in order.
void NvencEncoder::collect_done() {
	while (!in_flight_.empty()) {
		Slot &slot = slots_[in_flight_.front()];
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (slot.state != Slot::State::Done) {
				return;
			}
		}
		runtime_->api.nvEncUnmapInputResource(session_, slot.mapped);
		slot.mapped = nullptr;
		if (slot.locked) {
			note_timing(slot.copy_us, slot.encode_us, slot.packet.data.size());
			ready_.push_back(std::move(slot.packet));
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			slot.state = Slot::State::Free;
		}
		in_flight_.pop_front();
	}
}

void NvencEncoder::worker_loop() {
	for (;;) {
		int index;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			work_cv_.wait(lock, [&] { return stop_ || !to_lock_.empty(); });
			if (to_lock_.empty()) {
				return; // stopping, and nothing left to finish
			}
			index = to_lock_.front();
			to_lock_.pop_front();
		}
		lock_slot(index);
		uint64_t one = 1;
		(void)!write(done_fd_, &one, sizeof(one));
	}
}

// Every 5 s, like the PyroWave and x264 encoders: where a frame's time
// goes. "encode" runs from submitting the picture to its bitstream being
// locked, so it includes any wait before wraith collects it.
void NvencEncoder::note_timing(int64_t copy_us, int64_t encode_us, size_t bytes) {
	constexpr int64_t kTimingWindowUs = 5'000'000;
	int64_t now = monotonic_now_us();
	if (timing_.window_start_us == 0) {
		timing_.window_start_us = now;
	}
	timing_.frames++;
	timing_.copy_sum_us += copy_us;
	timing_.copy_max_us = std::max(timing_.copy_max_us, copy_us);
	timing_.encode_sum_us += encode_us;
	timing_.encode_max_us = std::max(timing_.encode_max_us, encode_us);
	timing_.bytes += bytes;
	int64_t elapsed = now - timing_.window_start_us;
	if (elapsed < kTimingWindowUs) {
		return;
	}
	double n = (double)timing_.frames;
	WLOG_INFO("nvenc: last %.1fs: %u frames (%.1f fps), %.0f KB avg, copy %.1f ms avg / %.1f max, "
			  "encode %.1f ms avg / %.1f max",
		elapsed / 1e6, timing_.frames, n * 1e6 / (double)elapsed, timing_.bytes / n / 1000.0,
		timing_.copy_sum_us / n / 1000.0, timing_.copy_max_us / 1000.0, timing_.encode_sum_us / n / 1000.0,
		timing_.encode_max_us / 1000.0);
	timing_ = Timing{};
	timing_.window_start_us = now;
}

void NvencEncoder::request_keyframe() {
	force_idr_next_ = true;
}

void NvencEncoder::set_bitrate(uint32_t bitrate_bps) {
	if (!session_ || bitrate_bps == config_.bitrate_bps) {
		return;
	}
	config_.bitrate_bps = bitrate_bps;
	NV_ENC_RC_PARAMS &rc = encode_config_.rcParams;
	rc.averageBitRate = bitrate_bps;
	rc.maxBitRate = bitrate_bps;
	rc.vbvBufferSize = bitrate_bps;
	rc.vbvInitialDelay = bitrate_bps;

	NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
	reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;
	reconfigure.reInitEncodeParams = init_params_; // its encodeConfig points at encode_config_
	reconfigure.resetEncoder = 0;
	reconfigure.forceIDR = 0;
	CudaContextScope scope(cu_context_);
	NVENCSTATUS st = runtime_->api.nvEncReconfigureEncoder(session_, &reconfigure);
	if (st != NV_ENC_SUCCESS) {
		WLOG_ERROR("nvenc: nvEncReconfigureEncoder (bitrate %u) failed (%d): %s", bitrate_bps, (int)st,
			last_error().c_str());
	}
}

std::vector<EncodedPacket> NvencEncoder::poll() {
	if (session_) {
		CudaContextScope scope(cu_context_);
		if (asynchronous_) {
			uint64_t count;
			(void)!read(done_fd_, &count, sizeof(count));
		} else {
			for (int index : in_flight_) {
				std::unique_lock<std::mutex> lock(mutex_);
				if (slots_[index].state == Slot::State::Encoding) {
					lock.unlock();
					lock_slot(index);
				}
			}
		}
		collect_done();
	}
	std::vector<EncodedPacket> out;
	out.swap(ready_);
	return out;
}

void NvencEncoder::close() {
	if (worker_.joinable()) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
		}
		work_cv_.notify_one();
		worker_.join(); // it finishes every frame already handed to it
	}
	stop_ = false;
	to_lock_.clear();
	if (cu_context_) {
		CudaContextScope scope(cu_context_);
		if (session_) {
			for (int index : in_flight_) {
				if (slots_[index].state == Slot::State::Encoding) {
					lock_slot(index);
				}
			}
			collect_done();
			ready_.clear();
			for (Slot &slot : slots_) {
				unregister_input(slot);
				if (slot.bitstream) {
					runtime_->api.nvEncDestroyBitstreamBuffer(session_, slot.bitstream);
					slot.bitstream = nullptr;
				}
			}
			runtime_->api.nvEncDestroyEncoder(session_);
			session_ = nullptr;
		}
		// After the session: NVENC must be done with the buffers first.
		importer_.reset();
		for (Slot &slot : slots_) {
			if (slot.own_input) {
				runtime_->cu->cuMemFree(slot.own_input);
				slot.own_input = 0;
			}
		}
	}
	if (cu_context_) {
		runtime_->cu->cuDevicePrimaryCtxRelease(cu_device_);
		cu_context_ = nullptr;
	}
	if (done_fd_ >= 0) {
		::close(done_fd_);
		done_fd_ = -1;
	}
	for (Slot &slot : slots_) {
		slot = Slot{};
	}
	in_flight_.clear();
	input_pitch_ = 0;
	import_failed_ = false;
}

std::vector<gdp::VideoCodec> NvencEncoder::encodable_codecs(int drm_fd) {
	std::vector<gdp::VideoCodec> result;
	CUdevice device = 0;
	std::string why;
	if (!cuda_device_for_render_node(drm_fd, &device, &why)) {
		return result;
	}
	const NvencRuntime *runtime = nvenc_runtime();
	CUcontext context = nullptr;
	if (runtime->cu->cuDevicePrimaryCtxRetain(&context, device) != CUDA_SUCCESS) {
		return result;
	}
	{
		CudaContextScope scope(context);
		void *session = open_nvenc_session(context, "nvenc probe");
		if (session) {
			std::vector<GUID> guids = session_codec_guids(runtime, session);
			for (gdp::VideoCodec codec :
				{gdp::VideoCodec::H264, gdp::VideoCodec::H265, gdp::VideoCodec::AV1}) {
				CodecGuids wanted;
				if (!guids_for(codec, &wanted)) {
					continue;
				}
				for (const GUID &guid : guids) {
					if (same_guid(guid, wanted.codec)) {
						result.push_back(codec);
						break;
					}
				}
			}
			runtime->api.nvEncDestroyEncoder(session);
		}
	}
	runtime->cu->cuDevicePrimaryCtxRelease(device);
	return result;
}

} // namespace wraith
