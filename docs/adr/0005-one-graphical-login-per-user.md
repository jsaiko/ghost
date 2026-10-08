# 0005. One graphical login per user

Status: Accepted

## Context

A ghost session and a graphical console session of the same user share
the user manager, `~/.config` and the keyring. Concurrent kwins rewrite
`kwinoutputconfig.json` whole, and one session's `pam_gnome_keyring`
close can stop the daemon the other uses. A console (seat0) session
can't be moved into wraith.

## Decision

A user never has both at once, and the console always wins.

- A console login ends the ghost session first: a PAM account hook
  (`ghostlogin`) asks ghostd, which logs the desktop out and closes the
  logind session before the display manager starts the console desktop.
- A remote login is refused (`LOCAL_SESSION_ACTIVE`) while the user has
  a graphical session on one of the host's seats.

Only graphical seat sessions count; text VTs and SSH don't.

## Consequences

- A user who left their office machine logged in and locked can't log in
  remotely until someone logs them out there. Accepted.
- The hook never refuses a console login, even when ghostd is down.

## Rejected alternatives

- **Remote wins** (ending the console session for a remote login).
  Kills someone's local work.
- **Letting both run.** Corrupts shared configuration and keyrings.
