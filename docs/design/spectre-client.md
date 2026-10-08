# The spectre client

Code: `client/spectre/` (the stream client), `client/spectre-qt/` (the
login and connect launcher), `libgdp/` (the protocol both link).
spectre runs on Linux, Windows and macOS; wraith is Linux only.

spectre-qt does the lobby phase through libgdp's `LobbyClient`, picks or
resumes a session, pins the login server's certificate, and starts
`spectre` as a child process with the session's address and certificate
fingerprint, and the token in its environment (`SPECTRE_TOKEN`).
`spectre` connects to wraith, decodes the video on the GPU where it can
and presents it with Vulkan. The two share no
source; both executables land in the build tree's `bin/` so the launcher
finds `spectre` beside itself.

## Network side

`src/net/session_client.*` is the session connection from the client's
end, mirroring wraith's `gdp_session`. It opens the control and input
streams, does the `SessionHello`/`SessionAccept` handshake, reassembles
video datagram slices into coded frames, and tracks a sliding per-stream
ack/loss bitmap, decode and present timings, received bytes and each
frame's first-slice one-way delay. Those go to the host as a
`StatsReport` every 250 ms, which drives wraith's rate controller
([transport and rate control](transport-and-rate-control.md)).

spectre refuses a session host whose certificate doesn't match the `-P`
fingerprint, before it sends the token (gdp-spec.md §2.3). spectre-qt
pins login servers on first use in `~/.config/spectre/known_hosts`;
deleting a host's line makes it ask again. A certificate a trusted CA
issued for the name typed needs no prompt or pin: the platform's CAs
count, and so does any CA in `~/.config/spectre/ca-certificates.pem`
([trust](trust.md)).

## Decoding

`src/decode/decoder.*` is FFmpeg decode of the negotiated codec (h264,
h265, av1; PyroWave has its own decoder, [PyroWave](pyrowave.md)).
Hardware paths keep the frame on the GPU:

| Platform | Native backend | Frame stays in |
|---|---|---|
| Linux | VA-API | a `VASurface` |
| Windows | D3D11VA | an `ID3D11Texture2D` |
| macOS | VideoToolbox | a `CVPixelBuffer` (copied, see below) |

Vulkan Video is the other hardware backend, wherever the GPU has a decode
queue (not MoltenVK): FFmpeg decodes on the presenter's own `VkDevice`
and frames arrive as `VkImage`s. It needs FFmpeg's Vulkan hwcontext at
build time and compiles out without it (`SPECTRE_VULKAN_DECODE`).

`-X` picks the backend to try first, Vulkan Video unless told otherwise;
the other hardware one is next. NVIDIA on Linux has no VA-API and never
tries it. A Vulkan attempt is skipped when the GPU's decode queue doesn't
list the codec. On Linux a V4L2 memory-to-memory decoder (FFmpeg's
`h264_v4l2m2m`, e.g. a Raspberry Pi's `bcm2835-codec`) comes before
software; its frames upload like software ones, and `decode()` waits up
to 100 ms for each packet's own picture. Software decode (`-X software`)
is the last resort, for h264 only.

h265 and av1 are hardware only: in software they cost far more CPU per
frame than a stream is worth. spectre offers them only where one of the
GPU's decoders takes them and never falls back to software; with no
hardware decoder for the stream, the session shows no video. av1 always
opens FFmpeg's own `av1` decoder: FFmpeg prefers libdav1d where it has
it, which takes no hwaccel.

The decoder always runs on the presenter's GPU: Vulkan Video on its own
device, VA-API on the DRM render node that picked it, D3D11VA on the
adapter LUID the Vulkan device reports (a decoded texture can only be
shared within one adapter). The fallback catches open-time failures and a
hwaccel FFmpeg can't set up for the stream at its first keyframe
(unsupported profile, level or size). A hardware decoder that opens and
then fails frame by frame isn't recovered into software.

Validated: Vulkan Video on RADV (h264, h265 and av1 bit-identical to the
VA-API path, refinement included) and on NVIDIA (h264, h265 and av1; an
RTX 3070 Ti under Linux, which also decodes PyroWave at 60 fps, and an
RTX 4070 under Windows), and D3D11VA in live sessions on Windows.

## Presentation

`src/present/` is split four ways. `VulkanDevice` owns the instance,
surface, physical device (pinned to the decoder's DRM render node on
Linux), queue, swapchain and the single command buffer and semaphore pair
of the one-frame-in-flight present loop. `VideoImageSource` turns a
decoded `AVFrame` into a sampled view. `OverlayRenderer` draws spectre's
own UI and the cursor. `VulkanPresenter` is the façade the stream
session talks to.

Each platform lands every decode path on one image format, so there is
one sampler and one pipeline:

- **Linux** stays planar: `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` with a
  `VK_KHR_sampler_ycbcr_conversion` sampler doing NV12 to RGB in sampling
  hardware. VA-API frames import as dmabufs
  (`VK_EXT_image_drm_format_modifier`, no copy) after a
  `vaSyncSurface()`. A surface whose two planes share one dmabuf (Intel's
  iHD, usually AMD) is bound once as an ordinary image, the plane layouts
  carrying the offsets; only planes in separate objects are bound
  disjoint. Bound twice at offset 0, ANV reads the chroma as zeros (a
  green picture). FFmpeg returns a VA-API frame when the
  decode is submitted, and on amdgpu there is no implicit dma-buf sync
  between radeonsi and RADV in one process, so without the wait the
  shader reads the surface's previous contents. Any dmabuf import must
  synchronize with its producer explicitly. Software frames upload into a
  host-visible staging image, interleaving YUV420P's U and V planes into
  NV12.
- **Windows** is `VK_FORMAT_B8G8R8A8_UNORM` with a plain sampler. A planar
  texture can't be shared out of D3D11 correctly: D3D11 packs NV12's
  chroma plane right after the luma rows, Vulkan pads the luma height
  first, so the import reads chroma from the wrong offset, and the driver
  reports `DEDICATED_ONLY`, which rules out binding planes at explicit
  offsets. So the D3D11VA path converts each frame NV12 to BGRA with
  `ID3D11VideoProcessor` into one shareable single-plane texture, imported
  once (`VK_KHR_external_memory_win32`) and reused. A `D3D11_QUERY_EVENT`
  plays `vaSyncSurface()`'s role. That image lives in
  `VK_IMAGE_LAYOUT_GENERAL`, transitioned once while empty; leaving
  `UNDEFINED` every frame would license the driver to discard what D3D11
  wrote. Software frames convert to BGRA on the CPU.
- **macOS** is NV12 with the Linux ycbcr sampler on MoltenVK. A
  VideoToolbox frame is copied, not imported: MoltenVK 1.4.2 can't import
  CoreVideo's NV12 IOSurfaces. The copy is one pass over the frame
  (shared memory).
- **Vulkan Video** has nothing to import. `acquire()` makes a
  sampling-only view of FFmpeg's `VkImage` (FFmpeg's storage and decode
  usages can't be on a ycbcr view) and follows FFmpeg's `AVVkFrame`
  protocol on the GPU: the present submit waits on the frame's timeline
  semaphore and signals one higher, the acquire barrier transitions from
  the layout FFmpeg left (on AMD the output image is a reference picture,
  so its contents must survive), and `mark_submitted()` writes the new
  layout and value back before unlocking. The image is the codec's coded
  size, so the presenter crops it with the `uvScale` push constant AV1
  uses.

The overlay is a `UiDrawList` of rectangles and text runs drawn with a
solid-rect pipeline and a font-atlas text pipeline (a Latin-1 subset of
DejaVu Sans embedded in the binary, rasterized by `stb_truetype.h` at 14
px times the display scale), plus the cursor: a
`VK_FORMAT_B8G8R8A8_UNORM` texture uploaded from wraith's `CursorShape`
and alpha-blended each frame. The cursor is drawn at the bitmap's own
size times the window's stretch of the video, times `-c`
([audio, cursor and gamepads](audio-cursor-gamepad.md#cursor)). The
shaders are compiled to SPIR-V at build time and embedded.

## Session window and input

`StreamSession` is the SDL3 window and event loop that ties network,
decode, presentation and audio together and captures input.

- **Mouse modes.** Absolute (1:1 with the window) by default. Scroll Lock
  captures the mouse: relative motion for mouselook in games and 3D
  views, the cursor drawn where wraith's `CursorPosition` says.
- **Fullscreen.** Ctrl+Shift+F11 toggles it (not Ctrl+Alt+Fn, which the
  Linux VT switcher takes below any application). Fullscreen grabs the
  keyboard so Alt+F4, Alt+Tab and Super act on the remote desktop.
  `-K` is kiosk mode: fullscreen for good, no minimize, a window the
  desktop takes out of fullscreen goes straight back. The toolbar's close
  and the menu's Disconnect stay, for a frozen link: spectre closes and
  the session keeps running.
- **Session menu.** The `-k` chord (default left Ctrl + left Alt + left
  Super) opens it, and while it is up no input reaches the remote.
  Opening it releases every key forwarded as pressed, so none stays
  stuck. Its main page runs in groups set apart by gaps. Audio: a row of
  mute glyphs, session audio and microphone (whichever are open;
  Left/Right choose, Enter toggles, the menu stays open), and volume
  (the local output's, when wpctl answers); the speaker glyph mutes the
  session audio spectre plays, not the machine's output. Display:
  fullscreen (not in kiosk mode), the view (fit or actual size), the
  resolution page (`ResolutionChange`, answered by `DisplaysChanged`)
  Lossless Refinement, which pauses or resumes the layer
  (`RefinePause`, gdp-spec.md §7.8) and is remembered like the view, and
  statistics. Input: capture mouse, mouse sensitivity, and the
  controllers' mode, raw or Xbox emulation, when the session can take
  raw controllers ([gamepads](audio-cursor-gamepad.md#raw-controllers)).
  Last come Disconnect, which
  closes spectre and leaves the session running, and End session, which
  sends `LogoutRequest` (gdp-spec.md §7.10) behind a confirmation; the
  session is over when wraith closes with `SESSION_ENDED`, shown as
  "logged out". The toolbar offers the mutes (as glyphs), capture mouse,
  fullscreen (not in kiosk mode) and End session as buttons. The statistics panel shows
  resolution and codec, encoder and decoder backends (labelled hardware
  or software), decoded fps, decode and present time, QUIC RTT, frames
  lost of the trailing 64, video Mb/s and a latency bar (green at 0 ms
  to red at 100 ms of decode plus present).
- **Redraws.** UI-only changes re-present the last frame, coalesced to
  one per main-loop iteration: a present blocks on vsync, so one per
  mouse-motion event would back the event queue up into pointer lag.
- **Gamepads** (`-G`) up to four. A standard pad sends one snapshot per
  slot per loop turn; a raw one (`input/raw_controller.cpp`) sends each
  input report from its own reader thread as it arrives
  ([raw controllers](audio-cursor-gamepad.md#raw-controllers)).

### Window size

spectre snaps the window to the remote size (`snap_window()`): one remote
pixel per screen pixel below the docked toolbar, or the largest window
that fits the display's usable area at the same aspect. It snaps when the
window opens, after a resolution pick, on leaving fullscreen and on the
toolbar's native-size icon. Fullscreen stays fullscreen when the
resolution changes; the picture scales with bars where the aspect
differs.

`-A` makes the remote resolution follow the window. Once the video area
has stayed at a new size for 400 ms, spectre sends `ResolutionChange`
rounded down to even, one request at a time, never re-asking for a size
the host refused. Only a change the user makes counts: the remembered
area resets as the window first settles, after spectre's own snaps and
after a menu pick, so a resolution picked at launch or from the menu
stands until the user resizes the window. Off by default.

The actual-size view (`-V actual`) draws the picture 1:1, centred when
smaller than the window. A bigger one hangs off the edges and pans: with
the pointer in a band about 24 px wide inside an overhanging edge the
view scrolls, faster the deeper the pointer, and with the mouse
captured it follows the remote cursor. The pan moves the video, the refinement
plane, the cursor, the tile outlines and the pointer mapping together.
`-A` sends nothing in this view.

## Audio

[Audio, cursor and gamepads](audio-cursor-gamepad.md#audio) describes the
wire. On the client, `src/audio/` decodes Opus through a jitter buffer
(6 frames, 60 ms target) and plays through SDL3's
`SDL_OpenAudioDeviceStream`, pulled from SDL's audio thread, not paced
from the main loop, which blocks in the vsync'd `present()` for most of
every video frame. When the buffer runs completely empty it stops,
re-buffers and re-anchors the expected sequence number on the next
arrival. That is also what makes a long silence at the source safe:
wraith's sequence counter freezes while it has nothing to send, and
concealing ahead would leave every later real packet looking already
played. The format comes from `SessionAccept.audio`; with none, the
client stays silent.

## Limitations

- **Windows picks `devices[0]`** as the Vulkan device and points the
  decoder at that adapter. If that is a software Vulkan implementation,
  decode silently falls back to software.
- **Windows pays an NV12 to BGRA conversion** per frame and moves 4 bytes
  per pixel through the share rather than 1.5. It is fixed-function
  hardware, so the cost is small, but it comes from the planar-sharing
  problem above. The software fallback's YUV420P to BGRA is a scalar
  loop.
- **macOS copies every VideoToolbox frame** on the CPU. A GPU-side
  alternative that works with MoltenVK as it is: blit the IOSurface's
  planes, as Metal textures, into the staging image
  (`vkExportMetalObjectsEXT`). Not done on macOS either: bundling dylibs
  and MoltenVK into `spectre-qt.app`, signing and notarization. Cmd is
  forwarded as Super.
- **One frame in flight.** `present()` waits for the queue after every
  submit: simple and correct, but it caps throughput below a pipelined
  design. If that is revisited, the CPU-side `vaSyncSurface()` (free
  today) and the D3D11 query wait become stalls to replace with a GPU
  wait: export a sync file from the dmabuf
  (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`) and import it as a
  `VK_KHR_external_semaphore_fd`, or on Windows share an `ID3D11Fence`.
- **Software upload is a scalar loop.** The YUV420P to NV12 interleave
  isn't SIMD; a real cost if software decode becomes the norm.
- **The statistics overlay is client-side numbers only.** There is no
  clock correlation with wraith, so nothing measures network plus encode
  end to end, and there is no A/V sync (audio is paced by the device
  clock alone).
- **The menu can't open before the first frame** has been presented:
  there is no UI-on-blank-frame path.
- **The client uses `/dev/dri/renderD128`** (`kDrmRenderNode`), the first
  GPU. A GPU-less client falls back to lavapipe and software decode; a
  client with several GPUs can't choose.
- **Display hotplug isn't handled.**
- **No rumble for standard gamepads, and no touch input.** (Raw
  controllers have rumble.) No adaptive jitter sizing or audio bitrate
  negotiation.
- **spectre-qt finds `spectre` only beside its own binary**, with no
  PATH lookup.
