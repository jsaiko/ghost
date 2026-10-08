// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// A small protobuf codec for the GDP session messages the browser client
// uses (libgdp/proto/session.proto), so the page needs no protobuf
// library and no build step. Messages are plain objects keyed by the
// .proto field names; 64-bit integers are BigInts. The schema below must
// follow session.proto: a field missing here is skipped when decoding and
// can't be sent.

// [field number, name, type, repeated?]. type is a scalar type or the
// name of another message.
const S = {
  ControlEnvelope: [
    [1, "hello", "SessionHello"], [2, "accept", "SessionAccept"], [3, "reject", "SessionReject"],
    [4, "stats", "StatsReport"], [5, "ping", "Ping"], [6, "pong", "Pong"],
    [8, "displays_changed", "DisplaysChanged"], [9, "resolution_change", "ResolutionChange"],
    [10, "cursor_shape", "CursorShape"], [11, "cursor_position", "CursorPosition"],
    [12, "keyframe_request", "KeyframeRequest"], [13, "network_profile_changed", "NetworkProfileChanged"],
    [14, "refine_pause", "RefinePause"], [15, "clipboard", "ClipboardData"], [16, "logout_request", "LogoutRequest"],
    [17, "diagnostics_request", "DiagnosticsRequest"], [18, "diagnostics_report", "DiagnosticsReport"],
  ],
  DisplayDescriptor: [[1, "width", "uint32"], [2, "height", "uint32"], [3, "refresh_mhz", "uint32"], [4, "scale", "double"]],
  SessionHello: [
    [1, "token", "string"], [2, "take_over", "bool"], [3, "via_gateway", "bool"], [4, "network_profile", "enum"],
    [5, "displays", "DisplayDescriptor", true], [6, "codecs", "string", true],
    [10, "capabilities", "string", true], [12, "decoders", "string", true],
  ],
  OutputDescriptor: [[1, "stream_id", "uint32"], [2, "display", "DisplayDescriptor"]],
  AudioConfig: [[1, "sample_rate_hz", "uint32"], [2, "channels", "uint32"], [3, "frame_ms", "uint32"], [4, "codec", "string"]],
  SessionAccept: [
    [1, "outputs", "OutputDescriptor", true], [2, "codec", "string"], [3, "bitrate_ceiling_bps", "uint32"],
    [4, "audio", "AudioConfig"], [5, "microphone", "AudioConfig"], [6, "capabilities", "string", true],
    [7, "network_profile", "enum"], [8, "via_gateway", "bool"], [9, "encoder", "string"],
    [10, "host_user", "string"], [11, "host_name", "string"],
  ],
  SessionReject: [[1, "reason", "string"]],
  DisplaysChanged: [[1, "outputs", "OutputDescriptor", true]],
  ResolutionChange: [[1, "stream_id", "uint32"], [2, "display", "DisplayDescriptor"]],
  KeyframeRequest: [[1, "stream_id", "uint32"]],
  ClipboardData: [[1, "mime_type", "string"], [2, "data", "bytes"]],
  CursorShape: [[1, "width", "uint32"], [2, "height", "uint32"], [3, "hotspot_x", "uint32"], [4, "hotspot_y", "uint32"], [5, "argb8888", "bytes"]],
  CursorPosition: [[1, "x", "double"], [2, "y", "double"]],
  LogoutRequest: [],
  DiagnosticsRequest: [],
  DiagnosticsReport: [[1, "text", "bytes"]],
  NetworkProfileChanged: [[1, "profile", "enum"]],
  RefinePause: [[1, "paused", "bool"]],
  FrameTrain: [[1, "frame_id", "uint32"], [2, "datagrams", "uint32"], [3, "bytes", "uint32"], [4, "span_us", "uint32"]],
  StreamStats: [
    [1, "stream_id", "uint32"], [2, "highest_frame_id_acked", "uint32"], [3, "loss_bitmap", "uint64"],
    [4, "decode_us", "uint32"], [5, "present_us", "uint32"], [6, "interval_us", "uint32"],
    [7, "bytes_received", "uint64"], [8, "delay_min_us", "sint32"], [9, "delay_avg_us", "sint32"],
    [10, "delay_samples", "uint32"], [11, "trains", "FrameTrain", true],
  ],
  StatsReport: [[1, "rtt_us", "uint32"], [2, "streams", "StreamStats", true]],
  Ping: [[1, "nonce", "uint64"]],
  Pong: [[1, "nonce", "uint64"]],
  InputEnvelope: [
    [1, "client_time_us", "uint64"], [2, "key", "KeyEvent"], [3, "pointer_motion", "PointerMotion"],
    [4, "pointer_button", "PointerButton"], [5, "pointer_axis", "PointerAxis"], [6, "touch", "TouchEvent"],
    [7, "gamepad_connect", "GamepadConnect"], [8, "gamepad", "GamepadState"], [9, "gamepad_disconnect", "GamepadDisconnect"],
  ],
  KeyEvent: [[1, "hid_usage", "uint32"], [2, "state", "enum"]],
  PointerMotion: [[1, "dx", "double"], [2, "dy", "double"], [3, "absolute", "bool"]],
  PointerButton: [[1, "button", "uint32"], [2, "state", "enum"]],
  PointerAxis: [[1, "horizontal", "double"], [2, "vertical", "double"]],
  GamepadConnect: [[1, "pad_index", "uint32"], [2, "name", "string"]],
  GamepadDisconnect: [[1, "pad_index", "uint32"]],
  GamepadState: [[1, "pad_index", "uint32"], [2, "axes", "float", true], [3, "buttons", "bool", true]],
  TouchEvent: [[1, "touch_id", "uint32"], [2, "phase", "enum"], [3, "x", "double"], [4, "y", "double"]],
};

export const KEY_PRESSED = 1;
export const KEY_RELEASED = 2;

const WIRE = { uint32: 0, uint64: 0, sint32: 0, enum: 0, bool: 0, double: 1, float: 5, string: 2, bytes: 2 };
const utf8 = new TextEncoder();
const fromUtf8 = new TextDecoder();

class Writer {
  constructor() { this.buf = new Uint8Array(64); this.len = 0; }
  ensure(n) {
    if (this.len + n <= this.buf.length) return;
    let size = this.buf.length * 2;
    while (size < this.len + n) size *= 2;
    const b = new Uint8Array(size);
    b.set(this.buf.subarray(0, this.len));
    this.buf = b;
  }
  byte(b) { this.ensure(1); this.buf[this.len++] = b; }
  varint(v) {
    if (typeof v === "bigint") {
      v = BigInt.asUintN(64, v);
      while (v >= 0x80n) { this.byte(Number(v & 0x7fn) | 0x80); v >>= 7n; }
      this.byte(Number(v));
      return;
    }
    if (v < 0) { this.varint(BigInt.asUintN(64, BigInt(v))); return; }
    while (v >= 0x80) { this.byte((v % 0x80) | 0x80); v = Math.floor(v / 0x80); }
    this.byte(v);
  }
  raw(bytes) { this.ensure(bytes.length); this.buf.set(bytes, this.len); this.len += bytes.length; }
  fixed64(v) { this.ensure(8); new DataView(this.buf.buffer).setFloat64(this.len, v, true); this.len += 8; }
  fixed32(v) { this.ensure(4); new DataView(this.buf.buffer).setFloat32(this.len, v, true); this.len += 4; }
  done() { return this.buf.subarray(0, this.len); }
}

function writeScalar(w, type, v) {
  switch (type) {
    case "uint32": case "enum": w.varint(v >>> 0); break;
    case "uint64": w.varint(BigInt(v)); break;
    case "sint32": w.varint(((v << 1) ^ (v >> 31)) >>> 0); break;
    case "bool": w.varint(v ? 1 : 0); break;
    case "double": w.fixed64(v); break;
    case "float": w.fixed32(v); break;
    case "string": { const b = utf8.encode(v); w.varint(b.length); w.raw(b); break; }
    case "bytes": w.varint(v.length); w.raw(v); break;
  }
}

function isDefault(type, v) {
  if (v === undefined || v === null) return true;
  switch (type) {
    case "string": case "bytes": return v.length === 0;
    case "bool": return !v;
    case "uint64": return BigInt(v) === 0n;
    default: return v === 0;
  }
}

function encodeInto(w, name, obj) {
  for (const [num, field, type, repeated] of S[name]) {
    const v = obj[field];
    if (v === undefined || v === null) continue;
    if (S[type]) {
      for (const item of repeated ? v : [v]) {
        const sub = encode(type, item);
        w.varint((num << 3) | 2);
        w.varint(sub.length);
        w.raw(sub);
      }
    } else if (repeated && WIRE[type] !== 2) {
      if (v.length === 0) continue;
      const sub = new Writer();
      for (const item of v) writeScalar(sub, type, item);
      const bytes = sub.done();
      w.varint((num << 3) | 2);
      w.varint(bytes.length);
      w.raw(bytes);
    } else {
      for (const item of repeated ? v : [v]) {
        if (!repeated && isDefault(type, item)) continue;
        w.varint((num << 3) | WIRE[type]);
        writeScalar(w, type, item);
      }
    }
  }
}

export function encode(name, obj) {
  const w = new Writer();
  encodeInto(w, name, obj);
  return w.done();
}

// gdp-spec.md §3.1: u32 LE length, then the message.
export function frame(name, obj) {
  const body = encode(name, obj);
  const out = new Uint8Array(4 + body.length);
  new DataView(out.buffer).setUint32(0, body.length, true);
  out.set(body, 4);
  return out;
}

class Reader {
  constructor(buf) { this.buf = buf; this.pos = 0; this.view = new DataView(buf.buffer, buf.byteOffset, buf.byteLength); }
  varint() {
    let result = 0n, shift = 0n;
    for (;;) {
      if (this.pos >= this.buf.length) throw new Error("truncated varint");
      const b = this.buf[this.pos++];
      result |= BigInt(b & 0x7f) << shift;
      if (!(b & 0x80)) return result;
      shift += 7n;
      if (shift > 63n) throw new Error("varint too long");
    }
  }
  bytes() {
    const n = Number(this.varint());
    if (this.pos + n > this.buf.length) throw new Error("truncated field");
    const b = this.buf.subarray(this.pos, this.pos + n);
    this.pos += n;
    return b;
  }
  skip(wire) {
    switch (wire) {
      case 0: this.varint(); break;
      case 1: this.pos += 8; break;
      case 2: this.bytes(); break;
      case 5: this.pos += 4; break;
      default: throw new Error("bad wire type " + wire);
    }
  }
}

function readScalar(r, type, wire) {
  switch (type) {
    case "uint32": case "enum": return Number(BigInt.asUintN(32, r.varint()));
    case "uint64": return BigInt.asUintN(64, r.varint());
    case "sint32": { const n = Number(BigInt.asUintN(32, r.varint())); return (n >>> 1) ^ -(n & 1); }
    case "bool": return r.varint() !== 0n;
    case "double": { const v = r.view.getFloat64(r.pos, true); r.pos += 8; return v; }
    case "float": { const v = r.view.getFloat32(r.pos, true); r.pos += 4; return v; }
    case "string": return fromUtf8.decode(r.bytes());
    case "bytes": return r.bytes().slice();
  }
}

export function decode(name, buf) {
  const fields = new Map(S[name].map((f) => [f[0], f]));
  const out = {};
  for (const [, field, type, repeated] of S[name]) if (repeated) out[field] = [];
  const r = new Reader(buf);
  while (r.pos < buf.length) {
    const key = Number(r.varint());
    const num = key >>> 3, wire = key & 7;
    const f = fields.get(num);
    if (!f) { r.skip(wire); continue; }
    const [, field, type, repeated] = f;
    if (S[type]) {
      const v = decode(type, r.bytes());
      if (repeated) out[field].push(v); else out[field] = v;
    } else if (repeated && wire === 2 && WIRE[type] !== 2) {
      const sub = new Reader(r.bytes());
      while (sub.pos < sub.buf.length) out[field].push(readScalar(sub, type));
    } else {
      const v = readScalar(r, type, wire);
      if (repeated) out[field].push(v); else out[field] = v;
    }
  }
  return out;
}

// Reassembles §3.1 frames from a stream's chunks.
export class FrameReader {
  constructor(onFrame, maxFrame = 1 << 20) {
    this.onFrame = onFrame;
    this.max = maxFrame;
    this.buf = new Uint8Array(0);
  }
  push(chunk) {
    const merged = new Uint8Array(this.buf.length + chunk.length);
    merged.set(this.buf);
    merged.set(chunk, this.buf.length);
    let pos = 0;
    while (merged.length - pos >= 4) {
      const len = new DataView(merged.buffer, merged.byteOffset + pos, 4).getUint32(0, true);
      if (len > this.max) throw new Error("frame too large");
      if (merged.length - pos - 4 < len) break;
      this.onFrame(merged.subarray(pos + 4, pos + 4 + len));
      pos += 4 + len;
    }
    this.buf = merged.slice(pos);
  }
}
