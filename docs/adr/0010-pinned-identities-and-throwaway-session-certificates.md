# 0010. Pinned identities; the host key never reaches wraith

Status: Accepted

## Context

Clients must know they are talking to the right host before sending a
password. Most hosts have no CA-issued certificate. wraith, which serves
the session itself, runs as the session's user: any key it can read, the
user can read.

## Decision

- A host's lobby certificate is self-signed and pinned by fingerprint,
  trust on first use (or accepted by CA chain when it has one, without a
  pin). Its key is root-only and only ghostd reads it.
- Each wraith generates a throwaway certificate at startup, and ghostd
  vouches for it in `Redirect.cert_sha256` over the already-trusted
  lobby connection. spectre requires exactly that certificate.
- Hosts and Veil pin each other's certificates at join.

## Consequences

- A user can impersonate at most their own session, never the host.
- A host joined to Veil keeps a stable self-signed certificate, since
  Veil pins it; CA-issued certificates suit standalone hosts and Veil's
  client-facing lobby.

## Rejected alternatives

- **Sharing the host key with wraith.** Any user could extract it and
  impersonate the host to other users to collect their passwords.
