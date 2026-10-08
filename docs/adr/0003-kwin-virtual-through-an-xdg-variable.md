# 0003. kwin's `--virtual` through an `XDG_` variable

Status: Accepted

## Context

A ghost Plasma session needs `kwin_wayland --virtual`: kwin's DRM backend
fails without a seat. But `startplasma-wayland` always starts kwin itself,
through `plasma-kwin_wayland.service`, and passes it no arguments. The
user manager is shared with any console login of the same user, so
whatever ghost changes there must not leak into a console session.

## Decision

Install a drop-in for `plasma-kwin_wayland.service` whose `ExecStart=` is
the stock command plus `$XDG_GHOST_KWIN_ARGS`. ghost's `plasma-screencast`
launcher sets that variable to `--virtual`. startplasma copies its
environment into the user manager before starting kwin, and systemd
expands an unset variable to nothing, so every other login runs the stock
command. The `XDG_` prefix is what keeps it from leaking: startplasma
removes from the user manager every `XDG_*` variable its own process
doesn't have before it starts kwin.

## Consequences

- One installed drop-in, inert for every non-ghost login.
- A console login that takes over the user manager after a ghost session
  can't inherit `--virtual` and start a headless (black-screen) desktop.

## Rejected alternatives

- **A variable without the `XDG_` prefix.** Survives in the user manager
  and starts the next console login headless.
- **Running `startplasma-wayland` inside a `kwin_wayland --virtual`**, as
  labwc runs `startlxqt`. With systemd it still starts
  `plasma-kwin_wayland.service`; without it, Plasma's legacy startup runs
  its own kwin nested inside the first.
- **Masking `plasma-kwin_wayland.service`.**
  `plasma-workspace-wayland.target` binds to it.
