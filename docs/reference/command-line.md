# Command line

Daemon settings live in configuration files
([configuration](configuration.md)); their flags override the file for
one run. wraith's flags are in
[host/wraith/README.md](../../host/wraith/README.md#run), and spectre's
are at the end of this page.

## ghostd

`ghostd [flags] [join ... | leave]`

| Flag | Overrides / does |
|---|---|
| `-f`, `--config FILE` | Read this file instead of `/etc/ghost/ghostd.toml`. Drop-ins come from the same path with a `.d` extension. A missing file is an error when named here, and fine at the default path. |
| `-t` | Check the configuration, the host certificate and the ghostauth and ghostseat sockets, then exit (as `sshd -t`). `ghostd.service` runs it before every start. |
| `-T` | Print the effective configuration as TOML, then exit (as `sshd -T`). |
| `--address` | `[lobby] address`; empty is every address. |
| `--port` | `[lobby] port` |
| `--cert`, `--key` | `[lobby] cert`, `key` |
| `--session-port-base` | `[sessions] port_base` |
| `--max-sessions` | `[sessions] max` |
| `--sessions-dir` | `[sessions] dir` |
| `--default-session-type` | `[sessions] default_type` |
| `--permit-root-login[=BOOL]` | `[auth] permit_root_login`; a bare flag means true. |
| `--permit-empty-passwords[=BOOL]` | `[auth] permit_empty_passwords` |
| `--login-grace-time` | `[auth] login_grace_time`: seconds, or with an `s`/`m`/`h` suffix. |
| `--max-startups` | `[auth.max_startups]`, as sshd's `start:rate:full` or `N`. |
| `--per-source-penalties` | `[auth.penalties]`: `no`, `yes`, or comma-separated `keyword:time` items (`authfail`, `noauth`, `grace-exceeded`, `max`, `min`). |
| `--log-level` | `[log] level`. `RUST_LOG`, when set, still wins. |

Subcommands, for hosts behind a Veil
([Veil](../design/veil.md)):

- `ghostd join VEIL --token TOKEN [--address ADDR] [--force]`: join this
  host to the Veil at `VEIL` (`host` or `host:port`). `TOKEN` is
  `<id>.<secret>:sha256:<fingerprint>`, exactly as Veil's admin UI or
  `veild token` prints it. `--address` is how clients reach this host
  directly (default: this machine's FQDN). It writes
  `ghostd.d/broker.toml` beside the configuration file; restart ghostd
  afterwards. `--force` joins although `broker.toml` says this host
  already is joined.
- `ghostd leave`: tell Veil to forget this host and remove `broker.toml`.

## veild

`veild [flags] [token | wisp-env]`

| Flag | Overrides / does |
|---|---|
| `-f`, `--config FILE` | As ghostd, with `/etc/ghost/veild.toml` as the default. |
| `-t`, `-T` | Check the configuration, the lobby certificate and the ghostauth socket; print the effective configuration. |
| `--address` | `[lobby] address`; empty is every address. |
| `--port` | `[lobby] port` |
| `--cert`, `--key` | `[lobby] cert`, `key` |
| `--database` | `[state] database` |
| `--log-level` | `[log] level`; `RUST_LOG` still wins. |

- `veild token [--ttl DURATION]`: issue a single-use join token and print
  the `ghostd join` command to run on the host, as the admin UI's "Add
  host" page does. The default lifetime is `[hosts] join_token_ttl`. It
  writes to the database directly, so veild needn't be running. Run it as
  root or as the `veil` user; root switches to the database's owner first.
- `veild wisp-env`: print the lobby certificate's fingerprint
  (`VEIL_CERT_SHA256`) and the thin clients' key (`WISP_KEY`) as the
  `.env` lines the Wisp boot server wants, so
  `veild wisp-env >> .env` works. Run it as root or as a member of the
  `veil` group.

## ghostseat and ghostlogin

Neither takes flags a person uses.

- `ghostseat` is a session's root process, which systemd starts from
  `ghostseat.socket` when ghostd opens a session. Never run it by hand
  ([login and sessions](../design/login-and-sessions.md)).
- `ghostlogin [SERVICE...]` is a PAM account hook for `pam_exec`. It
  ends the user's ghost session when they log in at the console
  ([one graphical login per user](../design/login-and-sessions.md#one-graphical-login-per-user)).
  Only the PAM services named as arguments trigger it; with none, the
  built-in list of display managers' services applies. On Debian,
  `pam-auth-update` installs the line.

## spectre

`spectre -h HOST -p PORT [-t TOKEN] -P SHA256 [flags]`

Normally started by spectre-qt, which passes the host, port and
fingerprint, and the token in `SPECTRE_TOKEN`. `spectre`
prints this list with no arguments; the logic of each is in
[the spectre client](../design/spectre-client.md).

| Flag | Meaning |
|---|---|
| `-h`, `-p` | The session host and port, from the lobby's redirect. |
| `-t TOKEN` | The session token. Without `-t`, spectre reads `SPECTRE_TOKEN` (and unsets it). Prefer the variable: other local users can read a command line. The token works once. |
| `-P SHA256` | The certificate fingerprint the session host must present: 64 hex digits, colons allowed. spectre refuses any other certificate before sending the token. For a direct-connect wraith, `openssl x509 -in cert.pem -noout -fingerprint -sha256` prints it. |
| `-X DECODER` | Decoder to try first: `vulkan` (default), `vaapi` or `v4l2` (Linux), `d3d11va` (Windows), `videotoolbox` (macOS), or `software`. A decoder that can't open falls back to the next. |
| `-f` | Open fullscreen. |
| `-K` | Kiosk: fullscreen for good, no minimize. The toolbar's close, the menu's Disconnect and End session still work. |
| `-T` | Take over: if the user's session is attached to another client, disconnect that client and attach. Exit status 4 means a take-over was refused. |
| `-r WxH` | Request this output resolution in `SessionHello`; the host's answer wins. |
| `-A` | Make the remote resolution follow the window. |
| `-V fit\|actual` | Picture scaled to the window, or 1:1 and panned. Default: whichever the menu last picked. |
| `-c SCALE` | Extra cursor size multiplier (default 1). |
| `-k CHORD` | Keys that open the session menu, `+`-joined; default `lctrl+lalt+lgui`. `ctrl`, `alt`, `shift`, `super` match either side, `lctrl`, `ralt`, ... one side, anything else is an SDL key name with `_` for spaces (`f12`, `scroll_lock`). |
| `-C CODEC` | Preferred codec, offered ahead of the rest: `pyrowave`, `h264`, `h265`, `av1`. A preference only: the host picks the first it can encode. |
| `-W` | Never offer PyroWave. Otherwise it is offered first when the host is directly reachable over a wired link of 1 Gbit/s or more (Linux and Windows). |
| `-N PROFILE` | Network profile for the host's rate control: `auto` (default), `lan`, `internet`, `mobile` ([transport and rate control](../design/transport-and-rate-control.md)). |
| `-R on\|off` | Start with lossless refinement on or off ([refinement](../design/refinement.md)): bit-exact settled text, at the cost of a readback and hashing every frame on the host. The menu's Lossless Refinement row switches it mid-session. Default: whichever the menu last picked, on at first. |
| `-D` | Outline each refined tile in red for a quarter second. No effect while lossless is off. |
| `-G` | Forward local gamepads: raw where the controller and host allow, so the host's own driver for it binds (touchpad, motion, lights, rumble, Steam Input), otherwise as a virtual Xbox pad ([gamepads](../design/audio-cursor-gamepad.md#gamepads)). |
| `-g` | Forward local gamepads, starting with every one as a virtual Xbox pad. The session menu's Controllers row switches between that and raw. |
| `-M` | Send this machine's microphone to the host. |

`spectre --probe-decoders` prints each working decode path as
`<path>:<codec>` (`vulkan:av1`, `vaapi:h264`, `software:h264`, ...) and
exits. `SPECTRE_LOG=debug` adds per-window detail and `SPECTRE_LOG=error`
keeps only failures (default `info`); output goes to stderr. The most recent
lines are also what spectre sends the host when the user runs
[`wraith --report`](../install/host.md#support-reports).
