# 0002. GNOME through mutter's private API

Status: Accepted

## Context

GNOME's standard remote-desktop route is the xdg-desktop-portal, which
asks the user for consent through a dialog. A ghost session is headless
and has no one to answer a dialog before the session exists.

## Decision

`screencast-gnome` uses mutter's private
`org.gnome.Mutter.RemoteDesktop` / `ScreenCast` D-Bus API directly, as
gnome-remote-desktop does: a linked session pair, `RecordVirtual` for the
frames, and `ConnectToEIS` for input through libei. No portal.

## Consequences

- No consent prompt, no allowlist: mutter's private API trusts any
  session-bus peer. If a future GNOME adds a check, this backend needs
  revisiting.
- The call order is strict (subscribe to `PipeWireStreamAdded` before
  `Start()`), and everything is scoped to the D-Bus connection that
  created the session.

## Rejected alternatives

- **The portal.** Needs interactive consent.
- **`NotifyPointer*` / `NotifyKeyboard*` D-Bus input.** mutter accepts
  the calls, but they never reach a client's seat; libei is the only
  working input path.
