// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Gamepad forwarding for the browser client (gdp-spec.md §8.5), on a session that negotiated "gamepad": the Gamepad API's
// standard mapping, reordered into SDL's axis and button order
// (libgdp/include/gdp/gamepad.hpp), sent as a full snapshot whenever it
// changes. Up to four pads, one slot each.
const MAX_PADS = 4;
// SDL button index -> standard-mapping button index (-1: none).
const BUTTONS = [0, 1, 2, 3, 8, 16, 9, 10, 11, 4, 5, 12, 13, 14, 15, -1, -1, -1, -1, -1, 17, -1, -1, -1, -1, -1];

export class Gamepads {
  constructor(send) {
    this.send = send;
    this.slots = new Array(MAX_PADS).fill(null); // browser gamepad index per slot
    this.last = new Array(MAX_PADS).fill("");
    this.running = true;
    this.onConnect = (e) => this.connect(e.gamepad);
    this.onDisconnect = (e) => this.disconnect(e.gamepad);
    window.addEventListener("gamepadconnected", this.onConnect);
    window.addEventListener("gamepaddisconnected", this.onDisconnect);
    for (const pad of navigator.getGamepads()) if (pad) this.connect(pad);
    const tick = () => {
      if (!this.running) return;
      this.poll();
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }

  connect(pad) {
    if (pad.mapping !== "standard" || this.slots.includes(pad.index)) return;
    const slot = this.slots.indexOf(null);
    if (slot < 0) return;
    this.slots[slot] = pad.index;
    this.last[slot] = "";
    this.send({ gamepad_connect: { pad_index: slot, name: pad.id.slice(0, 64) } });
  }

  disconnect(pad) {
    const slot = this.slots.indexOf(pad.index);
    if (slot < 0) return;
    this.slots[slot] = null;
    this.send({ gamepad_disconnect: { pad_index: slot } });
  }

  poll() {
    const pads = navigator.getGamepads();
    for (let slot = 0; slot < MAX_PADS; slot++) {
      const index = this.slots[slot];
      if (index === null) continue;
      const pad = pads[index];
      if (!pad) continue;
      const b = pad.buttons;
      const a = pad.axes;
      const axes = [a[0] || 0, a[1] || 0, a[2] || 0, a[3] || 0, b[6] ? b[6].value : 0, b[7] ? b[7].value : 0];
      const buttons = BUTTONS.map((i) => i >= 0 && !!b[i] && b[i].pressed);
      const key = axes.map((v) => v.toFixed(3)).join() + buttons.map(Number).join("");
      if (key === this.last[slot]) continue;
      this.last[slot] = key;
      this.send({ gamepad: { pad_index: slot, axes, buttons } });
    }
  }

  close() {
    this.running = false;
    window.removeEventListener("gamepadconnected", this.onConnect);
    window.removeEventListener("gamepaddisconnected", this.onDisconnect);
  }
}
