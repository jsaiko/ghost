# Audio, microphone, cursor and gamepads

Code: `host/wraith/src/audio/`, `host/wraith/src/session/gamepad_*`,
`host/ghostseat/src/devices.rs`, and the cursor handling in each capture
backend.

## Audio

A PipeWire virtual sink (`Audio/Sink`, `PipewireAudioCapture`) is the
session's audio output. It runs continuously once opened, on its own
PipeWire thread at a 10 ms cadence, whether or not a client is attached;
`AudioPipeline` encodes Opus (48 kHz stereo, 10 ms frames) only while a
viewer is there, and sends each frame as one audio datagram
(gdp-spec.md §10), ahead of any queued video. The format is advertised
in `SessionAccept.audio`; a host where capture didn't open still serves
video, and the client stays silent.

spectre decodes through a jitter buffer (60 ms target, Opus packet-loss
concealment for a missing frame, the oldest frame dropped on overrun,
re-buffering on a real underrun) and plays through SDL3, pulled from
SDL's audio thread rather than paced from the main loop, which blocks in
vsync'd presentation for most of a frame. Its toolbar and session menu
mute session audio on the client alone: frames are still pulled through
the jitter buffer, so unmuting plays what is current, but silence goes to
the device. The host doesn't know.

wraith runs in the user's systemd manager and connects to that user's
PipeWire, so each session has a graph of its own (one session per user).
The sink is `gdp-audio` ("Ghost remote audio" in device pickers). wraith
writes it as `default.configured.audio.sink` in the session's `default`
metadata (`DefaultNodeClaim`, shared with the microphone), and gives it
`priority.session` 10000, above any hardware sink. The claim makes it
the default at session start. WirePlumber often starts with the session
and restores its saved defaults just after wraith's write, so for 5 s
the claim writes again whenever another value lands; after that a
user's or app's choice stands. The priority keeps it the default when an
app clears the configured default, as Steam's Big Picture does a few
minutes in: WirePlumber then picks the highest-priority sink, which
would otherwise be the host's own sound card. The name carries no pid
(one session per user), so WirePlumber's saved defaults don't gain a
dead entry per session.

## Microphone

Behind the `microphone` capability (gdp-spec.md §10.1). spectre (`-M`)
sends Opus packets on datagram channel 0x03, and its toolbar and session
menu mute it (the device stays open; nothing is encoded or sent while
muted).
`MicrophoneSource` decodes them into a PipeWire virtual source, an
ordinary `Audio/Source`, so the session's apps record from it like a
hardware microphone. Its ring is
the jitter buffer: playback waits for a few frames before starting, a
gap is concealed with Opus PLC, and an underrun goes silent and
re-primes. The source is `gdp-mic` ("Ghost remote microphone"). wraith
also writes it as `default.configured.audio.source` in the session's
`default` metadata, with priority 11000: WirePlumber weighs the sink's
monitor as a source at the sink's priority, and the mic must beat it.
Otherwise WirePlumber picks the sink's monitor or a hardware microphone
as the default source, and apps would record the desktop's own audio or
the host's room.

## Cursor

wraith never draws the pointer into the video. The compositor's cursor
metadata (`SPA_META_Cursor` on GNOME and KDE, the cursor session on ext)
drives `CursorShape`, sent only when the shape changes; a 0x0 shape
hides the pointer. In absolute mode spectre knows where the pointer is,
so wraith sends no position; in relative mode (the mouse captured)
wraith sends `CursorPosition`, coalesced per input batch and only when
it changed.
spectre draws the shape itself.

`SPA_META_Cursor` is allocated only where both ends' `SPA_PARAM_Meta`
requests intersect. mutter offers a fixed `CURSOR_META_SIZE(384, 384)`,
and asking for a different fixed size silently drops the metadata (kwin
offers a range), so wraith asks for a range, which matches both.

## Gamepads

A controller plugged into the spectre machine becomes a device in the
session that games open through SDL, udev and hidraw like a physical
one. It lives below the compositor, so unlike keyboard and pointer
injection it is one implementation for every backend. There are two
ways in:

- **Raw** (`hid`, gdp-spec.md §8.6), the default where it works: the
  controller's own HID interface is recreated on the host through
  `uhid`, with its real identity and descriptor, and the host's own
  kernel driver for it binds. A PS4 pad is a PS4 pad in the session,
  with touchpad, motion sensors, light bar and rumble, and Steam Input
  sees the real controller (a Steam Controller included).
- **Standard** (`gamepad`, §8.5), the fallback: SDL's normalized state,
  presented as a virtual Xbox 360-layout pad through `uinput`. Used for
  controllers spectre can't open raw (an Xbox pad on USB has no HID
  interface; a hidraw node the client user can't open), on hosts without
  `uhid`, for anything the host refuses, and by the browser client.

**Wire** (gdp-spec.md §8.5): `GamepadConnect{pad_index, name}` and
`GamepadDisconnect{pad_index}` bracket a controller's life in one of four
slots, and `GamepadState` carries its complete state, every axis and
button in SDL3's order (`libgdp/include/gdp/gamepad.hpp`). A lost or
reordered message can't leave a button stuck: wraith diffs each state
against what it last wrote (`session/gamepad_evdev.cpp`). spectre sends
one snapshot per slot per main-loop turn. SDL normalizes every
controller to that layout, so the client side is the same on every
platform.

**Devices.** wraith runs as the session user and the session has no
seat, so it can neither open `/dev/uinput` (root's, and deliberately not
opened up to a group, since it is the right to create any input device,
keyboards included) nor reach the `/dev/input/eventN` a uinput device
becomes. By default udev tags a joystick `uaccess`, a uinput device lands
on seat0, and logind gives it to whoever is at the host's console, who
also sees a phantom controller. So:

- **ghostseat creates the device.** wraith sends `CreateDevice` on its
  control socket (a fresh connection per request). ghostseat, the
  session's root process, sets up the uinput device with `phys`
  `ghost/uid-<uid>/pad<n>`, waits for udev to process it, chowns every
  node it produces (the evdev `eventN` and the legacy joystick `jsN`) to
  the session's uid with mode 0600, and replies `DeviceCreated` with an
  fd as `SCM_RIGHTS` on the same `sendmsg`. wraith only writes
  `input_event`s to that fd (`session/gamepad_devices.cpp`); closing it
  destroys the device.
- **wraith never holds the device's own fd.** That fd is the device's
  control handle: `UI_DEV_DESTROY` and a new `UI_DEV_SETUP` would make
  it a keyboard, and the console's keyboard and SysRq handlers take
  input from every device whatever its seat. What wraith gets is one
  end of a `SOCK_SEQPACKET` socket pair; ghostseat keeps the device fd
  and relays each packet after checking it (`devices.rs`): whole
  `input_event`s, each a `SYN_REPORT`, one of the pad's buttons with
  value 0 or 1, or one of its axes within range. A batch with anything
  else is dropped. wraith closing its end ends the relay, which drops
  the device fd.
- **One udev rule, with no uid in it**,
  `packaging/system/udev/72-ghost-input.rules`: `ATTRS{phys}=="ghost/*"`
  → `ID_SEAT=ghost`, `TAG-="uaccess"`, `TAG-="seat"`, `MODE="0600"`. It
  only takes ghost's devices off seat0; ownership is ghostseat's chown.
  Numbered 72 to land after `70-uaccess` and before `73-seat-late`. The
  chown must follow udev's processing (the node exists before udevd sees
  it, and udevd applies `MODE` on the add), so ghostseat listens to
  udev's monitor and checks each event for `ID_SEAT=ghost`.
- **The capability is honest.** ghostseat checks for `/dev/uinput` and
  the rule at session start and reports it in
  `SessionInit.uinput_available`;
  wraith offers `gamepad` only when it is true. A missing rule means no
  gamepads, never pads leaking to the console.

Each session gets exactly its own devices, the console sees none of
them, and there is no per-user setup. A client's pads are unplugged when
it disconnects. spectre skips any local device whose `phys` (or, for a
HID device, `HID_PHYS`) starts with `ghost/`, so running it on the host
itself against the same account doesn't forward the virtual pad back.

### Raw controllers

**spectre.** SDL's gamepad events stay the hotplug source. When a
controller appears and the session negotiated `hid`, spectre looks for
its HID interface: the joystick's path when SDL's own HIDAPI driver
handles it, or on Linux the `hidraw` node beside its `eventN` in sysfs.
If it can open that with `SDL_hid_open_path()` and the controller is on
USB or Bluetooth, it reads the descriptor (`SDL_hid_get_report_descriptor`)
and identity and sends `HidConnect`; it never opens the controller
through `SDL_OpenGamepad()`, which would set lights and modes of SDL's
own. Otherwise, or when the host answers `HidRejected`, it forwards the
same controller as a standard gamepad. On a Linux client that takes read
and write access to the controller's hidraw node, which udev gives only
when something tags it: Steam's rules do for the common controllers, and
the Wisp image carries its own (`70-wisp-controllers.rules`, Sony,
Nintendo, Valve, Microsoft and 8BitDo). A reader thread per raw
controller sends each input report as it arrives, straight onto the
input stream (libgdp's stream send is thread-safe), so the main loop's
pacing adds nothing; a second thread per controller performs the host's
output, get-report and set-report requests, which block on the
controller for a USB control transfer or a Bluetooth round trip, and
sends the replies. spectre -G turns forwarding on as before; spectre -g
does too but starts with every controller on the standard path. Both
offer `hid`, and the session menu's Controllers row (Raw / Xbox
Emulation) switches mid-session by unplugging every forwarded controller
on the host and plugging it in again the other way: the escape hatch for
a game that only understands an Xbox pad, or Steam Input until the host
has a per-session `/dev/uinput`. A raw
controller raises no SDL gamepad events, so for spectre's hold on the
screen saver its input reports are the activity: the display stays awake
while one is on.

**ghostseat** creates the device (`CreateDevice`, `KIND_HID`): it checks
the bus (USB or Bluetooth only), the descriptor's size and the strings,
opens `/dev/uhid`, writes `UHID_CREATE2` with the client's name,
descriptor, IDs, version and `uniq`, and a `phys` of
`ghost/uid-<uid>/pad<n>/<seq>` (the sequence number keeps a replaced
slot's device apart from its successor while ghostseat still checks
it), and replies `DeviceCreated` at once with wraith's end of a socket
pair, relayed to the device as for a uinput pad. It has to:
`UHID_CREATE2` returns in a few milliseconds and the driver probes
afterwards, and a probe asks for reports (a PS4 pad's calibration and
firmware version) that only wraith can answer, through the fd, by
asking the client. The relay lets through `UHID_GET_REPORT_REPLY` and
`UHID_SET_REPORT_REPLY` from the start and `UHID_INPUT2` only after the
verdict, each checked for size; the driver's own events come back the
other way, one per packet. ghostseat keeps a duplicate of the device fd
and the connection, and waits up to 10 seconds for the device's `bind`
uevent. Then it checks:

- the driver is `hid-generic` or on the allowlist (`playstation`,
  `sony`, `steam`, `nintendo`, `microsoft`);
- every input device under it reports only `EV_SYN`, `EV_KEY`, `EV_ABS`,
  `EV_MSC`, `EV_FF` and `EV_LED`, and keys only in the gamepad, joystick,
  touch and digitizer button ranges (0x100 to 0x15f), the d-pad buttons
  (0x220 to 0x223) and `BTN_TRIGGER_HAPPY` (0x2c0 to 0x2e7).

Pass, and it sends `DeviceVerified` and lets input reports through.
Fail (or no bind in time), and it writes `UHID_DESTROY` through the
duplicate, which ends the relay and removes the device under wraith
too, and sends a `ControlError`.
Keys below `BTN_MISC` are what the console's keyboard handler and SysRq
attach to; `KEY_POWER` and friends, switches and relative motion are
what logind and pointer stacks act on. Checking the result rather than
the descriptor catches what a descriptor can't show: a driver chosen by
vendor and product ID creating devices of its own. A driver that
registers input devices later (`hid-steam` re-registers its gamepad
whenever Steam closes the hidraw node) is on the allowlist and creates
only gamepad devices.

**wraith** (`session/raw_controllers.cpp`) relays: `HidInput` becomes
`UHID_INPUT2`, held back until `DeviceVerified`; `UHID_OUTPUT`,
`UHID_GET_REPORT` and `UHID_SET_REPORT` read from the fd become
`HidOutput`, `HidGetReport` and `HidSetReport`, and the client's replies
go back as `UHID_GET_REPORT_REPLY` and `UHID_SET_REPORT_REPLY`. wraith
numbers requests itself, never reusing a number for the life of the
session, and maps a reply back to the device and the kernel's ID, so a
late reply can't land on a replacement device. A `ControlError` from
ghostseat, or a failed create, becomes `HidRejected`.

**Node ownership.** A uhid controller has more nodes than a uinput pad
(a PS4 pad: gamepad, motion-sensor and touchpad `eventN`, `jsN`, and
`hidrawN`) and some arrive later. So each ghostseat listens to udev's
monitor (netlink, after rules have run) instead of waiting per device:
on every `add` of an `input` or `hidraw` node whose input device `phys`
or whose HID parent's `HID_PHYS` is `ghost/uid-<uid>/...` for its own
uid, it chowns the node to that uid, mode 0600, and warns if the event
lacks `ID_SEAT=ghost` (the rule didn't run). Only root can create uinput
and uhid devices, so the uid in `phys` is ghostseat's own word. uinput
pads use the same path; there is no per-device wait any more.

**The udev rule** gains a second line. A uhid device's input devices
have an empty `phys` (`hid-playstation` doesn't copy it), so
`ATTRS{phys}` can't see them; `ATTRS{uevent}=="*HID_PHYS=ghost/*"`
matches the HID parent's uevent for its `input` and `hidraw` children
alike. Both lines also drop the `power-switch` tag. Without the rule a
uhid pad's `eventN` and `hidrawN` land on seat0 with `uaccess` (Steam's
`60-steam-input.rules` tags Sony and Valve hidraw nodes), which is why
ghostseat reports `uhid_available` only when the rule is installed.

## Limitations

- Standard pads have no rumble (the reverse path from the uinput fd's
  `EV_FF` to `SDL_RumbleGamepad`), and no paddles, touchpad or misc
  buttons: the Xbox 360 layout has no codes for them. Raw controllers
  have all of it.
- A raw controller's reports ride the input stream, so a lost packet
  holds them for a retransmission, as it does keys and pointer motion.
- A raw controller's battery is a `power_supply` the whole host sees:
  UPower shows it to the console's user too.
- A host with the same controller connected locally refuses it raw:
  `hid-playstation` won't bind two devices with one address.
- spectre on Windows gets the descriptor from hidapi's reconstruction
  from Windows' parsed form, not the device's own bytes; on macOS,
  whether SDL's own open of a controller leaves room for spectre's is
  unverified. Both fall back to standard.
- The control socket admits any process of the session's user, and that
  is the boundary (login-and-sessions.md, "The control socket"): a
  compromised wraith, or an SSH shell as the user, can create pads in
  that user's own session, at most four of each kind, and nothing else.
