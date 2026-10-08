# Architecture

A map of ghost for someone about to change it: the processes, where
their code lives, how a login becomes a running session, and the rules
the design depends on. Each area's details are in
[docs/design/](docs/design/); the wire format is
[docs/spec/gdp-spec.md](docs/spec/gdp-spec.md).

## What runs where

```
 client device                      session host                                  Veil (optional)
 -------------                      ------------                                  ---------------
 spectre-qt ──lobby (gdp/1)──────▶  ghostd (user ghost, system unit) ◀─gdp-host/1─ veild (user veil)
   │  execs                           │ PAM via        │ Open via                   │ lobby, gateway,
   ▼                                  ▼                ▼                            │ browser client,
 spectre ───session (gdp/1)──────▶  ghostauth        ghostseat (root, per session)  │ wisp/1; PAM via
                                    (per login,        holds the logind session,    │ ghostauth too
                                     user ghostauth)   starts wraith.service        ▼
                                                         │ (user unit) runs      Wisp thin clients,
                                                         ▼                       browsers
                                                       session leader
                                                       (gnome-shell, kwin, labwc)
```

| Process | Language | Runs as | Code |
|---|---|---|---|
| `ghostd` | Rust | system user `ghost`, `ghostd.service`, sandboxed | `host/ghostd/` |
| `ghostauth` | Rust | system user `ghostauth` with `CAP_DAC_READ_SEARCH`, one per login, started by `ghostauth.socket` | `host/ghostauth/` |
| `ghostseat` | Rust | root, one per logged-in user, started by `ghostseat.socket`, in that user's logind session scope | `host/ghostseat/` |
| `ghostlogin` | Rust | root, run by `pam_exec` in the display managers' account phase | `host/ghostlogin/` |
| `wraith` | C++ | the session's user, `wraith.service` (user unit) | `host/wraith/` |
| `veild` | Rust | system user `veil`, `veild.service`, on a Veil machine only | `host/veil/` |
| `spectre` | C++ | client device (Linux, Windows, macOS) | `client/spectre/` |
| `spectre-qt` | C++/Qt 6 | client device; runs the lobby, then execs `spectre` | `client/spectre-qt/` |
| `wisp-agent` | C++ | root on a Wisp thin client | `client/wisp/agent/` |

Libraries:

| Library | Used by | Code |
|---|---|---|
| `libgdp` | wraith, spectre, spectre-qt, wisp-agent | `libgdp/` (protos in `libgdp/proto/`) |
| `ipc` | ghostd, ghostauth, ghostseat, ghostlogin, veild: protobuf framing, generated types, socket paths | `host/ipc/` |
| `pamconv` | ghostd, veild: the client side of ghostauth, relaying PAM prompts | `host/pamconv/` |
| `authticket` | ghostauth, ghostseat: the ticket that proves a login happened | `host/authticket/` |
| `preauth` | ghostd, veild: sshd's MaxStartups and PerSourcePenalties | `host/preauth/` |
| `gdpnet` | ghostd, veild: certificate pins, dual-stack QUIC sockets | `host/gdpnet/` |
| `tomlconf` | ghostd, veild: layered TOML configuration | `host/tomlconf/` |

`host/` is a Cargo workspace plus wraith (CMake). `host/proto/` holds the
schemas that never leave a host (`control.proto`, `ghostseat.proto`,
`ghostauth.proto`, `ghostlogin.proto`, and `wraith.proto` for wraith's
own socket) and the host-to-Veil channel (`broker.proto`).

## From login to session

1. spectre-qt connects to ghostd's lobby (UDP 4442) and authenticates
   through PAM, one `AuthChallenge` per prompt; the PAM transaction runs
   in a `ghostauth` instance, which hands ghostd a signed ticket.
2. ghostd finds the user's open session through its `ghostseat`, or
   asks a new ghostseat instance to open one, presenting the ticket.
   ghostseat opens a logind session through PAM, which keeps the user's
   systemd manager running.
3. ghostseat starts `wraith.service` in the user's manager and hands
   wraith a session secret and a port range over `/run/ghost/<uid>.sock`.
   wraith launches the session type's compositor, binds a UDP port, and
   reports the port and its certificate fingerprint.
4. ghostseat mints a token from the session secret, one per spent
   ticket, and ghostd sends it to spectre-qt in a `Redirect`. spectre-qt execs spectre, which connects to wraith,
   presents the token, and the session runs.
5. When the desktop ends, `wraith.service`'s `ExecStopPost` reports it
   to ghostseat, which closes the logind session and tells ghostd.

Through Veil, steps 1 and 4 go through veild: it authenticates the user,
shows the hosts they are entitled to, relays the host's lobby over the
host channel, and may carry the session itself (the gateway).

Details: [login and sessions](docs/design/login-and-sessions.md),
[capture backends](docs/design/capture-backends.md),
[Veil](docs/design/veil.md).

## Rules the design depends on

- **wraith never composites.** Every session runs a real compositor on a
  virtual output; wraith is its client, capturing frames and injecting
  input. Desktops behave as they do on a physical login.
- **Ghost sessions are seatless and remote.** logind records them with
  no seat and `Remote=yes`. Anything gated on a seat or on polkit's
  `allow_active` behaves as for any remote session; device access comes
  from group membership or from ghostseat.
- **Nothing network-facing runs as root.** ghostd and veild run as
  their own users and never load PAM; ghostauth runs PAM unprivileged
  and ghostseat, the only root process, acts only on a ticket ghostauth
  signed ([why](docs/design/login-and-sessions.md#why-it-is-split-this-way)).
- **The client names a session type, never a command.** A session type
  is an id resolved against `sessions.d` profiles on the host.
- **Disconnecting is not logging out.** A client that closes, crashes or
  loses the network leaves the session running. Only the desktop
  ending, a `LogoutRequest`, or a console login ends it.
- **One session per user, and one graphical login per user.** A second
  login reattaches to the running session. A login at the host's own
  console ends the ghost session first; a remote login is refused while
  the user is logged in at the console.
- **Restarting ghostd ends nothing.** Sessions live in ghostseat's logind
  session and the user manager. ghostd finds them again at startup
  through each session's socket in `/run/ghost`.
- **Identity is a pinned fingerprint.** spectre pins a host's (or
  Veil's) lobby certificate on first use, unless it chains to a CA the
  client trusts. A session's certificate is throwaway, and ghostd
  vouches for it in the `Redirect`. Hosts and Veil pin each other.

## Where to read next

- [docs/design/](docs/design/): how each area works and why.
- [docs/adr/](docs/adr/): decisions and the alternatives rejected.
- [docs/spec/gdp-spec.md](docs/spec/gdp-spec.md): the wire protocol.
- [TODO.md](TODO.md): known gaps and unverified paths.
