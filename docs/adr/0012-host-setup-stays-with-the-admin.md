# 0012. Group membership and vgem stay with the admin

Status: Accepted (2026-09-19)

## Context

A ghost session is seatless, so logind's `uaccess` ACLs never reach it:
a session user needs the `render` group to use the GPU. KDE on a host
with no GPU needs the `vgem` module loaded before kwin can composite
with OpenGL. ghostd could do either at login.

## Decision

ghostd changes neither. It authenticates existing users and starts
their sessions; group membership is provisioned wherever the accounts
come from, and an admin who wants KDE on a GPU-less host loads `vgem`
themselves.

## Consequences

- ghostd never widens a user's privileges at login.
- A host missing the setup fails in a way the admin must recognise
  (documented in the install guides).

## Rejected alternatives

- **ghostd adding users to `render`/`video` at login.** Widens privileges
  the admin didn't grant.
- **ghostd loading `vgem`** on a host with no render node (the behaviour
  until 2026-09-19). vgem is a host-wide change: with a real GPU present
  it can be picked over the GPU, and unloading it under a session crashes
  Xorg.
