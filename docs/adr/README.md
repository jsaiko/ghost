# Architecture decision records

One file per significant decision: the context, the decision, and the
alternatives rejected. A record is never edited after it is accepted
beyond fixing links; a later decision that changes it is a new record
that says which one it supersedes.

New records take the next number and this shape:

```
# NNNN. Title

Status: Accepted (YYYY-MM-DD) | Superseded by NNNN

## Context
## Decision
## Consequences
## Rejected alternatives
```

| # | Decision |
|---|---|
| [0001](0001-wraith-attaches-to-a-real-compositor.md) | wraith attaches to a real compositor; it never composites |
| [0002](0002-gnome-through-mutters-private-api.md) | GNOME through mutter's private D-Bus API, no portal |
| [0003](0003-kwin-virtual-through-an-xdg-variable.md) | kwin's `--virtual` through a drop-in and `$XDG_GHOST_KWIN_ARGS` |
| [0004](0004-ghostseat-holds-the-logind-session.md) | ghostseat holds the logind session; no linger |
| [0005](0005-one-graphical-login-per-user.md) | One graphical login per user; the console wins |
| [0006](0006-ghostseat-creates-virtual-gamepads.md) | ghostseat creates virtual gamepads and keeps their fds; no `ghost` group |
| [0007](0007-refinement-damage-is-activity.md) | Refinement: damage marks activity, the hash decides change |
| [0008](0008-software-encode-is-h264-only.md) | Software encode is H.264 only |
| [0009](0009-stock-ngtcp2-congestion-control-and-own-pacing.md) | Stock ngtcp2 congestion control, CUBIC by default, with ghost's own pacing |
| [0010](0010-pinned-identities-and-throwaway-session-certificates.md) | Pinned identities; the host key never reaches wraith |
| [0011](0011-remembered-password-split-between-veil-and-browser.md) | The browser's remembered password is split between veild and the browser |
| [0012](0012-host-setup-stays-with-the-admin.md) | Group membership and vgem stay with the admin |
| [0013](0013-no-host-sleep-inhibitor.md) | ghost doesn't inhibit host sleep |
| [0014](0014-controllers-pass-through-as-raw-hid.md) | Controllers pass through as raw HID; the Xbox pad is the fallback |
| [0015](0015-security-keys-pass-through-with-a-fixed-descriptor.md) | Security keys pass through as FIDO HID, with the host's own descriptor |
| [0016](0016-key-maintenance-by-consent.md) | A forwarded key's maintenance passes with its user's consent |
| [0017](0017-smart-cards-forward-over-pcsc.md) | Smart cards forward over PC/SC, with writes by consent |
| [0018](0018-video-fec-over-a-parity-channel.md) | Video FEC: Reed-Solomon parity on a channel of its own |
