// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The browser client's audio output: a small jitter buffer between the
// Opus decoder (audio.js) and the audio device. Decoded 10 ms frames
// arrive as planar Float32 over the port; the buffer aims to hold about
// `target` seconds, plays silence when it runs dry, and drops the oldest
// audio when it holds more than `max`, so latency can't build up.
class GdpPlayer extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions || {};
    this.channels = o.channels || 2;
    this.target = Math.round((o.targetMs || 40) * sampleRate / 1000);
    this.max = Math.round((o.maxMs || 150) * sampleRate / 1000);
    this.size = sampleRate * 2;
    this.ring = Array.from({ length: this.channels }, () => new Float32Array(this.size));
    this.read = 0;
    this.write = 0;
    this.filled = 0;
    this.started = false;
    this.port.onmessage = (e) => this.push(e.data);
  }

  push(planes) {
    const n = planes[0].length;
    for (let c = 0; c < this.channels; c++) {
      const src = planes[Math.min(c, planes.length - 1)];
      for (let i = 0; i < n; i++) this.ring[c][(this.write + i) % this.size] = src[i];
    }
    this.write = (this.write + n) % this.size;
    this.filled += n;
    if (this.filled > this.max) {
      const drop = this.filled - this.target;
      this.read = (this.read + drop) % this.size;
      this.filled -= drop;
    }
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    if (!this.started && this.filled < this.target) return true;
    this.started = true;
    if (this.filled < n) {
      // Ran dry: silence, and wait for the buffer to fill again.
      this.started = false;
      return true;
    }
    for (let c = 0; c < out.length; c++) {
      const ring = this.ring[Math.min(c, this.channels - 1)];
      for (let i = 0; i < n; i++) out[c][i] = ring[(this.read + i) % this.size];
    }
    this.read = (this.read + n) % this.size;
    this.filled -= n;
    return true;
  }
}

registerProcessor("gdp-player", GdpPlayer);
