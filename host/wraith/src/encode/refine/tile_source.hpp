// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Where lossless refinement's tile tracker (tile_tracker.hpp) gets a
// frame's pixels from: a hash per tile, every real frame, and the pixels of
// the few rects it decides to send. Those are the only two things it ever
// reads, which is what lets the frame stay on the GPU.
//
// CpuTileSource is the frame in host memory -- a software encoder's input,
// or a read-back. screencast/dmabuf_tile_source.hpp is the other one: the
// captured dmabuf itself, hashed by a compute shader and with only the
// rects that go out read back, so a hardware encoder stays zero-copy under
// refinement. On a GPU behind a slow bus (a VM's passed-through card) the
// full read-back that replaces is ~11 ms a 4K frame; this is ~0.3 ms.
//
// Both hash with hash_tile() below, bit for bit -- the shader is a port of
// it -- so a session that moves between the two (the GPU path failing
// mid-session falls back to read-back) sees no tile change that isn't one.
#pragma once

#include "gdp/refine.hpp"

#include <cstdint>
#include <vector>

namespace wraith {

// FNV-1a over a tile's rows of XRGB8888 pixels, four interleaved lanes; see
// the .cpp. The definition every TileSource hashes with.
uint64_t hash_tile(const uint8_t *data, uint32_t stride, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

class TileSource {
public:
	virtual ~TileSource() = default;

	virtual uint32_t width() const = 0;
	virtual uint32_t height() const = 0;

	// hash_tile() of every `tile_size` tile of the frame into `out`, grid
	// row-major (cols * rows entries, edge tiles clipped to the frame).
	// False if the source couldn't read the frame; then failed() is true.
	virtual bool hash_tiles(uint32_t tile_size, uint64_t *out) = 0;

	// Appends each of `rects`' pixels, in order, row-major and tightly
	// packed with the X byte dropped -- gdp::RefineLayer::tile_pixels's
	// layout. False (and failed()) if the source couldn't read them; `out`
	// is then unusable.
	virtual bool append_rects(const std::vector<gdp::RefineRect> &rects, std::vector<uint8_t> *out) = 0;

	// A read failed since the source was last bound to a frame. Sticky until
	// then, so a caller can check once after handing the source on.
	bool failed() const { return failed_; }

protected:
	bool failed_ = false;
};

// A frame in host memory, read in place: `data` must outlive the calls.
// Never fails.
class CpuTileSource final : public TileSource {
public:
	CpuTileSource() = default;
	CpuTileSource(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride)
		: data_(data), width_(width), height_(height), stride_(stride) {}

	uint32_t width() const override { return width_; }
	uint32_t height() const override { return height_; }
	bool hash_tiles(uint32_t tile_size, uint64_t *out) override;
	bool append_rects(const std::vector<gdp::RefineRect> &rects, std::vector<uint8_t> *out) override;

private:
	const uint8_t *data_ = nullptr;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint32_t stride_ = 0;
};

} // namespace wraith
