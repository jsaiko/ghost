# Configuration

Every daemon reads a TOML file in `/etc/ghost`. The shipped copies in
`packaging/config/` list every key, commented out at its default, with
its meaning; they are the key reference. `make install` puts each in
`/etc/ghost` only if nothing is there, and always leaves a pristine copy
in `$PREFIX/share/doc/ghost`. A unit test per daemon uncomments every key
in its shipped file and requires the result to equal the built-in
defaults, so the files can't drift from the code.

## ghostd.toml

`/etc/ghost/ghostd.toml`, read by `host/ghostd/src/config.rs` through the
`tomlconf` crate.

**Layers**, later winning: built-in defaults, `ghostd.toml`, then
`ghostd.d/*.toml` in name order, then command-line flags. Files merge key
by key and tables merge rather than replace, so a drop-in can set one
field of `[auth.penalties]` and keep the rest. A missing `ghostd.toml` at
the default path means defaults; a file named with `-f` must exist.
`ghostd join` writes its settings to `ghostd.d/broker.toml`.

- **Strict.** Unknown keys and wrong types fail startup, reported with
  the file, line and column (each file is parsed on its own before the
  merge). Checks across keys (the session port range against the lobby
  port, `max_startups` ordering, the `[broker]` keys together) run on the
  merged result.
- **Owned by root.** ghostd refuses a file or drop-in directory that
  isn't owned by root (or the `ghost` user it runs as) or is writable by
  group or others, as sshd's StrictModes does: these files decide who
  may log in as root.
- **`ghostd -t`** checks the configuration, loads the host certificate
  and checks that the ghostauth and ghostseat sockets exist, then exits;
  the unit runs it as `ExecStartPre`.
  **`ghostd -T`** prints the effective configuration with every key.
- **Durations** are sshd-style strings (`"90s"`, `"10m"`) or plain
  seconds; `-T` prints the largest whole unit.
- **No reload.** Restart ghostd to apply a change; running sessions
  survive a restart.
- **Flags** exist for every key (`--port`, `--max-sessions`,
  `--permit-root-login[=false]`, ...) for development runs.
  `--max-startups` and `--per-source-penalties` take sshd's one-line
  syntax. The unit passes none, since a flag silently overrides the
  file.
- **Logging** is `[log] level`, a tracing EnvFilter string. ghostd starts
  logging only after the configuration loads (a configuration error goes
  to stderr, which the journal keeps), and exports the filter as
  `RUST_LOG` to the ghostseat processes it spawns. A `RUST_LOG` already
  in ghostd's environment wins.

## veild.toml

`/etc/ghost/veild.toml`, read by `host/veil/src/config.rs` through the
same `tomlconf` crate, with the same layers (`veild.d/*.toml`, then
flags), strictness, `-t` / `-T` and duration syntax as ghostd.toml.
veild runs as the `veil` user, so the ownership check always applies:
the files must belong to root (or the user running veild) and not be
writable by group or others. SIGHUP reloads only the web certificate;
anything else needs a restart, which ends every gateway session.

## wraith.toml

`/etc/ghost/wraith.toml`, read by `host/wraith/src/util/config.cpp`:
`[log] level`, `[network]` (congestion control, pacing, the rate
trace), `[encode]` (GOP length, bitrate ceiling, forcing the software
encoder, NVENC's zero-copy input, PyroWave's ceiling, and which codecs
are offered) and `[refine]` (lossless refinement's settle time and
bandwidth budget, with per-profile overrides in `[refine.lan]`,
`[refine.internet]` and `[refine.mobile]`). It differs from ghostd.toml
because of who reads it:

- **Read per session.** Each wraith reads it once at startup, so a
  change applies to sessions started after it; nothing restarts.
- **Rejected whole, never fatal.** A file that doesn't parse, or has a
  wrong type or out-of-range value, is ignored entirely and the session
  runs on defaults, with an error in its log; a typo shouldn't lock
  users out of their desktops. Unknown keys are logged and skipped.
  `wraith --check-config [file]` validates a file the same way and
  prints the settings a session would run with, exiting 1 on a file
  wraith would reject.
- **No ownership check.** wraith runs as the session's user and nothing
  in the file decides who may log in; the file is root's by being in
  `/etc/ghost`.
- **`-C <file>`** reads another file instead, for development runs and
  the netem harness.
