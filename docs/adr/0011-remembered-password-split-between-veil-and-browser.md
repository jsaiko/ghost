# 0011. The browser's remembered password is split

Status: Accepted

## Context

A browser user signs in to Veil with their password, then picks a host.
The host runs its own PAM and needs the password again, and ghostseat
needs it to unlock the user's wallet. Asking twice is poor; keeping the
password in veild is a liability (core dumps, swap, VM snapshots,
backups).

## Decision

At sign-in veild seals the password with a fresh random key
(ChaCha20-Poly1305), keeps only the ciphertext, in memory, and gives the
key to the browser in an `HttpOnly`, `SameSite=Strict` cookie scoped to
`/api`, keeping no copy. A host login brings the two together and wipes
the result. The pair is bound to the signing-in address, and ends at
sign-out, a new sign-in, a veild restart or `[web] remember_password`.

## Consequences

- Neither veild's memory, its database nor the cookie alone yields the
  password.
- A stolen pair of cookies used from the same address can log in to the
  user's hosts. Accepted.
- A host that refuses the remembered password is never offered it again
  in that session, so drift can't trip a host-side lockout.

## Rejected alternatives

- **Asking for the password at every host.** Works, but every login
  becomes two password prompts.
- **Keeping the password in veild** in memory or the database.
