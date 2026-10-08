# 0001. wraith attaches to a real compositor

Status: Accepted

## Context

A remote session needs a desktop to capture. wraith could be that
desktop's compositor (it had a wlroots compositor of its own), host a
desktop's compositor nested inside its own, or be a client of a real
compositor running on a virtual output. Users expect their own desktop:
its shell, panels, settings and window management, Xwayland, and games
that lock the pointer.

## Decision

Every session type runs a real compositor top-level on a virtual output
as the session leader, and wraith is its client: it captures frames
(PipeWire or `ext-image-copy-capture-v1`), injects input (libei or the
wlroots virtual pointer and keyboard) and syncs the clipboard. One
backend per capture mechanism: `screencast-gnome`, `screencast-kwin`,
`screencast-ext`. wraith's own compositor stayed as a development harness
(`Backend=native`), used by no shipped profile, until it was removed on
2026-10-05: its wlroots 0.19 build dependency had become a burden on new
builds.

## Consequences

- Desktops behave as on a physical login, with nothing reimplemented.
- One extra copy per frame on the ext backend (the compositor's blit into
  wraith's buffers). Measured against wraith's own compositor on the
  same wlroots: the same frame rate and CPU, about 1 ms more latency
  with VA-API and about 3 ms with x264 at 4K.
- Each desktop family needs its own setup code, and GNOME and KDE need
  private APIs ([0002](0002-gnome-through-mutters-private-api.md),
  [0003](0003-kwin-virtual-through-an-xdg-variable.md)).

## Rejected alternatives

- **wraith as the compositor for every session.** Means reimplementing
  each desktop's window management and integration, and full desktops
  can't run that way.
- **Nested compositors** (a desktop's compositor as a client of
  wraith's), retired 2026-09-15: a nested kwin only locks the pointer
  upstream on a Right Ctrl toggle, so games never got mouselook.
