// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Keyboard, pointer, wheel and touch for the browser client
// (gdp-spec.md §8.2). Absolute pointer positions are 0..1 over the video
// (the canvas is laid out to the video's aspect ratio, so its box is the
// video); with the mouse captured the pointer is locked (Pointer Lock) and moves
// are relative, in pixels, as spectre sends them. Buttons go as Linux
// BTN_* codes, keys as USB HID usages (keymap.js), the wheel in notches
// with spectre's sign convention.
import { hidUsage } from "./keymap.js";
import { KEY_PRESSED, KEY_RELEASED } from "./proto.js";

const BTN = [0x110, 0x112, 0x111, 0x113, 0x114]; // left, middle, right, back, forward

export class Input {
  constructor(canvas, send, onMenuKey) {
    this.canvas = canvas;
    this.send = send; // (InputEnvelope fields) => void
    this.onMenuKey = onMenuKey; // our own shortcuts: (name) => void
    this.keys = new Set();
    this.buttons = new Set();
    this.locked = false;
    this.onLockChange = null; // (locked) => void
    this.lastPosition = null; // the last absolute position sent, 0..1
    this.enabled = true;
    this.listeners = [];
    this.attach();
  }

  on(target, type, fn, opts) {
    target.addEventListener(type, fn, opts);
    this.listeners.push([target, type, fn, opts]);
  }

  detach() {
    for (const [target, type, fn, opts] of this.listeners) target.removeEventListener(type, fn, opts);
    this.listeners = [];
    this.releaseAll();
  }

  attach() {
    const c = this.canvas;
    this.on(window, "keydown", (e) => this.key(e, true), { capture: true });
    this.on(window, "keyup", (e) => this.key(e, false), { capture: true });
    this.on(window, "blur", () => this.releaseAll());
    this.on(c, "pointermove", (e) => this.move(e));
    this.on(c, "pointerdown", (e) => this.button(e, true));
    this.on(c, "pointerup", (e) => this.button(e, false));
    this.on(c, "pointercancel", (e) => this.touchEnd(e, 4));
    this.on(c, "contextmenu", (e) => e.preventDefault());
    this.on(c, "wheel", (e) => this.wheel(e), { passive: false });
    this.on(document, "pointerlockchange", () => {
      const locked = document.pointerLockElement === c;
      if (locked === this.locked) return;
      this.locked = locked;
      if (this.onLockChange) this.onLockChange(locked);
    });
  }

  key(e, down) {
    if (!this.enabled) return;
    // Ctrl+Alt+Shift: the client's own menu keys, never sent.
    if (down && e.ctrlKey && e.altKey && e.shiftKey) {
      const name = { KeyF: "fullscreen", KeyG: "capture", KeyS: "stats", KeyQ: "disconnect" }[e.code];
      if (name) {
        e.preventDefault();
        this.onMenuKey(name);
        return;
      }
    }
    // Scroll Lock captures or releases the mouse, as in spectre, and never reaches
    // the host: in fullscreen with the mouse captured it is the one key
    // that gets out, since Keyboard Lock sends a tapped Esc to the remote
    // (only a held Esc leaves fullscreen).
    if (e.code === "ScrollLock") {
      e.preventDefault();
      if (down && !e.repeat) this.onMenuKey("capture");
      return;
    }
    const hid = hidUsage(e.code);
    if (!hid) return;
    e.preventDefault();
    e.stopPropagation();
    if (down) {
      if (e.repeat && this.keys.has(hid)) return; // the host repeats on its own
      this.keys.add(hid);
    } else if (!this.keys.delete(hid)) {
      return;
    }
    this.send({ key: { hid_usage: hid, state: down ? KEY_PRESSED : KEY_RELEASED } });
  }

  releaseAll() {
    for (const hid of this.keys) this.send({ key: { hid_usage: hid, state: KEY_RELEASED } });
    this.keys.clear();
    for (const btn of this.buttons) this.send({ pointer_button: { button: btn, state: KEY_RELEASED } });
    this.buttons.clear();
  }

  position(e) {
    const r = this.canvas.getBoundingClientRect();
    return {
      x: Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)),
      y: Math.min(1, Math.max(0, (e.clientY - r.top) / r.height)),
    };
  }

  move(e) {
    if (!this.enabled) return;
    if (e.pointerType === "touch") {
      const p = this.position(e);
      this.send({ touch: { touch_id: e.pointerId, phase: 2, x: p.x, y: p.y } });
      return;
    }
    if (this.locked) {
      if (e.movementX || e.movementY) this.send({ pointer_motion: { dx: e.movementX, dy: e.movementY, absolute: false } });
      return;
    }
    const p = this.position(e);
    this.lastPosition = p;
    this.send({ pointer_motion: { dx: p.x, dy: p.y, absolute: true } });
  }

  button(e, down) {
    if (!this.enabled) return;
    if (e.pointerType === "touch") {
      if (down) {
        this.canvas.setPointerCapture(e.pointerId);
        const p = this.position(e);
        this.send({ touch: { touch_id: e.pointerId, phase: 1, x: p.x, y: p.y } });
      } else {
        this.touchEnd(e, 3);
      }
      return;
    }
    const btn = BTN[e.button];
    if (!btn) return;
    e.preventDefault();
    if (down) {
      this.canvas.focus();
      // Locked, the pointer is already ours (and setPointerCapture throws
      // there), and the position is the host's: send only the button.
      if (!this.locked) {
        this.canvas.setPointerCapture(e.pointerId);
        this.move(e);
      }
      this.buttons.add(btn);
    } else if (!this.buttons.delete(btn)) {
      return;
    }
    this.send({ pointer_button: { button: btn, state: down ? KEY_PRESSED : KEY_RELEASED } });
  }

  touchEnd(e, phase) {
    if (e.pointerType !== "touch") return;
    const p = this.position(e);
    this.send({ touch: { touch_id: e.pointerId, phase, x: p.x, y: p.y } });
  }

  wheel(e) {
    if (!this.enabled) return;
    e.preventDefault();
    // deltaMode: pixels (about 100 a notch in Chromium), lines (3 a
    // notch), pages. The wire (gdp-spec.md §8.2) is in notches,
    // positive down and right: the DOM's deltaY and deltaX as they are.
    const scale = e.deltaMode === 0 ? 1 / 100 : e.deltaMode === 1 ? 1 / 3 : 1;
    const h = e.deltaX * scale;
    const v = e.deltaY * scale;
    if (h || v) this.send({ pointer_axis: { horizontal: h, vertical: v } });
  }

  async setCaptured(on) {
    if (on) {
      try {
        await this.canvas.requestPointerLock({ unadjustedMovement: true });
      } catch (_) {
        await this.canvas.requestPointerLock();
      }
    } else if (document.pointerLockElement) {
      document.exitPointerLock();
    }
  }
}
