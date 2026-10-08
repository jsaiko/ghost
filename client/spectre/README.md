# spectre

The GDP client. It connects to a wraith session with the token and
certificate fingerprint ghostd's lobby handed out (spectre-qt does the
lobby phase and launches this binary), decodes the H.264, H.265, AV1 or
PyroWave stream on the GPU where it can, and presents it with Vulkan,
without a copy on Linux and Windows. It runs on Linux, Windows and macOS;
the host side, wraith, is Linux only. The macOS and Windows builds are
described in [packaging/macos/README.md](../../packaging/macos/README.md)
and [packaging/windows/README.md](../../packaging/windows/README.md).

How it works is in [the spectre client](../../docs/design/spectre-client.md),
with [audio, cursor and gamepads](../../docs/design/audio-cursor-gamepad.md)
and [refinement](../../docs/design/refinement.md) for the features it
shares with wraith. Flags are in
[command line](../../docs/reference/command-line.md#spectre).

The Qt launcher, `spectre-qt` (`../spectre-qt/`), shares no source with
spectre: its login goes through libgdp's `gdp::LobbyClient`. Both
executables land in the build tree's `bin/`, where the launcher looks for
`spectre`.

## Build

Requires SDL3, FFmpeg (`libavcodec`, `libavutil`), libva and libva-drm
(Linux only), Vulkan (headers and a loader), `glslangValidator`
(`glslang-tools` on Ubuntu) and Qt6 Widgets for the launcher. On Windows
the same list comes from vcpkg, and on macOS from vcpkg plus MoltenVK.

```sh
cmake -S ../.. -B ../../build -G Ninja
cmake --build ../../build
```

## Run

```sh
SPECTRE_TOKEN=<token> ./spectre -h <host> -p <port> -P <sha256> [flags]
```

`-t <token>` works too, but puts the token where other local users can
read it. A token from ghostd works once. Or run `spectre-qt` for the
login form. For a wraith started directly with `-l` (no ghostd), the
token is the one given to wraith (reusable) and `-P` is `openssl x509
-in cert.pem -noout -fingerprint -sha256` of its certificate.

Left Ctrl + left Alt + left Super opens the session menu (`-k` picks
other keys); see [installing the client](../../docs/install/spectre.md#during-a-session).

## Code layout

| Directory | What it holds |
|---|---|
| `src/net/` | `SessionClient` (the GDP session connection), `lan_link` (the wired-link check that gates PyroWave) |
| `src/decode/` | `Decoder` (FFmpeg and its hardware backends), the PyroWave decoder, the codec-support probe |
| `src/present/` | `VulkanDevice`, `VideoImageSource`, `LosslessPlane`, `OverlayRenderer`, `VulkanPresenter` |
| `src/stream/` | `StreamSession`: the SDL3 window, event loop and input |
| `src/ui/` | The session menu, toolbar, statistics panel, toasts, font and hotkey parsing; no Vulkan |
| `src/audio/` | Opus decode, the jitter buffer, SDL3 playback, the microphone |
| `shaders/` | GLSL for the video, rectangle, text and cursor pipelines, compiled to SPIR-V at build time |
| `tools/` | `netprobe` and `netem-harness` (below) |
| `tests/` | `jitter_buffer_test`, `ui_font_test`, run by `ctest` |

`ui_font_test` pins the rasterizer (every printable glyph at four sizes)
and can dump a PGM preview. `jitter_buffer_test` pins the buffer's rules
and runs a two-thread stress pass: bursty arrivals, 43 ms main-loop
stalls, a 5 s wire gap, 2% loss and the 2^16 sequence wrap.

## Measuring without a window

`spectre_netprobe` (`tools/netprobe.cpp`) is the same `SessionClient`
with no decoder or GPU, printing one line of arrival statistics a second.
`tools/netem-harness` runs it against a real wraith across a
netem-shaped link in a network namespace (sudo for the namespace and
qdiscs only; see the script's header and
[transport and rate control](../../docs/design/transport-and-rate-control.md#testing)).
