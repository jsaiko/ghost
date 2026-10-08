# Session profiles

A session type is a `sessions.d/<id>.conf` file; the file name's stem is
the id a client picks. ghostd reads profiles to build the lobby's list
and to validate the chosen id (`host/ghostd/src/profiles.rs`). wraith
resolves the id and launches the profile
(`host/wraith/src/session/session_profile.cpp`). The client never sends a
command, only the id, and wraith rejects an id containing `/` or `..`.

## Search order

ghostd.toml's `[sessions] dir` (default `/etc/ghost/sessions.d`), then
`$PREFIX/share/ghost/sessions.d`. ghostd lists profiles from this order;
ghostseat reads the same `dir` from the root-owned ghostd.toml itself
and passes it to wraith at session start, so both see the same files. A
standalone wraith (no ghostd) searches `/etc/ghost/sessions.d` and the
shipped directory. The first file with a given id wins, so an admin
overrides a shipped profile with a file of the same name, and hides it
with an empty file or one holding only `Enabled=false`.

A profile's `Exec` runs as the session user, so under ghostd the
directory and the file must be owned by root and not writable by group
or others. ghostseat sends no `dir` that fails this, and wraith refuses
a profile that does rather than skipping to the next directory.

## Keys

All keys are in one `[Session]` section. Lines starting with `#` or `;`
are comments.

| Key | Meaning |
|---|---|
| `Name` | Display name in the lobby. Required. |
| `Exec` | The session leader's command. Required. |
| `Backend` | `screencast-gnome`, `screencast-kwin` or `screencast-ext` ([capture backends](../design/capture-backends.md)). Required: ghostd doesn't list a profile without one, and wraith rejects an unknown value. |
| `Enabled` | `false` hides the profile, as if absent; such a file needs no other keys. Default `true`. |
| `TryExec` | Availability check, read by ghostd only: the profile is listed only if every program named here (space-separated; `Exec`'s first word when unset) is an executable path or is on root's `$PATH`. User-local installs therefore read as unavailable. |
| `LogoutExec` | Command that logs the desktop out gracefully on a client's `LogoutRequest`. Unset means `SIGTERM` to the leader. Read by wraith only. |
| `Environment` | `KEY=VALUE` for the leader. Repeatable. Read by wraith only. |

## Shipped profiles

| id | Name | Backend | Leader |
|---|---|---|---|
| `terminal` | Terminal | screencast-ext | `labwc-screencast` → labwc running `foot`; listed only where foot is installed |
| `steam` | Steam (Big Picture) | screencast-ext | `labwc-screencast` → labwc running `steam -bigpicture`; `LogoutExec=steam -shutdown` |
| `plasma` | KDE Plasma | screencast-kwin | `plasma-screencast` → `startplasma-wayland` |
| `gnome` | GNOME | screencast-gnome | `gnome-screencast` → `gnome-session` |
| `lxqt` | LXQt | screencast-ext | `lxqt-screencast` → labwc running `startlxqt` |

The launchers live in `$PREFIX/lib/ghost/sessions`; the labwc configs of
the labwc-hosted types in `$PREFIX/share/ghost/labwc`.

ghostd.toml's `sessions.dir` and `sessions.default_type` set where ghostd
looks first and which type a new user is offered.
