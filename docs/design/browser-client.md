# Browser client

veild serves a browser client at `https://<veil>/`: the page and its
scripts (`host/veil/app/`, plain ES modules compiled into veild, no build
step), a JSON login API (`host/veil/src/web/portal.rs`), and the session
over WebTransport or a WebSocket (gdp-spec.md §15). A browser can't speak
GDP's QUIC, so its sessions always go through Veil's
[gateway](veil.md#the-gateway), whatever the device's mode.

## Signing in to Veil

`POST /api/login` runs PAM against `/etc/pam.d/veild` with the lobby's
penalties. A successful sign-in is a row in `portal_sessions` and a
`veil_session` cookie (random id, only its hash stored; `Secure`,
`HttpOnly`, `SameSite=Strict`, `Path=/`). It lasts until
`[web] portal_idle` (12 h without a request) or `portal_max` (7 days),
sign-out (`POST /api/logout`), or an administrator clearing it.
`GET /api/session` returns who is signed in, their devices and the
session's CSRF token, which every state-changing call carries in
`X-Veil-CSRF`. The admin UI shares the same sign-in.

## The remembered password

So that picking a host needs no second password while the host still
runs its own PAM and ghostseat still unlocks the wallet, the password is
split between veild and the browser (`web/remember.rs`):

- At sign-in veild seals it with ChaCha20-Poly1305 under a fresh random
  key and keeps only the ciphertext, in memory, under the portal
  session. The key goes to the browser in a `veil_key` cookie (`Secure`,
  `HttpOnly`, `SameSite=Strict`, `Path=/api`); veild keeps no copy.
- `/api/connect` brings the two together for one host login and wipes
  the result.
- Neither half alone gives the password back: a core dump, swap or VM
  snapshot of veild holds only ciphertext, the database holds nothing,
  and the cookie holds no ciphertext.
- It is bound to the address that signed in; a request from any other
  address drops it.
- It ends at sign-out, a new sign-in, a veild restart, or after
  `[web] remember_password` (12 h, at most `portal_max`; 0 turns it
  off).

The cost: a stolen pair of cookies, used from the same address, can log
in to the user's hosts without learning the password, where a stolen
`veil_session` alone only lists them.

## Signing in to a host

`POST /api/connect {device_id, username?, password?}` checks the
signed-in user's entitlement and opens the brokered login on the host's
channel (`relay.rs`, shared with the QUIC lobby). With no password and
the user's own account, the remembered password answers the host's
first prompt; otherwise the typed one does. Either is wiped there.

- `/api/flows/{id}/answer` and `/api/flows/{id}/open` advance the login
  through further prompts and the session type. A flow is in memory
  only, lasts at most five minutes, and only its user can advance it.
- A host that refuses the remembered password (`HOST_AUTH_FAILED`) is
  never offered it again in that session, so retries can't trip a
  host-side lockout, and the refusal costs no penalty. A typed password
  the host refuses counts against the client's address.
- A finished login gets a gateway token and session URLs built from the
  `Host` the page was loaded from.

The device list marks a device `remembered` when the remembered password
can be used for it; the page then connects straight away.

## Transport

- **WebTransport** is served by `wtransport` on the web port over UDP,
  with the web certificate, so the browser needs no
  `serverCertificateHashes` and never sees wraith's certificates. The
  session's first bidi stream is the control stream, the second the
  input stream, and datagrams carry video and audio. WebTransport can't
  carry the GDP close code, so the gateway first sends it on a
  unidirectional stream: `GDPCLOSE` and the code as a u32 LE.
- **WebSocket** (`/gdp/ws`, `host/veil/src/ws.rs`) is the fallback. Every
  message is one channel byte (0 control, 1 input, 2 a datagram toward
  the browser) and that channel's bytes. Datagrams that find the send
  queue full are dropped, so latency stays bounded. A close carries 4000
  plus the GDP code.

Both become a client leg of the gateway (`gateway.rs`), beside spectre's
QUIC leg. The WebTransport leg's datagram size comes from its own path
MTU, less HTTP/3's quarter-stream-id; the WebSocket leg lets wraith use
the full cap.

## The page

| File | What it does |
|---|---|
| `main.js` | Sign-in, the host list (tiles; offline hosts dimmed), password prompts, the session view, and a Settings tab whose choices stay in this browser's `localStorage` (`veil.settings`) |
| `transport.js` | WebTransport and WebSocket behind one interface: two byte streams, datagrams, a close code |
| `session.js` | spectre's `SessionClient` again: `SessionHello`, control messages, reassembly, and a `StatsReport` every 250 ms with the same loss mask, delay and frame-train figures, so wraith's rate control works as for spectre |
| `proto.js` | A schema-driven protobuf codec for the `session.proto` messages it uses (64-bit fields as BigInts), checked against prost by `gateway.rs`'s tests |
| `video.js` | WebCodecs: offers only codecs `VideoDecoder.isConfigSupported` accepts, configures on the first keyframe, drops deltas until then and reports them lost, draws only the newest frame |
| `refine.js` | The `"refine"` capability: tiles and clears drawn on a transparent canvas over the video, each layer applied in frame order (vendored `fzstd.js` for Zstd) |
| `audio.js`, `audio-worklet.js` | Opus through WebCodecs into a jitter buffer in an AudioWorklet (40 ms target, the oldest dropped past 150 ms) |
| `input.js`, `keymap.js` | `KeyboardEvent.code` to HID usages, pointer positions over the video, the wheel in notches, touch, Pointer Lock for mouse capture, Keyboard Lock in fullscreen. Ctrl+Alt+Shift+F/G/S/Q are the page's own (fullscreen, capture mouse, stats, disconnect), and Scroll Lock captures or releases the mouse as in spectre; neither reaches the host. In fullscreen a tapped Esc goes to the host and only a held Esc leaves fullscreen (Keyboard Lock) |
| `gamepad.js` | The Gamepad API's standard mapping reordered to SDL's order, up to four pads, sent on change |
| `log.js` | The page's warnings and events, to the console and into a ring of the last 300 lines for diagnostics |

Diagnostics (gdp-spec.md §7.11): when the user runs `wraith --report`,
`session.js` answers `DiagnosticsRequest` as spectre does, at most once
every 30 s and noting each request in its log. The report holds the user
agent, the transport, the codecs offered and chosen, the WebCodecs
configuration (codec string, `hardwareAcceleration` preference, decoder
state, frames shown, decode errors), the GPU as WebGL names it, and the
log ring. WebCodecs never says whether decode actually runs on the GPU;
the decode time and errors are the evidence. The GPU name is read only
here, since it is a fingerprinting surface; browsers may mask it.

The session's toolbar (stats, Lossless when `refine` is in effect,
capture mouse, fullscreen, End Session, disconnect) is docked above the
picture in a window. Lossless sends `RefinePause` and is remembered in
the browser's settings, on until turned off. In fullscreen it is
an auto-hiding panel over the top of the picture, as spectre's: the
pointer at the top edge with no button held slides it in, it hides once
the pointer is below it, and it never appears while the mouse is
captured. With no mouse (touch) there is no way to reveal it. The page
shows no hint of its own on capturing the mouse: Chrome's pointer-lock
and fullscreen notices say how to get out.

Clipboard text: the host's goes to `navigator.clipboard` when the page
has focus, and the browser's is read and sent when the page gains focus,
never echoing back what the other side just sent.

RTT comes from WebTransport's `getStats()`, or from `Ping`/`Pong` over a
WebSocket. The browser measures the whole path itself and ignores
`GatewayPath`.

## Limitations

- Firefox's hardware decoder ignores an H.264/H.265 stream's crop: the
  macroblock-padded picture (1088 rows for 1080) is scaled into the
  display size, with a band of padding at the bottom, and the frame says
  nothing of it. `video.js` therefore asks Firefox for software decode
  whenever the session's width or height isn't a multiple of 16
  (switching at the keyframe after a resolution change); hardware decode
  is kept for aligned sizes and for AV1, which the page crops itself.
- No images on the clipboard.
- Tried in Chrome and Firefox; Safari is untested.
- A self-signed web certificate needs a browser exception, and Chrome
  then refuses WebTransport, so the page falls back to the WebSocket.
