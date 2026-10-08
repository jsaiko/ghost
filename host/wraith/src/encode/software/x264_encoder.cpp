// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

#include "encode/software/x264_encoder.hpp"

#include "util/clock.hpp"
#include "util/log.hpp"
#include "encode/software/xrgb_convert.hpp"

#include <algorithm>
#include <cstring>

#include <sys/eventfd.h>
#include <unistd.h>

namespace wraith {

namespace {

// Named once so the log line below cannot drift from what was actually
// asked for. See the ladder in open().
constexpr const char *kPreset = "superfast";

} // namespace

X264Encoder::~X264Encoder() {
	close();
}

bool X264Encoder::open(const EncoderConfig &config) {
	config_ = config;

	// The preset ladder, measured on desktop content at 4K:
	//
	//   ultrafast  cheapest, but sets i_partitions to 0, disabling intra
	//              4x4/8x8 analysis -- every glyph is predicted as part of a
	//              16x16 block and text comes out visibly blurry
	//   superfast  restores i4x4/i8x8, the 8x8 transform and adaptive
	//              quantisation; the cheapest preset that keeps text legible
	//   faster+    trellis and deeper subpel search: no visible improvement on
	//              desktop content for several times the CPU
	//
	// superfast is the floor for a session without lossless refinement,
	// which has nothing but the base layer to render text with. A refined
	// session could afford ultrafast -- its lossless overlay makes settled
	// text bit-exact whatever the base layer did, leaving blur only on
	// content still in motion -- but this encoder can't tell the two apart:
	// RefineEncoder wraps it from outside, and EncoderConfig doesn't say
	// whether refinement is on.
	if (x264_param_default_preset(&params_, kPreset, "zerolatency") != 0) {
		WLOG_ERROR("x264: x264_param_default_preset failed");
		return false;
	}

	params_.i_width = (int)config.width;
	params_.i_height = (int)config.height;
	// 4:2:0, not 4:4:4. 4:4:4 would keep colored text crisp (4:2:0 throws
	// away 3/4 of the chroma samples), but it forces High 4:4:4 Predictive,
	// which no VA-API decoder implements -- there is no 4:4:4 H.264 decode
	// profile in VA-API at all, so spectre's hardware decode path cannot
	// display such a stream. Revisit only alongside a client that negotiates
	// software decode for it.
	params_.i_csp = X264_CSP_I420;
	// xrgb_to_i420() is BT.601 studio range; say so in the VUI, as the
	// VA-API and NVENC encoders do, or decoders that guess (Firefox: BT.709
	// for HD) shift every colour.
	params_.vui.b_fullrange = 0;
	params_.vui.i_colorprim = 6; // SMPTE 170M
	params_.vui.i_transfer = 6;
	params_.vui.i_colmatrix = 6;
	params_.i_fps_num = config.framerate_num;
	params_.i_fps_den = config.framerate_den;
	params_.b_vfr_input = 0;
	params_.i_timebase_num = 1;
	params_.i_timebase_den = 1'000'000; // pts_us is already in microseconds

	params_.i_keyint_max = config.gop_size != 0 ? (int)config.gop_size : X264_KEYINT_MAX_INFINITE;

	// Rate control is left at the preset's own defaults (CRF 23); only the
	// VBV ceiling is ours, because that is the knob GdpSession's congestion
	// controller drives through set_bitrate() (gdp_session.cpp).
	apply_vbv(config.bitrate_bps);

	// SPS/PPS before every keyframe, so any keyframe is a valid entry
	// point -- the same one-Annex-B-packet-per-frame contract as
	// VaapiH264Encoder.
	params_.b_repeat_headers = 1;
	params_.b_annexb = 1;

	// Sliced threading, never frame threading. Frame threading pipelines
	// whole frames, so x264_encoder_encode() returns nothing for the first
	// few calls and then some earlier frame -- and push_cpu() stamps each
	// packet with its own call's pts_us. Sliced threading encodes one
	// frame's slices in parallel and returns that frame from that call.
	// Thread count is left to x264 (0 = one per core): at 4K that takes a
	// frame from ~19ms to ~6.5ms, for a few percent of bitstream size in
	// per-slice overhead.
	params_.b_sliced_threads = 1;
	params_.i_threads = 0;
	params_.i_sync_lookahead = 0;
	params_.i_bframe = 0;
	// Pinned rather than left to the tune: zerolatency sets these too, but
	// push_cpu() depends on them for correctness, not just latency. Any
	// frame-level buffering (lookahead, B-frames) breaks its assumption that
	// what x264_encoder_encode() returns belongs to the frame just handed
	// in, and packets would carry the wrong pts_us. Keeping them explicit
	// means changing the preset or tune cannot quietly reintroduce that.
	params_.rc.i_lookahead = 0;
	params_.rc.b_mb_tree = 0;

	// No x264_param_apply_profile(): the preset's own choices stand, so the
	// output profile is a consequence of the preset rather than something this
	// backend asks for. superfast gives High (8x8dct, CABAC); ultrafast would
	// give Baseline, having turned both off. Every hardware H.264 decoder
	// takes both, so spectre's hardware path handles either.

	encoder_ = x264_encoder_open(&params_);
	if (!encoder_) {
		WLOG_ERROR("x264: x264_encoder_open failed for %ux%u", config.width, config.height);
		return false;
	}

	// I420: full-res Y plane + quarter-res U/V planes, each byte-packed
	// (stride == width for Y, width/2 for U/V) -- the layout push_cpu()'s
	// conversion fills every call.
	size_t luma = (size_t)config.width * config.height;
	auto init_picture = [&](x264_picture_t *pic, std::vector<uint8_t> *i420) {
		i420->resize(luma * 3 / 2);
		x264_picture_init(pic);
		pic->img.i_csp = X264_CSP_I420;
		pic->img.i_plane = 3;
		pic->img.plane[0] = i420->data();
		pic->img.plane[1] = pic->img.plane[0] + luma;
		pic->img.plane[2] = pic->img.plane[1] + luma / 4;
		pic->img.i_stride[0] = (int)config.width;
		pic->img.i_stride[1] = (int)config.width / 2;
		pic->img.i_stride[2] = (int)config.width / 2;
	};

	if (asynchronous_) {
		wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (wake_fd_ < 0) {
			WLOG_ERROR("x264: eventfd failed; encoding on the caller's thread");
			asynchronous_ = false;
		}
	}
	if (asynchronous_) {
		for (Slot &slot : slots_) {
			init_picture(&slot.pic, &slot.i420);
		}
		stop_ = false;
		worker_ = std::thread([this] { worker_loop(); });
	} else {
		init_picture(&pic_, &i420_);
	}

	force_idr_next_ = true;
	WLOG_INFO(
		"x264: opened %ux%u software H.264 (4:2:0, preset %s/zerolatency, %s colour conversion, sliced threads)",
		config.width, config.height, kPreset, xrgb_to_i420_impl_name());
	return true;
}

// Unused: this backend is CPU-only (wants_cpu_frame() == true), so
// SessionServices never calls this -- push_cpu() below is the real entry
// point.
bool X264Encoder::push(const DmabufFrame &, int64_t) {
	return false;
}

void X264Encoder::note_timing(int64_t convert_us, int64_t encode_us) {
	constexpr int64_t kTimingWindowUs = 5'000'000;
	int64_t now = monotonic_now_us();
	if (timing_.window_start_us == 0) {
		timing_.window_start_us = now;
	}
	timing_.frames++;
	timing_.convert_sum_us += convert_us;
	timing_.convert_max_us = std::max(timing_.convert_max_us, convert_us);
	timing_.encode_sum_us += encode_us;
	timing_.encode_max_us = std::max(timing_.encode_max_us, encode_us);
	int64_t elapsed = now - timing_.window_start_us;
	if (elapsed < kTimingWindowUs) {
		return;
	}
	double n = (double)timing_.frames;
	WLOG_INFO(
		"x264: last %.1fs: %u frames (%.1f fps), convert %.1f ms avg / %.1f max, encode %.1f ms avg / %.1f max",
		elapsed / 1e6, timing_.frames, n * 1e6 / (double)elapsed, timing_.convert_sum_us / n / 1000.0,
		timing_.convert_max_us / 1000.0, timing_.encode_sum_us / n / 1000.0, timing_.encode_max_us / 1000.0);
	timing_ = Timing{};
	timing_.window_start_us = now;
}

bool X264Encoder::push_cpu(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride,
	int64_t pts_us, const DamageRegion *) {
	if (!encoder_ || width != config_.width || height != config_.height) {
		return false;
	}

	if (asynchronous_) {
		// The slot the worker isn't encoding: it only ever holds the one
		// submitted last, and slots alternate.
		Slot &slot = slots_[next_slot_];
		int64_t t0 = monotonic_now_us();
		xrgb_to_i420(data, width, height, stride, slot.pic.img.plane[0], slot.pic.img.plane[1],
			slot.pic.img.plane[2]);
		int64_t convert_us = monotonic_now_us() - t0;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return job_ == nullptr || stop_; });
			if (stop_) {
				return false;
			}
			slot.pic.i_type = force_idr_next_ ? X264_TYPE_IDR : X264_TYPE_AUTO;
			slot.pic.i_pts = pts_us;
			job_ = &slot;
			job_convert_us_ = convert_us;
		}
		force_idr_next_ = false;
		next_slot_ ^= 1;
		cv_.notify_all();
		return true;
	}

	int64_t t0 = monotonic_now_us();
	xrgb_to_i420(data, width, height, stride, pic_.img.plane[0], pic_.img.plane[1], pic_.img.plane[2]);
	int64_t t1 = monotonic_now_us();

	pic_.i_type = force_idr_next_ ? X264_TYPE_IDR : X264_TYPE_AUTO;
	force_idr_next_ = false;
	pic_.i_pts = pts_us;
	bool ok = encode_picture(&pic_, pts_us, &ready_);
	note_timing(t1 - t0, monotonic_now_us() - t1);
	return ok;
}

bool X264Encoder::encode_picture(x264_picture_t *pic, int64_t pts_us, std::vector<EncodedPacket> *out) {
	x264_nal_t *nals = nullptr;
	int nal_count = 0;
	x264_picture_t pic_out;
	int frame_size = x264_encoder_encode(encoder_, &nals, &nal_count, pic, &pic_out);
	if (frame_size < 0) {
		WLOG_ERROR("x264: x264_encoder_encode failed");
		return false;
	}
	if (frame_size == 0) {
		return true; // frame buffered internally; nothing to drain yet
	}

	// Sliced threading with no B-frames/lookahead (see open()) means
	// x264_encoder_encode() is synchronous here, so every NAL it reports
	// belongs to this call's frame -- concatenate them into one Annex-B
	// packet, same framing VaapiEncoderBase hands to gdp_session.cpp. With
	// several slices per frame there are simply more NALs to concatenate.
	EncodedPacket packet;
	packet.pts_us = pts_us;
	packet.keyframe = IS_X264_TYPE_I(pic_out.i_type);
	packet.data.resize((size_t)frame_size);
	size_t offset = 0;
	for (int nal = 0; nal < nal_count; nal++) {
		std::memcpy(packet.data.data() + offset, nals[nal].p_payload, (size_t)nals[nal].i_payload);
		offset += (size_t)nals[nal].i_payload;
	}
	out->push_back(std::move(packet));
	return true;
}

void X264Encoder::worker_loop() {
	for (;;) {
		Slot *slot;
		int64_t convert_us;
		uint32_t bitrate;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [this] { return job_ != nullptr || stop_; });
			if (stop_) {
				return;
			}
			slot = job_;
			convert_us = job_convert_us_;
			bitrate = pending_bitrate_bps_;
			pending_bitrate_bps_ = 0;
		}
		if (bitrate != 0) {
			apply_vbv(bitrate);
			x264_encoder_reconfig(encoder_, &params_);
		}
		int64_t t0 = monotonic_now_us();
		std::vector<EncodedPacket> out;
		encode_picture(&slot->pic, slot->pic.i_pts, &out);
		note_timing(convert_us, monotonic_now_us() - t0);
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (EncodedPacket &packet : out) {
				ready_.push_back(std::move(packet));
			}
			job_ = nullptr;
		}
		cv_.notify_all();
		if (!out.empty()) {
			uint64_t one = 1;
			(void)!write(wake_fd_, &one, sizeof(one));
		}
	}
}

void X264Encoder::request_keyframe() {
	force_idr_next_ = true;
}

void X264Encoder::set_bitrate(uint32_t bitrate_bps) {
	if (!encoder_) {
		return;
	}
	// CRF has no target bitrate to move -- this adjusts the VBV ceiling
	// instead, so GdpSession's congestion controller still caps output
	// under network pressure while CRF 23 governs quality on a static or
	// slow-moving picture.
	if (asynchronous_) {
		// Never alongside an encode: the worker applies it between frames.
		std::lock_guard<std::mutex> lock(mutex_);
		pending_bitrate_bps_ = bitrate_bps;
		return;
	}
	apply_vbv(bitrate_bps);
	x264_encoder_reconfig(encoder_, &params_);
}

void X264Encoder::apply_vbv(uint32_t bitrate_bps) {
	params_.rc.i_vbv_max_bitrate = (int)(bitrate_bps / 1000); // x264 takes kbps
	params_.rc.i_vbv_buffer_size = (int)(bitrate_bps / 1000);
}

std::vector<EncodedPacket> X264Encoder::poll() {
	std::vector<EncodedPacket> out;
	if (!asynchronous_) {
		out.swap(ready_);
		return out;
	}
	uint64_t count;
	(void)!read(wake_fd_, &count, sizeof(count));
	std::lock_guard<std::mutex> lock(mutex_);
	out.swap(ready_);
	return out;
}

void X264Encoder::close() {
	if (worker_.joinable()) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			stop_ = true;
		}
		cv_.notify_all();
		worker_.join();
		job_ = nullptr;
	}
	if (wake_fd_ >= 0) {
		::close(wake_fd_);
		wake_fd_ = -1;
	}
	if (encoder_) {
		x264_encoder_close(encoder_);
		encoder_ = nullptr;
	}
}

} // namespace wraith
