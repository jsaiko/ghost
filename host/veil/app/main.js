// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The browser client's page (docs/design/browser-client.md): signing
// in to Veil, the host list, then the session, all over veild's JSON API
// (web/portal.rs). Veil remembers who is signed in (a cookie) and, sealed,
// the password for their hosts (web/remember.rs); a host it can't use the
// remembered password for asks for one here.
import { decodableCodecs } from "./video.js";
import { openTransport } from "./transport.js";
import { Session, ERRORS } from "./session.js";

const $ = (id) => document.getElementById(id);
const views = ["login-view", "devices-view", "password-view", "prompt-view", "types-view", "settings-view", "busy-view"];
let csrf = null; // set while signed in to Veil
let flow = null;
let deviceName = "";
let pendingDevice = null;
let currentUser = ""; // who is signed in to Veil: the host login's default account
let hostUser = ""; // the account last typed for a host

// The settings page: this browser's own choices, in localStorage (which
// may be missing or refuse writes; then they last until the page closes).
const SETTINGS_KEY = "veil.settings";
const settings = { useWebSocket: false, softwareDecode: false, lossless: true };
try {
  Object.assign(settings, JSON.parse(localStorage.getItem(SETTINGS_KEY) || "{}"));
} catch (_) {}

function saveSettings() {
  try {
    localStorage.setItem(SETTINGS_KEY, JSON.stringify(settings));
  } catch (_) {}
}

for (const [id, key] of [["use-websocket", "useWebSocket"], ["software-decode", "softwareDecode"]]) {
  $(id).checked = !!settings[key];
  $(id).addEventListener("change", (e) => {
    settings[key] = e.target.checked;
    saveSettings();
  });
}

function show(view, error = "") {
  for (const v of views) $(v).hidden = v !== view;
  $("topbar").hidden = view === "login-view";
  $("tab-settings").classList.toggle("active", view === "settings-view");
  $("tab-desktops").classList.toggle("active", view !== "settings-view");
  const focus = { "login-view": "username", "password-view": "host-password", "prompt-view": "prompt-input" }[view];
  const focusView = () => focus && $(focus).focus();
  // A failed sign-in reads best under its form; once signed in, a
  // problem is a modal over whatever screen it lands on.
  const inline = view === "login-view" ? error : "";
  $("error").hidden = !inline;
  $("error").textContent = inline;
  if (error && !inline) notice(error, focusView);
  else setTimeout(focusView, 0);
}

// A modal with `text` and OK; `then` runs once it is closed.
function notice(text, then = () => {}) {
  const dialog = $("notice");
  $("notice-text").textContent = text;
  dialog.addEventListener("close", then, { once: true });
  if (!dialog.open) dialog.showModal();
  $("notice-ok").focus();
}

function busy(text) {
  $("busy-text").textContent = text;
  show("busy-view");
}

// GET when `body` is undefined, else POST. Every call after signing in
// carries the session's CSRF token.
async function api(path, body) {
  const headers = {};
  if (csrf) headers["X-Veil-CSRF"] = csrf;
  const init = { headers };
  if (body !== undefined) {
    init.method = "POST";
    headers["Content-Type"] = "application/json";
    init.body = JSON.stringify(body);
  }
  const r = await fetch(path, init);
  let json;
  try {
    json = await r.json();
  } catch (_) {
    json = { step: "error", message: `The server answered ${r.status}.` };
  }
  return json;
}

// Veil's own refusals get words of our own, as spectre-qt's do.
function errorText(step) {
  switch (step.code) {
    case 13: return "That host isn't connected to Veil right now. Try again later, or pick another host.";
    case 14: return "The host rejected your password, although Veil accepted it. Your password on that host is probably different; ask your administrator.";
    case 15: return "You aren't allowed to use that host.";
    case 11: return "You are logged in at the host itself; log out there first.";
    default: return step.message || "The login failed.";
  }
}

async function handle(step) {
  if (step.flow) flow = step.flow; // a host login in progress (prompt, session_list)
  switch (step.step) {
    case "devices":
      csrf = step.csrf;
      // Signed in at /settings (opened there, or reloaded): that page.
      if (location.pathname === "/settings") return showSettings();
      return chooseDevice(step);
    case "signed_out":
      return signedOut(csrf ? step.message : "");
    case "prompt": return prompt(step);
    case "session_list": return chooseType(step);
    case "ready": return connect(step);
    default: {
      flow = null;
      const text = errorText(step);
      // Not signed in: it is the sign-in that failed.
      if (!csrf) return show("login-view", text);
      // The host wouldn't take the credentials (or Veil didn't like the
      // username): ask again for the same host.
      if ((step.code === 21 || step.code === 20) && pendingDevice) return askPassword(pendingDevice, text);
      return backToDevices(text);
    }
  }
}

function signedOut(message = "") {
  csrf = null;
  flow = null;
  pendingDevice = null;
  show("login-view", message);
}

// The host list again, freshly fetched (a host's state may have changed),
// with `error` if one is to be shown.
async function backToDevices(error = "") {
  flow = null;
  pendingDevice = null;
  const step = await api("/api/session");
  if (step.step !== "devices") return handle(step);
  csrf = step.csrf;
  chooseDevice(step, error);
}

$("login-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  busy("Signing in…");
  const password = $("password").value;
  $("password").value = "";
  handle(await api("/api/login", { username: $("username").value, password }));
});

$("devices-back").addEventListener("click", async () => {
  document.activeElement.blur(); // or the user menu stays open on the next sign-in
  await api("/api/logout", {});
  signedOut();
});

$("password-cancel").addEventListener("click", () => backToDevices());
$("types-back").addEventListener("click", () => backToDevices());
$("prompt-back").addEventListener("click", () => backToDevices());

// The top bar's tabs, each with its own address (/ and /settings) so
// reload, Back and Forward keep the page. Leaving a host login half done
// abandons it.
function showSettings() {
  flow = null;
  pendingDevice = null;
  show("settings-view");
}

function go(path) {
  if (location.pathname !== path) history.pushState(null, "", path);
}

$("tab-desktops").addEventListener("click", (e) => {
  e.preventDefault();
  go("/");
  backToDevices();
});
$("tab-settings").addEventListener("click", (e) => {
  e.preventDefault();
  go("/settings");
  showSettings();
});
// Back/Forward between the tabs. Not signed in, the sign-in form stays;
// in a session, the session does.
window.addEventListener("popstate", () => {
  if (!csrf || !$("session-view").hidden) return;
  if (location.pathname === "/settings") showSettings();
  else backToDevices();
});

$("password-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const password = $("host-password").value;
  $("host-password").value = "";
  deviceName = pendingDevice.name;
  hostUser = $("host-username").value.trim() || currentUser;
  busy(`Connecting to ${deviceName}…`);
  handle(await api("/api/connect", { device_id: pendingDevice.id, username: hostUser, password }));
});

function choice(list, label, note, enabled, onPick) {
  const li = document.createElement("li");
  const b = document.createElement("button");
  b.type = "button";
  b.disabled = !enabled;
  const name = document.createElement("span");
  name.textContent = label;
  b.append(name);
  if (note) {
    const n = document.createElement("span");
    n.className = "note";
    n.textContent = note;
    b.append(n);
  }
  b.addEventListener("click", onPick);
  li.append(b);
  list.append(li);
}

function chooseDevice(step, error = "") {
  const list = $("device-list");
  list.replaceChildren();
  currentUser = step.user;
  $("whoami").textContent = step.user;
  $("admin-link").hidden = !step.admin;
  for (const d of step.devices) {
    const note = !d.online ? "offline" : d.has_session ? "session running" : "";
    tile(list, d.name, note, d.online, () => (d.remembered ? connectRemembered(d) : askPassword(d)));
  }
  $("no-devices").hidden = step.devices.length > 0;
  show("devices-view", error);
}

// A host as a tile: its icon, its name, and a note under it.
function tile(list, label, note, enabled, onPick) {
  const li = document.createElement("li");
  const b = document.createElement("button");
  b.type = "button";
  b.disabled = !enabled;
  const icon = document.createElement("img");
  icon.src = "/app/display-icon.png";
  icon.alt = "";
  const name = document.createElement("span");
  name.className = "tile-name";
  name.textContent = label;
  b.append(icon, name);
  if (note) {
    const n = document.createElement("span");
    n.className = "note";
    n.textContent = note;
    b.append(n);
  }
  b.addEventListener("click", onPick);
  li.append(b);
  list.append(li);
}

// Logs in to `device` as the signed-in user with the password Veil
// remembers. If the host refuses it, Veil won't offer it there again and
// the refusal lands in askPassword.
async function connectRemembered(device) {
  pendingDevice = device;
  deviceName = device.name;
  busy(`Connecting to ${deviceName}…`);
  handle(await api("/api/connect", { device_id: device.id }));
}

// A host Veil has no usable password for asks for one; it goes with the
// connect call and is not kept.
function askPassword(device, error = "") {
  pendingDevice = device;
  $("password-host").textContent = device.name;
  // The user's own account unless they changed it for this host.
  $("host-username").value = hostUser || currentUser;
  $("host-password").value = "";
  show("password-view", error);
}

function prompt(step) {
  $("prompt-host").textContent = deviceName;
  $("prompt-text").textContent = step.prompt || "Response:";
  $("prompt-input").type = step.echo ? "text" : "password";
  $("prompt-input").value = "";
  show("prompt-view");
}

$("prompt-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  const response = $("prompt-input").value;
  $("prompt-input").value = "";
  busy("Checking…");
  handle(await api(`/api/flows/${flow}/answer`, { response }));
});

async function chooseType(step) {
  const open = async (session_type, text) => {
    busy(text);
    handle(await api(`/api/flows/${flow}/open`, { session_type }));
  };
  // One session per user: a running one is resumed whatever the type.
  if (step.running.length > 0) return open(step.running[0].session_type, `Resuming your session on ${deviceName}…`);
  if (step.types.length <= 1) return open("", `Starting your session on ${deviceName}…`);
  $("types-host").textContent = deviceName;
  const list = $("type-list");
  list.replaceChildren();
  for (const t of step.types) {
    choice(list, t.name, t.id === step.default_type ? "default" : "", true, () => open(t.id, `Starting your session on ${deviceName}…`));
  }
  show("types-view");
}

async function connect(step) {
  flow = null;
  busy(`Opening the session on ${step.device}…`);
  const codecs = await decodableCodecs(new URLSearchParams(location.search).get("codec"));
  if (codecs.length === 0) {
    backToDevices("This browser can't decode video with WebCodecs. Try a current Chrome, Edge or Firefox.");
    return;
  }
  let transport;
  try {
    transport = await openTransport(step, settings.useWebSocket ? "websocket" : "");
  } catch (e) {
    backToDevices(`Couldn't reach the session: ${e.message || e}`);
    return;
  }
  runSession(transport, step, codecs);
}

// In fullscreen the toolbar is an auto-hiding panel over the top of the
// picture, as spectre's: it slides in when the pointer reaches the top
// edge with no button held (a drag that started on the remote stays
// there), never while the mouse is captured, and hides again once the
// pointer is below it. Windowed, it is the docked bar above the stage.
const REVEAL_EDGE_PX = 2;
{
  const toolbar = $("toolbar");
  const hide = () => toolbar.classList.remove("revealed");
  $("session-view").addEventListener("pointermove", (e) => {
    if (!document.fullscreenElement) return;
    if (document.pointerLockElement) return hide();
    if (toolbar.classList.contains("revealed")) {
      if (e.clientY > toolbar.offsetHeight) hide();
    } else if (e.clientY <= REVEAL_EDGE_PX && e.buttons === 0 && e.pointerType === "mouse") {
      toolbar.classList.add("revealed");
    }
  });
  document.addEventListener("fullscreenchange", hide);
  document.addEventListener("pointerlockchange", () => document.pointerLockElement && hide());
}

function runSession(transport, step, codecs) {
  document.getElementById("portal").hidden = true;
  $("topbar").hidden = true;
  const view = $("session-view");
  view.hidden = false;
  $("btn-lossless").hidden = true; // until the session says "refine" is in effect
  const canvas = $("screen");
  const status = $("session-status");
  $("session-title").textContent = `${step.device} · ${transport.kind === "webtransport" ? "WebTransport" : "WebSocket"}`;
  const session = new Session(transport, {
    token: step.token,
    canvas,
    overlay: $("overlay"),
    cursor: $("cursor"),
    codecs,
    softwareDecode: !!settings.softwareDecode,
    // The Lossless button's last choice in this browser.
    lossless: settings.lossless !== false,
    onLossless: (on) => {
      const b = $("btn-lossless");
      b.hidden = false;
      b.textContent = on ? "Lossless: On" : "Lossless: Off";
      b.setAttribute("aria-pressed", on ? "true" : "false");
    },
    onStatus: (text) => (status.textContent = text),
    onStats: (s) => {
      $("stats").textContent =
        `${s.codec} (${s.encoder}) over ${s.kind}\n` +
        `${s.mbps.toFixed(1)} Mbit/s, ${s.lost} lost of the last 64\n` +
        `rtt ${s.rttMs.toFixed(1)} ms, decode ${s.decodeMs === null ? "-" : s.decodeMs.toFixed(1)} ms`;
    },
    onEnd: (code) => {
      if (document.fullscreenElement) document.exitFullscreen();
      view.hidden = true;
      document.getElementById("portal").hidden = false;
      // Back to the host list, not the sign-in: Veil still knows who you are.
      // A disconnect the user asked for needs no message.
      backToDevices(code === 0 ? "" : ERRORS[code] || `The session closed (code ${code}).`);
    },
  });
  const menu = (name) => {
    switch (name) {
      case "fullscreen":
        if (document.fullscreenElement) document.exitFullscreen();
        else view.requestFullscreen().then(() => navigator.keyboard && navigator.keyboard.lock().catch(() => {}));
        break;
      case "capture":
        session.input && session.input.setCaptured(!session.input.locked);
        break;
      case "lossless":
        settings.lossless = !session.lossless;
        saveSettings();
        session.setLossless(settings.lossless);
        break;
      case "stats":
        $("stats").hidden = !$("stats").hidden;
        break;
      case "disconnect":
        session.end(0);
        break;
    }
  };
  session.onMenu = menu;
  $("btn-fullscreen").onclick = () => menu("fullscreen");
  $("btn-capture").onclick = () => menu("capture");
  $("btn-lossless").onclick = () => menu("lossless");
  $("btn-stats").onclick = () => menu("stats");
  $("btn-disconnect").onclick = () => menu("disconnect");
  $("btn-logout").onclick = () => {
    if (confirm("End the remote session? This logs out of the remote desktop; unsaved work there may be lost.")) session.logout();
  };
  // The canvas keeps the video's aspect ratio inside the stage, so its
  // box is exactly the video (input.js maps pointer positions over it).
  const stage = $("stage");
  const fit = () => {
    const w = canvas.width || 16;
    const h = canvas.height || 9;
    const scale = Math.min(stage.clientWidth / w, stage.clientHeight / h);
    canvas.style.width = `${Math.floor(w * scale)}px`;
    canvas.style.height = `${Math.floor(h * scale)}px`;
    // The refine overlay sits exactly over the video.
    const overlay = $("overlay");
    overlay.style.width = canvas.style.width;
    overlay.style.height = canvas.style.height;
    session.drawCursor();
  };
  new ResizeObserver(fit).observe(stage);
  session.onResize = fit;
  // Browsers start audio only after a gesture in the page.
  const resume = () => session.audio && session.audio.resume();
  view.addEventListener("pointerdown", resume);
  window.addEventListener("keydown", resume);
  canvas.focus();
  session.start();
  window.gdpSession = session; // for debugging from the console
}

// Signed in already (the cookie), or the sign-in form. Nothing shows
// until the answer, so neither flashes up before the other.
api("/api/session").then(handle);
