// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Lossless refinement's TileSource (encode/refine/tile_source.hpp) for a
// captured dmabuf that never leaves the GPU: DmabufReader's compute shaders
// hash its tiles and copy out just the rects that go out, so a refined
// session over a hardware encoder hands that encoder the dmabuf as is.
//
// ScreencastHost keeps one, bound to the frame it is delivering
// (bind()); the idle pump goes on reading the same frame through it until
// the next one arrives, which FrameHold keeps alive until then.
#pragma once

#include "encode/refine/tile_source.hpp"
#include "screencast/dmabuf_reader.hpp"

#include <vector>

namespace wraith {

class DmabufTileSource final : public TileSource {
public:
	// `reader` must outlive this and be can_hash_tiles().
	explicit DmabufTileSource(DmabufReader &reader) : reader_(reader) {}

	// Points the source at `frame` (from the capture buffer `token`, whose
	// fds must stay open while it is read) and clears failed().
	void bind(const DmabufFrame &frame, void *token);

	uint32_t width() const override { return (uint32_t)frame_.width; }
	uint32_t height() const override { return (uint32_t)frame_.height; }
	bool hash_tiles(uint32_t tile_size, uint64_t *out) override;
	bool append_rects(const std::vector<gdp::RefineRect> &rects, std::vector<uint8_t> *out) override;

private:
	DmabufReader &reader_;
	DmabufFrame frame_{};
	void *token_ = nullptr;
	std::vector<uint32_t> gathered_; // read_rects()' XRGB8888, reused
};

} // namespace wraith
