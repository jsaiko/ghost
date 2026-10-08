# wraith

GDP's host-side session agent, one per logged-in user. wraith does not
draw a desktop. It starts a session type's compositor headless on a
virtual output (GNOME, KDE Plasma, labwc, ...) and attaches to it as a
client. From there it:

- **captures** the compositor's output (PipeWire for GNOME and KDE, the
  standard `ext-image-copy-capture-v1` protocol for wlroots-family
  compositors);
- **encodes** it: H.264/H.265/AV1 on VA-API or NVENC, an x264 fallback
  for H.264, PyroWave on wired LANs, and the optional lossless
  refinement layer;
- **carries** the session over GDP via libgdp: video and audio
  datagrams, the control and input streams, rate control and loss
  repair;
- **feeds the client back in**: keyboard and pointer into the compositor
  (libei, or the wlroots virtual pointer/keyboard), gamepads as uinput
  devices, the microphone as a PipeWire source, text clipboard both
  ways, and the compositor's cursor out as metadata rather than pixels;
- **owns the session's lifetime**: the leader process, logout, and
  reporting to ghostd when the desktop ends.

How it works is in [docs/design/](../../docs/design/):
[capture backends](../../docs/design/capture-backends.md),
[encoding](../../docs/design/encoding.md),
[PyroWave](../../docs/design/pyrowave.md),
[refinement](../../docs/design/refinement.md),
[transport and rate control](../../docs/design/transport-and-rate-control.md),
[clipboard](../../docs/design/clipboard.md),
[audio, cursor and gamepads](../../docs/design/audio-cursor-gamepad.md),
and [login and sessions](../../docs/design/login-and-sessions.md) for
how ghostd starts it. Its settings file is described in
[configuration](../../docs/reference/configuration.md#wraithtoml).

## Build

Requires wayland-protocols, wayland-client, wayland-server (only for its
event loop), xkbcommon, libdrm, gbm, EGL +
GLESv2, Vulkan, libva + libva-drm, libei-1.0, libsystemd, PipeWire, Opus,
x264, tomlplusplus, protobuf, OpenSSL, and libgdp (built alongside as a
sibling CMake target; see `../../libgdp/README.md` for its dependencies).
PyroWave is optional (`packaging/build-pyrowave.sh`). On Ubuntu,
`libei-dev` and `libeis-dev` need their own `apt install`, and
`libsystemd-dev` may too.

```sh
cmake -S ../.. -B ../../build -G Ninja
cmake --build ../../build
```

The Wayland protocols the backends speak as a client are generated with
`wayland-scanner`; the wlroots-family ones (virtual pointer and
keyboard, output management) are vendored under
`third_party/wlr-protocol/`, the KDE ones under `third_party/kde-protocol/`.

## Run

Deployed, `wraith.service` runs `wraith -G <socket>` and ghostd names the
session type. Standalone:

```sh
./wraith -p terminal -o 1920x1080 -l 4433 -t secret -c cert.pem -k key.pem
```

- `-p type-id`: session type to launch, resolved from
  `sessions.d/<type-id>.conf` ([session profiles](../../docs/reference/session-profiles.md)).
  With `-G`, overrides the type ghostd sent. wraith exits once the
  session leader exits.
- `-d dir`: a datadir to search for `sessions.d/*.conf` before the
  compiled-in one (default order: ghostd's `sessions.dir` when run with `-G`,
  `/etc/ghost/sessions.d`, then `$PREFIX/share/ghost/sessions.d`). Under
  `-G` a profile must be root-owned and not group- or world-writable.
- `-o WIDTHxHEIGHT`: output size until a client asks for its own
  (default 1920x1080).
- `-G socket`: ghostd-mediated mode, the deployed path: read
  `SessionInit` from ghostd's control socket, bind a port in its range,
  verify spectre's session token and present a throwaway certificate
  ghostd vouches for.
- `-l port -t token -c cert.pem -k key.pem`: direct-connect mode for
  local testing: listen on `port` with that certificate, and require
  `SessionHello.token` to equal `token` exactly. No ghostd, so no
  gamepads. `host/veil/examples/gdp_probe.rs --direct` is a headless
  client for it.
- `-e path.h264`: also append the encoded stream to this file (Annex B),
  so `ffprobe`/`ffmpeg` can check it with no client attached.
- `-b bitrate-bps`: bitrate ceiling, overriding `[encode]
  max_bitrate_mbps`.
- `-F`: force the software (x264) encoder, as `[encode] force_software`
  does.
- `-C file`: read this settings file instead of
  `/etc/ghost/wraith.toml`. `--check-config [file]` validates one and
  prints the settings a session would run with.
- `--session-ended socket`: report the session as over; run by
  `wraith.service`'s `ExecStopPost=`, never by hand.
- `--report [directory]`: run inside the remote desktop. Writes
  `ghost-report-<date>-<time>.tar.gz` (default: the home directory) with
  the session's negotiation, the attached client's own report
  ([gdp-spec.md §7.11](../../docs/spec/gdp-spec.md#711-diagnostics)),
  wraith's journal, the settings file, and GPU and system information.
  Nothing is sent anywhere; ghostd's and Veil's logs are root's and are
  not included.

To test a session type headlessly as an ordinary user, give the run a
private, short `XDG_RUNTIME_DIR` (`mktemp -d /tmp/...`, `chmod 700`).
Otherwise the ext backend can attach to a stray `wayland-0` left by
another run. Keep the path short: a Wayland socket path must fit in
108 bytes.

## Code layout

| Directory | What it holds |
|---|---|
| `src/main.cpp` | Flags, the `-G` handshake, choosing the host for the profile's backend |
| `src/session/` | The backend-independent session: `SessionHost` (the interface every host implements), `SessionServices` (encoder, audio, clipboard, idle pump), `GdpSession` (the GDP connection), the rate controller and path-rate estimator, the leader process, profiles, tokens, the session certificate, ghostd's control socket, gamepads |
| `src/screencast/` | `ScreencastHost` and the three backends (GNOME, KWin, ext): PipeWire and ext capture, libei and virtual-pointer input, dmabuf read-back and GPU tile hashing, clipboard mechanisms |
| `src/encode/` | The `Encoder` interface and its backends: `vaapi/`, `nvenc/`, `pyrowave/`, `software/` (x264), and `refine/` (lossless refinement) |
| `src/audio/` | The session's PipeWire sink and Opus encode, and the microphone source |
| `src/util/` | Logging, wraith.toml, render-node choice, small event-loop helpers |
| `tests/` | Unit tests, run by `ctest` |

## Tests

`ctest` in the build directory runs the unit tests in `tests/`: the
rate controller, path-rate estimator, tile tracker, refinement encoder,
frame hold, token, session certificate, profiles, config, HID and
gamepad translation, the dmabuf fd hand-off, the XRGB conversion, the
GPU tile hash and PyroWave's input path.

Rate control is exercised against shaped links by
`client/spectre/tools/netem-harness`, which runs a real wraith against
a headless client across netem
([transport and rate control](../../docs/design/transport-and-rate-control.md#testing)).
It needs labwc, mpv and root for the network namespace, so it
isn't part of `ctest`.
