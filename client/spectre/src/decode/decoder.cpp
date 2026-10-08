// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "decode/decoder.hpp"
#ifdef SPECTRE_HAVE_PYROWAVE
#include "decode/pyrowave_decode.hpp"
#else
namespace spectre {
// Never constructed without pyrowave; complete only so pyrowave_ can be.
class PyrowaveDecode {};
} // namespace spectre
#endif

#include "decode/codec_support.hpp"
#include "gdp/clock.hpp"
#include "gdp/refine.hpp"
#include "present/vulkan_device.hpp"
#include "log.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#ifdef __linux__
#include <libavutil/hwcontext_vaapi.h>
#endif
#ifdef SPECTRE_VULKAN_DECODE
#include <libavutil/hwcontext_vulkan.h>
#endif
}

#ifdef _WIN32
#include <dxgi1_2.h>
#else
#include <unistd.h>
#endif

#include <cstdio>
#include <cstring>

namespace spectre {

namespace {

// Only h264 decodes in software: h265 and av1 there cost far more CPU per
// frame than a stream is worth, so they decode on the GPU or not at all.
bool hardware_only(gdp::VideoCodec codec) {
	return codec != gdp::VideoCodec::H264;
}

} // namespace

// FFmpeg's get_format callback, reached through AVCodecContext::opaque. A
// friend of Decoder so it can read hw_pix_fmt_ and demote backend_.
struct DecoderFormatPicker {
	static enum AVPixelFormat pick(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
		auto *decoder = (Decoder *)ctx->opaque;
		if (decoder->backend_ != DecodeBackend::Software) {
			for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
				if (*p == decoder->hw_pix_fmt_) {
					return *p;
				}
			}
		}
		// No hardware format on offer: either the hardware backend was
		// never there, or FFmpeg already tried it for this stream and its
		// hwaccel failed to initialise (a profile, level or size the GPU's
		// video engine doesn't do) -- FFmpeg then drops that format and asks
		// again. Take the software format rather than failing every frame
		// from here on; VideoImageSource handles both kinds of frame. Not
		// for a hardware-only codec.
		if (hardware_only(decoder->codec_)) {
			SLOG_ERROR("decoder: %s can't decode this %s stream", decoder->backend_name(),
				gdp::video_codec_token(decoder->codec_));
			return AV_PIX_FMT_NONE;
		}
		enum AVPixelFormat software = AV_PIX_FMT_NONE;
		for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
			const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
			if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
				software = *p;
				break;
			}
		}
		if (decoder->backend_ != DecodeBackend::Software) {
			SLOG_INFO("decoder: %s can't decode this stream, falling back to software decode",
				decoder->backend_name());
			decoder->backend_ = DecodeBackend::Software;
		}
		return software;
	}
};

namespace {

#ifdef _WIN32
// FFmpeg's D3D11VA device factory takes the adapter as a decimal index into
// IDXGIFactory1::EnumAdapters1, not a LUID, so translate. -1 if no adapter
// carries that LUID (a GPU that went away between Vulkan device creation and
// here, or a Vulkan implementation reporting a LUID DXGI doesn't know).
int adapter_index_for_luid(const uint8_t luid[8]) {
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory))) {
		SLOG_ERROR("decoder: CreateDXGIFactory1 failed");
		return -1;
	}
	int found = -1;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; found < 0 && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
			memcmp(&desc.AdapterLuid, luid, sizeof(desc.AdapterLuid)) == 0) {
			found = (int)i;
		}
		adapter->Release();
	}
	factory->Release();
	return found;
}
#endif // _WIN32

// Wire codec token -> FFmpeg decoder. The one place spectre's decode side
// knows anything codec-specific; adding a codec is a line here plus its
// token in gdp/video_codec.hpp (docs/design/encoding.md). Lossless
// refinement is not FFmpeg's business -- decode() strips its container
// off first, whatever the codec.
AVCodecID codec_id(gdp::VideoCodec codec) {
	switch (codec) {
	case gdp::VideoCodec::H264: return AV_CODEC_ID_H264;
	case gdp::VideoCodec::H265: return AV_CODEC_ID_HEVC;
	case gdp::VideoCodec::AV1: return AV_CODEC_ID_AV1;
	case gdp::VideoCodec::Pyrowave: // not FFmpeg's: pyrowave_decode.hpp
	case gdp::VideoCodec::Unknown: break;
	}
	return AV_CODEC_ID_NONE;
}

const AVCodec *find_codec(gdp::VideoCodec codec) {
	// av1 is hardware only, so always FFmpeg's own decoder, the one its
	// hwaccels hang off: FFmpeg prefers libdav1d where it has it, which
	// takes no hwaccel and would decode on the CPU.
	if (codec == gdp::VideoCodec::AV1) {
		return avcodec_find_decoder_by_name("av1");
	}
	AVCodecID id = codec_id(codec);
	// Not just a token -> id mapping: a distro FFmpeg built without,
	// say, an H.265 decoder must not have spectre offering h265.
	return id == AV_CODEC_ID_NONE ? nullptr : avcodec_find_decoder(id);
}

} // namespace

bool Decoder::can_decode(const std::string &codec) {
	gdp::VideoCodec id = gdp::video_codec_from_token(codec);
	if (id == gdp::VideoCodec::Pyrowave) {
		// Built in or not; whether the GPU runs it is codec_support's call.
#ifdef SPECTRE_HAVE_PYROWAVE
		return true;
#else
		return false;
#endif
	}
	return find_codec(id) != nullptr;
}

bool Decoder::can_decode_in_software(const std::string &codec) {
	gdp::VideoCodec video_codec = gdp::video_codec_from_token(codec);
	if (hardware_only(video_codec)) {
		return false;
	}
	AVCodecID id = codec_id(video_codec);
	void *it = nullptr;
	while (const AVCodec *c = av_codec_iterate(&it)) {
		if (c->id != id || !av_codec_is_decoder(c)) {
			continue;
		}
		// Wrappers around a vendor's decode API (cuvid, qsv, ...).
		if (c->capabilities & AV_CODEC_CAP_HARDWARE) {
			continue;
		}
		return true;
	}
	return false;
}

Decoder::Decoder() : packet_(av_packet_alloc()), frame_(av_frame_alloc()) {}

Decoder::~Decoder() {
	close();
	av_packet_free(&packet_);
	av_frame_free(&frame_);
}

#ifdef _WIN32
void Decoder::set_adapter_luid(const uint8_t luid[8]) {
	memcpy(adapter_luid_, luid, sizeof(adapter_luid_));
	adapter_luid_valid_ = true;
}
#endif

bool Decoder::backend_from_token(const std::string &token, DecodeBackend *out) {
	if (token == "vulkan") {
		*out = DecodeBackend::Vulkan;
	} else if (token == "software") {
		*out = DecodeBackend::Software;
#if defined(_WIN32)
	} else if (token == "d3d11va" || token == "d3d") {
		*out = DecodeBackend::D3d11va;
#elif defined(__APPLE__)
	} else if (token == "videotoolbox" || token == "vt") {
		*out = DecodeBackend::VideoToolbox;
#else
	} else if (token == "vaapi") {
		*out = DecodeBackend::Vaapi;
	} else if (token == "v4l2" || token == "v4l2m2m") {
		*out = DecodeBackend::V4l2m2m;
#endif
	} else {
		return false;
	}
	return true;
}

const char *Decoder::backend_name() const {
	if (pyrowave_) {
		return "pyrowave (vulkan compute)";
	}
	switch (backend_) {
	case DecodeBackend::Vaapi: return "vaapi (hardware)";
	case DecodeBackend::D3d11va: return "d3d11va (hardware)";
	case DecodeBackend::VideoToolbox: return "videotoolbox (hardware)";
	case DecodeBackend::Vulkan: return "vulkan (hardware)";
	case DecodeBackend::V4l2m2m: return "v4l2m2m (hardware)";
	case DecodeBackend::Software: break;
	}
	return "ffmpeg (software)";
}

// The native backend's device: VA-API on the render node (Linux), D3D11VA
// on the adapter matching set_adapter_luid() (Windows), VideoToolbox
// (macOS, which picks its own engine).
bool Decoder::create_native_device(const HardwareDecode &hw) {
#if defined(__APPLE__)
	(void)hw;
	if (av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0) != 0) {
		SLOG_ERROR("decoder: av_hwdevice_ctx_create(VideoToolbox) failed");
		return false;
	}
	hw_pix_fmt_ = AV_PIX_FMT_VIDEOTOOLBOX;
#elif !defined(_WIN32)
	if (av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI, hw.drm_render_node, nullptr, 0) !=
		0) {
		SLOG_ERROR("decoder: av_hwdevice_ctx_create(VAAPI, %s) failed", hw.drm_render_node);
		return false;
	}
	hw_pix_fmt_ = AV_PIX_FMT_VAAPI;
#else
	(void)hw;
	if (!adapter_luid_valid_) {
		SLOG_INFO("decoder: no adapter LUID set, skipping D3D11VA");
		return false;
	}
	int adapter = adapter_index_for_luid(adapter_luid_);
	if (adapter < 0) {
		SLOG_INFO("decoder: no DXGI adapter matches the Vulkan device, skipping D3D11VA");
		return false;
	}
	char adapter_arg[16];
	snprintf(adapter_arg, sizeof(adapter_arg), "%d", adapter);
	if (av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_D3D11VA, adapter_arg, nullptr, 0) != 0) {
		SLOG_ERROR("decoder: av_hwdevice_ctx_create(D3D11VA, adapter %d) failed", adapter);
		return false;
	}
	hw_pix_fmt_ = AV_PIX_FMT_D3D11;
#endif
	return true;
}

// The Vulkan Video backend's device: not a new one, but FFmpeg's wrapper
// around the presenter's (an AVVulkanDeviceContext filled in by hand and
// then initialised, rather than av_hwdevice_ctx_create()). FFmpeg never
// destroys a device it was handed, so this is safe to free before
// VulkanDevice goes away -- and must be, see StreamSession's teardown.
bool Decoder::create_vulkan_device(const VulkanDecodeDevice &vk) {
#ifdef SPECTRE_VULKAN_DECODE
	hw_device_ctx_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
	if (!hw_device_ctx_) {
		SLOG_INFO("decoder: this FFmpeg has no Vulkan hwcontext");
		return false;
	}
	auto *device_ctx = (AVHWDeviceContext *)hw_device_ctx_->data;
	auto *vk_ctx = (AVVulkanDeviceContext *)device_ctx->hwctx;
	vk_ctx->get_proc_addr = vk.get_instance_proc_addr;
	vk_ctx->inst = vk.instance;
	vk_ctx->phys_dev = vk.physical_device;
	vk_ctx->act_dev = vk.device;
	vk_ctx->device_features = *vk.features;
	vk_ctx->enabled_inst_extensions = vk.instance_extensions;
	vk_ctx->nb_enabled_inst_extensions = vk.instance_extension_count;
	vk_ctx->enabled_dev_extensions = vk.device_extensions;
	vk_ctx->nb_enabled_dev_extensions = vk.device_extension_count;

	// One queue per family, exactly what VulkanDevice created. The graphics
	// one is shared with the presenter; FFmpeg only uses it for frame
	// setup, and on the same thread (Decoder::decode() and present() both
	// run on StreamSession's loop), so its internal per-queue mutex is
	// enough and no lock_queue hook is needed.
	vk_ctx->qf[0].idx = (int)vk.graphics_family;
	vk_ctx->qf[0].num = 1;
	vk_ctx->qf[0].flags = (VkQueueFlagBits)vk.graphics_flags;
	vk_ctx->nb_qf = 1;
	if (vk.decode_family == vk.graphics_family) {
		vk_ctx->qf[0].flags = (VkQueueFlagBits)(vk.graphics_flags | VK_QUEUE_VIDEO_DECODE_BIT_KHR);
		vk_ctx->qf[0].video_caps = (VkVideoCodecOperationFlagBitsKHR)vk.decode_ops;
	} else {
		vk_ctx->qf[1].idx = (int)vk.decode_family;
		vk_ctx->qf[1].num = 1;
		vk_ctx->qf[1].flags = VK_QUEUE_VIDEO_DECODE_BIT_KHR;
		vk_ctx->qf[1].video_caps = (VkVideoCodecOperationFlagBitsKHR)vk.decode_ops;
		vk_ctx->nb_qf = 2;
	}

	int ret = av_hwdevice_ctx_init(hw_device_ctx_);
	if (ret < 0) {
		char errbuf[64];
		av_strerror(ret, errbuf, sizeof(errbuf));
		SLOG_ERROR("decoder: av_hwdevice_ctx_init(Vulkan) failed: %s", errbuf);
		return false;
	}
	hw_pix_fmt_ = AV_PIX_FMT_VULKAN;
	return true;
#else
	(void)vk;
	SLOG_INFO("decoder: built without FFmpeg's Vulkan hwcontext");
	return false;
#endif
}

// Everything between "which GPU" and a decoder context producing
// `backend`'s hardware frames. False (with a reason logged) leaves open()
// to try the next backend; the partial state is torn down by its close()
// call, not here.
bool Decoder::open_hardware(const AVCodec *codec, DecodeBackend backend, const HardwareDecode &hw) {
	if (backend == DecodeBackend::Vulkan) {
		// Checked here, not left to FFmpeg: its Vulkan hwaccel would only
		// find out at the first keyframe.
		VkVideoCodecOperationFlagsKHR op = 0;
		switch (codec_) {
		case gdp::VideoCodec::H264: op = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR; break;
		case gdp::VideoCodec::H265: op = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR; break;
		case gdp::VideoCodec::AV1: op = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR; break;
		case gdp::VideoCodec::Pyrowave:
		case gdp::VideoCodec::Unknown: break;
		}
		if (!(hw.vulkan->decode_ops & op)) {
			SLOG_INFO("decoder: the GPU's Vulkan decode queue doesn't do this codec");
			return false;
		}
		if (!create_vulkan_device(*hw.vulkan)) {
			return false;
		}
	} else if (!create_native_device(hw)) {
		return false;
	}

	ctx_ = avcodec_alloc_context3(codec);
	if (!ctx_) {
		return false;
	}
	backend_ = backend;
	ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
	ctx_->opaque = this;
	ctx_->get_format = DecoderFormatPicker::pick;
	if (avcodec_open2(ctx_, codec, nullptr) != 0) {
		SLOG_ERROR("decoder: %s avcodec_open2 failed", backend_name());
		backend_ = DecodeBackend::Software;
		return false;
	}
	return true;
}

// FFmpeg's stateful V4L2 decoder for `codec`, when a /dev/video* device
// decodes it. Asks for YUV420P, the format VideoImageSource uploads: through
// get_format on Raspberry Pi's FFmpeg (which offers DRM_PRIME first), and
// through pix_fmt and its pixel_format option otherwise.
bool Decoder::open_v4l2(gdp::VideoCodec codec) {
#ifdef __linux__
	const char *name = codec == gdp::VideoCodec::H264 ? "h264_v4l2m2m"
		: codec == gdp::VideoCodec::H265              ? "hevc_v4l2m2m"
													  : nullptr;
	const AVCodec *v4l2 = name ? avcodec_find_decoder_by_name(name) : nullptr;
	if (!v4l2) {
		return false;
	}
	if (!v4l2_m2m_decodes(codec)) {
		SLOG_INFO("decoder: no V4L2 decoder for this codec, skipping %s", name);
		return false;
	}
	ctx_ = avcodec_alloc_context3(v4l2);
	if (!ctx_) {
		return false;
	}
	backend_ = DecodeBackend::V4l2m2m;
	hw_pix_fmt_ = AV_PIX_FMT_YUV420P;
	ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
	ctx_->opaque = this;
	ctx_->get_format = DecoderFormatPicker::pick;
	AVDictionary *options = nullptr;
	av_dict_set(&options, "pixel_format", "yuv420p", 0);
	int ret = avcodec_open2(ctx_, v4l2, &options);
	av_dict_free(&options);
	if (ret != 0) {
		SLOG_ERROR("decoder: %s avcodec_open2 failed", name);
		backend_ = DecodeBackend::Software;
		return false;
	}
	v4l2_seq_ = 0;
	v4l2_late_ = 0;
	v4l2_timeouts_ = 0;
	return true;
#else
	(void)codec;
	return false;
#endif
}

bool Decoder::open(const std::string &codec_name, bool refine, const HardwareDecode &hw) {
	// SessionClient already refuses an accept naming anything it didn't
	// offer, and the offer (probe_decodable_codecs()) only holds codecs
	// can_decode() passes -- so by the time we're here the codec is one
	// find_codec() resolves, or pyrowave.
	codec_ = gdp::video_codec_from_token(codec_name);
	if (codec_ == gdp::VideoCodec::Pyrowave) {
		lossless_valid_ = false;
		refine_ = refine;
		backend_ = DecodeBackend::Vulkan;
#ifdef SPECTRE_HAVE_PYROWAVE
		if (!hw.compute) {
			SLOG_ERROR("decoder: pyrowave session, but the presenter's GPU can't run its decode");
			return false;
		}
		pyrowave_ = std::make_unique<PyrowaveDecode>();
		if (!pyrowave_->open(*hw.compute)) {
			pyrowave_.reset();
			return false;
		}
		SLOG_INFO("decoder: using %s", backend_name());
		return true;
#else
		SLOG_ERROR("decoder: built without pyrowave");
		return false;
#endif
	}
	const AVCodec *codec = find_codec(codec_);
	if (!codec) {
		SLOG_ERROR("decoder: no decoder for codec \"%s\"", codec_name.c_str());
		return false;
	}
	if (!packet_ || !frame_) {
		SLOG_ERROR("decoder: out of memory");
		return false;
	}
	lossless_valid_ = false;
	refine_ = refine;
	backend_ = DecodeBackend::Software;

	// Hardware decode first (the no-readback path): the backend `hw` names,
	// then the platform's other one, then V4L2 on Linux, then software (h264
	// only). Only
	// open-time failures are handled here -- device creation or
	// avcodec_open2 itself failing, the common cases for "no capable GPU
	// here" or "driver missing/broken". A hwaccel FFmpeg
	// can't set up for the actual stream falls back to software (h264) at
	// the first keyframe instead (DecoderFormatPicker). What isn't recovered is
	// a hardware decoder that sets up fine and then fails frame by frame --
	// see docs/design/spectre-client.md#limitations.
	//
	// Which GPU: always the presenter's. Vulkan Video decodes on its very
	// device; VA-API on the render node it was picked by; D3D11VA on the
	// adapter with its LUID. The decoded surface is shared into Vulkan,
	// never copied through the CPU, and a share only works within one GPU.
	// (VideoToolbox is the exception: its frames are copied in, and the
	// memory is shared anyway -- see video_image_source.hpp.)
	const DecodeBackend native = kNativeDecodeBackend;
	// The requested hardware backend, then the other one; nothing for
	// software.
	DecodeBackend order[3];
	int count = 0;
	if (hw.backend == DecodeBackend::Vulkan) {
		order[count++] = DecodeBackend::Vulkan;
		order[count++] = native;
	} else if (hw.backend == DecodeBackend::V4l2m2m) {
		order[count++] = DecodeBackend::V4l2m2m;
		order[count++] = DecodeBackend::Vulkan;
		order[count++] = native;
	} else if (hw.backend != DecodeBackend::Software) {
		order[count++] = native;
		order[count++] = DecodeBackend::Vulkan;
	}
#ifdef __linux__
	// Last of the hardware ones unless asked for: a GPU decoder never needs
	// the copy V4L2's frames take.
	if (count > 0 && hw.backend != DecodeBackend::V4l2m2m) {
		order[count++] = DecodeBackend::V4l2m2m;
	}
#endif
	for (int i = 0; i < count; i++) {
		DecodeBackend backend = order[i];
		if (backend == DecodeBackend::V4l2m2m) {
			if (open_v4l2(codec_)) {
				SLOG_INFO("decoder: using %s", backend_name());
				return true;
			}
			close();
			continue;
		}
		if (backend == DecodeBackend::Vulkan && !hw.vulkan) {
			SLOG_INFO("decoder: the GPU has no Vulkan Video decode queue, skipping Vulkan");
			continue;
		}
		if (backend != DecodeBackend::Vulkan && !hw.native_importable) {
			SLOG_INFO("decoder: %s frames can't be imported here, skipping it", kNativeDecodeName);
			continue;
		}
		if (open_hardware(codec, backend, hw)) {
			SLOG_INFO("decoder: using %s", backend_name());
			return true;
		}
		close(); // tear down the partial attempt before the next one
	}
	if (hardware_only(codec_)) {
		SLOG_ERROR("decoder: no hardware decoder opened for %s, which has no software decode",
			codec_name.c_str());
		return false;
	}
	if (count > 0) {
		SLOG_INFO("decoder: no hardware decoder opened, falling back to software decode");
	}

	ctx_ = avcodec_alloc_context3(codec);
	if (!ctx_) {
		return false;
	}
	if (avcodec_open2(ctx_, codec, nullptr) < 0) {
		SLOG_ERROR("decoder: software avcodec_open2 failed");
		close();
		return false;
	}
	SLOG_INFO("decoder: using %s", backend_name());
	return true;
}

bool Decoder::decode(const uint8_t *data, size_t len) {
	lossless_valid_ = false;

	// On a refined session every payload is a container: the codec's own
	// bytes plus this frame's lossless tiles (gdp/refine.hpp). Split it
	// off before FFmpeg sees anything -- `data`/`len` below are the base
	// layer from here on. A payload that doesn't parse is dropped rather
	// than fed to the decoder as-is (the caller reports it lost, and the
	// keyframe that triggers recovers).
	if (refine_) {
		const uint8_t *base = nullptr;
		size_t base_len = 0;
		if (!gdp::refine_parse_frame(data, len, &base, &base_len, &lossless_)) {
			SLOG_ERROR("decoder: malformed refined frame (%zu bytes), dropping", len);
			return false;
		}
		lossless_valid_ = true;
		data = base;
		len = base_len;
		// A layer-only frame (gdp-spec.md §9.5: base_len 0): nothing for
		// the codec, the layer applies to the picture already on screen.
		// Nothing is sent to FFmpeg -- an empty packet would mean "flush".
		if (len == 0) {
			return true;
		}
	}

#ifdef SPECTRE_HAVE_PYROWAVE
	if (pyrowave_) {
		if (!pyrowave_->decode(data, len, frame_)) {
			return false;
		}
		if (on_frame) {
			on_frame(frame_);
		}
		av_frame_unref(frame_);
		return true;
	}
#endif

	// Copy into an av_new_packet() buffer rather than pointing at the
	// caller's bytes: FFmpeg requires packet data to be followed by
	// AV_INPUT_BUFFER_PADDING_SIZE zero bytes (the bitstream readers may
	// read past the end), which av_new_packet() provides and the
	// reassembler's exactly-sized vector does not.
	if (av_new_packet(packet_, (int)len) < 0) {
		SLOG_ERROR("decoder: failed to allocate packet");
		return false;
	}
	memcpy(packet_->data, data, len);

	// A V4L2 decoder works asynchronously: FFmpeg hands it the packet and,
	// with little queued, returns at once without waiting for the picture,
	// which would then only come out with the *next* packet -- on an idle
	// desktop that can be seconds later. So wait here for this packet's
	// picture (a few ms of hardware time), polling, up to a bound that
	// keeps a stuck decoder from freezing the session. The packet's pts is
	// a counter so a picture that does come out late shows up in the log.
	const bool v4l2 = backend_ == DecodeBackend::V4l2m2m;
	constexpr uint64_t kV4l2WaitUs = 100000;
	const uint64_t wait_until_us = v4l2 ? gdp::monotonic_us() + kV4l2WaitUs : 0;
	bool got_frame = false;
	if (v4l2) {
		packet_->pts = ++v4l2_seq_;
	}

	int send_ret = avcodec_send_packet(ctx_, packet_);
	av_packet_unref(packet_);
	if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
		char errbuf[64];
		av_strerror(send_ret, errbuf, sizeof(errbuf));
		SLOG_ERROR("decoder: avcodec_send_packet failed: %s", errbuf);
		return false;
	}

	for (;;) {
		int recv_ret = avcodec_receive_frame(ctx_, frame_);
		if (recv_ret == AVERROR(EAGAIN) && v4l2 && !got_frame) {
			if (gdp::monotonic_us() < wait_until_us) {
#ifndef _WIN32
				usleep(500);
#endif
				continue;
			}
			if (v4l2_timeouts_++ % 100 == 0) {
				SLOG_INFO("decoder: v4l2m2m: no picture within %llu ms of its packet (%llu times so far)",
					(unsigned long long)(kV4l2WaitUs / 1000), (unsigned long long)v4l2_timeouts_);
			}
		}
		if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
			break;
		}
		if (recv_ret < 0) {
			char errbuf[64];
			av_strerror(recv_ret, errbuf, sizeof(errbuf));
			SLOG_ERROR("decoder: avcodec_receive_frame failed: %s", errbuf);
			break;
		}
		if (v4l2) {
			got_frame = true;
			if (frame_->pts != AV_NOPTS_VALUE && frame_->pts < v4l2_seq_ && v4l2_late_++ % 100 == 0) {
				SLOG_INFO("decoder: v4l2m2m: picture %lld came out with packet %lld (%llu late so far)",
					(long long)frame_->pts, (long long)v4l2_seq_, (unsigned long long)v4l2_late_);
			}
		}
		if (on_frame) {
			on_frame(frame_);
		}
		av_frame_unref(frame_);
	}
	return true;
}

void Decoder::close() {
	pyrowave_.reset();
	if (ctx_) {
		avcodec_free_context(&ctx_);
		ctx_ = nullptr;
	}
	if (hw_device_ctx_) {
		av_buffer_unref(&hw_device_ctx_);
		hw_device_ctx_ = nullptr;
	}
}

} // namespace spectre
