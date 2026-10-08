# 0004. ghostseat holds the logind session

Status: Accepted (amended 2026-10-07: systemd starts ghostseat, not ghostd)

## Context

A desktop needs a logind session (for `user@<uid>.service`, the keyring,
and anything that asks logind about the session). logind ties a session
to the process that opened it. ghostd serves every user, so if it held
the sessions, restarting ghostd would end them all.

## Decision

A small root process, ghostseat, runs per logged-in user: ghostd asks
for one through `/run/ghost/seat.sock` and systemd starts it
(`ghostseat.socket`), since ghostd itself runs unprivileged. It opens
the logind session through PAM and holds it until told to close.
ghostd finds it again after its own restart from a socket path derived
from the uid. ghostd never enables linger: ghostseat's session is what
keeps the user manager, and so the desktop, alive.

## Consequences

- Restarting ghostd ends no session.
- Ending a session is one lever: close ghostseat, or
  `loginctl terminate-session`.
- ghostseat is a single point of failure for its session: if it dies,
  the session ends. Accepted.
- ghostseat must start from its socket unit; started from a terminal
  it would land in the terminal's own session and `pam_systemd` would
  refuse to open another.

## Rejected alternatives

- **ghostd as the session leader.** A restart logs every user out.
- **Linger.** Keeps user managers alive with no session, makes teardown
  two levers, and changes a setting the admin may own.
