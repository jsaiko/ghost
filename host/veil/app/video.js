// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Video for the browser client: WebCodecs decodes what wraith encodes
// (gdp-spec.md §6.6) and the frames are drawn on a canvas.
//
// The client offers only what VideoDecoder.isConfigSupported accepts.
// Every codec arrives as an Annex B (H.264, H.265) or OBU (AV1)
// elementary stream with no out-of-band description, which is what
// WebCodecs decodes when `description` is left out.
//
// With lossless refinement (refine.js) each frame may carry a layer for the
// overlay canvas. The decoder is asynchronous, so a layer waits in `layers`
// until its frame has been output and is applied in frame order: a later
// layer-only frame must not overtake an earlier frame's clears.

import { log } from "./log.js";

const PROBES = {
  h264: "avc1.640033",
  h265: "hvc1.1.6.L153.B0",
  av1: "av01.0.12M.08",
};

/// The codec tokens this browser can decode, `preferred` first.
export async function decodableCodecs(preferred) {
  const out = [];
  if (typeof VideoDecoder === "undefined") return out;
  for (const [token, codec] of Object.entries(PROBES)) {
    try {
      const { supported } = await VideoDecoder.isConfigSupported({ codec, optimizeForLatency: true });
      if (supported) out.push(token);
    } catch (_) {
      // not this one
    }
  }
  if (preferred && out.includes(preferred)) {
    out.splice(out.indexOf(preferred), 1);
    out.unshift(preferred);
  }
  return out;
}

// H.264's codec string from the SPS in a keyframe: avc1.PPCCLL.
function avcCodecString(data) {
  for (let i = 0; i + 4 < data.length; i++) {
    if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1 && (data[i + 3] & 0x1f) === 7) {
      const hex = (b) => b.toString(16).padStart(2, "0");
      return "avc1." + hex(data[i + 4]) + hex(data[i + 5]) + hex(data[i + 6]);
    }
  }
  return PROBES.h264;
}

// Firefox's hardware decode (VA-API on Linux) ignores an H.264/H.265
// stream's crop: the macroblock-padded picture (1088 rows for 1080) is
// scaled into the display size, squeezing the picture up over a band of
// padding at the bottom, and the frame it hands over says nothing of it.
// Its software decoder crops correctly, so that is used whenever the
// size needs a crop there.
const FIREFOX = typeof navigator !== "undefined" && /\bFirefox\//.test(navigator.userAgent);

export class Video {
  constructor(canvas, token, onTiming, onBroken, overlay, softwareDecode = false) {
    this.canvas = canvas;
    this.ctx = canvas.getContext("2d", { alpha: false, desynchronized: true });
    this.token = token;
    this.onTiming = onTiming; // (decodeUs) per decoded frame
    this.onBroken = onBroken; // the decoder failed: ask for a keyframe
    this.overlay = overlay || null; // Overlay (refine.js) when refinement is in effect
    this.softwareDecode = softwareDecode; // the Settings page's choice
    this.layers = []; // {pts, layer, ready}, oldest first
    this.decoder = null;
    this.waitingForKey = true;
    this.decodeStart = new Map();
    this.pending = null;
    this.drawScheduled = false;
    this.frames = 0;
    this.errors = 0; // decoder errors and rejected chunks, for diagnostics
    this.codecString = "";
    this.acceleration = ""; // the hardwareAcceleration the decoder was configured with
    this.lastFrame = ""; // the last drawn frame's geometry and colour, for diagnostics
    this.width = 0;
    this.height = 0;
  }

  setSize(width, height) {
    if (!width || !height) return;
    if (this.canvas.width !== width || this.canvas.height !== height) {
      this.canvas.width = width;
      this.canvas.height = height;
      if (this.overlay) this.overlay.resize(width, height);
      if (this.onResize) this.onResize();
    }
    this.width = width;
    this.height = height;
  }

  configure(keyframe) {
    const codec = this.token === "h264" ? avcCodecString(keyframe) : PROBES[this.token];
    this.decoder = new VideoDecoder({
      output: (frame) => this.output(frame),
      error: (e) => {
        log.warn("video decoder error:", e);
        this.errors++;
        this.decoder = null;
        this.waitingForKey = true;
        this.flushLayers();
        if (this.onBroken) this.onBroken();
      },
    });
    const hardwareAcceleration = this.wantedAcceleration();
    this.decoder.configure({ codec, optimizeForLatency: true, hardwareAcceleration });
    this.codecString = codec;
    this.acceleration = hardwareAcceleration;
    log.info(`video decoder configured: ${codec}, hardwareAcceleration ${hardwareAcceleration}`);
  }

  wantedAcceleration() {
    if (this.softwareDecode) return "prefer-software";
    const crops = this.token !== "av1" && (this.width % 16 !== 0 || this.height % 16 !== 0);
    return FIREFOX && crops ? "prefer-software" : "no-preference";
  }

  /// One reassembled frame. Returns false when it was dropped (no
  /// keyframe yet since the decoder (re)started), which the session
  /// reports as a loss.
  decode(keyframe, pts, data, layer) {
    // A resolution change can change which decoder Firefox needs: switch at
    // the keyframe that starts the new size.
    if (keyframe && this.decoder && this.decoder.state !== "closed" && this.acceleration !== this.wantedAcceleration()) {
      this.decoder.close();
      this.decoder = null;
      this.waitingForKey = true;
      this.flushLayers();
    }
    if (this.waitingForKey) {
      if (!keyframe) return false;
      if (!this.decoder || this.decoder.state === "closed") this.configure(data);
      this.waitingForKey = false;
    }
    if (!this.decoder || this.decoder.state !== "configured") return false;
    // Behind by more than a few frames: the decoder can't keep up. Drop
    // deltas until it catches up rather than letting latency build.
    if (this.decoder.decodeQueueSize > 8 && !keyframe) return false;
    this.decodeStart.set(pts, performance.now());
    if (layer) this.queueLayer({ pts, layer, ready: false });
    try {
      this.decoder.decode(new EncodedVideoChunk({ type: keyframe ? "key" : "delta", timestamp: pts, data }));
    } catch (e) {
      log.warn("decode failed:", e);
      this.errors++;
      this.waitingForKey = true;
      this.flushLayers();
      return false;
    }
    return true;
  }

  /// A layer-only frame (no video bytes): applied once everything queued
  /// before it has been.
  decodeLayerOnly(layer) {
    this.queueLayer({ pts: -1, layer, ready: true });
  }

  queueLayer(entry) {
    this.layers.push(entry);
    // A frame the decoder never outputs would block the queue for good.
    if (this.layers.length > 32) this.flushLayers();
    else this.drainLayers();
  }

  drainLayers() {
    while (this.layers.length && this.layers[0].ready) {
      const { layer } = this.layers.shift();
      if (this.overlay) this.overlay.apply(layer);
    }
  }

  flushLayers() {
    for (const e of this.layers) e.ready = true;
    this.drainLayers();
  }

  output(frame) {
    const started = this.decodeStart.get(frame.timestamp);
    if (started !== undefined) {
      this.decodeStart.delete(frame.timestamp);
      if (this.onTiming) this.onTiming(Math.round((performance.now() - started) * 1000));
    }
    if (this.decodeStart.size > 64) this.decodeStart.clear();
    const entry = this.layers.find((e) => e.pts === frame.timestamp && !e.ready);
    if (entry) {
      entry.ready = true;
      this.drainLayers();
    }
    // Only the newest frame is drawn: one waiting for the next paint is
    // replaced, never queued.
    if (this.pending) this.pending.close();
    this.pending = frame;
    if (!this.drawScheduled) {
      this.drawScheduled = true;
      requestAnimationFrame(() => this.draw());
    }
  }

  draw() {
    this.drawScheduled = false;
    const frame = this.pending;
    this.pending = null;
    if (!frame) return;
    const w = this.width || frame.displayWidth;
    const h = this.height || frame.displayHeight;
    if (this.canvas.width !== w || this.canvas.height !== h) this.setSize(w, h);
    this.lastFrame = describeFrame(frame);
    // The coded size can be padded past the display size (AV1 can't
    // crop, gdp-spec.md §7.2): draw only the display rectangle.
    this.ctx.drawImage(frame, 0, 0, w, h, 0, 0, w, h);
    frame.close();
    this.frames++;
  }

  close() {
    if (this.pending) this.pending.close();
    this.pending = null;
    if (this.decoder && this.decoder.state !== "closed") this.decoder.close();
    this.decoder = null;
    this.layers = [];
  }
}

// What the decoder says about a frame: how big it is, which part is the
// picture, and how its colours are to be read. Browsers disagree on these
// (cropping, colour defaults), so a support report carries them.
function describeFrame(frame) {
  const r = frame.visibleRect;
  const c = frame.colorSpace || {};
  return (
    `${frame.format} coded ${frame.codedWidth}x${frame.codedHeight}, ` +
    `visible ${r ? `${r.width}x${r.height} at ${r.x},${r.y}` : "?"}, display ${frame.displayWidth}x${frame.displayHeight}, ` +
    `colour primaries ${c.primaries}, transfer ${c.transfer}, matrix ${c.matrix}, full range ${c.fullRange}`
  );
}

/// A CursorShape (gdp-spec.md §7.4), premultiplied ARGB8888 (B, G, R, A
/// in memory), as a PNG data URL with its size and hotspot, or null for a
/// hidden (0x0) cursor. Drawn as the CSS cursor normally, and as the
/// session's own overlay while the pointer is locked (session.js).
export function cursorImage(shape) {
  if (!shape.width || !shape.height || !shape.argb8888 || shape.argb8888.length < shape.width * shape.height * 4) {
    return null;
  }
  const canvas = document.createElement("canvas");
  canvas.width = shape.width;
  canvas.height = shape.height;
  const ctx = canvas.getContext("2d");
  const image = ctx.createImageData(shape.width, shape.height);
  const src = shape.argb8888;
  const dst = image.data;
  for (let i = 0; i < src.length; i += 4) {
    const a = src[i + 3];
    const un = (c) => (a === 0 ? 0 : Math.min(255, Math.round((c * 255) / a)));
    dst[i] = un(src[i + 2]);
    dst[i + 1] = un(src[i + 1]);
    dst[i + 2] = un(src[i]);
    dst[i + 3] = a;
  }
  ctx.putImageData(image, 0, 0);
  return {
    url: canvas.toDataURL("image/png"),
    width: shape.width,
    height: shape.height,
    hotspotX: shape.hotspot_x,
    hotspotY: shape.hotspot_y,
  };
}
