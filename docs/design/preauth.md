# Pre-authentication limits

The lobby is the one network-facing surface that runs before
authentication, so it uses sshd's defences with sshd's names and
defaults. It runs unprivileged, and PAM runs in a separate process
([authentication](login-and-sessions.md#authentication)); these limits
are what stands between the network and that process. The same code
(the `preauth` crate) protects veild's lobby, host channel, thin-client
channel and web sign-in. Settings are in `[auth]` of ghostd.toml and
veild.toml.

| Key | sshd option | Default |
|---|---|---|
| `permit_root_login` | `PermitRootLogin` | `false` |
| `permit_empty_passwords` | `PermitEmptyPasswords` | `false` |
| `login_grace_time` | `LoginGraceTime` | `"2m"` (0 is no limit) |
| `[auth.max_startups]` `start`, `rate`, `full` | `MaxStartups` | 10, 30, 100 |
| `[auth.penalties]` `enabled`, `authfail`, `noauth`, `grace_exceeded`, `max`, `min` | `PerSourcePenalties` | `true`, `"5s"`, `"1s"`, `"10s"`, `"10m"`, `"15s"` |

## Login policy

- **Empty passwords** are refused as sshd refuses them:
  `pam_authenticate` gets `PAM_DISALLOW_NULL_AUTHTOK`, which `pam_unix`
  applies after its own arguments, so it overrides the `nullok` in
  Debian's `common-auth`.
- **Root** is refused after PAM succeeds and answered with the same
  `AUTH_FAILED` as a wrong password, so a correct guess at root's
  password gets no confirmation. The check is uid 0, not the name.
- **The grace time** runs from the QUIC handshake to PAM's answer. After
  it, only an authenticated user can hold a connection, and then only
  for the ticket's ten minutes: a `SessionOpen` that hasn't arrived by
  then ends the connection with `SESSION_START_FAILED`. One uid may have
  four such connections waiting; a fifth gets `HOST_FULL`.
- **Frames** before authentication are capped at 16 KiB, as wraith caps
  its own unauthenticated peers.

## MaxStartups

Counts connections from accept to PAM's answer. From `start` on, a new
connection is refused with probability `rate`%, rising linearly to 100%
at `full` (sshd's random early drop). Refusal is `Incoming::refuse()`,
before any crypto. Once dropping has started, a client whose address
isn't validated is sent a QUIC Retry first, so spoofed Initial packets
can't fill the count. This is QUIC's counterpart of the SYN cookies that
protect sshd.

## PerSourcePenalties

Following OpenSSH's `srclimit.c`, each offense adds its time to the
source's penalty, capped at `max`. Once more than `min` is outstanding,
the source is refused before the handshake until the penalty runs out.

| Offense | When |
|---|---|
| `authfail` | Authentication failed, or a root login was refused. |
| `noauth` | The client disconnected or broke the protocol before answering a prompt. |
| `grace_exceeded` | `login_grace_time` ran out. |

- Offenses are recorded only after the handshake completes, when the
  address is proven; otherwise a spoofer could get someone else's
  address blocked.
- Sources are IPv4 addresses and IPv6 /64s. sshd uses /128 by default,
  which one host can sidestep within its own /64.
- With `pam_unix`'s ~2 s failure delay, one source gets about eight
  guesses before its first block. This slows guessing; it doesn't
  replace good passwords. `pam_faillock` in the `ghostd` and `veild`
  stacks records nothing, since ghostauth runs unprivileged, so these
  penalties are what there is.
- At most 65536 sources are tracked; past that, new sources aren't,
  which is sshd's default "permissive" overflow.
- On a login relayed by Veil, ghostd penalises the client's address as
  Veil reports it, never Veil's own.

## Not ported

- `AllowUsers` / `AllowGroups`: use `pam_access` or `pam_succeed_if` in
  the PAM stack.
- `MaxAuthTries`: there is one PAM transaction per connection.
- `PerSourcePenaltyExemptList`.
