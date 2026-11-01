# TODO

Known gaps, unverified paths and bugs, grouped by area. Design limits
that are accepted rather than open are in each design doc's
Limitations section.

## Login and sessions

- Login latency has not been measured; the target is about 2 s.
- ghostlogin ignores Plasma Login Manager (`plasmalogin`,
  `plasmalogin-autologin` aren't in `DEFAULT_SERVICES`), and outside
  Debian nothing hooks ghostlogin in: docs/install/host.md should say
  which line to add to the display manager's PAM stack.

## Veil

- Gateway sessions are untested with spectre's own feedback loop: the
  netem matrix (loss and delay on each leg, against direct), a 4K
  session's latency, settled bitrate and refinement through the gateway.
- The browser client is untested in Safari.
- The browser client has no microphone: add `getUserMedia` capture and
  Opus through WebCodecs `AudioEncoder` on datagram channel 0x03
  (gdp-spec.md §10.1), offer `microphone`, and a mute control.
- Firefox's VA-API decode ignores the H.264/H.265 crop
  ([browser client](docs/design/browser-client.md#limitations)): find
  the cause or file it upstream, then drop `video.js`'s software-decode
  workaround.
- The browser client has no FEC (gdp-spec.md §9.7, ADR 0018): a GF(2⁸)
  Reed-Solomon decoder in `video.js`'s reassembly (or WASM), parity
  datagrams on channel 0x04, and `fec` offered. Until then every lost
  frame costs it a keyframe.
- Gateway sessions with `fec` are untested: that parity datagrams fit
  the wraith leg's slices (Veil sizes it from the client leg) and that
  the netem matrix through the gateway gains as direct sessions do.
- The browser client has no file transfer (gdp-spec.md §7.12): offer
  `file`, send dropped files, and fetch the host's copied ones with a
  download button (a browser can't mount or paste them).

## wraith

- NVENC's AV1 encode (8 and 10 bits) needs an Ada or newer GPU to test.
- KDE: a popup menu (desktop right-click, Konsole's window menu)
  sometimes doesn't appear until the pointer moves over it (KWin 6.7.5,
  NVIDIA). Suspected KWin bug (a repaint dropped when no PipeWire buffer
  is free; draining every queued buffer didn't help): confirm with
  `QT_LOGGING_RULES=kwin_screencast.debug=true`, then report upstream.
- On NVIDIA, a failed GPU read-back in the ext backend retries for good
  (its host-memory fallback needs LINEAR buffers): fall back to wl_shm
  capture instead.
- Per-buffer damage tracking in the ext backend, so the compositor
  copies only what changed.
- Steam Input doesn't work in a session (no `/dev/uinput`). Plan: a
  per-session uinput stand-in from ghostseat (CUSE), bind-mounted over
  `/dev/uinput` by a `user@.service` drop-in. ghostseat creates each
  device itself (`ghost/uid-<uid>/...` phys, handed over by the udev rule,
  raw-controller key policy). Not the `input` group or a uinput ACL
  (ADR 0006).
- A preflight check that turns "this user can't open any DRM node" into a
  clear error instead of a 30 s session-start timeout.
- Copied files (gdp-spec.md §7.12): the data-control clipboard path is
  tested on labwc both ways (a headless client and wl-clipboard), not yet
  in a KDE session; on GNOME, reading a copy is tested, putting the
  client's files on the clipboard (`SetSelection` with file types) isn't.
- Shared folders (`file-share`) are untested with a real Nautilus and
  Dolphin: that the sidebar entries appear and go while they run, and how
  their thumbnailers and Baloo treat the mount. spectre `-F` (share my
  files) and the hidden-file filter are tested on Linux only: the
  Windows drive list and hidden attribute are compiled (MinGW), not run;
  macOS's `UF_HIDDEN` and `/Volumes` not even compiled.

## Client

- FEC (gdp-spec.md §9.7) has run on the netem harness only, on x86 with
  AVX2 at both ends. Untested: a real Wi-Fi or mobile link, the SSSE3
  and scalar paths outside the unit tests, NEON on an arm64 client (a
  Mac, a Raspberry Pi Wisp image), MSVC's CPUID path (not compiled yet),
  and the policy's constants (the per-profile floors, the 3x gain, 64
  slices per block) beyond the harness's four lossy profiles.

- Security keys (gdp-spec.md §8.7, ADR 0015) are tested end to end on
  Linux only (a YubiKey 5: a browser sign-in in a session on another
  host, and the key unplugged and replugged mid-session). Untested: a
  Wisp thin client (whether the kiosk user can open the key's hidraw
  node), macOS's IOHID path, two keys at once, and a session through
  Veil's gateway.
- Key maintenance by consent (ADR 0016) is unit-tested in libgdp only.
  Untested end to end: each prompt against a real key through a browser's
  security-key settings page and `fido2-token` (set and change the PIN,
  list and delete passkeys, a reset right after a replug), a passkey
  delete naming its site from a real key's listing replies, the
  keep-alives holding Chrome and libfido2 while a prompt is up, a
  CANCEL withdrawing one, and a bio key (YubiKey Bio) for enrolment.
- Smart cards (gdp-spec.md §8.8, ADR 0017) are tested end to end with a
  YubiKey 5C NFC on a Linux client shared with a Chromium-based app (so in
  shared mode): over loopback on that machine, and across the LAN to a
  CachyOS host. There: `ykman piv info`, OpenSC's `pkcs11-tool -O`
  and a `--login` with the PIN, a slot 9E signature verified against its
  certificate, `ykman piv access change-pin` approved, a write denied at
  its prompt, `-u` refusing one unasked, and the key replugged
  mid-session. Untested: exclusive mode, Firefox and `ssh -I` through
  OpenSC, GnuPG through scdaemon, two readers, extended APDUs against a
  real card, T=0 cards, a session through Veil's gateway, macOS's
  PCSC.framework and Windows' WinSCard (neither compiled), and the Wisp
  image's pcscd.
- Security keys on Windows: spectre can't open a FIDO HID device without
  elevation (Windows 10 1903 on). Plan (ADR 0015): a helper service the
  installer adds, as LocalSystem, that opens the keys and relays their
  packets to spectre over a named pipe only the session's user may open;
  check first that it coexists with Windows' own WebAuthn service.
  Until then `-U` finds keys there but can't open them.
- Raw controllers (gdp-spec.md §8.6) are untested end to end: a PS4 pad
  over Bluetooth (touchpad, motion, light bar, rumble, Steam Input), a
  Steam Controller, a Switch Pro pad (does `hid-nintendo` pass ghostd's
  key check?), the fallback on `HidRejected`, and spectre on Windows and
  macOS.
- spectre's 10-bit and BT.709 paths are untested beyond RADV: Vulkan
  Video on NVIDIA; on Windows D3D11VA's P010 input, R10G10B10A2 output
  and the software path's BT.709 coefficients; VideoToolbox's P010 copy
  on a Mac.
- spectre's file paste (gdp-spec.md §7.12) is untested in a real window:
  the SDL3 clipboard offer of the host's files (Nautilus and Dolphin
  pasting from it), the clipboard update on focus that announces a local
  copy, and a clipboard manager taking the offer over. What it is built
  on is tested end to end (`wraith_remote_files_test`,
  `wraith_client_files_test`, and a headless client against labwc).
- Pasting the host's files into Windows Explorer (virtual files,
  `host_files_clipboard_win.*`) is tested only against an in-memory host
  through `Shell.Application`'s Paste: nested and unlisted folders, an
  empty file, 5 MiB, the offer gone after teardown. Untested against a
  real host: a file over 4 GiB, pasting to the desktop, a network share,
  Outlook or a browser's upload field, cancelling in Explorer's progress
  dialog, a disconnect mid-paste, a host text copy replacing the offer, a
  Windows copy right after one, Win+V's history. Whether a read waiting
  on the host holds up other calls into the offer enough to need the
  streams on a thread of their own is unmeasured. Copying from Explorer
  to the host (`CF_HDROP`) is written but untested on Windows. The
  Finder has neither.
- A FUSE mount under `$XDG_RUNTIME_DIR` may be out of a Flatpak file
  manager's sight (the document portal would be the way in).
