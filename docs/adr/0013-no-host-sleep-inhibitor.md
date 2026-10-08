# 0013. ghost doesn't inhibit host sleep

Status: Accepted (2026-09-28)

## Context

A host whose power policy suspends it when idle (logind's `IdleAction`,
or the console desktop's or greeter's power manager) suspends under a
running ghost session and kills it. ghost sessions themselves can't
suspend the host: the polkit rule denies them every login1 power action.

## Decision

ghost doesn't take a sleep inhibitor. Turning off idle suspend on a
session host is the admin's job, and the install guide says so.

## Consequences

- ghost never overrides a site's power policy.
- A host left with idle suspend on will suspend under sessions.

## Rejected alternatives

- **A logind `sleep` block inhibitor held by wraith for each session.**
  Needs a polkit grant for ghost sessions (the polkit rule otherwise only
  takes rights away), and lets a forgotten client keep a machine awake
  indefinitely.
