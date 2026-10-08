# Installing a session host

A session host runs `ghostd` (the lobby and login) and starts a `wraith`
per logged-in user. Clients connect with spectre. For a Veil in front of
several hosts see [Veil](veil.md); for a host with no GPU see
[hosts without a GPU](gpu-less-hosts.md).

## Build and install

You need CMake 3.20 or later, a C++20 compiler, a stable Rust toolchain
and the development packages for your distribution. spectre needs SDL3,
so the host needs a release that packages it, plus ngtcp2 with its
OpenSSL helper: Ubuntu 26.04 or later
(what CI builds on), Fedora 43 or later, or current Arch. The package
lists are in the [top-level README](../../README.md#prerequisites).

```sh
make                 # as your normal user; builds into build/
make test            # optional
sudo make install
```

Run `make` first and as yourself: `make install` doesn't build, because
building as root would leave root-owned files in your build tree. What
lands where is in [files and paths](../reference/files-and-paths.md).
The prefix is `/usr/local`; for `/usr` (a packaged host) pass
`PREFIX=/usr` to both commands, since it is fixed at configure time and
`make` reconfigures when it changes. `DESTDIR` is honoured for staging.
`make uninstall` removes what `install` put down, but leaves
`/etc/ghost`.

`make install` also:

- generates the host's certificate and key in `/etc/ghost` if neither
  exists, and prints the fingerprint. Clients pin it on first connection,
  so replacing it makes every client warn that it changed. `make cert`
  runs just this step. It needs the `openssl` CLI.
- installs `ghostd.toml` and `wraith.toml` there if absent, every setting
  commented out at its default ([configuration](../reference/configuration.md)).
  `ghostd -t` checks an edit, `ghostd -T` prints what ghostd will run
  with, and `wraith --check-config` checks wraith's file.
- creates the `ghost` and `ghostauth` system accounts and `/run/ghost`
  (`systemd-sysusers` and `systemd-tmpfiles` on the installed
  fragments), and generates the authentication ticket key at
  `/etc/ghost/auth-ticket.key` if absent
  ([authentication](../design/login-and-sessions.md#authentication)).
- reloads udev, so the gamepad rule applies without a reboot, and
  enables ghostlogin in `common-account` where `pam-auth-update` exists.
- installs `/etc/pam.d/ghostd` and `ghostseat`, which include the
  distro's own stacks: `common-*` on Debian and Ubuntu, `system-auth`
  (and `system-login` where it exists) on Arch and Fedora. The build
  picks them by probing `/etc/pam.d` at configure time; set
  `make PAM_STACK=debian` or `PAM_STACK=system-auth` when it can't, and
  run `make reconfigure` to change it for an existing build directory.

A certificate issued by a CA for the name clients type works in place of
the generated one. spectre-qt then trusts it without asking and takes
renewals in its stride, except on a host joined to Veil, which pins the
certificate and would need a re-join after a renewal
([trust](../design/trust.md)).

## Start it

Open UDP 4442 (the lobby) and 14400-14463 (one port per session;
`[sessions]` in `ghostd.toml`) in the firewall. On a host with a
management interface, `[lobby] address` keeps the lobby off it (the
session ports still bind every address). Then:

```sh
sudo systemctl enable --now ghostauth.socket ghostseat.socket ghostd
```

ghostd runs as the `ghost` user and needs the two socket units: a
ghostauth instance runs each login's PAM transaction, and a ghostseat
instance opens each session as root. `pam_systemd` refuses to create a
session for a leader already inside one, which is why ghostseat is
started by systemd and never from a terminal. A development ghostd runs
the same way, with the socket units installed and active:

```sh
sudo systemd-run --unit=ghostd-dev --collect --uid=ghost --gid=ghost -- /path/to/ghostd
journalctl -u ghostd-dev -f
```

The sessions' and helpers' logs are in `journalctl -u 'ghostseat@*'`
and `journalctl -u 'ghostauth@*'`.

Flags such as `--port` override single keys ([command line](../reference/command-line.md)).

Connect with spectre-qt. `spectre` itself takes `-h host -p port -P
sha256`, with the token in `SPECTRE_TOKEN` (or `-t token`), for a
session you already hold an unused redirect token and fingerprint for.

## Accounts and devices

- **GPU access.** A ghost session is seatless, so logind's `uaccess`
  ACLs never reach it; every session user needs the `render` group on a
  host with a GPU. ghostd doesn't manage this
  ([ADR 0012](../adr/0012-host-setup-stays-with-the-admin.md)), so set it
  up wherever the accounts come from.
- **Users.** Logins are checked by the host's PAM (`/etc/pam.d/ghostd`),
  so any account PAM accepts can log in, subject to `[auth]` in
  `ghostd.toml`. root and empty passwords are refused by default.
- **Session types.** A login starts a session type, a
  `sessions.d/<id>.conf` profile. The shipped ones need their compositor
  installed (`labwc` for `terminal`, `steam` and `lxqt`; GNOME or Plasma
  for theirs), and `terminal` needs `foot` too; a type whose `TryExec`
  programs aren't all present isn't listed.
  Override or hide one with a same-named file in `/etc/ghost/sessions.d`
  ([session profiles](../reference/session-profiles.md)).

## Power management

ghost doesn't stop a host from sleeping. If the host suspends when idle
(logind's `IdleAction`, or the console desktop's or login greeter's
power manager), it suspends under a running session and the session is
lost. Turn idle suspend off on a host that serves sessions
([ADR 0013](../adr/0013-no-host-sleep-inhibitor.md)). The simplest way
that covers every cause:

```sh
sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target
```

Check `IdleAction=` in `/etc/systemd/logind.conf` as well, and the
greeter's own setting if the console runs one (for GDM, the `gdm` user's
`org.gnome.settings-daemon.plugins.power` keys). A ghost session itself
can't suspend or power off the host: the polkit rule denies it every
power action ([polkit](../design/login-and-sessions.md#polkit)).

## Local logins

Only one graphical login per user is allowed at a time. A user who logs
in at the host's console ends their ghost session first, and a ghost
login is refused while the user is on a local seat
([one graphical login per user](../design/login-and-sessions.md#one-graphical-login-per-user)).

## Support reports

When something misbehaves (a codec that wasn't used, a decoder that fell
back to software, a black or green picture), the user runs this in a
terminal inside the remote desktop:

```
wraith --report
```

It writes `~/ghost-report-<date>-<time>.tar.gz` (or into the directory
given as an argument) and prints the path. The archive holds:

| File | What |
|---|---|
| `wraith-status.txt` | What the session negotiated: the codecs and decoders the client offered, what the host can encode, direct or via a gateway, the codec chosen. |
| `client.txt` | The attached client's own report: its decoder, GPU facts and recent log ([diagnostics](../spec/gdp-spec.md#711-diagnostics)). spectre and the browser client both answer; one that doesn't within 5 s is noted instead. |
| `wraith-journal.txt` | This user's `wraith.service` journal, the last 5000 lines. |
| `desktop-warnings.txt` | Warnings from the compositor, PipeWire and the portals. |
| `wraith.toml` | The settings file. |
| `system.txt`, `gpu.txt` | Kernel, distribution, the session's environment, render nodes, `vainfo` and `vulkaninfo --summary`. |

Nothing is sent anywhere: the report is made on the host, in the user's
account, and the user decides who gets it. ghostd's and Veil's logs belong
to root and aren't in it; an administrator adds them with
`journalctl -u ghostd` and `journalctl -u veild`. The report does name the
host, the user's uid and the session's certificate fingerprint (public, it
is what clients pin), so look it over before posting it publicly. It only
works inside a session: the command talks to that session's wraith over
`$XDG_RUNTIME_DIR/wraith.sock`.

The client's log holds what passed its level, `info` by default
([command line](../reference/command-line.md#spectre)). Start spectre-qt with
`-D` (it runs spectre with `SPECTRE_LOG=debug`), or spectre with
`SPECTRE_LOG=debug` set, before reproducing a problem that needs more
detail.
