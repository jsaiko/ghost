# 0014. Controllers pass through as raw HID

Status: Accepted (2026-10-06)

## Context

Forwarded controllers became a virtual Xbox 360 pad whatever they were
(0006): SDL normalizes the controller on the client, and ghostseat creates
a uinput device with Microsoft's IDs. A PS4 pad showed up as an Xbox
controller, and its touchpad, motion sensors, light bar and rumble were
lost; a Steam Controller was a plain pad that Steam Input couldn't
recognise. Passing only the controller's IDs through would not fix it:
SDL and Steam on the host map buttons by IDs, against the codes the
real kernel driver produces, so Sony's IDs on Xbox codes would scramble
the mapping.

## Decision

A controller is forwarded as its raw HID interface where it can be. spectre
opens it through SDL's HID API and sends its identity and report
descriptor (gdp-spec.md §8.6, capability `hid`); ghostseat creates a
`uhid` device with them, so the host's own driver for that controller
binds; wraith relays input reports one way and output, get-report and
set-report requests the other. The uinput Xbox pad stays as the
fallback for everything that can't go raw.

ghostseat screens the descriptor (no keyboard, consumer or system-control
usages) and hands wraith the relayed socket before the driver binds (the
probe needs wraith to answer it), with input reports held back; then it
checks what the driver created: an allowlisted driver, and input devices
with gamepad codes only. A device that fails is destroyed through
ghostseat's duplicate of the fd. ghostseat chowns device nodes from a
udev monitor rather than per device, because uhid drivers add nodes
after creation.

## Consequences

- Controllers keep their identity and every feature their host driver
  supports, with no per-controller code in ghost.
- The client can plug HID hardware into the host, bounded by ghostseat's
  checks: a driver allowlist, and no key below `BTN_MISC`, no relative
  motion, switches or sounds.
- Raw reports ride the reliable input stream, a few hundred to a
  thousand small messages a second per controller.
- Hosts need `/dev/uhid` and the updated udev rule; without them every
  controller takes the standard path.

## Rejected alternatives

- **Pass the vendor and product IDs through on the uinput pad.** Maps
  buttons wrongly wherever SDL's or Steam's database has the real
  controller, and still loses everything beyond the Xbox layout.
- **A uinput layout per controller family** (Sony, Nintendo, Xbox).
  Fixes button labels only; touchpad, motion, lights and rumble need a
  driver, and Steam Input needs hidraw.
- **Predicting the driver from `modules.alias` before creating the
  device.** Built-in drivers are missing from it on kernels without
  `modules.builtin.alias`, and it says nothing about what the driver
  will create.
- **A new stream per raw controller** (gdp-spec.md §2.2). Isolates one
  controller's retransmissions from other input, at the cost of the
  stream machinery neither end has yet; the input stream is already
  reliable and ordered.
