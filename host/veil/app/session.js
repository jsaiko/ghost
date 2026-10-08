// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// A GDP session in the browser (gdp-spec.md §6-§11), the counterpart of
// spectre's SessionClient: SessionHello with the gateway token, then the
// control stream's messages, the input stream, and the video and audio
// datagrams, with a StatsReport every 250 ms so wraith's rate control
// works as it does for spectre (same loss bitmap, delay and train
// figures).
import { decode, encode, frame, FrameReader } from "./proto.js";
import { Video, cursorImage } from "./video.js";
import { Overlay, parseRefine } from "./refine.js";
import { Audio } from "./audio.js";
import { Input } from "./input.js";
import { Gamepads } from "./gamepad.js";
import { log, recentLog } from "./log.js";

const STATS_INTERVAL_MS = 250;
const PING_INTERVAL_MS = 1000;
const MIN_TRAIN_DATAGRAMS = 16;
const MAX_TRAINS = 8;
// gdp-spec.md §7.9: 1 MiB − 1 KiB, so the message fits one control frame.
const CLIPBOARD_MAX = (1 << 20) - 1024;
// gdp-spec.md §7.4: the largest CursorShape side.
const CURSOR_MAX_DIM = 384;
// gdp-spec.md §7.11: a report is at most 512 KiB, and a client answers at
// most once every 30 s.
const DIAGNOSTICS_MAX = 512 << 10;
const DIAGNOSTICS_INTERVAL_MS = 30000;

export const ERRORS = {
  0: "The session closed.",
  11: "The session broke a protocol rule.",
  13: "This browser can't decode any video codec the host offers.",
  15: "The host doesn't support something this session needs.",
  20: "The session couldn't be authenticated; log in again.",
  24: "You're already connected to this session from another client.",
  32: "The session couldn't start.",
  40: "The session ended.",
  41: "You were signed out by a login at the host itself.",
  42: "Your session was taken over by another client.",
};

// gdp-spec.md §11: compare 32-bit counters as signed differences.
const diff32 = (a, b) => (a - b) | 0;

export class Session {
  constructor(transport, { token, canvas, overlay, cursor, codecs, softwareDecode, lossless, onStatus, onEnd, onStats, onLossless }) {
    this.t = transport;
    this.token = token;
    this.canvas = canvas;
    // While the pointer is locked the browser hides every cursor, so the
    // host's is drawn here instead, where wraith reports it
    // (CursorPosition), as spectre does with the mouse captured.
    this.cursorEl = cursor || null;
    this.cursor = null; // cursorImage() of the host's current shape
    this.cursorPos = { x: 0.5, y: 0.5 };
    this.overlayCanvas = overlay || null; // the refine layer is offered only with one
    this.codecs = codecs;
    this.softwareDecode = !!softwareDecode;
    // The lossless layer to start with; the host starts every session
    // with it on, so off is a RefinePause right after SessionAccept.
    this.lossless = lossless !== false;
    this.onLossless = onLossless; // (on) => void, only when "refine" is in effect
    this.onStatus = onStatus;
    this.onEnd = onEnd;
    this.onStats = onStats;
    this.accepted = null;
    this.epoch = 0;
    this.video = null;
    this.audio = null;
    this.input = null;
    this.gamepads = null;
    this.capabilities = [];
    this.ended = false;
    // Reassembly: one frame in progress (gdp::VideoFrameReassembler).
    this.pendingFrame = null;
    // Ack state (spectre's note_frame_seen): bit i of `mask` is frame
    // highest - i completed.
    this.ack = { has: false, highest: 0, mask: 0n, span: 0 };
    this.window = this.newWindow();
    this.train = null;
    this.timing = { decodeSum: 0, count: 0 };
    this.clipboardLastSent = null;
    this.clipboardLastReceived = null;
    this.timers = [];
    this.pings = new Map();
    this.rttUs = 0;
    this.counters = { frames: 0, bytes: 0, lost: 0 };
    this.lastDiagnostics = null;
  }

  newWindow() {
    return { start: performance.now(), bytes: 0, delayMin: 0, delaySum: 0, delaySamples: 0, trains: [] };
  }

  async start() {
    const reader = new FrameReader((bytes) => this.control(decode("ControlEnvelope", bytes)));
    this.t.control.onData = (chunk) => {
      try {
        reader.push(chunk);
      } catch (e) {
        log.warn("control stream:", e);
        this.end(6);
      }
    };
    this.t.input.onData = () => {};
    this.t.onDatagram = (d, at) => this.datagram(d, at);
    this.t.onClose = (code) => this.end(code);
    const capabilities = [];
    if (navigator.clipboard && window.isSecureContext) capabilities.push("clipboard");
    if (navigator.getGamepads) capabilities.push("gamepad");
    if (this.overlayCanvas) capabilities.push("refine");
    this.t.control.write(
      frame("ControlEnvelope", {
        hello: {
          token: this.token,
          codecs: this.codecs,
          decoders: ["webcodecs"],
          capabilities,
          network_profile: 0,
        },
      }),
    );
    this.onStatus("Starting the session…");
  }

  sendControl(msg) {
    if (!this.ended) this.t.control.write(frame("ControlEnvelope", msg));
  }

  sendInput(event) {
    if (this.ended) return;
    this.t.input.write(frame("InputEnvelope", { client_time_us: BigInt(Math.round(performance.now() * 1000)), ...event }));
  }

  control(env) {
    if (env.accept) return this.accept(env.accept);
    if (!this.accepted) return; // nothing else before SessionAccept
    if (env.ping) return this.sendControl({ pong: { nonce: env.ping.nonce } });
    if (env.pong) {
      const sent = this.pings.get(env.pong.nonce);
      if (sent !== undefined) {
        this.pings.delete(env.pong.nonce);
        this.rttUs = Math.round((performance.now() - sent) * 1000);
        this.t.lastRttUs = this.rttUs;
      }
      return;
    }
    if (env.cursor_shape) {
      // gdp-spec.md §7.4: a shape over the size cap, or whose pixels
      // don't match its size, is ignored rather than drawn as hidden.
      const s = env.cursor_shape;
      const w = s.width || 0, h = s.height || 0;
      if (w > CURSOR_MAX_DIM || h > CURSOR_MAX_DIM || (s.argb8888 ? s.argb8888.length : 0) !== w * h * 4) return;
      this.cursor = cursorImage(s);
      const c = this.cursor;
      this.canvas.style.cursor = c ? `url(${c.url}) ${c.hotspotX} ${c.hotspotY}, auto` : "none";
      this.drawCursor();
      return;
    }
    if (env.cursor_position) {
      this.cursorPos = { x: env.cursor_position.x || 0, y: env.cursor_position.y || 0 };
      this.drawCursor();
      return;
    }
    if (env.displays_changed) return this.applyOutputs(env.displays_changed.outputs);
    if (env.clipboard) return this.clipboardFromHost(env.clipboard);
    if (env.diagnostics_request) return this.diagnostics();
    if (env.network_profile_changed) {
      this.profile = env.network_profile_changed.profile;
      return;
    }
  }

  async accept(a) {
    if (this.accepted) return;
    if (!this.codecs.includes(a.codec)) {
      // gdp-spec.md §6.6: a codec we didn't offer is a protocol violation.
      this.t.close(13);
      this.end(13);
      return;
    }
    this.accepted = a;
    log.info(`session accepted: codec ${a.codec} (host encoder ${a.encoder || "?"}), capabilities ${(a.capabilities || []).join(", ") || "none"}`);
    // gdp-spec.md §11: the client's session clock starts at SessionAccept.
    this.epoch = performance.now();
    this.capabilities = a.capabilities || [];
    this.profile = a.network_profile;
    this.video = new Video(
      this.canvas,
      a.codec,
      (us) => {
        this.timing.decodeSum += us;
        this.timing.count++;
      },
      () => this.sendControl({ keyframe_request: { stream_id: 0 } }),
      this.capabilities.includes("refine") ? new Overlay(this.overlayCanvas) : null,
      this.softwareDecode,
    );
    this.video.onResize = () => this.onResize && this.onResize();
    this.applyOutputs(a.outputs);
    this.input = new Input(this.canvas, (event) => this.sendInput(event), (name) => this.onMenu && this.onMenu(name));
    this.input.onLockChange = (locked) => {
      // Start the drawn cursor where the real one was.
      if (locked && this.input.lastPosition) this.cursorPos = { ...this.input.lastPosition };
      this.drawCursor();
    };
    if (this.capabilities.includes("refine")) {
      if (!this.lossless) this.sendControl({ refine_pause: { paused: true } });
      if (this.onLossless) this.onLossless(this.lossless);
    }
    if (this.capabilities.includes("gamepad")) this.gamepads = new Gamepads((event) => this.sendInput(event));
    this.timers.push(setInterval(() => this.report(), STATS_INTERVAL_MS));
    this.timers.push(setInterval(() => this.ping(), PING_INTERVAL_MS));
    if (this.capabilities.includes("clipboard")) {
      this.onFocus = () => this.clipboardToHost();
      window.addEventListener("focus", this.onFocus);
    }
    this.onStatus("");
    try {
      this.audio = await Audio.create(a.audio);
    } catch (e) {
      log.warn("no audio:", e);
    }
  }

  applyOutputs(outputs) {
    const out = (outputs || []).find((o) => (o.stream_id || 0) === 0);
    if (out && out.display && this.video) this.video.setSize(out.display.width, out.display.height);
  }

  datagram(d, arrival) {
    if (!this.accepted || d.length < 1) return;
    if (d[0] === 0x02) {
      // §10: channel, seq u16, pts u32, Opus.
      if (d.length > 7 && this.audio) {
        const pts = new DataView(d.buffer, d.byteOffset + 3, 4).getUint32(0, true);
        this.audio.push(pts, d.subarray(7));
      }
      return;
    }
    if (d[0] !== 0x01 || d.length < 17) return;
    // §9.1: channel, then stream_id u8, frame_id u32, slice_idx u16,
    // slice_count u16, flags u8, pts u32, host_lat u16.
    const v = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const stream = d[1];
    if (stream !== 0) return;
    const frameId = v.getUint32(2, true);
    const sliceIdx = v.getUint16(6, true);
    const sliceCount = v.getUint16(8, true);
    const keyframe = (d[10] & 1) !== 0;
    const pts = v.getUint32(11, true);
    if (sliceCount === 0 || sliceIdx >= sliceCount) return;
    const payload = d.subarray(17);

    this.window.bytes += d.length;
    this.counters.bytes += d.length;
    if (sliceIdx === 0) {
      const now = ((performance.now() - this.epoch) * 1000) >>> 0;
      const delay = diff32(now, pts);
      if (this.window.delaySamples === 0 || delay < this.window.delayMin) this.window.delayMin = delay;
      this.window.delaySum += delay;
      this.window.delaySamples++;
    }
    // Train timing: the datagrams of one frame, first arrival to last.
    if (!this.train || this.train.frameId !== frameId) {
      this.train = { frameId, first: arrival, last: arrival, bytesAfterFirst: 0, datagrams: 0 };
    } else {
      this.train.bytesAfterFirst += d.length;
    }
    this.train.last = arrival;
    this.train.datagrams++;

    let f = this.pendingFrame;
    if (!f || f.frameId !== frameId) {
      // A newer frame: whatever was pending is lost (nothing is resent).
      if (f && diff32(frameId, f.frameId) < 0) return; // a late slice of an older frame
      f = { frameId, keyframe, pts, slices: new Array(sliceCount), received: 0 };
      this.pendingFrame = f;
    }
    if (sliceCount !== f.slices.length || f.slices[sliceIdx]) return;
    f.slices[sliceIdx] = payload.slice();
    f.received++;
    if (f.received < f.slices.length) return;
    this.pendingFrame = null;

    this.noteTrain(sliceCount);
    let size = 0;
    for (const s of f.slices) size += s.length;
    const data = new Uint8Array(size);
    let pos = 0;
    for (const s of f.slices) {
      data.set(s, pos);
      pos += s.length;
    }
    let consumed;
    if (this.capabilities.includes("refine")) {
      // Every payload is a container once "refine" is in effect, even a
      // layer-only one (no base bytes, no keyframe).
      const parsed = parseRefine(data);
      if (!parsed) consumed = false;
      else if (parsed.base.length) consumed = this.video.decode(f.keyframe, f.pts, parsed.base, parsed.layer);
      else {
        this.video.decodeLayerOnly(parsed.layer);
        consumed = true;
      }
    } else {
      consumed = this.video.decode(f.keyframe, f.pts, data);
    }
    this.noteFrameSeen(frameId, consumed);
    this.counters.frames++;
  }

  noteTrain(sliceCount) {
    const t = this.train;
    if (!t || t.datagrams !== sliceCount || t.datagrams < MIN_TRAIN_DATAGRAMS || t.last <= t.first) return;
    if (this.window.trains.length >= MAX_TRAINS) return;
    this.window.trains.push({
      frame_id: t.frameId,
      datagrams: t.datagrams,
      bytes: t.bytesAfterFirst,
      span_us: Math.round((t.last - t.first) * 1000),
    });
    this.train = null;
  }

  // spectre's SessionClient::note_frame_seen.
  noteFrameSeen(frameId, completed) {
    const st = this.ack;
    const bit = completed ? 1n : 0n;
    if (!st.has) {
      Object.assign(st, { has: true, highest: frameId, mask: bit, span: 1 });
      return;
    }
    if (frameId === st.highest) {
      st.mask = (st.mask & ~1n) | bit;
      return;
    }
    const d = diff32(frameId, st.highest);
    if (d > 0) {
      st.mask = d >= 64 ? bit : BigInt.asUintN(64, (st.mask << BigInt(d)) | bit);
      st.span = d >= 64 - st.span ? 64 : st.span + d;
      st.highest = frameId;
    } else if (-d < 64) {
      const m = 1n << BigInt(-d);
      st.mask = completed ? st.mask | m : st.mask & ~m;
    }
  }

  report() {
    if (!this.accepted) return;
    const st = this.ack;
    const msg = { stats: { rtt_us: this.rttUs, streams: [] } };
    if (st.has) {
      const seen = st.span >= 64 ? (1n << 64n) - 1n : (1n << BigInt(st.span)) - 1n;
      const loss = BigInt.asUintN(64, ~st.mask) & seen;
      const now = performance.now();
      const w = this.window;
      const stream = {
        stream_id: 0,
        highest_frame_id_acked: st.highest,
        loss_bitmap: loss,
        interval_us: Math.max(1, Math.round((now - w.start) * 1000)),
        bytes_received: BigInt(w.bytes),
        trains: w.trains,
      };
      if (this.timing.count > 0) stream.decode_us = Math.round(this.timing.decodeSum / this.timing.count);
      if (w.delaySamples > 0) {
        stream.delay_samples = w.delaySamples;
        stream.delay_min_us = w.delayMin;
        stream.delay_avg_us = Math.round(w.delaySum / w.delaySamples) | 0;
      }
      msg.stats.streams.push(stream);
      if (this.onStats) {
        this.onStats({
          kind: this.t.kind,
          codec: this.accepted.codec,
          encoder: this.accepted.encoder,
          mbps: (w.bytes * 8) / ((now - w.start) * 1000),
          delayMs: w.delaySamples ? w.delaySum / w.delaySamples / 1000 : null,
          rttMs: this.rttUs / 1000,
          decodeMs: this.timing.count ? this.timing.decodeSum / this.timing.count / 1000 : null,
          lost: countBits(loss),
          fps: this.video ? this.video.frames : 0,
        });
      }
      this.timing = { decodeSum: 0, count: 0 };
      this.window = this.newWindow();
    }
    this.sendControl(msg);
  }

  ping() {
    const nonce = BigInt(Math.floor(Math.random() * 2 ** 52));
    this.pings.set(nonce, performance.now());
    if (this.pings.size > 8) this.pings.delete(this.pings.keys().next().value);
    this.sendControl({ ping: { nonce } });
  }

  // gdp-spec.md §7.9: text only, at most CLIPBOARD_MAX bytes, never
  // echo back what the other side just sent.
  async clipboardFromHost(c) {
    if (!this.capabilities.includes("clipboard") || c.mime_type !== "text/plain") return;
    if (!c.data || c.data.length === 0 || c.data.length > CLIPBOARD_MAX) return;
    const text = new TextDecoder().decode(c.data);
    this.clipboardLastReceived = text;
    try {
      await navigator.clipboard.writeText(text);
    } catch (_) {
      // not focused: it lands on the next focus instead
      this.clipboardPending = text;
    }
  }

  async clipboardToHost() {
    if (this.clipboardPending !== undefined) {
      const text = this.clipboardPending;
      this.clipboardPending = undefined;
      try {
        await navigator.clipboard.writeText(text);
      } catch (_) {
        // still not allowed
      }
      return;
    }
    let text;
    try {
      text = await navigator.clipboard.readText();
    } catch (_) {
      return; // no permission
    }
    if (!text || text === this.clipboardLastReceived || text === this.clipboardLastSent) return;
    const data = new TextEncoder().encode(text);
    if (data.length > CLIPBOARD_MAX) return;
    this.clipboardLastSent = text;
    this.sendControl({ clipboard: { mime_type: "text/plain", data } });
  }

  // gdp-spec.md §7.11: the user ran `wraith --report` in the remote
  // desktop. Answer with what this side knows; each request is noted in
  // the log, and one within 30 s of the last answer is ignored.
  diagnostics() {
    const now = performance.now();
    const recently = this.lastDiagnostics !== null && now - this.lastDiagnostics < DIAGNOSTICS_INTERVAL_MS;
    log.info(`the host asked for diagnostics${recently ? " (too soon, ignored)" : ""}`);
    if (recently) return;
    this.lastDiagnostics = now;
    const a = this.accepted;
    const v = this.video;
    const decoder = v
      ? `webcodecs ${v.codecString || "(not configured yet)"}, hardwareAcceleration ` +
        `${v.acceleration || "-"}, state ${v.decoder ? v.decoder.state : "none"}, ` +
        `${v.frames} frames shown, ${v.errors} decode errors`
      : "none";
    const text =
      "browser client diagnostics\n" +
      `user agent: ${navigator.userAgent}\n` +
      `veil: ${location.host}\n` +
      `transport: ${this.t.kind}\n` +
      `offered codecs: ${this.codecs.join(", ") || "none"}\n` +
      `session codec: ${a.codec} (host encoder: ${a.encoder || "?"})\n` +
      `capabilities: ${this.capabilities.join(", ") || "none"}\n` +
      `decoder: ${decoder}\n` +
      `last frame: ${(v && v.lastFrame) || "none"}\n` +
      `canvas: ${this.canvas.width}x${this.canvas.height}\n` +
      `gpu (WebGL renderer): ${webglRenderer()}\n` +
      `frames received: ${this.counters.frames}, bytes: ${this.counters.bytes}\n` +
      `audio: ${this.audio ? "playing" : "none"}\n` +
      "\n--- log (the page's recent warnings and events) ---\n" +
      recentLog() +
      "\n";
    let bytes = new TextEncoder().encode(text);
    if (bytes.length > DIAGNOSTICS_MAX) bytes = bytes.subarray(bytes.length - DIAGNOSTICS_MAX);
    this.sendControl({ diagnostics_report: { text: bytes } });
  }

  logout() {
    this.sendControl({ logout_request: {} });
  }

  // The drawn cursor: shown only while the pointer is locked, at the
  // host's last reported position over the canvas.
  drawCursor() {
    const el = this.cursorEl;
    if (!el) return;
    const c = this.cursor;
    if (!c || !this.input || !this.input.locked || this.ended) {
      el.hidden = true;
      return;
    }
    const r = this.canvas.getBoundingClientRect();
    if (el.getAttribute("src") !== c.url) el.src = c.url;
    el.style.width = `${c.width}px`;
    el.style.height = `${c.height}px`;
    el.style.left = `${r.left + this.cursorPos.x * r.width - c.hotspotX}px`;
    el.style.top = `${r.top + this.cursorPos.y * r.height - c.hotspotY}px`;
    el.hidden = false;
  }

  // The lossless layer on or off (RefinePause, gdp-spec.md §7.8), as
  // spectre's Lossless row. No-op on a session without "refine".
  setLossless(on) {
    if (!this.capabilities.includes("refine") || on === this.lossless) return;
    this.lossless = on;
    this.sendControl({ refine_pause: { paused: !on } });
    if (this.onLossless) this.onLossless(on);
  }

  end(code) {
    if (this.ended) return;
    this.ended = true;
    for (const t of this.timers) clearInterval(t);
    if (this.onFocus) window.removeEventListener("focus", this.onFocus);
    if (this.input) {
      this.input.setCaptured(false);
      this.input.detach();
    }
    this.drawCursor();
    if (this.gamepads) this.gamepads.close();
    if (this.video) this.video.close();
    if (this.audio) this.audio.close();
    this.t.close(code);
    this.onEnd(code);
  }
}

// The GPU as WebGL names it, for a support report only: a fingerprinting
// surface, so it is read here and nowhere else. Browsers may mask or
// generalise it.
function webglRenderer() {
  try {
    const gl = document.createElement("canvas").getContext("webgl");
    if (!gl) return "no WebGL";
    const info = gl.getExtension("WEBGL_debug_renderer_info");
    const name = gl.getParameter(info ? info.UNMASKED_RENDERER_WEBGL : gl.RENDERER);
    const lose = gl.getExtension("WEBGL_lose_context");
    if (lose) lose.loseContext();
    return name || "unknown";
  } catch (_) {
    return "unknown";
  }
}

function countBits(n) {
  let c = 0;
  while (n) {
    c += Number(n & 1n);
    n >>= 1n;
  }
  return c;
}
