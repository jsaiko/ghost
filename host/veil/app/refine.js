// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Lossless refinement for the browser client (gdp-spec.md §7.8, §9.5),
// the counterpart of libgdp's
// refine_parse_frame and spectre's overlay plane.
//
// The overlay is its own transparent canvas stacked over the video canvas,
// so the video's draws never touch it: a tile is putImageData'd (an exact
// replace, not a blend), a clear is clearRect.
import { decompress } from "./fzstd.js";

const MAGIC = 0x52;
const VERSION = 1;
const HEADER = 16;
const RECT = 8;
const FLAG_RESET = 1;
const FORMAT_BGR8 = 0; // the only tile format this client declares
// gdp::kMaxRefineLayerPixelBytes: the tile rects alone set what we
// allocate, so a frame claiming more is dropped, not trusted.
const MAX_PIXEL_BYTES = 64 << 20;

/// Splits a refined payload into its base bitstream and layer, or returns
/// null for anything malformed (the caller treats that as a dropped frame).
/// `base` is a view into `data`.
export function parseRefine(data) {
  if (data.length < HEADER || data[0] !== MAGIC || data[1] !== VERSION) return null;
  const v = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const flags = data[2];
  if (flags & ~FLAG_RESET) return null; // reserved bits must be 0
  if (data[3] !== FORMAT_BGR8 || data[5] !== 0) return null;
  const clearCount = data[4];
  const tileCount = v.getUint16(6, true);
  const baseLen = v.getUint32(8, true);
  const tilesLen = v.getUint32(12, true);
  if (HEADER + baseLen + (clearCount + tileCount) * RECT + tilesLen > data.length) return null;

  let p = HEADER;
  const base = data.subarray(p, p + baseLen);
  p += baseLen;
  const rects = (n) => {
    const out = [];
    for (let i = 0; i < n; i++, p += RECT) {
      out.push({ x: v.getUint16(p, true), y: v.getUint16(p + 2, true), w: v.getUint16(p + 4, true), h: v.getUint16(p + 6, true) });
    }
    return out;
  };
  const clears = rects(clearCount);
  const tiles = rects(tileCount);

  let expected = 0;
  for (const t of tiles) expected += t.w * t.h * 3;
  let pixels = null;
  if (tilesLen === 0) {
    if (expected !== 0) return null; // a tile list with nothing to fill it
  } else {
    if (expected === 0 || expected > MAX_PIXEL_BYTES) return null;
    try {
      pixels = decompress(data.subarray(p, p + tilesLen), new Uint8Array(expected));
    } catch (_) {
      return null;
    }
    if (pixels.length !== expected) return null;
  }
  return { base, layer: { reset: (flags & FLAG_RESET) !== 0, clears, tiles, pixels } };
}

export class Overlay {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext("2d");
  }

  /// A new frame size starts from an empty plane.
  resize(width, height) {
    if (this.canvas.width !== width || this.canvas.height !== height) {
      this.canvas.width = width;
      this.canvas.height = height;
    }
  }

  apply(layer) {
    const ctx = this.ctx;
    if (layer.reset) ctx.clearRect(0, 0, this.canvas.width, this.canvas.height);
    for (const c of layer.clears) ctx.clearRect(c.x, c.y, c.w, c.h);
    let pos = 0;
    for (const t of layer.tiles) {
      if (t.w === 0 || t.h === 0) continue;
      // The wire's B, G, R bytes become an opaque RGBA pixel.
      const img = new ImageData(t.w, t.h);
      const dst = img.data;
      const src = layer.pixels;
      for (let i = 0, n = t.w * t.h; i < n; i++, pos += 3) {
        dst[i * 4] = src[pos + 2];
        dst[i * 4 + 1] = src[pos + 1];
        dst[i * 4 + 2] = src[pos];
        dst[i * 4 + 3] = 255;
      }
      ctx.putImageData(img, t.x, t.y);
    }
  }
}
