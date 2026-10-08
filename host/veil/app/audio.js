// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Session audio for the browser client (gdp-spec.md §10): Opus packets
// from the audio datagrams, decoded by WebCodecs and played through an
// AudioWorklet jitter buffer (audio-worklet.js). Optional: a browser
// without Opus in WebCodecs, or a session without audio, runs silent.

import { log } from "./log.js";

export class Audio {
  static async create(config) {
    if (typeof AudioDecoder === "undefined" || !config) return null;
    const codec = config.codec || "opus";
    if (codec !== "opus") return null;
    const sampleRate = config.sample_rate_hz || 48000;
    const channels = config.channels || 2;
    const { supported } = await AudioDecoder.isConfigSupported({ codec: "opus", sampleRate, numberOfChannels: channels });
    if (!supported) return null;
    const audio = new Audio(sampleRate, channels);
    await audio.start();
    return audio;
  }

  constructor(sampleRate, channels) {
    this.sampleRate = sampleRate;
    this.channels = channels;
    this.ctx = null;
    this.node = null;
    this.decoder = null;
  }

  async start() {
    this.ctx = new AudioContext({ sampleRate: this.sampleRate, latencyHint: "interactive" });
    await this.ctx.audioWorklet.addModule("/app/audio-worklet.js");
    this.node = new AudioWorkletNode(this.ctx, "gdp-player", {
      outputChannelCount: [this.channels],
      processorOptions: { channels: this.channels, targetMs: 40, maxMs: 150 },
    });
    this.node.connect(this.ctx.destination);
    this.decoder = new AudioDecoder({
      output: (data) => this.output(data),
      error: (e) => log.warn("audio decoder error:", e),
    });
    this.decoder.configure({ codec: "opus", sampleRate: this.sampleRate, numberOfChannels: this.channels });
  }

  /// Browsers only start audio after a user gesture: the session view
  /// calls this from its first click or key.
  resume() {
    if (this.ctx && this.ctx.state !== "running") this.ctx.resume();
  }

  /// One audio datagram's payload.
  push(pts, payload) {
    if (!this.decoder || this.decoder.state !== "configured") return;
    try {
      this.decoder.decode(new EncodedAudioChunk({ type: "key", timestamp: pts, data: payload }));
    } catch (e) {
      log.warn("audio decode failed:", e);
    }
  }

  output(data) {
    const planes = [];
    for (let c = 0; c < data.numberOfChannels; c++) {
      const plane = new Float32Array(data.numberOfFrames);
      data.copyTo(plane, { planeIndex: c, format: "f32-planar" });
      planes.push(plane);
    }
    data.close();
    this.node.port.postMessage(planes, planes.map((p) => p.buffer));
  }

  close() {
    if (this.decoder && this.decoder.state !== "closed") this.decoder.close();
    if (this.ctx) this.ctx.close();
  }
}
