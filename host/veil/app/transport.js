// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The browser end of a GDP session connection, over WebTransport or the
// WebSocket fallback (gdp-spec.md §15). Both give the
// session the same shape: two byte streams (control, input), datagrams
// toward the browser, and a close with a GDP code (gdp-spec.md §12).
//
//   const t = await openTransport(urls, kind);
//   t.control.write(bytes); t.control.onData = (bytes) => ...
//   t.input.write(bytes)
//   t.onDatagram = (bytes, arrivalMs) => ...
//   t.onClose = (code, reason) => ...
//   t.close(code)
//   await t.rttUs()

import { log } from "./log.js";

const CLOSE_MAGIC = new TextEncoder().encode("GDPCLOSE");

class WebTransportLeg {
  constructor(wt) {
    this.kind = "webtransport";
    this.wt = wt;
    this.onDatagram = null;
    this.onClose = null;
    this.closeCode = null;
    this.closed = false;
  }

  async start() {
    const control = await this.wt.createBidirectionalStream();
    const input = await this.wt.createBidirectionalStream();
    this.control = streamEnds(control);
    this.input = streamEnds(input);
    this.readDatagrams();
    this.readCloseNotice();
    this.wt.closed
      .then((info) => this.finish(this.closeCode ?? info?.closeCode ?? 0, info?.reason || ""))
      .catch((e) => this.finish(this.closeCode ?? 0, String(e?.message || e)));
  }

  async readDatagrams() {
    const reader = this.wt.datagrams.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) return;
        if (this.onDatagram) this.onDatagram(value, performance.now());
      }
    } catch (_) {
      // the session closed
    }
  }

  // veild can't send WebTransport's close capsule, so it says why the
  // session ended on a one-off unidirectional stream: "GDPCLOSE" and the
  // GDP code, u32 LE, just before it closes.
  async readCloseNotice() {
    const streams = this.wt.incomingUnidirectionalStreams.getReader();
    try {
      for (;;) {
        const { value, done } = await streams.read();
        if (done) return;
        const bytes = await readAll(value);
        if (bytes.length === 12 && CLOSE_MAGIC.every((b, i) => bytes[i] === b)) {
          this.closeCode = new DataView(bytes.buffer, bytes.byteOffset + 8, 4).getUint32(0, true);
        }
      }
    } catch (_) {
      // the session closed
    }
  }

  finish(code, reason) {
    if (this.closed) return;
    this.closed = true;
    if (this.onClose) this.onClose(code, reason);
  }

  close(code = 0) {
    try {
      this.wt.close({ closeCode: code, reason: "" });
    } catch (_) {
      // already closed
    }
    this.finish(code, "");
  }

  async rttUs() {
    try {
      const stats = await this.wt.getStats();
      if (stats && stats.smoothedRtt) return Math.round(stats.smoothedRtt * 1000);
    } catch (_) {
      // not supported here
    }
    return 0;
  }
}

function streamEnds(stream) {
  const writer = stream.writable.getWriter();
  const ends = {
    onData: null,
    write(bytes) {
      writer.write(bytes).catch(() => {});
    },
  };
  (async () => {
    const reader = stream.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) return;
        if (ends.onData) ends.onData(value);
      }
    } catch (_) {
      // the session closed
    }
  })();
  return ends;
}

async function readAll(stream) {
  const reader = stream.getReader();
  const parts = [];
  let length = 0;
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    parts.push(value);
    length += value.length;
    if (length > 64) break;
  }
  const out = new Uint8Array(length);
  let pos = 0;
  for (const p of parts) {
    out.set(p, pos);
    pos += p.length;
  }
  return out;
}

// One WebSocket, one byte of channel in front of every message: 0 control,
// 1 input, 2 a datagram. Close codes are 4000 + the GDP code.
class WebSocketLeg {
  constructor(ws) {
    this.kind = "websocket";
    this.ws = ws;
    this.onDatagram = null;
    this.onClose = null;
    this.closed = false;
    this.pingSent = 0;
    this.lastRttUs = 0;
    const channel = (n) => ({
      onData: null,
      write: (bytes) => {
        if (ws.readyState !== WebSocket.OPEN) return;
        const msg = new Uint8Array(bytes.length + 1);
        msg[0] = n;
        msg.set(bytes, 1);
        ws.send(msg);
      },
    });
    this.control = channel(0);
    this.input = channel(1);
    ws.onmessage = (e) => {
      const data = new Uint8Array(e.data);
      if (data.length === 0) return;
      const payload = data.subarray(1);
      switch (data[0]) {
        case 0: if (this.control.onData) this.control.onData(payload); break;
        case 1: if (this.input.onData) this.input.onData(payload); break;
        case 2: if (this.onDatagram) this.onDatagram(payload, performance.now()); break;
      }
    };
    ws.onclose = (e) => this.finish(e.code >= 4000 && e.code < 5000 ? e.code - 4000 : 0, e.reason);
  }

  async start() {}

  finish(code, reason) {
    if (this.closed) return;
    this.closed = true;
    if (this.onClose) this.onClose(code, reason);
  }

  close(code = 0) {
    try {
      this.ws.close(4000 + code);
    } catch (_) {
      // already closed
    }
    this.finish(code, "");
  }

  // The session's own Ping/Pong measures this leg (session.js).
  async rttUs() {
    return this.lastRttUs;
  }
}

export async function openTransport(urls, prefer) {
  const tryWebTransport = prefer !== "websocket" && typeof WebTransport !== "undefined";
  if (tryWebTransport) {
    try {
      const wt = new WebTransport(urls.webtransport, { congestionControl: "low-latency" });
      await withTimeout(wt.ready, 5000);
      const leg = new WebTransportLeg(wt);
      await leg.start();
      log.info("transport: WebTransport");
      return leg;
    } catch (e) {
      log.warn("WebTransport failed, falling back to WebSocket:", e);
      if (prefer === "webtransport") throw e;
    }
  }
  const ws = new WebSocket(urls.websocket);
  ws.binaryType = "arraybuffer";
  await new Promise((resolve, reject) => {
    ws.onopen = resolve;
    ws.onerror = () => reject(new Error("the WebSocket connection failed"));
  });
  const leg = new WebSocketLeg(ws);
  await leg.start();
  log.info("transport: WebSocket");
  return leg;
}

function withTimeout(promise, ms) {
  return Promise.race([promise, new Promise((_, reject) => setTimeout(() => reject(new Error("timed out")), ms))]);
}
