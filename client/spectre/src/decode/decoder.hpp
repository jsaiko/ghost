// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Video decode. Every codec token this build can decode: h264, h265 and
// av1 through FFmpeg (where it has a decoder for them; see codec_id() in
// decoder.cpp), and pyrowave through PyrowaveDecode. FFmpeg decode prefers
// hardware, with the frame never leaving the GPU. Two hardware backends per
// platform -- open() tries the one HardwareDecode names, then the other:
// - The platform-native one. Linux: VA-API -- frames stay in a VASurface
//   (AV_PIX_FMT_VAAPI) and video_image_source.cpp imports that surface's
//   dmabuf into Vulkan. Windows: D3D11VA -- frames stay in an
//   ID3D11Texture2D (AV_PIX_FMT_D3D11) and video_image_source.cpp shares
//   that texture into Vulkan. Needs set_adapter_luid() first. macOS:
//   VideoToolbox -- frames arrive as CVPixelBuffers
//   (AV_PIX_FMT_VIDEOTOOLBOX) and video_image_source.cpp copies their
//   planes into its staging image.
// - Vulkan Video, wherever the GPU has a decode queue: FFmpeg decodes on
//   the presenter's own VkDevice (VulkanDevice::decode_device()), so frames
//   arrive as VkImages (AV_PIX_FMT_VULKAN) with nothing to import. The only
//   hardware path on NVIDIA under Linux, which has no VA-API.
// Linux, after both of those (or first, under -X v4l2): a V4L2
// memory-to-memory decoder through FFmpeg's h264_v4l2m2m (hevc_v4l2m2m),
// wherever a /dev/video* device decodes the codec (v4l2_m2m_decodes()) --
// the Raspberry Pi 4's bcm2835-codec. Its frames come back as
// AV_PIX_FMT_YUV420P in system memory and upload like software ones;
// decode() waits for each packet's picture (see there).
// If none opens (no capable GPU, missing/broken driver, etc.), open()
// falls back to FFmpeg's plain software decoder, which yields
// AV_PIX_FMT_YUV420P frames in system memory; video_image_source.cpp
// uploads those via a host-visible staging image instead. The same happens
// mid-stream if FFmpeg can't set a hardware decoder up for the stream it
// actually gets (a profile the GPU doesn't do) -- see backend(). Only
// h264 falls back: h265 and av1 decode in hardware or not at all.
#pragma once

#include "gdp/refine.hpp"
#include "gdp/video_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct AVCodec;
struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;

namespace spectre {

struct VulkanDecodeDevice;
class PyrowaveDecode;

// Which decoder open() landed on (and, after a mid-stream fallback, is on
// now). Vaapi only exists on Linux builds, D3d11va only on Windows ones,
// VideoToolbox only on macOS ones.
enum class DecodeBackend {
	Software,
	Vaapi,
	D3d11va,
	VideoToolbox,
	Vulkan,
	// Linux only: FFmpeg's *_v4l2m2m decoders (see the file comment).
	V4l2m2m,
};

// This platform's own hardware decoder (the "native" one above), and its
// name for log lines.
#if defined(_WIN32)
constexpr DecodeBackend kNativeDecodeBackend = DecodeBackend::D3d11va;
constexpr const char *kNativeDecodeName = "D3D11VA";
#elif defined(__APPLE__)
constexpr DecodeBackend kNativeDecodeBackend = DecodeBackend::VideoToolbox;
constexpr const char *kNativeDecodeName = "VideoToolbox";
#else
constexpr DecodeBackend kNativeDecodeBackend = DecodeBackend::Vaapi;
constexpr const char *kNativeDecodeName = "VA-API";
#endif

// What open() should decode with. `backend` is the decoder to try first;
// the other hardware one is next (then V4L2 on Linux), then software
// (Software itself means software only). A backend whose frames the presenter can't take is
// skipped.
struct HardwareDecode {
	DecodeBackend backend = DecodeBackend::Software;
	// VA-API / D3D11VA / VideoToolbox: whether the presenter can take their
	// frames (VulkanPresenter::supports_dmabuf_import() / adapter_luid();
	// always on macOS).
	bool native_importable = false;
	// VA-API: which GPU to decode on. Must be the node VulkanPresenter is
	// bound to -- both need the same physical GPU for the zero-copy
	// VASurface -> VkImage import to succeed.
	const char *drm_render_node = nullptr;
	// Vulkan Video: the presenter's device, or null when it has no decode
	// queue (VulkanPresenter::decode_device()).
	const VulkanDecodeDevice *vulkan = nullptr;
	// PyroWave's compute decode: the presenter's device, or null when it
	// can't run it (VulkanPresenter::compute_device()). The only way
	// "pyrowave" decodes -- there is no software or other fallback.
	const VulkanDecodeDevice *compute = nullptr;
};

class Decoder {
public:
	Decoder();
	~Decoder();
	Decoder(const Decoder &) = delete;
	Decoder &operator=(const Decoder &) = delete;

	// `codec` is the session's negotiated video codec as named on the wire
	// (SessionAccept.codec / gdp/video_codec.hpp's tokens). Opening one
	// this build has no decoder for fails -- the offer
	// (probe_decodable_codecs()) only holds codecs that pass can_decode(),
	// and SessionClient refuses an accept naming anything else.
	//
	// `refine` is whether the session negotiated lossless refinement
	// (gdp/refine.hpp): every payload is then a container around the
	// codec's bytes plus a lossless tile layer, and decode() splits the
	// two (see lossless_update()). Without it, payloads are the bare
	// elementary stream.
	//
	// `hw` names the decoder to try first (see the file comment). Returns
	// false only if software fails to open too.
	bool open(const std::string &codec, bool refine, const HardwareDecode &hw);

#ifdef _WIN32
	// Windows' equivalent of `drm_render_node`: which GPU to decode on. The
	// decoded NV12 texture is shared into Vulkan, which only works within one
	// adapter, so this must be the LUID VulkanDevice::device_luid() reports
	// (8 bytes, a Win32 LUID). open() skips the D3D11VA attempt entirely
	// unless this has been called first -- guessing an adapter would as
	// easily land on the wrong GPU as the right one.
	void set_adapter_luid(const uint8_t luid[8]);
#endif

	// Which backend decode() is delivering frames from. Hardware frames
	// present without a readback -- AV_PIX_FMT_VAAPI (dmabuf import),
	// AV_PIX_FMT_D3D11 (shared-texture import) or AV_PIX_FMT_VULKAN (the
	// presenter's own VkImage). Software means AV_PIX_FMT_YUV420P frames,
	// CPU-uploaded. Valid after a successful open(); can drop to Software
	// mid-stream (see the file comment), never the other way.
	DecodeBackend backend() const { return backend_; }
	bool using_hardware() const { return backend_ != DecodeBackend::Software; }
	// For the stats overlay and logs: "vaapi (hardware)", "ffmpeg (software)"...
	const char *backend_name() const;

	// spectre -X's tokens: "vulkan", "software", and this platform's own --
	// "vaapi" and "v4l2" (or "v4l2m2m") on Linux, "d3d11va" (or "d3d") on
	// Windows, "videotoolbox" (or "vt") on macOS. False for anything else,
	// including another platform's token.
	static bool backend_from_token(const std::string &token, DecodeBackend *out);

	// Feeds one coded frame (SessionClient::on_video_frame's payload,
	// already reassembled from slices). May invoke on_frame zero, one, or
	// more times per call (decoder-internal reordering/buffering). On a
	// refined session a payload with no base bytes at all is a layer-only
	// frame: it is not fed to the codec and on_frame is not called, but
	// lossless_update() holds its layer. Returns false for a payload that
	// could not be parsed or that the codec rejected -- the caller reports
	// that frame as lost.
	bool decode(const uint8_t *data, size_t len);

	// The lossless tile layer split off the payload of the most recent
	// decode() call, or null on a session without lossless refinement or
	// a frame with no layer. Valid from that decode() call's on_frame
	// callback (if any) until the next decode() call, so a caller can
	// apply it whether or not a picture came out.
	//
	// The layer belongs to the picture the same call emits, which holds
	// because wraith's encoders use no B-frames and so never reorder. A
	// codec that does reorder would need the layer carried on the
	// AVFrame's opaque_ref instead.
	const gdp::RefineLayer *lossless_update() const { return lossless_valid_ ? &lossless_ : nullptr; }

	// True if this build can open a decoder for `codec` -- what
	// probe_decodable_codecs() filters the offer through, so that spectre
	// never offers a codec its FFmpeg (or build) is without.
	static bool can_decode(const std::string &codec);
	// True if FFmpeg can decode `codec` with no hardware at all -- what
	// open() falls back to when every hardware decoder is out. Only ever
	// h264: h265 and av1 decode in hardware only.
	static bool can_decode_in_software(const std::string &codec);

	// Fired synchronously from decode(). `frame` is only valid for the
	// duration of the callback -- unref'd immediately after it returns, so
	// the callback must finish presenting (or copying out of) it before
	// returning.
	std::function<void(AVFrame *frame)> on_frame;

	void close();

private:
	friend struct DecoderFormatPicker;

	bool open_hardware(const AVCodec *codec, DecodeBackend backend, const HardwareDecode &hw);
	bool create_native_device(const HardwareDecode &hw);
	bool create_vulkan_device(const VulkanDecodeDevice &vk);
	bool open_v4l2(gdp::VideoCodec codec);

	AVCodecContext *ctx_ = nullptr;
	AVBufferRef *hw_device_ctx_ = nullptr;
	// Reused by every decode() call rather than allocated per frame.
	AVPacket *packet_ = nullptr;
	AVFrame *frame_ = nullptr;
	DecodeBackend backend_ = DecodeBackend::Software;
	// The AVPixelFormat get_format picks while backend_ is a hardware one.
	int hw_pix_fmt_ = -1;
	// V4l2m2m: each packet's pts (a counter), so decode() can tell its
	// picture from a late one; and how often a picture came out late or
	// not at all within the wait, for the log.
	int64_t v4l2_seq_ = 0;
	uint64_t v4l2_late_ = 0;
	uint64_t v4l2_timeouts_ = 0;
#ifdef _WIN32
	uint8_t adapter_luid_[8] = {};
	bool adapter_luid_valid_ = false;
#endif

	gdp::VideoCodec codec_ = gdp::VideoCodec::Unknown;
	bool refine_ = false;
	// Set instead of ctx_ on a "pyrowave" session (pyrowave_decode.hpp).
	std::unique_ptr<PyrowaveDecode> pyrowave_;
	// Reused across frames so the tile-pixel buffer isn't reallocated 60
	// times a second; lossless_valid_ says whether it holds this frame's
	// layer or a stale one.
	gdp::RefineLayer lossless_;
	bool lossless_valid_ = false;
};

} // namespace spectre
