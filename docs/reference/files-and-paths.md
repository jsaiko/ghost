# Files and paths

`$PREFIX` is where `make install` puts the build: `/usr/local` by
default, `/usr` for a packaged host. Settings are in
[configuration](configuration.md), flags in
[command line](command-line.md).

## Configuration and identity, `/etc/ghost/`

Put down once and never overwritten or removed by `make uninstall`; they
are the admin's files from then on. Pristine copies of the configuration
are in `$PREFIX/share/doc/ghost/`.

| File | What |
|---|---|
| `ghostd.toml`, `ghostd.d/*.toml` | ghostd's settings and drop-ins. `ghostd join` writes `ghostd.d/broker.toml`. |
| `wraith.toml` | wraith's settings. |
| `sessions.d/*.conf` | Admin session profiles; these win over the shipped ones ([session profiles](session-profiles.md)). |
| `host-cert.pem`, `host-key.pem` | The host's identity; clients pin its fingerprint ([trust](../design/trust.md)). The key is readable by the `ghost` group. |
| `auth-ticket.key` | The key ghostauth signs its tickets with and ghostseat verifies them ([authentication](../design/login-and-sessions.md#authentication)); root:ghostauth 0640. |
| `veild.toml`, `veild.d/*.toml` | Veil's settings and drop-ins. |
| `veil-cert.pem`, `veil-key.pem` | Veil's lobby identity; the key is readable by the `veil` user. |
| `veil-web-cert.pem`, `veil-web-key.pem` | The browser client's HTTPS certificate, if configured. |
| `veil-wisp.key` | The key Wisp thin clients use; `veild wisp-env` prints it. |

## Installed files

| Path | What |
|---|---|
| `$PREFIX/bin/ghostd`, `wraith`, `spectre`, `spectre-qt` | The daemon, the session agent, the client and its launcher. |
| `$PREFIX/bin/veild` | Veil, from `make install-veil` only. |
| `$PREFIX/lib/ghost/ghostauth`, `ghostseat`, `ghostlogin` | The PAM helper, the session's root process and the PAM hook. Run by systemd's socket units and by PAM, not from a shell. |
| `$PREFIX/lib/ghost/sessions/` | Launchers the shipped profiles point at. |
| `$PREFIX/share/ghost/sessions.d/` | Shipped session profiles. |
| `$PREFIX/share/ghost/labwc/` | The labwc session types' configuration. |
| `$PREFIX/lib/systemd/system/ghostd.service`, `veild.service` | System units. |
| `$PREFIX/lib/systemd/system/ghostauth.socket`, `ghostauth@.service`, `ghostseat.socket`, `ghostseat@.service` | The socket units that start a ghostauth per login and a ghostseat per session ([processes](../design/login-and-sessions.md#processes)). |
| `$PREFIX/lib/sysusers.d/ghostd.conf`, `ghostauth.conf`; `$PREFIX/lib/tmpfiles.d/ghost.conf` | The `ghost` and `ghostauth` accounts and `/run/ghost`. `make install` applies them; a package's scripts do on theirs. |
| `$PREFIX/lib/systemd/user/wraith.service` | wraith's per-user unit, started by ghostseat. |
| `$PREFIX/lib/systemd/user/plasma-kwin_wayland.service.d/50-ghost.conf` | Lets a ghost Plasma session start kwin with `--virtual` via `XDG_GHOST_KWIN_ARGS` ([ADR 0003](../adr/0003-kwin-virtual-through-an-xdg-variable.md)). |
| `/etc/pam.d/ghostd`, `ghostseat`, `veild` | The PAM stacks the daemons authenticate and open sessions on. |
| `/usr/share/pam-configs/ghostlogin` | Debian's `pam-auth-update` snippet that adds ghostlogin to the account stack. |
| polkit rules directory, `49-ghost-sessions.rules` | Denies network-control and power actions to ghost sessions ([polkit](../design/login-and-sessions.md#polkit)). |
| udev rules directory, `72-ghost-input.rules` | Keeps ghost's virtual gamepads and raw controllers off seat0 ([audio, cursor and gamepads](../design/audio-cursor-gamepad.md#gamepads)). Raw controllers need the version with the `HID_PHYS` line; ghostseat checks. |

## Runtime state

| Path | What |
|---|---|
| `/run/ghost/` | Every ghost socket; root's, mode 1771, group `ghost` (tmpfiles.d): ghostd creates its own sockets here but can't unlink ghostseat's, and it outlives a ghostd restart ([the sockets](../design/login-and-sessions.md#the-sockets)). |
| `/run/ghost/auth.sock` | ghostd and veild to a fresh ghostauth per login; root:ghost 0660. |
| `/run/ghost/seat.sock` | ghostd to a fresh ghostseat per session; root:ghost 0660. |
| `/run/ghost/<uid>-seat.sock` | ghostd to that user's open session (`STATUS`, `CLOSE`); root:ghost 0660. |
| `/run/ghost/<uid>.sock` | That user's wraith to its ghostseat; the user's, 0600. wraith talks only if the peer is root. |
| `/run/ghost/events.sock` | Every ghostseat to ghostd; ghostd's, 0600. |
| `/run/ghost/ghostlogin.sock` | ghostlogin to ghostd; ghostd's, 0600. |
| `/run/ghost/spent-tickets/` | The authentication tickets ghostseat has accepted, one empty file per nonce, pruned after their ten minutes; root, 0700 ([authentication](../design/login-and-sessions.md#authentication)). |
| `$XDG_RUNTIME_DIR/wraith.sock` | The running wraith's own control socket, mode 0600, for that user's commands (`wraith --report`). |
| `/var/lib/ghost/` | ghostd's state directory, owned by `ghost`, mode 0700. |
| `/var/lib/ghost/last-types.json` | Each user's last session type, the lobby's preselection ([what ghostd remembers](../design/login-and-sessions.md#what-ghostd-remembers)). |
| `/var/lib/veil/veil.db` | Veil's database (`[state] database`), owned by `veil`, mode 0700. |

A support report made by `wraith --report` is written to the user's home
directory as `ghost-report-<date>-<time>.tar.gz`.

## spectre, per user

Preferences are in `~/.local/share/spectre/spectre/` on Linux and
`%APPDATA%\spectre\spectre` on Windows.
