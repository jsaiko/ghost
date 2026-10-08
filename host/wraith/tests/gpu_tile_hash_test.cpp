// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// DmabufReader's tile shaders (screencast/dmabuf_reader.cpp) against the
// CPU path they stand in for (encode/refine/tile_source.cpp): on random
// content written into a real dmabuf, every tile hash must be bit for bit
// hash_tile()'s, and every gathered rect byte for byte what CpuTileSource
// packs -- at sizes whose edge tiles are clipped and odd-widthed, and at
// other tile sizes than refinement's 16. A refined session's tracker
// compares hashes across frames, so a GPU hash that drifted from the CPU's
// would turn every tile "changed" whenever a session moved between them.
//
// Needs a render node with GLES 3.1 (WRAITH_TEST_RENDER_NODE, default
// /dev/dri/renderD128); without one it skips (exit 77).
#include "encode/refine/tile_source.hpp"
#include "screencast/dmabuf_reader.hpp"
#include "screencast/dmabuf_tile_source.hpp"

#include <gbm.h>
#include <libdrm/drm_fourcc.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(expr)                                                                                          \
	do {                                                                                                     \
		if (!(expr)) {                                                                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr);                         \
			g_failures++;                                                                                    \
		}                                                                                                    \
	} while (0)

constexpr int kSkip = 77;

struct Bo {
	gbm_bo *bo = nullptr;
	wraith::DmabufFrame frame;
	~Bo() {
		if (frame.fd[0] > 0) {
			close(frame.fd[0]);
		}
		if (bo) {
			gbm_bo_destroy(bo);
		}
	}
};

// A linear XRGB8888 dmabuf filled with `pixels` (tightly packed). The X
// byte is 0xff, which is what the GPU samples and read() returns for it.
bool make_frame(gbm_device *gbm, uint32_t w, uint32_t h, const std::vector<uint8_t> &pixels, Bo *out) {
	out->bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
	if (!out->bo) {
		return false;
	}
	uint32_t stride = 0;
	void *map_data = nullptr;
	auto *map =
		static_cast<uint8_t *>(gbm_bo_map(out->bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &stride, &map_data));
	if (!map) {
		return false;
	}
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w * 4; x++) {
			map[(size_t)y * stride + x] = pixels[(size_t)y * w * 4 + x];
		}
	}
	gbm_bo_unmap(out->bo, map_data);
	wraith::DmabufFrame &f = out->frame;
	f.width = (int32_t)w;
	f.height = (int32_t)h;
	f.format = DRM_FORMAT_XRGB8888;
	f.modifier = gbm_bo_get_modifier(out->bo);
	f.n_planes = 1;
	f.fd[0] = gbm_bo_get_fd(out->bo);
	f.offset[0] = gbm_bo_get_offset(out->bo, 0);
	f.stride[0] = gbm_bo_get_stride(out->bo);
	return f.fd[0] >= 0;
}

std::vector<uint8_t> random_pixels(uint32_t w, uint32_t h, uint32_t seed) {
	std::vector<uint8_t> pixels((size_t)w * h * 4);
	for (size_t i = 0; i < pixels.size(); i++) {
		seed = seed * 1664525u + 1013904223u;
		pixels[i] = (i % 4 == 3) ? 0xff : (uint8_t)(seed >> 24);
	}
	return pixels;
}

void check_size(wraith::DmabufReader &reader, gbm_device *gbm, uint32_t w, uint32_t h, uint32_t tile_size) {
	std::vector<uint8_t> pixels = random_pixels(w, h, w * 7919 + h);
	Bo bo;
	if (!make_frame(gbm, w, h, pixels, &bo)) {
		fprintf(stderr, "%ux%u: can't make a linear dmabuf\n", w, h);
		g_failures++;
		return;
	}
	wraith::CpuTileSource cpu(pixels.data(), w, h, w * 4);
	wraith::DmabufTileSource gpu(reader);
	gpu.bind(bo.frame, &bo);

	const uint32_t tiles = ((w + tile_size - 1) / tile_size) * ((h + tile_size - 1) / tile_size);
	std::vector<uint64_t> want(tiles), got(tiles);
	CHECK(cpu.hash_tiles(tile_size, want.data()));
	CHECK(gpu.hash_tiles(tile_size, got.data()));
	uint32_t differ = 0;
	for (uint32_t i = 0; i < tiles; i++) {
		differ += want[i] != got[i];
	}
	if (differ) {
		fprintf(stderr, "%ux%u, %u px tiles: %u of %u hashes differ from the CPU's\n", w, h, tile_size,
			differ, tiles);
	}
	CHECK(differ == 0);

	// Rects as the tracker makes them (tile-aligned, the edge ones
	// clipped) and some that aren't, in one call.
	std::vector<gdp::RefineRect> rects;
	auto rect = [&](uint32_t x, uint32_t y, uint32_t rw, uint32_t rh) {
		rects.push_back(gdp::RefineRect{(uint16_t)x, (uint16_t)y, (uint16_t)rw, (uint16_t)rh});
	};
	rect(0, 0, std::min(w, 48u), std::min(h, 16u));
	rect(w - (w % 16 ? w % 16 : 16), h - (h % 16 ? h % 16 : 16), w % 16 ? w % 16 : 16, h % 16 ? h % 16 : 16);
	rect(w / 3, h / 5, std::min(w - w / 3, 37u), std::min(h - h / 5, 21u));
	rect(1, 1, 1, 1);
	std::vector<uint8_t> want_px, got_px;
	CHECK(cpu.append_rects(rects, &want_px));
	CHECK(gpu.append_rects(rects, &got_px));
	CHECK(!gpu.failed());
	if (want_px != got_px) {
		fprintf(stderr, "%ux%u: gathered rects differ from the CPU's\n", w, h);
	}
	CHECK(want_px == got_px);
	reader.forget(&bo);
}

} // namespace

int main() {
	const char *node = getenv("WRAITH_TEST_RENDER_NODE");
	if (!node) {
		node = "/dev/dri/renderD128";
	}
	int fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("gpu_tile_hash_test: no %s, skipped\n", node);
		return kSkip;
	}
	gbm_device *gbm = gbm_create_device(fd);
	int result = kSkip;
	{
		wraith::DmabufReader reader;
		if (!gbm || !reader.open(fd) || !reader.can_hash_tiles()) {
			printf("gpu_tile_hash_test: no GLES 3.1 tile shaders on %s, skipped\n", node);
		} else {
			check_size(reader, gbm, 3840, 2160, 16);
			check_size(reader, gbm, 1366, 768, 16); // clipped edge tiles
			check_size(reader, gbm, 1365, 767, 16); // ... with an odd width
			check_size(reader, gbm, 100, 37, 16);
			check_size(reader, gbm, 64, 64, 8);
			check_size(reader, gbm, 333, 211, 64);
			result = g_failures ? 1 : 0;
		}
	}
	if (gbm) {
		gbm_device_destroy(gbm);
	}
	close(fd);
	if (result == 1) {
		fprintf(stderr, "gpu_tile_hash_test: %d check(s) failed\n", g_failures);
	} else if (result == 0) {
		printf("gpu_tile_hash_test: ok\n");
	}
	return result;
}
