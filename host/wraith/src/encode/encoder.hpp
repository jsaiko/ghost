// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Encoder interface (docs/design/encoding.md): open(config) -> push(dmabuf,
// pts) -> poll() -> packets. VA-API (vaapi/) and NVENC (nvenc/) are the
// hardware backends, both zero-copy from a dmabuf; pyrowave/ runs on
// Vulkan compute; software/ (x264, CPU-side) is the fallback for hosts
// with no hardware encoder -- see wants_cpu_frame()/push_cpu() below.
//
// Which backend serves a given session is encoder_factory.hpp's job: it
// maps the negotiated wire codec (EncoderConfig::codec) to an ordered list
// of candidate backends and opens the first that works. Everything below
// is codec-agnostic; a new codec is a new backend plus a row in that
// table, not a change here. refine/ is not a backend but a decorator that
// wraps whichever one opened (see refine/refine_encoder.hpp).
#pragma once

#include "gdp/video_codec.hpp"
#include "util/config.hpp" // LinkProfile

#include <cstdint>
#include <vector>

namespace wraith {

class TileSource; // refine/tile_source.hpp

// One dmabuf-backed frame, as a capture source hands it over:
// PipeWireCapture fills one in straight from a PipeWire buffer,
// ExtImageCopyCapture from its own gbm buffers.
struct DmabufFrame {
	static constexpr int kMaxPlanes = 4;

	int32_t width = 0;
	int32_t height = 0;
	uint32_t format = 0;   // DRM_FORMAT_*
	uint64_t modifier = 0; // DRM_FORMAT_MOD_*
	int n_planes = 0;
	uint32_t offset[kMaxPlanes] = {};
	uint32_t stride[kMaxPlanes] = {};
	int fd[kMaxPlanes] = {};
};

// Which part of a CPU frame the compositor repainted since the frame
// pushed before it, in frame pixels. Callers that know pass it alongside
// push_cpu()'s pixels (a screencast source's damage metadata); a null pointer means "unknown". Plain codecs
// ignore it; lossless refinement (refine/tile_tracker.hpp) treats the
// tiles it touches as active -- a playing video keeps repainting its
// still patches, which is what keeps them from settling -- but never
// trusts it to say what changed: the pixels are hashed regardless.
struct DamageRect {
	int32_t x = 0;
	int32_t y = 0;
	int32_t width = 0;
	int32_t height = 0;
};

struct DamageRegion {
	std::vector<DamageRect> rects;
};

struct EncoderConfig {
	// The session's negotiated wire codec (gdp-spec.md §6.6). Which
	// backend actually serves it is encoder_factory.cpp's business --
	// "h264" is VA-API, NVENC or x264, and lossless refinement wraps
	// whichever row opened (refine/refine_encoder.hpp). A backend only ever sees a
	// codec it was registered for.
	gdp::VideoCodec codec = gdp::VideoCodec::H264;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t bitrate_bps = 80'000'000;
	uint32_t framerate_num = 60;
	uint32_t framerate_den = 1;
	// Frames between periodic IDRs. Long, because every lost frame is
	// already repaired by a requested keyframe (GdpSession::repair_loss());
	// the periodic one only bounds damage a repair somehow missed, and on a
	// slow link each is a burst many P-frames in size. 0 is an infinite
	// GOP: no periodic IDRs, only the first and requested ones.
	uint32_t gop_size = 600;
	int drm_fd = -1; // render node used to init the encoder's device
};

struct EncodedPacket {
	// The codec's elementary stream for one frame: Annex-B (start-code
	// prefixed) for h264/h265, a temporal unit of OBUs for av1.
	std::vector<uint8_t> data;
	int64_t pts_us = 0;
	bool keyframe = false;
};

class Encoder {
public:
	virtual ~Encoder() = default;

	virtual bool open(const EncoderConfig &config) = 0;

	// `frame` must stay valid (fds open) only for the duration of the call;
	// the encoder does not take ownership of the fds. Backends with no
	// zero-copy import path (wants_cpu_frame() == true) don't implement
	// this -- the caller (SessionServices::encode_frame(), or the
	// screencast host's CPU-frame path) routes to push_cpu() instead and
	// never calls this.
	virtual bool push(const DmabufFrame &frame, int64_t pts_us) = 0;

	// True for backends that need pixels in host memory (via push_cpu())
	// rather than a GPU dmabuf (via push()) -- i.e. software encoders with
	// no device to import into. Decided by open(); the one exception is
	// NVENC, which turns it true for good if a dmabuf import fails at
	// runtime (nvenc/nvenc_encoder.hpp), so a caller that asks per frame
	// -- SessionServices::encode_frame() does -- falls back to read-back
	// rather than dropping every frame. It never turns back false.
	virtual bool wants_cpu_frame() const { return false; }

	// CPU-memory counterpart to push(), for wants_cpu_frame() backends only.
	// `data` is DRM_FORMAT_XRGB8888, row-major, `stride` bytes/row, valid
	// only for the duration of the call. `damage` is what changed since
	// the previous push (see DamageRegion; null = unknown). Backends that
	// don't need it inherit this no-op default rather than implementing it.
	virtual bool push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride,
		int64_t pts_us, const DamageRegion *damage) {
		(void)data;
		(void)width;
		(void)height;
		(void)stride;
		(void)pts_us;
		(void)damage;
		return false;
	}

	// Lossless refinement over a hardware base encoder: true if the next
	// frame may come in as a dmabuf plus a TileSource that reads that same
	// dmabuf on the GPU (push_tiled()), rather than as a read-back through
	// push_cpu(). Asked per frame, like wants_cpu_frame(); a caller with no
	// GPU tile source (a driver without compute) just keeps using
	// push_cpu(). False for every plain codec.
	virtual bool takes_tiled_dmabuf() const { return false; }

	// For takes_tiled_dmabuf(): `frame` goes to the base encoder as push()
	// would hand it, and `tiles` is what refinement hashes and copies its
	// tiles from. Both are only used during the call. False if the frame
	// wasn't taken -- tiles.failed() then says whether that was the source,
	// in which case the caller re-sends the frame through push_cpu().
	virtual bool push_tiled(const DmabufFrame &frame, TileSource &tiles, int64_t pts_us,
		const DamageRegion *damage) {
		(void)frame;
		(void)tiles;
		(void)pts_us;
		(void)damage;
		return false;
	}

	// Which DRM format modifiers (drm_fourcc.h's DRM_FORMAT_MOD_*) this
	// encoder can import `drm_format` dmabufs with -- for a capture source
	// that itself gets to choose a modifier (PipeWire's
	// dmabuf-with-modifiers negotiation, or ext capture's own gbm
	// allocation). Empty for wants_cpu_frame() backends (no import at all).
	virtual std::vector<uint64_t> supported_import_modifiers(uint32_t drm_format) const {
		(void)drm_format;
		return {};
	}

	virtual void request_keyframe() = 0;
	// Loss repair, in two halves (GdpSession's StatsReport handling):
	// frames_lost() names the packets, by pts_us, that the client reported
	// lost, as soon as the report arrives; request_repair_keyframe() asks
	// for the IDR that repairs the video, later if the network profile's
	// repair interval says so. Only lossless refinement keeps state of its
	// own for the first -- it redoes what those packets' layers did, where
	// a plain request_keyframe() would wipe and repaint its whole plane --
	// so for every plain codec the first is a no-op and the second is
	// request_keyframe().
	virtual void frames_lost(const std::vector<int64_t> &pts_us) { (void)pts_us; }
	virtual void request_repair_keyframe() { request_keyframe(); }
	virtual void set_bitrate(uint32_t bitrate_bps) = 0;
	// The session's network profile, at the start and whenever an AUTO
	// session's is reclassified. Only lossless refinement has per-profile
	// settings ([refine.<profile>] in wraith.toml); every plain codec
	// ignores it.
	virtual void set_link_profile(LinkProfile profile) { (void)profile; }
	// The client's RefinePause (gdp-spec.md §7.8,
	// the client's Lossless switch): stop or resume building the lossless layer.
	// Only lossless refinement has one to stop; every plain codec ignores
	// it.
	virtual void set_refine_paused(bool paused) { (void)paused; }

	// True for backends that can still make progress on an *unchanged*
	// frame -- i.e. that keep some notion of "how long has this looked the
	// same" across pushes, rather than treating every push as fresh work.
	// Lossless refinement is the one example: its tile layer only sends a
	// region once it has been unchanged for a settle time
	// (encode/refine/tile_tracker.hpp), which needs more calls after the
	// last change -- and a caller that only pushes when the source content
	// actually changed (SessionServices::encode_frame(), driven by real
	// compositor damage) never supplies them once the desktop stops
	// changing. false (the default) means the caller loses nothing by not
	// pushing when nothing changed, which is true of every plain codec.
	//
	// Decides whether the caller keeps a copy of the last frame for the
	// idle pump at all (SessionServices::service_idle_pump()). Constant
	// over the life of one encoder instance.
	virtual bool wants_idle_pump() const { return false; }

	// For a wants_idle_pump() backend only: true if a pump() with the
	// *same* pixels it was last given would still change what poll()
	// eventually returns (refinement: some tile hasn't reached
	// its settle time, or has but is waiting on a byte budget). The caller
	// uses this to decide whether to keep pumping an idle frame or stop
	// until something organic happens again -- so an idle desktop that has
	// fully settled costs literally nothing, same as a plain h264 session.
	virtual bool has_pending_work() const { return false; }

	// For a wants_idle_pump() backend only: advance whatever idle-time
	// state it keeps as though the frame `tiles` reads -- which must be the
	// very frame of the last push_cpu() or push_tiled(), unchanged -- had
	// been pushed again, *without* encoding a frame. Anything it decides to
	// send comes out of the next poll() (refinement: a layer-only packet,
	// gdp/refine.hpp). Returns true if it did something; false from a
	// backend with nothing to pump.
	virtual bool pump(TileSource &tiles, int64_t pts_us) {
		(void)tiles;
		(void)pts_us;
		return false;
	}

	// Asks a backend that can encode off the caller's thread to do so
	// (before open()). x264, VA-API and PyroWave can; lossless refinement
	// passes it on to its base and holds its layer-only packets until no
	// base frame is still being encoded, so they stay in order.
	virtual void set_asynchronous(bool on) { (void)on; }
	// For an asynchronous backend: an fd that turns readable when poll()
	// has packets to give, for the caller's event loop to watch. -1 (every
	// synchronous backend): call poll() right after each push.
	virtual int completion_fd() const { return -1; }
	// For an asynchronous backend: whether a push now would start encoding
	// at once. A backend slower than the screen (VA-API on an older GPU)
	// says no while a frame is still encoding, and the caller skips frames
	// -- keeping their damage -- and re-delivers the newest once it is
	// done, rather than queueing them up on the GPU, where each would add
	// a whole encode to the latency of the ones behind it.
	virtual bool ready_for_frame() const { return true; }
	// Drains whatever packets have finished encoding since the last call.
	// May block briefly on the oldest still-in-flight frame; never blocks
	// with nothing in flight. An asynchronous backend never blocks: what
	// isn't done yet comes out of a later call, after completion_fd().
	virtual std::vector<EncodedPacket> poll() = 0;

	virtual void close() = 0;
};

} // namespace wraith
