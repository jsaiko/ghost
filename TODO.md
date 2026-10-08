# TODO

Known gaps, unverified paths and bugs, grouped by area. Design limits
that are accepted rather than open are in each design doc's
Limitations section.

## Login and sessions

- Login latency has not been measured; the target is about 2 s.
- ghostlogin ignores Plasma Login Manager: its PAM services,
  `plasmalogin` and `plasmalogin-autologin`, aren't in
  `DEFAULT_SERVICES`. And outside Debian nothing hooks ghostlogin in:
  docs/install/host.md should tell the admin which line to add to the
  display manager's stack (pc03 and viper, on CachyOS, have none).

## Veil

- Gateway sessions are untested with spectre's own feedback loop: the
  netem matrix (loss and delay on each leg, against direct), a 4K
  session's latency, settled bitrate and refinement through the gateway.
- The browser client is untested in Safari.
- The browser client has no microphone: it never offers the
  `microphone` capability. It needs `getUserMedia` capture, Opus through
  WebCodecs `AudioEncoder` (offered only where `isConfigSupported` says
  so), packets on datagram channel 0x03 as spectre sends them
  (gdp-spec.md §10.1), and a mute control in its toolbar. wraith's side
  is unchanged.
- Firefox's hardware decode (VA-API on Linux) ignores an H.264/H.265
  crop: a 1080-row session's 1088-row picture comes out squeezed into
  1080 rows with the padding as a black band at the bottom, and the
  VideoFrame (coded, visible and display all 1920x1080) gives the page
  nothing to undo it with. `video.js` works around it by asking Firefox
  for software decode when the size isn't a multiple of 16. Find the
  real cause (Bugzilla, `FFmpegVideoDecoder`/`DMABufSurface` on
  searchfox; try `media.ffmpeg.vaapi.enabled` and the DMA-BUF prefs) or
  file it upstream, then drop the workaround.

## wraith

- NVENC's AV1 encode needs an Ada or newer GPU to test
  ([encoding](docs/design/encoding.md#limitations)).
- KDE: a popup menu (desktop right-click, Konsole's window menu)
  sometimes doesn't appear until the pointer moves over it, with the
  mouse captured too. Seen with KWin 6.7.5 on NVIDIA. Suspected KWin
  bug, not confirmed: ScreenCastStream::record() returns when no PipeWire buffer
  is free, with the pending repaint already cleared, and never
  reschedules it, so that frame is lost until the next repaint. Taking
  every queued buffer per process callback (pipewire_capture.cpp) didn't
  cure it. Next: confirm with KWin's screencast debug logging
  (QT_LOGGING_RULES=kwin_screencast.debug=true), then report upstream.
- On NVIDIA, if the ext backend's GPU read-back ever fails, capture
  retries for good: its fallback, frames in host memory, needs LINEAR
  buffers, which NVIDIA can't allocate. Falling back to wl_shm capture
  instead, converting from whatever format the compositor offers, would
  cover it.
- The ext backend declares every pool buffer fully damaged, so the
  compositor copies the whole output each capture; per-buffer damage
  tracking would cut that to what changed.
- No audio jitter or bitrate adaptation, and the jitter buffer doesn't
  follow the network profile.
- Standard gamepads have no rumble, and their paddles, touchpad and misc
  buttons are dropped (raw controllers have all of it). Touch input is
  parsed and ignored.
- Steam Input doesn't work in a session: Steam creates its virtual pad
  through `/dev/uinput`, which a seatless session can't open ("Couldn't
  open /dev/uinput for writing" in Steam's controller log), and the pad
  would land on seat0 anyway. Plan: ghostseat serves a per-session
  uinput stand-in (CUSE), and a `user@.service` drop-in bind-mounts it
  over `/dev/uinput` (`BindPaths=-`, so a session without one is
  untouched). ghostseat creates each device Steam asks for itself, with a
  `ghost/uid-<uid>/...` phys, the udev rule and monitor hand it to the
  session, and the raw-controller key policy applies. Not the `input`
  group or a uinput ACL: those give every input device on the host
  (ADR 0006).
- A preflight check that turns "this user can't open any DRM node" into a
  clear error instead of a 30 s session-start timeout.

## Client

- Raw controllers (gdp-spec.md §8.6) are untested end to end: a PS4 pad
  over Bluetooth into a session (touchpad, motion, light bar, rumble,
  Steam Input), a Steam Controller, a Switch Pro pad (does
  `hid-nintendo` pass ghostd's key check?), the fallback on `HidRejected`,
  and spectre on Windows (hidapi's reconstructed descriptor) and macOS
  (SDL's own open of the controller).
- spectre and the browser client don't read `OutputDescriptor.color`:
  each picks its YCbCr conversion from the codec (BT.601 limited, or
  BT.709 full for PyroWave), which matches what wraith sends but not
  gdp-spec.md §7.3's "a client MUST apply `color`". Drive the sampler's
  matrix and range from `color`, and re-apply it on `DisplaysChanged`.
- The remaining client limits (Windows validation, NVIDIA Vulkan Video,
  macOS frame copy, ...) are listed in
  [the spectre client](docs/design/spectre-client.md#limitations).
