# Login and session lifetime

How a lobby login becomes a running desktop, what keeps it alive, and
what ends it. Code: `host/ghostd/`, `host/ghostauth/`,
`host/ghostseat/`, `host/ghostlogin/`, and wraith's
`session/seat_client.cpp` and `session/session_process.cpp`. Wire
format: gdp-spec.md §4 (lobby).

## Processes

- **ghostd** is the host agent: the lobby, the broker link, and the
  memory of which sessions are open. It runs unprivileged, as the
  `ghost` user, in `ghostd.service`, and is the only process that reads
  bytes from the network.
- **ghostauth** runs one PAM authentication per login. systemd starts
  it for each connection to `/run/ghost/auth.sock` (`ghostauth.socket`),
  as the `ghostauth` user, whose one privilege is `CAP_DAC_READ_SEARCH`,
  read access to `/etc/shadow`. veild uses it the same way.
- **ghostseat** is a session's root process. systemd starts it for each
  connection to `/run/ghost/seat.sock` (`ghostseat.socket`). It opens
  and holds the user's logind session, starts wraith, owns wraith's
  control socket and the session's input devices, and closes the
  session at the end.
- **wraith** is the session agent, running as the user in
  `wraith.service` under the user's systemd manager.

ghostd never opens a logind session itself. logind ties a session to the
process that opened it, and ghostd serves every user, so restarting
ghostd would end every session at once. ghostseat is the equivalent of
xrdp's sesexec, and the only root code in ghost.

One group, `ghost`, gates every ghost socket (mode 0660 for the ones
systemd owns): the `ghost` user is in it by its primary group, and on a
broker `veil` joins it. `ghostauth` is not: its read access to
`/etc/shadow` is the privilege boundary.

### Why it is split this way

Nothing ghostd does before authentication needs root, and everything
that does happens after it, on behalf of one known uid. The split is
sshd's privilege separation: a bug in rustls, quinn, prost or the
lobby code yields the `ghost` user, a bug in a PAM module fed an
unauthenticated peer's username and password yields the `ghostauth`
user in a throwaway sandbox, and neither can open a session except for
a user who just authenticated, since only ghostauth mints tickets.
ghostseat is the only root code, and it reads small protobuf messages
from local sockets only. The price is two system accounts on a host
(one more on a broker), three socket units, a ticket key, and two local
round trips per session start.

Rejected:

- **A root PAM helper exec'd by ghostd.** Crash isolation only; a
  module bug still gives root.
- **seccomp around the PAM modules as the boundary.** Their syscall
  sets vary by distro and module.
- **A setgid-shadow helper binary**, as `unix_chkpwd`. No way to limit
  who executes it: a local password oracle for every account.
- **The `shadow` group as ghostauth's privilege.** See
  [Authentication](#authentication): only Debian ships `/etc/shadow`
  with that group, and changing the file's mode fights RHEL's hardening
  baselines.
- **A long-lived root session starter beside ghostseat.** ghostseat is
  already the per-session root process; starting it from a socket unit
  leaves no long-lived root at all.
- **A root helper that trusts ghostd's word.** Without the ticket a
  compromised ghostd could open a session as anyone, which is most of
  what root is worth here.

## Authentication

PAM never runs in ghostd or veild. The lobby connects to
`/run/ghost/auth.sock`, systemd starts a `ghostauth@.service` instance
with the connection as its stdin, and the two speak
`host/proto/ghostauth.proto` (the `pamconv` crate on the caller's side):

1. `Start` names the PAM service (`ghostd` or `veild`, nothing else),
   the username, the client address (`PAM_RHOST`) and whether empty
   passwords are allowed.
2. Each PAM conversation callback is a `Prompt`, which the lobby relays
   to the client as an `AuthChallenge`; the `AuthResponse` goes back as
   an `Answer`. The helper keeps no copy of any answer. Dropping the
   connection fails the conversation, which is how a grace-time expiry
   or a client that went away cancels a login.
3. `Done` carries the outcome. On success it carries a ticket
   (`host/authticket`): base64url of a nonce, an expiry ten minutes out,
   and an HMAC-SHA256 over them, the PAM service, the username and the
   client address, keyed by `/etc/ghost/auth-ticket.key`
   (root:ghostauth 0640). ghostd never reads the key; ghostseat verifies
   the ticket as root before it opens a session, accepts only the
   `ghostd` service's (a veild on the same machine is in the `ghost`
   group too, and its logins mint tickets on `veild`), and spends each
   nonce once in `/run/ghost/spent-tickets`. A reattaching login spends
   its ticket the same way, for the redirect token ghostseat mints
   ([trust](trust.md#session-tokens)), so a compromised ghostd can open
   or attach to one session, for a user who really just authenticated.

The PAM service files are the distro's auth and account stacks only
(`/etc/pam.d/ghostd`, `/etc/pam.d/veild`: `common-*` on Debian,
`system-auth` on Arch and Fedora, filled in at install time by
`packaging/system/pam-render.sh`). `pam_unix` reads `/etc/shadow`
itself through the unit's `CAP_DAC_READ_SEARCH`, so the file's mode
doesn't matter (Debian's `root:shadow` 0640, Arch's and Fedora's
`root:root` with no group alike). The `shadow` group would not do: on
the latter two, `unix_chkpwd` drops to the caller's uid for any user
but its own, so an unprivileged process can never check another
account's password. Should a `pam_unix` fork `unix_chkpwd` anyway, the
ambient capability survives the exec and the seccomp filter allows the
`setuid(getuid())` it makes first. `pam_sss`, `pam_ldap` and
`pam_krb5` work unprivileged. `pam_faillock` cannot write its tally from
here and is inert; ghost's own [penalties](preauth.md) stand in for it.

## The login path

1. spectre-qt connects to the lobby (UDP 4442, ALPN `gdp/1`) and sends
   `LobbyHello`. ghostd runs the login through ghostauth
   ([above](#authentication)), one `AuthChallenge` per prompt, and
   keeps the ticket.
2. After authentication, ghostd checks the login policy (root, a local
   console session; see [preauth](preauth.md) and
   [one graphical login per user](#one-graphical-login-per-user)), then
   sends `SessionList`: the user's open session, if any, the session
   types on offer, and a default (the user's last type if still offered,
   else `sessions.default_type`, else the first).
3. On `SessionOpen`, `SessionManager::ensure_session()` takes a per-uid
   lock and asks `/run/ghost/<uid>-seat.sock` for `STATUS`. An answer
   is the user's open session: ghostd reuses it, whatever session type
   was asked for, with the port and certificate fingerprint the reply
   carries, and sends `MintTokenRequest` with the login's ticket for the
   redirect token. `ENOENT` or `ECONNREFUSED` means there is none.
4. Then ghostd connects to `/run/ghost/seat.sock` and sends `Open`: the
   uid and username, the client address, the ticket, the password copy
   for the wallet, the session type and the port range. systemd starts a `ghostseat@.service` instance for the
   connection.
5. ghostseat verifies the ticket for that username and address (and
   that the uid is theirs), spends it, refuses if a seat already answers
   for the uid, binds `/run/ghost/<uid>-seat.sock` (root:ghost 0660) and
   `/run/ghost/<uid>.sock` (0600, owned by the user), then opens the
   session on the `ghostseat` PAM service with `PAM_RHOST` set to the
   client's address and `XDG_SESSION_TYPE=wayland`,
   `XDG_SESSION_CLASS=user` in the PAM environment. If PAM hasn't
   returned within 30 s, the start fails. It generates the session
   secret the redirect tokens are minted with (ghostd never sees it, so
   a compromised ghostd can't mint tokens for a session it opened), sets
   the PAM environment in
   the user manager (`systemctl --user set-environment`) and starts
   `wraith.service` through `systemctl --user --machine=<user>@`.
6. wraith connects to the control socket and receives `SessionInit`:
   the session secret, the session type, the port range, the sessions
   directory (ghostd.toml's `sessions.dir`, read by ghostseat itself
   and sent only if root-owned; [session profiles](../reference/session-profiles.md#search-order))
   and whether gamepads are available. It binds the first free UDP port in the range
   and replies `SessionReady` with the port and its certificate's
   fingerprint. With every port taken it replies `ControlError` with
   `HOST_FULL`, which ghostseat relays in its `GhostseatError` and
   ghostd passes on in its `LobbyError`. A failure anywhere here closes
   the logind session again and the instance exits.
7. ghostseat answers `Opened` with the port, the fingerprint and a
   token minted from the session secret (gdp-spec.md §4.8, the
   `authticket` crate) valid for 30 s; ghostd replies `Redirect{port,
   token, expiry, cert_sha256}` with `host` empty, meaning the lobby's
   own address. spectre-qt execs spectre with it.

The per-uid lock is what keeps two concurrent logins for one user from
opening two sessions: the second waits and takes the reuse path. Logins
for different users never contend. ghostd's `Open` waits up to 60 s,
the sum of ghostseat's own deadlines.

The port range is `sessions.port_base` through `port_base + max - 1`.
ghostd doesn't track which ports are in use; wraith skips a port only
when its bind fails. ghostd refuses to start if the range is below 1024,
runs past 65535 or contains the lobby port.

## Wallet and keyring unlock

A display manager unlocks KWallet or GNOME Keyring by running
`pam_kwallet5` or `pam_gnome_keyring` in both the auth phase (remember
the password) and the session phase (unlock with it) on one PAM handle.
ghost's auth and session run in different processes, so:

- ghostd keeps a copy of the first echo-off answer it relays to
  ghostauth (applications can't read `PAM_AUTHTOK`, and the helper keeps
  nothing) and sends it in `Open`, over the socket, never argv or the
  environment.
- ghostseat runs `pam_authenticate` on its own stack, answering the
  password prompt with that copy. `pam_unix` there only loads it into
  `PAM_AUTHTOK` for the keyring modules; nothing in this stack can fail
  the login. Then it runs `pam_open_session`.
- The keyring modules' session halves leave a hand-off in the PAM
  environment (`PAM_KWALLET5_LOGIN`, the socket `pam_kwallet_init` feeds
  to `ksecretd`). ghostseat sets that environment, minus `pam_systemd`'s
  `XDG_*` variables, in the user manager before starting wraith.

Every copy of the password is zeroized after use, and ghostd,
ghostauth, ghostseat and veild are non-dumpable. A failure anywhere here leaves the
wallet locked and the login unaffected. A login that reattaches to an
open session opens nothing and unlocks nothing.

Through Veil, the same password reaches the host's own PAM prompt, so
the unlock works the same way ([Veil](veil.md)).

## Session identity

A ghost session is `Class=user Type=wayland Remote=yes` with no seat.
It never gets one, so anything gated on a seat or on polkit's
`allow_active` behaves as for any remote session. wraith and the desktop
run in `user@<uid>.service`, not in ghostseat's session scope, so
`sd_pid_get_session()` and the `XDG_SESSION_*` variables don't reach
them.

Device access follows from that: logind's `uaccess` ACLs never reach a
seatless session, so a session user needs the `render` group for the
GPU. Gamepads are created by ghostseat
([devices](audio-cursor-gamepad.md#gamepads)).

## What keeps a session alive

ghostseat's logind session is what starts `user@<uid>.service` and keeps
it running. ghostd never enables linger and never changes linger an
admin set. Ending the session is therefore a single lever, and
`loginctl terminate-session` works as an operator expects.

The cost is that ghostseat is a single point of failure: if it dies,
logind ends the session, the user manager stops and the desktop goes
with it. The next login starts a new session. A `systemd-logind` restart
does not end it.

ghostseat also reaps the children session modules leave behind
(`pam_kwallet5` forks one), but only between open and close, so a module
that waits for its own child still finds it. After closing, it sends
`SIGTERM` to anything left in its session scope: `ksecretd --pam-login`
never exits on its own and would hold the session in `State=closing`.

## Teardown

Every end of a session converges in the session's ghostseat: it closes
the logind session (`pam_close_session`), sends `SIGTERM` to whatever
the session modules left in the session scope, removes its two sockets,
reports `SessionEnded` to ghostd on `/run/ghost/events.sock` and exits.
ghostd forgets the session and tells Veil. Each step tolerates having
already happened.

What leads there:

- **The desktop ends** (logout from the desktop, the leader crashing,
  `systemctl --user stop wraith.service`). wraith exits when the session
  leader exits, and `wraith.service`'s
  `ExecStopPost=-wraith --session-ended /run/ghost/%U.sock` sends one
  `SessionEnded` frame carrying systemd's `$SERVICE_RESULT`,
  `$EXIT_CODE` and `$EXIT_STATUS`. The `-` keeps a failed report from
  failing the unit. ghostseat keeps the control listener open for the
  whole session to receive it.
- **spectre asks**: `LogoutRequest` (gdp-spec.md §7.10). wraith runs the
  profile's `LogoutExec=` if it has one, or else sends `SIGTERM` to the
  leader. A leader still alive after 30 s is killed. wraith then exits,
  closing the client with `SESSION_ENDED`. Holding a valid token is the
  only authorization.
- **ghostd asks**: `CLOSE` on `/run/ghost/<uid>-seat.sock`, for a
  console login ([below](#one-graphical-login-per-user)). The reply
  comes once the logind session is closed.
- **ghostd restarts.** Nothing ends. At startup ghostd scans
  `/run/ghost` for `<uid>-seat.sock` files and asks each for `STATUS`:
  an answer is an open session, found again; no listener is a stale
  file, removed. This is the only polling in the design: one request per
  socket file at startup. An event sent while ghostd was down is lost
  and corrected the same way.

A client disconnecting, closing its window or losing the network is not
a logout and touches none of this.

A ghostseat failure at login fails the login with
`SESSION_START_FAILED` (or wraith's own code), since going on would
start a desktop with no logind session; the instance closes whatever it
had opened and exits. One session per uid: an open session is reused
whichever type was requested.

## What ghostd remembers

Nothing that matters survives a ghostd restart, by design. Each
session's ghostseat is the source of truth and answers `STATUS` with
the uid, username, logind session id, session type, wraith's port and
certificate fingerprint, and whether a viewer is attached; the session
secret the redirect tokens are minted with never leaves ghostseat. In
memory ghostd keeps
the per-uid lock, the uids it has told Veil about (so `SessionEnded` is
news once), the viewers, and the last redirect to each session
(`SessionInfo.last_active_unix`, falling back to the open time).

`/var/lib/ghost/last-types.json` (ghostd's own, mode 0600, written
atomically) maps each uid to the session type it last started, used to
preselect the lobby's default. It lives outside `/run` so the choice
survives a reboot, and keeps a uid whose account is gone until it is
deleted by hand.

## The sockets

All in `/run/ghost`, root's, mode 1771 with group `ghost`
(`packaging/system/tmpfiles.d/ghost.conf`): ghostd creates its own
sockets there but, with the sticky bit, can't unlink ghostseat's; a
session user reaches their own control socket and lists nothing. Framed
as GDP frames are (gdp-spec.md §3.1), one request per connection:

| Socket | Owner, mode | Listener | Clients |
|---|---|---|---|
| `auth.sock` | root:ghost 0660, `ghostauth.socket` | a new ghostauth per connection | ghostd, veild |
| `seat.sock` | root:ghost 0660, `ghostseat.socket` | a new ghostseat per connection | ghostd |
| `<uid>-seat.sock` | root:ghost 0660 | that session's ghostseat | ghostd: `STATUS`, `CLOSE`, `MintToken`; a peer that is neither root nor in the `ghost` group is dropped |
| `<uid>.sock` | the user, 0600 | that session's ghostseat | wraith (`host/proto/control.proto`), which talks only to a root peer |
| `events.sock` | ghost 0600 | ghostd | ghostseat (root): `ViewerAttached`, `ViewerDetached`, `SessionEnded` |
| `ghostlogin.sock` | ghost 0600 | ghostd | ghostlogin (root) |

`ghostd -t` checks that `auth.sock` and `seat.sock` exist, so a missing
socket unit fails the start the way a broken configuration does.

## The control socket

`/run/ghost/<uid>.sock` carries `host/proto/control.proto` between
wraith and its ghostseat. The first connection is the startup handshake
(`SessionInit`, then `SessionReady` or `ControlError`). After that, each
request is its own connection, read with a 5 s limit:

- `SessionEnded` (no reply) ends the session.
- `CreateDevice` is answered with `DeviceCreated` and an fd as
  `SCM_RIGHTS`, or a `ControlError`. The fd is wraith's end of a socket
  pair, never the device's own fd
  ([devices](audio-cursor-gamepad.md#gamepads)).
- `ViewerAttached` / `ViewerDetached` (no reply) go to ghostd and on to
  Veil.

Anything running as the user can connect to it, so ghostseat checks
what it is asked for, and refuses any other uid by `SO_PEERCRED`. wraith
checks the other way too: it takes `SessionInit` (the session secret and
the sessions directory) only from a peer that is root, so nothing that
could bind at the socket's path in ghostseat's place is believed. The
uid is the boundary by design: a secret only wraith knew would have to
live where the user can read it, and the user can stop and replace
wraith through a user-level drop-in anyway. What an impostor gets stays
in the user's own session: a capped gamepad, its own session closed,
a wrong viewer state in Veil's listing, or (by racing wraith to the
handshake) a session secret that only verifies tokens for that session.

## One graphical login per user

A user never has a ghost session and a graphical session on one of the
host's seats at once. The two would share the user manager,
`~/.config` and the keyring: concurrent kwins rewrite
`kwinoutputconfig.json` whole, and one session's `pam_gnome_keyring`
close can stop the daemon the other uses. The console always wins.

**A console login ends the ghost session.** `ghostlogin` is a PAM
account hook (`account optional pam_exec.so quiet .../ghostlogin`). On
Debian, `packaging/system/pam-configs/ghostlogin.in` puts it in
`common-account` at priority 192, above sss's `account sufficient
pam_localuser.so` (128), which would otherwise end the stack first.
Elsewhere it is added to the display managers' stacks by hand.

- It acts only for display-manager services (`sddm`, `gdm-password`,
  `lightdm` and their variants, or the services given as arguments):
  `common-account` also covers `sudo`, cron and ghostd's own login.
- The account phase runs after authentication, so a wrong password at
  the console ends nothing.
- GNOME's lock screen authenticates through `gdm-password` too, without
  a `PAM_TTY`; the greeter always has one. The hook skips
  `gdm-password` with an empty `PAM_TTY`.
- It sends `LocalLoginRequest` on `/run/ghost/ghostlogin.sock` (ghostd's
  own; ghostd accepts only root) and waits up to 25 s for the reply.
- It never refuses a login: failures go to syslog
  (`journalctl -t ghostlogin`) and it exits 0. With ghostd not
  listening, it runs `loginctl terminate-session` on the user's
  ghostseat sessions itself.

ghostd's `SessionManager::end_for_local_login()` then:

1. records the uid, so the lobby refuses it for 30 s even before logind
   lists the new console session (the display manager opens it only
   after the hook returns). The hold is skipped when the user already
   has a local session (GDM re-authenticating into a locked one), and
   dropped once the lobby sees logind list one: that session refuses
   GDP logins itself, and logging it out frees the uid at once;
2. under the uid lock, sends the session's ghostseat `CLOSE` with
   `logout` set. ghostseat sends wraith `SIGUSR1`: wraith closes the
   viewer with `ENDED_BY_LOCAL_LOGIN`, refuses reconnects with the same
   code, and logs the desktop out as for a `LogoutRequest`;
3. ghostseat waits 10 s for `wraith.service` to stop, then stops it and
   waits 5 s more;
4. closes the logind session whether or not wraith stopped, and only
   then replies, so the console login goes on with the session gone.

spectre exits with status 3 on that close code, and spectre-qt says why.

**A remote login is refused while the console is in use.** After PAM,
and again under the uid lock in `ensure_session()`, the lobby refuses
with `LOCAL_SESSION_ACTIVE` when the user has a logind session that is
on a seat, class `user`, type `x11`, `wayland` or `mir`, not remote and
not `closing` (a logged-out session with stray processes can stay
`closing` indefinitely). Text VTs and SSH don't count. A failed logind
query lets the login through: this is a policy, not a security
boundary.

The cost: a user who left the office machine logged in and locked
can't get in remotely until someone logs them out there.

## polkit

`packaging/system/polkit/49-ghost-sessions.rules` denies two families of
action to any user with a ghostseat session:

- `org.freedesktop.NetworkManager.network-control`, which would ask for
  admin authentication (a dialog no one can answer) and is host-wide;
- `org.freedesktop.login1.{power-off,reboot,halt,suspend,hibernate}` and
  their variants. logind allows these to a sole active session without
  authentication, which would let a desktop's own shutdown menu power
  off the host.

`subject.session` is always null for these sessions, so the rule finds
them through `subject.user` and logind's files under
`/run/systemd/sessions/`. It must not call `loginctl` or `busctl`: for
the login1 actions it runs inside logind's handling of the call, and a
synchronous call back into logind deadlocks it.

## Running ghostd

ghostd needs its two socket units and runs as the `ghost` user with a
full sandbox (no capabilities, `ProtectSystem=strict`, a system-call
filter, Unix and IP sockets only): it opens sockets, reads
`/etc/ghost`, runs `loginctl`, writes its own sockets into `/run/ghost`
(root's, made by tmpfiles.d, so the sessions' sockets stay across a
restart) and writes its state directory (`StateDirectory=ghost`).
For development, run it the same way:
`sudo systemd-run --unit=ghostd-dev --collect --uid=ghost --gid=ghost
-- <path>/ghostd ...`, with the socket units installed and active.

`ghostseat@.service` has no sandbox: an instance needs PAM, logind,
`systemctl --user --machine=`, `/dev/uinput` and `/dev/uhid` as root.
`pam_systemd` moves it into its session scope, where logind wants a
session leader, which is also why ghostseat can't be started from a
terminal: a leader already inside a session gets no session.
`ghostauth@.service` is sandboxed like ghostd, with `RuntimeMaxSec` as
a backstop for a caller that never finishes. Settings are in
`/etc/ghost/ghostd.toml` ([configuration](../reference/configuration.md)).

## Session types

The lobby offers the `sessions.d/<id>.conf` profiles whose `TryExec` (or
`Exec`'s first word) is on root's `$PATH`; `/etc/ghost/sessions.d`
overrides the shipped directory per id, and an empty file or
`Enabled=false` hides a type. ghostd only lists and validates ids;
wraith resolves the id and launches it. Format:
[session profiles](../reference/session-profiles.md).

## Limitations

- ghostseat is a single point of failure for its session.
- `pam_faillock` in the `ghostd` and `veild` stacks records nothing,
  since ghostauth is unprivileged; ghost's penalties stand in.
- A user logged in at the console can't log in remotely until that
  session ends.
- Usernames are limited to `[A-Za-z0-9._$-]`, 256 bytes: ghostseat
  refuses any other, since the name goes into
  `systemctl --user --machine=<name>@`, where systemd reads `@` and `:`
  as separators. An sssd fully-qualified name (`user@domain`) can't log
  in; use a short name (`use_fully_qualified_names = false`).
- A session token isn't bound to the TLS connection. It is single-use,
  so a token that leaks before spectre presents it can still be used
  once, within its 30 s; spectre then fails with `AUTH_FAILED`.
