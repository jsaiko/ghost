// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: MIT

// Lossless refinement (gdp-spec.md §7.8, §9.5): a second, lossless layer riding on top of whatever
// video codec the session runs on.
//
// It is a *capability* ("refine"), not a codec. The base layer is the
// session's ordinary elementary stream -- H.264, H.265 or AV1, from
// whichever backend the host picked -- and nothing about it knows the
// layer exists, which is what lets refinement reuse the entire existing
// encode/decode path unchanged. On top of that, each frame may carry a
// set of *lossless tiles*: rectangles of the source framebuffer,
// byte-exact, Zstd-compressed together.
//
// The client keeps a persistent full-frame overlay plane. A tile write
// sets that rectangle of the plane (opaque); a `clear` rectangle resets it
// (transparent, so the decoded video shows through there). The renderer
// draws the plane over the decoded video, so any region the host has
// refreshed losslessly is shown byte-exact until the host says it changed
// again.
//
// That is the whole "settle to lossless" behaviour: while a region is
// moving, the host sends clears and lets the codec carry it; once the
// region has been still for a short settle time the host sends it as a
// tile and the region becomes exact. A desktop that stops changing converges to a
// bit-exact image, while video and dragging stay on the lossy path.
//
// A frame may carry no base bytes at all (base_len 0): a *layer-only*
// frame, which the client applies to the picture it is already showing.
// The host uses these once the screen has stopped changing and only the
// lossless layer still has tiles to send, so settling costs no video
// encode or decode.
//
// Loss recovery: a datagram loss can drop a frame and with it that frame's
// clears and tiles; a lost clear would otherwise leave stale lossless
// pixels on screen indefinitely. wraith keeps what each recent frame's
// layer carried and, when a StatsReport reports the frame lost, sends its
// clears again and re-arms its tiles -- only that frame's work, not the
// plane (gdp-spec.md §9.5). A reset (kFlagReset: the client clears the
// whole plane) is kept for when the host can't know what the plane holds:
// a new client, a lost reset, or a loss older than its history. A client
// that reassembles a frame and then discards it (no window yet, a
// malformed container, a decoder error) reports it as lost for the same
// reason. Nothing on the wire ties a reset to a keyframe: the plane is
// independent of the video's references, and a base encoder's own IDRs,
// periodic or loss repairs, cost the overlay nothing.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gdp {

// SessionHello/SessionAccept.capabilities name for lossless refinement
// (gdp-spec.md §6.7). When negotiated, every §9.5 payload of the session
// is the container below around the codec's elementary stream; when not,
// payloads are the bare elementary stream and this header is unused.
//
// spectre always offers it and the user switches the layer with
// RefinePause (the menu's Lossless Refinement row, spectre -R on|off): the
// host hashes every tile of every frame -- on the GPU where it can, else
// from a full-frame readback -- and copies out the tiles it sends, which
// is the right trade for reading text and the wrong one for a game. wraith always supports it: every
// encoder backend has a push_cpu() path.
inline constexpr const char *kCapabilityRefine = "refine";

// Rectangle in frame pixels. u16 each, so the container caps out at a
// 65535-pixel frame dimension -- far past anything wraith composites.
struct RefineRect {
	uint16_t x = 0;
	uint16_t y = 0;
	uint16_t width = 0;
	uint16_t height = 0;
};

// Wire header, little-endian, at the head of every refined §9.5 payload.
// The diagram is the documentation and RefineHeader only holds its
// constants; pack/parse below do the byte work.
//
//  +-------+---------+-------+--------+-------------+----------+------------+----------+-----------+
//  | magic | version | flags | format | clear_count | reserved | tile_count | base_len | tiles_len |
//  | u8    | u8      | u8    | u8     | u8          | u8 (0)   | u16 LE     | u32 LE   | u32 LE    |
//  +-------+---------+-------+--------+-------------+----------+------------+----------+-----------+
//
// followed by:
//   base_len bytes   -- the codec's elementary stream, exactly what the
//                       session would have carried for this frame without
//                       refinement
//   clear_count * 8  -- RefineRect, u16 LE x,y,width,height each
//   tile_count  * 8  -- RefineRect, same encoding
//   tiles_len bytes  -- one Zstd frame; decompresses to the tiles'
//                       pixels concatenated in tile order, each tile
//                       row-major and tightly packed, in `format`:
//                       kRefineFormatBgr8 is width*3 bytes per row, height
//                       rows, each pixel the B, G, R bytes of its XRGB8888
//                       form with the X byte dropped
//
// `format` is negotiated (SessionHello.refine_formats, gdp-spec.md §9.5)
// so a deeper session can carry deeper tiles; BGR8 is the only one
// defined, and the only one this build packs or parses.
//
// clear_count is a u8 rather than a u16 on purpose: the host coalesces
// adjacent clears into runs and stacks of runs, so a repainting window is
// a handful of rects, and change too scattered to coalesce is sent as one
// clear over its bounding box. It never needs more than 255.
struct RefineHeader {
	static constexpr uint8_t kMagic = 0x52; // 'R'
	static constexpr uint8_t kVersion = 1;
	// Drop the entire overlay plane before applying this frame's rects.
	// Only ever sent alongside a keyframe request, though not every
	// keyframe carries it; see the file comment on loss recovery.
	static constexpr uint8_t kFlagReset = 1u << 0;

	// 1 + 1 + 1 + 1 + 1 + 1 + 2 + 4 + 4 -- keep this in step with the
	// diagram above and with pack/parse's offsets. The reserved byte keeps
	// the u16 and u32s aligned.
	static constexpr size_t kSize = 16;
	static constexpr size_t kRectSize = 8;
};

// One frame's lossless layer, in both directions: what pack_frame()
// compresses and what parse_frame() hands back decompressed.
// RefineHeader's `format`: SessionHello.refine_formats' RefineFormat.
inline constexpr uint8_t kRefineFormatBgr8 = 0;

struct RefineLayer {
	bool reset = false;
	// Always kRefineFormatBgr8 today; pack and parse refuse anything else.
	uint8_t format = kRefineFormatBgr8;
	std::vector<RefineRect> clears;
	std::vector<RefineRect> tiles;
	// Tiles' pixels in the wire's 3-byte B, G, R form (see the diagram
	// above), concatenated in `tiles` order, each tightly packed at
	// width*kRefineBytesPerPixel bytes per row. The host drops the X byte
	// in the copy it makes out of the frame anyway, and the client puts
	// one back in the copy it makes into its plane, so neither end pays
	// a pass of its own for the smaller form. Must be exactly
	// refine_layer_pixel_bytes(tiles) long.
	std::vector<uint8_t> tile_pixels;

	bool empty() const { return !reset && clears.empty() && tiles.empty(); }
};

// Bytes per pixel in RefineLayer::tile_pixels and on the wire.
inline constexpr size_t kRefineBytesPerPixel = 3;

// One row of `width` pixels between the frame's XRGB8888 and the wire's
// 3-byte form: pack drops each pixel's X byte, unpack puts it back as
// 0xff (opaque, which is what the client's overlay plane needs). These
// are the copies each end makes anyway -- host out of the frame, client
// into its plane -- so the smaller form costs no pass of its own. Four
// pixels a step through 64-bit words, which measured about 2x a
// byte-by-byte loop and runs the same on every target.
void refine_pack_row(const uint8_t *xrgb, uint8_t *out, uint32_t width);
void refine_unpack_row(const uint8_t *in, uint8_t *xrgb, uint32_t width);

// sum(w * h * kRefineBytesPerPixel) over `tiles` -- the exact size
// tile_pixels must have.
size_t refine_layer_pixel_bytes(const std::vector<RefineRect> &tiles);

// Most decompressed tile bytes one frame's layer may carry (gdp-spec.md
// §9.5). The tile rects alone set the size a receiver allocates, and a
// few bytes of header can claim a 65535x65535 tile, so a receiver drops
// any frame claiming more than this rather than trust it. 64 MiB is a
// full 4K frame over twice over (24 MiB at 3 bytes a pixel), far past
// wraith's own 768 KiB per-frame budget
// (TileTrackerConfig::max_tile_bytes_per_frame).
inline constexpr size_t kMaxRefineLayerPixelBytes = 64u << 20;

// Builds one §9.5 payload from a base frame (the codec's own bytes) and (optionally) a lossless
// layer. Compresses layer.tile_pixels with Zstd at `zstd_level` (1..19; wraith uses 3, see
// refine_encoder.cpp). Returns false only on a malformed layer (tile_pixels not matching `tiles`,
// or over kMaxRefineLayerPixelBytes) or a Zstd failure; an empty layer is fine and simply produces
// a header plus the base bytes.
bool refine_pack_frame(const uint8_t *base, size_t base_len, const RefineLayer &layer, int zstd_level,
	std::vector<uint8_t> *out);

// Splits a §9.5 payload back into its base bitstream and lossless layer.
// `base_out` points into `payload` (no copy) and is valid as long as it
// is; `layer_out->tile_pixels` is decompressed into. Returns false for
// anything malformed -- wrong magic, a version or format this build
// doesn't know, a reserved flag bit or byte set, truncation, a Zstd failure, tile rects claiming more than
// kMaxRefineLayerPixelBytes, or a decompressed size that disagrees with
// the tile rects. Callers treat that as a dropped frame, not a fatal
// error: this parses attacker-reachable bytes off the wire.
bool refine_parse_frame(const uint8_t *payload, size_t payload_len, const uint8_t **base_out,
	size_t *base_len_out, RefineLayer *layer_out);

} // namespace gdp
