# 0006. ghostseat creates virtual gamepads

Status: Accepted (amended 2026-10-07: the device fd stays in ghostseat)

## Context

Forwarded controllers become uinput devices in the session. wraith runs
as the session user in a seatless session: it can't open `/dev/uinput`
(root's), and udev's defaults tag a joystick `uaccess`, put a uinput
device on seat0, and give it to whoever sits at the console, who also
sees a phantom controller, while the session user gets `EACCES`.

A uinput or uhid fd is the device's control handle, not just an event
sink: its holder can destroy the device and set it up again as a
keyboard, and the console's keyboard and SysRq handlers take input from
every input device whatever seat udev put it on. Handing wraith that fd
(the first version of this decision) gave any process of the session's
user keystrokes on the host console.

## Decision

ghostseat, the session's root process, creates each device on wraith's
request, with a `phys` of `ghost/uid-<uid>/pad<n>`, and keeps the
device fd. wraith gets one end of a `SOCK_SEQPACKET` socket pair over
the control socket (`SCM_RIGHTS`); ghostseat relays each packet to the
device after checking it is something the pad was set up to report, and
relays a uhid driver's events back. Closing wraith's end destroys the
device. One udev rule with no uid in it moves any `ghost/*` device off
seat0, and ghostseat chowns the device nodes to the session's user after
udev has processed them. wraith offers the `gamepad` capability only
when ghostseat reports uinput and the rule are present.

ghostseat holds at most four devices of each kind per session, by slot:
a request for an occupied slot destroys the device in it first.

## Consequences

- Each session gets exactly its own devices; the console sees none.
- No per-user setup, and no group to manage.
- The control socket admits any process of the user, and that is the
  boundary: a compromised wraith, or anything else running as the user,
  can produce gamepad input in that user's own session and nothing
  else.
- Every event crosses one more process. A batch is one packet, checked
  in a few comparisons.

## Rejected alternatives

- **A shared `ghost` group with access to `/dev/uinput`** (the original
  design). Gives every user every other user's pads, and any user
  process the ability to put a virtual keyboard on the console.
- **Handing wraith the device fd** (the first version of this decision).
  See Context.
- **Authenticating wraith on the control socket.** Any secret wraith
  could present comes from somewhere the user controls, and a process of
  the user can stop and replace wraith through a user-level drop-in;
  within one uid there is no boundary to enforce.
