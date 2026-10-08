// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// RefineEncoder's pause (gdp-spec.md §7.8 RefinePause, the client's
// Lossless switch): the packets it produces around a pause and a resume, against a
// fake base encoder that records which input path each frame took. What
// matters on the wire is that a pause wipes the client's plane exactly
// once and then carries empty layers, that a resume starts over from a
// reset, and that the base's own stream never notices -- no IDR asked for
// either way.
//
// The tiled path (push_tiled(): the dmabuf to the base, a GPU TileSource to
// the tracker) against a stand-in source that can be told to fail: it is
// only taken over a dmabuf-importing base while unpaused, builds the same
// layers the CPU path does, and a source failure leaves nothing behind --
// no frame pushed, an owed reset still owed, chosen tiles re-armed.
//
// And loss repair: a frame the client reports lost is looked up in the
// sent history and repaired without a plane reset; only a lost reset, or
// a frame the history has forgotten, costs one. (What a repair does to
// tiles and clears is the tracker's, in tile_tracker_test.)
#include "encode/refine/refine_encoder.hpp"
#include "encode/refine/tile_source.hpp"

#include "gdp/refine.hpp"

#include <cstdio>
#include <memory>
#include <vector>

#include <unistd.h>

namespace {

// Not <cassert>: the tree's default build type is RelWithDebInfo, which
// defines NDEBUG and would compile every check -- calls included -- away.
int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr uint32_t kStride = kWidth * 4;

// A base encoder that "encodes" each frame as one byte saying which path it
// came in by ('C' push_cpu, 'D' push), synchronously, like x264.
class FakeBase : public wraith::Encoder {
public:
	bool gpu_capable = true; // wants_cpu_frame() == false, as VA-API's is
	bool fail_next = false;
	// Like x264's worker: packets are held until finish() and reported
	// through completion_fd() rather than handed to the next poll().
	bool async = false;
	int keyframe_requests = 0;
	std::vector<char> paths;

	bool open(const wraith::EncoderConfig &) override { return true; }
	bool wants_cpu_frame() const override { return !gpu_capable; }
	bool push(const wraith::DmabufFrame &, int64_t pts_us) override { return take('D', pts_us); }
	bool push_cpu(const uint8_t *, uint32_t, uint32_t, uint32_t, int64_t pts_us,
		const wraith::DamageRegion *) override {
		return take('C', pts_us);
	}
	void request_keyframe() override { keyframe_requests++; }
	void set_bitrate(uint32_t) override {}
	std::vector<wraith::EncodedPacket> poll() override { return std::move(out_); }
	void close() override {}
	int completion_fd() const override { return async ? 99 : -1; }
	void finish() {
		for (auto &packet : held_) {
			out_.push_back(std::move(packet));
		}
		held_.clear();
	}
	// Just the oldest frame in flight.
	void finish_one() {
		if (!held_.empty()) {
			out_.push_back(std::move(held_.front()));
			held_.erase(held_.begin());
		}
	}

private:
	bool take(char path, int64_t pts_us) {
		if (fail_next) {
			fail_next = false;
			return false;
		}
		paths.push_back(path);
		wraith::EncodedPacket packet;
		packet.data = {(uint8_t)path};
		packet.pts_us = pts_us;
		(async ? held_ : out_).push_back(packet);
		return true;
	}
	std::vector<wraith::EncodedPacket> out_;
	std::vector<wraith::EncodedPacket> held_;
};

struct Decoded {
	char base = 0; // the fake's path byte, 0 for a layer-only packet
	gdp::RefineLayer layer;
};

std::vector<Decoded> drain(wraith::RefineEncoder &encoder) {
	std::vector<Decoded> out;
	for (const wraith::EncodedPacket &packet : encoder.poll()) {
		Decoded d;
		const uint8_t *base = nullptr;
		size_t base_len = 0;
		CHECK(gdp::refine_parse_frame(packet.data.data(), packet.data.size(), &base, &base_len, &d.layer));
		d.base = base_len == 1 ? (char)base[0] : 0;
		out.push_back(std::move(d));
	}
	return out;
}

int64_t g_pts = 0;
std::vector<uint8_t> g_pixels((size_t)kStride *kHeight, 0x20);

std::vector<Decoded> cpu_frame(wraith::RefineEncoder &encoder, bool expect_ok = true) {
	CHECK(encoder.push_cpu(g_pixels.data(), kWidth, kHeight, kStride, g_pts += 16667, nullptr) == expect_ok);
	return drain(encoder);
}

std::vector<Decoded> gpu_frame(wraith::RefineEncoder &encoder, bool expect_ok = true) {
	wraith::DmabufFrame frame;
	frame.width = kWidth;
	frame.height = kHeight;
	CHECK(encoder.push(frame, g_pts += 16667) == expect_ok);
	return drain(encoder);
}

wraith::RefineEncoder *open_encoder(std::unique_ptr<wraith::RefineEncoder> &holder, FakeBase **base_out);

// Host pixels behind the TileSource interface, failing on request: the
// GPU source's stand-in.
class FlakySource : public wraith::TileSource {
public:
	explicit FlakySource(const uint8_t *data) : cpu_(data, kWidth, kHeight, kStride) {}
	bool fail_hash = false;
	bool fail_rects = false;

	uint32_t width() const override { return kWidth; }
	uint32_t height() const override { return kHeight; }
	bool hash_tiles(uint32_t tile_size, uint64_t *out) override {
		if (fail_hash) {
			failed_ = true;
			return false;
		}
		return cpu_.hash_tiles(tile_size, out);
	}
	bool append_rects(const std::vector<gdp::RefineRect> &rects, std::vector<uint8_t> *out) override {
		if (fail_rects) {
			failed_ = true;
			return false;
		}
		return cpu_.append_rects(rects, out);
	}
	void rebind() { failed_ = false; } // what DmabufTileSource::bind() does

private:
	wraith::CpuTileSource cpu_;
};

std::vector<Decoded> tiled_frame(wraith::RefineEncoder &encoder, FlakySource &tiles, bool expect_ok = true) {
	wraith::DmabufFrame frame;
	frame.width = kWidth;
	frame.height = kHeight;
	tiles.rebind();
	CHECK(encoder.push_tiled(frame, tiles, g_pts += 16667, nullptr) == expect_ok);
	return drain(encoder);
}

// Only a base that imports dmabufs takes the tiled path, and only unpaused
// (paused, the plain zero-copy push() is the way in).
void test_tiled_path_routing() {
	auto base_owner = std::make_unique<FakeBase>();
	FakeBase *base = base_owner.get();
	wraith::RefineEncoder encoder(std::move(base_owner));
	wraith::EncoderConfig config;
	config.width = kWidth;
	config.height = kHeight;
	CHECK(encoder.open(config));
	FlakySource tiles(g_pixels.data());

	CHECK(encoder.takes_tiled_dmabuf());
	std::vector<Decoded> packets = tiled_frame(encoder, tiles);
	CHECK(packets.size() == 1 && packets[0].base == 'D' && packets[0].layer.reset);

	encoder.set_refine_paused(true);
	CHECK(!encoder.takes_tiled_dmabuf());
	CHECK(tiled_frame(encoder, tiles, false).empty());
	encoder.set_refine_paused(false);
	CHECK(encoder.takes_tiled_dmabuf());

	base->gpu_capable = false; // x264, PyroWave: they read the frame back anyway
	CHECK(!encoder.takes_tiled_dmabuf());
	CHECK(tiled_frame(encoder, tiles, false).empty());
}

// The same frames make the same layers whichever way they come in.
void test_tiled_matches_cpu() {
	std::unique_ptr<wraith::RefineEncoder> cpu_holder, gpu_holder;
	auto make = [](std::unique_ptr<wraith::RefineEncoder> &holder) -> wraith::RefineEncoder & {
		holder = std::make_unique<wraith::RefineEncoder>(std::make_unique<FakeBase>());
		wraith::EncoderConfig config;
		config.width = kWidth;
		config.height = kHeight;
		CHECK(holder->open(config));
		return *holder;
	};
	wraith::RefineEncoder &cpu = make(cpu_holder);
	wraith::RefineEncoder &gpu = make(gpu_holder);
	std::vector<uint8_t> pixels(g_pixels.size());
	for (size_t i = 0; i < pixels.size(); i++) {
		pixels[i] = (uint8_t)(i * 7 + i / 300);
	}
	FlakySource tiles(pixels.data());

	CHECK(cpu.push_cpu(pixels.data(), kWidth, kHeight, kStride, g_pts += 16667, nullptr));
	std::vector<Decoded> a = drain(cpu);
	std::vector<Decoded> b = tiled_frame(gpu, tiles);
	CHECK(a.size() == 1 && b.size() == 1 && a[0].layer.reset && b[0].layer.reset);

	usleep(150 * 1000); // past the settle time
	wraith::CpuTileSource last(pixels.data(), kWidth, kHeight, kStride);
	CHECK(cpu.pump(last, g_pts += 16667));
	tiles.rebind();
	CHECK(gpu.pump(tiles, g_pts));
	a = drain(cpu);
	b = drain(gpu);
	CHECK(a.size() == 1 && b.size() == 1);
	if (a.size() == 1 && b.size() == 1) {
		CHECK(!a[0].layer.tile_pixels.empty());
		CHECK(a[0].layer.tile_pixels == b[0].layer.tile_pixels);
		CHECK(a[0].layer.tiles.size() == b[0].layer.tiles.size());
	}
}

// A frame whose tiles can't be hashed isn't pushed at all, and the tracker
// hasn't moved: a reset asked for is still owed, and the caller's
// read-back of the same frame carries it.
void test_tiled_hash_failure_changes_nothing() {
	std::unique_ptr<wraith::RefineEncoder> holder;
	FakeBase *base = nullptr;
	wraith::RefineEncoder &encoder = *open_encoder(holder, &base);
	FlakySource tiles(g_pixels.data());
	CHECK(tiled_frame(encoder, tiles).size() == 1); // the session's own reset
	encoder.request_keyframe();                     // a new client: another
	size_t pushed = base->paths.size();
	tiles.fail_hash = true;
	CHECK(tiled_frame(encoder, tiles, false).empty());
	CHECK(tiles.failed());
	CHECK(base->paths.size() == pushed);

	std::vector<Decoded> packets = cpu_frame(encoder);
	CHECK(packets.size() == 1 && packets[0].base == 'C' && packets[0].layer.reset);
}

// Tiles chosen but not read (the gather failed) are given back: the next
// pump sends them.
void test_tiled_rect_failure_rearms_tiles() {
	std::unique_ptr<wraith::RefineEncoder> holder;
	FakeBase *base = nullptr;
	wraith::RefineEncoder &encoder = *open_encoder(holder, &base);
	FlakySource tiles(g_pixels.data());
	CHECK(tiled_frame(encoder, tiles).size() == 1);

	usleep(150 * 1000); // past the settle time
	tiles.fail_rects = true;
	tiles.rebind();
	CHECK(!encoder.pump(tiles, g_pts += 16667));
	CHECK(drain(encoder).empty());

	tiles.fail_rects = false;
	tiles.rebind();
	CHECK(encoder.pump(tiles, g_pts += 16667));
	std::vector<Decoded> packets = drain(encoder);
	CHECK(packets.size() == 1 && packets[0].base == 0);
	if (packets.size() == 1) {
		CHECK(packets[0].layer.tile_pixels.size() == (size_t)kWidth * kHeight * gdp::kRefineBytesPerPixel);
	}
}

void test_pause_and_resume() {
	auto base_owner = std::make_unique<FakeBase>();
	FakeBase *base = base_owner.get();
	wraith::RefineEncoder encoder(std::move(base_owner));
	wraith::EncoderConfig config;
	config.width = kWidth;
	config.height = kHeight;
	CHECK(encoder.open(config));
	CHECK(encoder.wants_cpu_frame());
	CHECK(!gpu_frame(encoder, false).size()); // unpaused: no dmabuf path

	// Session start: the first layer is a reset.
	std::vector<Decoded> packets = cpu_frame(encoder);
	CHECK(packets.size() == 1 && packets[0].layer.reset && packets[0].base == 'C');
	int keyframes_before = base->keyframe_requests;

	// Pause: the base's own preference (GPU) from now on, the next packet
	// wipes the plane, the ones after carry nothing, nothing is pending.
	encoder.set_refine_paused(true);
	CHECK(!encoder.wants_cpu_frame());
	CHECK(!encoder.has_pending_work());
	packets = gpu_frame(encoder);
	CHECK(packets.size() == 1 && packets[0].base == 'D');
	CHECK(packets[0].layer.reset && packets[0].layer.tiles.empty());
	for (int i = 0; i < 3; i++) {
		packets = gpu_frame(encoder);
		CHECK(packets.size() == 1 && packets[0].base == 'D');
		CHECK(!packets[0].layer.reset && packets[0].layer.tiles.empty() && packets[0].layer.clears.empty());
	}
	wraith::CpuTileSource last(g_pixels.data(), kWidth, kHeight, kStride);
	CHECK(encoder.pump(last, g_pts += 16667));
	CHECK(drain(encoder).empty());

	// Resume: host pixels again, and the tracker starts over from a reset.
	encoder.set_refine_paused(false);
	CHECK(encoder.wants_cpu_frame());
	packets = cpu_frame(encoder);
	CHECK(packets.size() == 1 && packets[0].base == 'C' && packets[0].layer.reset);
	CHECK(base->keyframe_requests == keyframes_before); // no IDR either way

	// A base that wants host pixels anyway (x264, or a screencast host's
	// CPU frames) keeps getting them while paused.
	base->gpu_capable = false;
	encoder.set_refine_paused(true);
	CHECK(encoder.wants_cpu_frame());
	packets = cpu_frame(encoder);
	CHECK(packets.size() == 1 && packets[0].base == 'C' && packets[0].layer.reset);
}

// A frame the base refuses doesn't take the pause's reset with it: the
// next frame that does go out carries it.
void test_lost_pause_reset_is_resent() {
	auto base_owner = std::make_unique<FakeBase>();
	FakeBase *base = base_owner.get();
	wraith::RefineEncoder encoder(std::move(base_owner));
	wraith::EncoderConfig config;
	config.width = kWidth;
	config.height = kHeight;
	CHECK(encoder.open(config));
	cpu_frame(encoder);
	int keyframes_before = base->keyframe_requests;

	encoder.set_refine_paused(true);
	base->fail_next = true;
	CHECK(gpu_frame(encoder, false).empty());
	std::vector<Decoded> packets = gpu_frame(encoder);
	CHECK(packets.size() == 1 && packets[0].layer.reset);
	CHECK(base->keyframe_requests == keyframes_before);
}

wraith::RefineEncoder *open_encoder(std::unique_ptr<wraith::RefineEncoder> &holder, FakeBase **base_out) {
	auto base_owner = std::make_unique<FakeBase>();
	*base_out = base_owner.get();
	holder = std::make_unique<wraith::RefineEncoder>(std::move(base_owner));
	wraith::EncoderConfig config;
	config.width = kWidth;
	config.height = kHeight;
	CHECK(holder->open(config));
	return holder.get();
}

// A lost frame the history knows is repaired in place: no reset on the
// next frame, and no IDR asked of the base -- the video's own repair is
// request_repair_keyframe(), which asks for the IDR and nothing else.
void test_reported_loss_does_not_reset() {
	std::unique_ptr<wraith::RefineEncoder> holder;
	FakeBase *base = nullptr;
	wraith::RefineEncoder &encoder = *open_encoder(holder, &base);
	cpu_frame(encoder); // the session's reset
	cpu_frame(encoder);
	int64_t lost_pts = g_pts;
	cpu_frame(encoder);
	int keyframes_before = base->keyframe_requests;

	encoder.frames_lost({lost_pts});
	std::vector<Decoded> packets = cpu_frame(encoder);
	CHECK(packets.size() == 1 && !packets[0].layer.reset);
	CHECK(base->keyframe_requests == keyframes_before);

	encoder.request_repair_keyframe();
	CHECK(base->keyframe_requests == keyframes_before + 1);
	packets = cpu_frame(encoder);
	CHECK(packets.size() == 1 && !packets[0].layer.reset);
}

// A pts the history doesn't hold (older than it, or never sent) can't be
// repaired piecemeal: the plane is reset, with an IDR, as before.
void test_forgotten_loss_resets() {
	std::unique_ptr<wraith::RefineEncoder> holder;
	FakeBase *base = nullptr;
	wraith::RefineEncoder &encoder = *open_encoder(holder, &base);
	cpu_frame(encoder);
	cpu_frame(encoder);
	int keyframes_before = base->keyframe_requests;

	encoder.frames_lost({g_pts - 1}); // never sent
	CHECK(base->keyframe_requests == keyframes_before + 1);
	std::vector<Decoded> packets = cpu_frame(encoder);
	CHECK(packets.size() == 1 && packets[0].layer.reset);
}

// A lost reset is sent again (the plane can't be trusted without it), but
// once a later reset has gone out, losing an earlier frame -- the old
// reset included -- needs nothing: the later reset wiped all of it.
void test_lost_reset_is_resent_once() {
	std::unique_ptr<wraith::RefineEncoder> holder;
	FakeBase *base = nullptr;
	wraith::RefineEncoder &encoder = *open_encoder(holder, &base);
	cpu_frame(encoder);
	int64_t first_reset_pts = g_pts;
	cpu_frame(encoder);
	int keyframes_before = base->keyframe_requests;

	encoder.frames_lost({first_reset_pts});
	std::vector<Decoded> packets = cpu_frame(encoder);
	CHECK(packets.size() == 1 && packets[0].layer.reset);
	CHECK(base->keyframe_requests == keyframes_before); // the plane's, not the video's

	encoder.frames_lost({first_reset_pts});
	packets = cpu_frame(encoder);
	CHECK(packets.size() == 1 && !packets[0].layer.reset);
}

} // namespace

// With an asynchronous base, a layer-only packet must not overtake a frame
// still being encoded: it waits for that frame and goes out right behind it.
void test_layer_only_waits_for_frames_in_flight() {
	auto base_owner = std::make_unique<FakeBase>();
	FakeBase *base = base_owner.get();
	base->gpu_capable = false;
	base->async = true;
	wraith::RefineEncoder encoder(std::move(base_owner));
	wraith::EncoderConfig config;
	config.width = kWidth;
	config.height = kHeight;
	CHECK(encoder.open(config));
	CHECK(encoder.completion_fd() == 99);

	std::vector<Decoded> out = cpu_frame(encoder);
	CHECK(out.empty()); // still encoding
	base->finish();
	out = drain(encoder);
	CHECK(out.size() == 1 && out[0].base == 'C');

	// A second, unchanged frame goes in and stays in flight; meanwhile the
	// tiles settle and the idle pump builds a layer-only packet.
	wraith::DamageRegion unchanged;
	CHECK(encoder.push_cpu(g_pixels.data(), kWidth, kHeight, kStride, g_pts += 16667, &unchanged));
	usleep(150 * 1000); // past the settle time
	CHECK(encoder.has_pending_work());
	wraith::CpuTileSource last(g_pixels.data(), kWidth, kHeight, kStride);
	CHECK(encoder.pump(last, g_pts += 16667));
	out = drain(encoder);
	CHECK(out.empty()); // held behind the frame in flight

	base->finish();
	out = drain(encoder);
	CHECK(out.size() == 2);
	if (out.size() == 2) {
		CHECK(out[0].base == 'C');                                    // the frame first
		CHECK(out[1].base == 0 && !out[1].layer.tile_pixels.empty()); // then the layer-only tiles
	}
}

// ...and a frame pushed after the layer-only packet was built must not
// overtake it either: that frame's layer may take those very tiles back
// (they changed), and the client applies layers in arrival order -- the
// tiles arriving late would sit over the newer picture, stale (a base
// with two frames in flight, NVENC, is where this bites).
void test_layer_only_goes_before_later_frames() {
	auto base_owner = std::make_unique<FakeBase>();
	FakeBase *base = base_owner.get();
	base->gpu_capable = false;
	base->async = true;
	wraith::RefineEncoder encoder(std::move(base_owner));
	wraith::EncoderConfig config;
	config.width = kWidth;
	config.height = kHeight;
	CHECK(encoder.open(config));

	cpu_frame(encoder);
	base->finish();
	CHECK(drain(encoder).size() == 1);

	wraith::DamageRegion unchanged;
	CHECK(encoder.push_cpu(g_pixels.data(), kWidth, kHeight, kStride, g_pts += 16667, &unchanged));
	usleep(150 * 1000); // past the settle time
	wraith::CpuTileSource last(g_pixels.data(), kWidth, kHeight, kStride);
	CHECK(encoder.pump(last, g_pts += 16667));
	// A newer frame goes in while the first is still encoding.
	CHECK(encoder.push_cpu(g_pixels.data(), kWidth, kHeight, kStride, g_pts += 16667, &unchanged));
	CHECK(drain(encoder).empty());

	base->finish();
	std::vector<Decoded> out = drain(encoder);
	CHECK(out.size() == 3);
	if (out.size() == 3) {
		CHECK(out[0].base == 'C'); // the frame pushed before the tiles were built
		CHECK(out[1].base == 0);   // the layer-only tiles
		CHECK(out[2].base == 'C'); // the frame pushed after
	}
}

// pts needn't increase from push to push: a compositor stamps its frames
// with its own clock, a re-delivered frame gets wraith's. A frame coming
// back must not count a later push with a smaller pts as done too, or a
// layer-only packet built behind that later push overtakes it.
void test_in_flight_tracks_push_order_not_pts() {
	auto base_owner = std::make_unique<FakeBase>();
	FakeBase *base = base_owner.get();
	base->gpu_capable = false;
	base->async = true;
	wraith::RefineEncoder encoder(std::move(base_owner));
	wraith::EncoderConfig config;
	config.width = kWidth;
	config.height = kHeight;
	CHECK(encoder.open(config));

	cpu_frame(encoder);
	base->finish();
	CHECK(drain(encoder).size() == 1);

	wraith::DamageRegion unchanged;
	int64_t later = g_pts += 1'000'000;
	CHECK(encoder.push_cpu(g_pixels.data(), kWidth, kHeight, kStride, later, &unchanged));
	// Re-delivered, stamped behind the compositor's clock.
	CHECK(encoder.push_cpu(g_pixels.data(), kWidth, kHeight, kStride, later - 500'000, &unchanged));
	usleep(150 * 1000); // past the settle time
	wraith::CpuTileSource last(g_pixels.data(), kWidth, kHeight, kStride);
	CHECK(encoder.pump(last, later + 16667));

	base->finish_one();
	std::vector<Decoded> out = drain(encoder);
	CHECK(out.size() == 1 && out[0].base == 'C'); // the layer-only waits for the re-delivered frame
	base->finish_one();
	out = drain(encoder);
	CHECK(out.size() == 2);
	if (out.size() == 2) {
		CHECK(out[0].base == 'C');
		CHECK(out[1].base == 0);
	}
}

int main() {
	test_layer_only_waits_for_frames_in_flight();
	test_in_flight_tracks_push_order_not_pts();
	test_layer_only_goes_before_later_frames();
	test_pause_and_resume();
	test_lost_pause_reset_is_resent();
	test_reported_loss_does_not_reset();
	test_forgotten_loss_resets();
	test_lost_reset_is_resent_once();
	test_tiled_path_routing();
	test_tiled_matches_cpu();
	test_tiled_hash_failure_changes_nothing();
	test_tiled_rect_failure_rearms_tiles();
	if (g_failures > 0) {
		fprintf(stderr, "refine_encoder_test: %d check(s) failed\n", g_failures);
		return 1;
	}
	printf("refine_encoder_test: ok\n");
	return 0;
}
