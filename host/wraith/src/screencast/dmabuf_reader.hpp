// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Reads a captured dmabuf back into host memory with EGL/GLES on the host's
// render node, for an encoder that wants CPU pixels (lossless refinement's
// tile hashing, or software x264) on a screencast session.
//
// The alternative, asking the compositor for CPU (memfd) frames, puts the
// read-back inside the compositor's own frame: KWin's virtual output then
// presents every 20 ms instead of every 16.7 at 4K. Taking dmabufs and
// reading them back here keeps that cost out of the compositor;
// ScreencastHost decides per frame whether a frame needs reading at all
// (refinement paused by the client goes back to zero-copy encode).
//
// It also hashes tiles for lossless refinement on the GPU and copies out
// just the rects that go out (can_hash_tiles(); dmabuf_tile_source.hpp),
// which is what keeps a refined session over a hardware encoder zero-copy.
//
// Single-threaded: the context is made current on the thread that calls
// open() (wraith's main thread) and stays current there. Imports are cached
// per capture buffer, identified by the capture's release token.
#pragma once

#include "encode/encoder.hpp" // DmabufFrame
#include "gdp/refine.hpp"     // RefineRect

#include <cstdint>
#include <memory>
#include <vector>

namespace wraith {

class DmabufReader {
public:
	DmabufReader();
	~DmabufReader();
	DmabufReader(const DmabufReader &) = delete;
	DmabufReader &operator=(const DmabufReader &) = delete;

	// An EGL display on `drm_fd` (not taken over; must outlive this) and a
	// GLES context current on the calling thread. False if the driver
	// lacks dmabuf import, surfaceless contexts or BGRA read-back.
	bool open(int drm_fd);
	bool is_open() const;

	// The DRM format modifiers this reader can import `drm_format`
	// dmabufs with, to offer the compositor. Just DRM_FORMAT_MOD_LINEAR
	// where the driver can't list them.
	std::vector<uint64_t> supported_modifiers(uint32_t drm_format) const;

	// Reads `frame` (from the capture buffer `token`) into `dst` as tightly
	// packed XRGB8888 rows (width * 4 bytes each). False if the import or
	// the read fails.
	bool read(const DmabufFrame &frame, void *token, uint8_t *dst);

	// Whether open() got a GLES 3.1 context the two tile shaders below
	// build on. Without it, a refined session reads the frame back (read())
	// and hashes on the CPU.
	bool can_hash_tiles() const;
	// encode/refine/tile_source.hpp's hash_tile() of every `tile_size` tile
	// of `frame`, grid row-major, into `out` -- bit for bit what the CPU
	// hash of read()'s pixels gives. Waits for the GPU. False on failure.
	bool hash_tiles(const DmabufFrame &frame, void *token, uint32_t tile_size, uint64_t *out);
	// Each of `rects`' pixels, in order, into `out` as XRGB8888 (X = 0xff),
	// row-major and tightly packed. Waits for the GPU. False on failure.
	bool read_rects(const DmabufFrame &frame, void *token, const std::vector<gdp::RefineRect> &rects,
		std::vector<uint32_t> *out);

	// The capture withdrew the buffer behind `token`, or every buffer
	// (close): drop the cached imports, whose fds are about to close.
	void forget(void *token);
	void forget_all();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace wraith
