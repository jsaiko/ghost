# Capture backends

How wraith gets frames out of a desktop and input into it. Code:
`host/wraith/src/screencast/`, with the shared session layer in
`host/wraith/src/session/`.

## wraith attaches, it never composites

Every session type runs a real compositor top-level, on a virtual
output, as the session leader. wraith is that compositor's client: it
captures its output, injects input into it, and syncs its clipboard. A
desktop therefore runs as it does on a physical login, and pointer lock
for games, the desktop's own window management and Xwayland all work
without wraith reimplementing them. The three desktops (GNOME, Plasma,
LXQt) run their compositor unmodified; the single-app types
(`terminal`, `steam`) run labwc with a config of ghost's own.

The extra hop costs little: compared with the compositor of wraith's own
it once had (on the same wlroots), labwc over `screencast-ext` ran at the same frame
rate and CPU, with about 1 ms more latency from commit to encoded bytes
with VA-API and about 3 ms at 4K with x264 (labwc's blit into wraith's
buffers plus wraith's read-back).

## The session layer

`session/session_host.hpp` defines `SessionHost` and `InputSink`: the
event loop, output size, `redeliver_frame()`, the render DRM fd, input
injection, logout, and access to `SessionServices`. `GdpSession` and
`SessionServices` depend only on that interface, so the GDP, encode,
audio and clipboard paths are the same whichever compositor is
underneath.

`ScreencastHost` (`screencast/screencast_host.cpp`) is the `SessionHost`
for every backend. It owns the session leader, a retry loop that opens
capture and input once the compositor is up (every 250 ms for up to
30 s, then teardown), the encoder, audio, ghostd reporting, logout and
teardown. It names no protocol; it holds three interfaces:

- **`RemoteSession`** (`remote_session.hpp`): compositor-specific setup,
  and factories for the other two.
- **`FrameSource`** (`frame_source.hpp`): open, close and release, plus
  the frame and cursor callbacks. `FrameHold` keeps exactly the newest
  frame so `redeliver_frame()` can re-encode it: every compositor's
  stream is damage-driven, and an idle desktop would otherwise send a
  newly attached client nothing.
- **`ScreencastInput`**: an `InputSink` plus its connection lifecycle.

When the viewer detaches, `ScreencastHost` closes the `FrameSource` and
nothing else, so the compositor stops copying its output for nobody;
the `RemoteSession`, input and the compositor's output (GNOME's virtual
monitor) stay. The next attach makes a new `FrameSource`, whose first
frame carries the attach's keyframe: mutter, KWin and an ext compositor
all send a full frame to a new capture within a few ms, idle desktop or
not. Capture runs from session start until the first detach, and
throughout while `-e` dumps the stream.

`WaylandClient` (`wayland_client.cpp`) is the shared "wraith as a Wayland
client" piece: the connection on the host's event loop, the registry,
binding by interface with a version cap.

Video runs single-threaded on wraith's `wl_event_loop`, with PipeWire's
loop fd added to it. A captured buffer goes to the encoder and isn't
returned until the encoder is done with it; with one thread that is an
ordering rule rather than a cross-thread protocol. Audio has its own
PipeWire thread.

## The three backends

| | `screencast-gnome` | `screencast-kwin` | `screencast-ext` |
|---|---|---|---|
| Compositor | `gnome-shell --headless` | `kwin_wayland --virtual` | any exporting `ext_image_copy_capture_v1` |
| Setup | mutter's private D-Bus API | Wayland client + kwin's EIS D-Bus method | Wayland client only |
| Frames | PipeWire (`PipeWireCapture`) | PipeWire (`PipeWireCapture`) | `ExtImageCopyCapture`, into wraith's own buffers |
| Input | libei (`EiInput`) | libei (`EiInput`) | `VirtualInput`: `zwlr_virtual_pointer_v1` + `zwp_virtual_keyboard_v1` |
| Cursor | `SPA_META_Cursor` | `SPA_META_Cursor` | `ext_image_copy_capture_cursor_session_v1` |
| Clipboard | mutter's `RemoteDesktop` clipboard methods | `ext-data-control-v1` | `ext-data-control-v1` |
| Profiles | `gnome` | `plasma` | `terminal`, `steam`, `lxqt` |

Captured buffers are dmabufs wherever the compositor offers them. When
the encoder needs CPU pixels (x264, or lossless refinement on a CPU
path), wraith reads the dmabuf back itself on its render node
(`DmabufReader`), rather than asking the compositor for memfd frames,
which would put the read-back inside the compositor's own frame. Captured
buffers come from the compositor's GPU, so on a host where it runs on a
different GPU from wraith's render node, they cross devices.
`DmabufReader` waits, bounded, for the buffer's pending write fence
before each import: NVIDIA's GL doesn't wait on a dmabuf's fences by
itself, so it could otherwise read the buffer's previous contents.

`PipeWireCapture` takes every buffer queued since its last process
callback, not one per call. Cursor-only updates go back once their cursor
metadata is read; of the video frames only the newest is delivered, and
the older ones go back at once with their damage merged into it. A
compositor holding only a few buffers is then never left without a free
one while wraith catches up.

### GNOME

`GnomeRemoteSession` holds one sd-bus connection for the session's life
and makes the calls in the order mutter requires:
`RemoteDesktop.CreateSession` → its `SessionId` →
`ScreenCast.CreateSession({'remote-desktop-session-id': ...})` →
`RecordVirtual({'cursor-mode': 2})` → subscribe to `PipeWireStreamAdded`
→ `RemoteDesktop.Session.Start()`.

- The subscription must come before `Start()`: a missed signal isn't
  replayed. `Start()` on the remote-desktop session starts both halves.
- `ConnectToEIS` on the same session object yields the libei fd. mutter
  honours input only once the linked stream is being consumed, so
  capture must be live first. The `NotifyPointer*`/`NotifyKeyboard*`
  methods are accepted but never reach a client's seat; libei is the
  only input path.
- There is no portal, consent dialog or allowlist: mutter's private API
  trusts any session-bus peer.
- The launcher must not pass `--virtual-monitor`: `RecordVirtual` creates
  its own monitor, and a second one would take the panel. It leaves off
  `--no-x11`, so mutter runs Xwayland on demand as a GDM login does.
- mutter runs headless by itself when the user's logind session has no
  seat, which a ghost session never has, so the stock
  `org.gnome.Shell@.service` is used unchanged.

### KDE Plasma

`KwinRemoteSession` binds kwin's privileged
`zkde_screencast_unstable_v1` (v6, vendored in
`host/wraith/third_party/kde-protocol/`) and calls `stream_output` on the
first `wl_output` with cursor metadata; `created(node)` is the PipeWire
node.

- **Permission.** kwin hides privileged globals unless the client's
  executable has a desktop file whose `Exec=` resolves to its
  `/proc/<pid>/exe` and whose `X-KDE-Wayland-Interfaces=` lists the
  interface. `packaging/desktop/wraith.desktop.in`, installed in
  `share/applications`, lists `zkde_screencast_unstable_v1`,
  `kde_output_management_v2`, `kde_output_device_v2` and
  `kde_output_device_registry_v2`. `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1` is a
  development bypass, never shipped.
- **Input** is `org.kde.KWin.EIS.RemoteDesktop.connectToEIS(3)` on the
  session bus, `disconnect(cookie)` on close.
- **`--virtual`.** `startplasma-wayland` always starts kwin itself
  through `plasma-kwin_wayland.service` and forwards no arguments, and
  kwin's DRM backend fails without a seat. So an installed drop-in
  (`packaging/system/systemd/plasma-kwin_wayland.service.d/`) runs the
  stock command plus `$XDG_GHOST_KWIN_ARGS`, which the `plasma-screencast`
  launcher sets to `--virtual`. startplasma copies its environment into
  the user manager before starting kwin, and systemd expands an unset
  variable to nothing, so every other login runs the stock command. The
  `XDG_` prefix matters: startplasma removes from the user manager every
  `XDG_*` variable its own process doesn't have, so a console login that
  takes the user manager over from a ghost session can't inherit
  `--virtual` and start headless.
- **Size.** `KwinRemoteSession::open()` sets the virtual output's size
  over `kde_output_management_v2` before `stream_output`
  (`kwin_output_size.cpp`: a one-entry custom mode list, then a mode
  switch, as `kscreen-doctor` does). The output to size is found through
  `kde_output_device_registry_v2` (v21) when kwin offers it, as 6.7 does
  instead of a `kde_output_device_v2` global per output, and through
  those globals on 6.6. kwin saves the mode in
  `~/.config/kwinoutputconfig.json` under the virtual output's own
  entry, which no physical monitor matches.
- The first `stream_output` right after kwin starts usually times out
  before kwin's PipeWire connection is up; the next retry succeeds.

### ext

The standard capture protocol (wayland-protocols staging), implemented
by wlroots 0.19, Smithay-based compositors, Hyprland and others, but not
by kwin or mutter, so it doesn't replace the other two. wraith owns the
buffers: the compositor advertises a buffer size, a dmabuf device and
formats; wraith allocates a three-deep `XRGB8888` pool on that device
through gbm, imports each with `zwp_linux_dmabuf_v1`, and runs
`create_frame` → `attach_buffer` → `damage_buffer` → `capture` →
`ready`, with a shared-memory fallback. The next frame is requested
before the ready one is handed to the encoder, so a slow encode doesn't
make every request miss the output's next refresh (the pool keeps a
buffer free for it). Against a compositor missing a global, `open()`
fails naming it.

- **Modifiers.** wraith picks the pool's modifier and prefers LINEAR.
  NVIDIA can't render to LINEAR (its GBM refuses the allocation, and its
  EGL calls a LINEAR import external-only), so where no LINEAR buffer
  comes out, wraith allocates from the modifiers both the compositor and
  the encoder import. A consumer that wants host memory needs LINEAR and
  gets no such fallback.
- **NVIDIA.** wlroots' GLES2 renderer submits the capture copy with no
  fence and relies on implicit sync, which NVIDIA's driver doesn't do, so
  frames arrive before the copy into them has finished. The labwc
  launchers set `WLR_RENDERER=vulkan` when `nvidia_drm` is loaded and
  `WLR_RENDERER` is unset; that renderer attaches a sync file to the
  buffer, which wraith's copies wait on.
- **Cursor.** The cursor session opens when the seat gains a pointer.
  wlroots offers it one shm format, its renderer's read-back format:
  `ARGB8888`, or on NVIDIA `ABGR8888` or an X variant whose X byte still
  carries alpha. wraith takes all four, swapping red and blue for the
  `*BGR` ones, and logs once if the offer is none of them.

The backend is named after the protocol family, not wlroots: the input
half (wlroots' virtual pointer and keyboard) is the less portable part,
and is a separate class so an EIS sink could replace it.

## Output size

Every `RemoteSession` resizes its compositor's output when it opens
(`sizes_output_on_open()`): GNOME through `RecordVirtual`, which sizes
its monitor from the stream; KDE through `kde_output_management_v2`; ext
through `zwlr_output_manager_v1` on the first head.

A client whose `SessionHello.displays` asks for a different size gets
capture and input closed and reopened at that size against the same
running compositor, on every connect. A `ResolutionChange` mid-session
does the same, reopens the encoder at the new size, and answers with
`DisplaysChanged` before the first frame (a keyframe). The reopen takes
about 10 ms; the desktop sees a mode change. A compositor that can't
resize keeps its size, the request is declined, and `SessionAccept`
reports the real size. Nothing relaunches the leader to change size.

## Limitations

- No Hyprland session type (removed with Hyprland 0.56.2). Aquamarine
  gets a GPU allocator only from its DRM backend, which needs a logind
  seat (`AQ_DRM_DEVICES` takes KMS cards, not a render node), so a
  headless-only start aborts with "no allocator available". Nested in a
  headless labwc it gave no frames: on NVIDIA, Aquamarine asked for
  LINEAR `XRGB8888`, which NVIDIA's GBM can't allocate; on AMD its
  toplevel never got past the first commit. It could come back if
  Aquamarine's headless backend gains a render-node allocator, or with a
  vkms-backed seat of its own.
- The ext backend declares every pool buffer fully damaged, so the
  compositor copies the whole output on every capture.
- KDE on a host with no GPU needs vgem loaded by the admin
  ([GPU-less hosts](../install/gpu-less-hosts.md)).
