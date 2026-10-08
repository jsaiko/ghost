# 0009. Stock ngtcp2 congestion control, with ghost's own pacing

Status: Accepted

## Context

GDP runs over QUIC (ngtcp2, which replaced msquic). wraith has its own
rate controller for the encoder, but each QUIC connection also has a
congestion controller underneath. ngtcp2 has no hook for a custom
controller. Keyframes leave as bursts, and a burst faster than a slower
hop on the path (a 10G host behind a 2.5G switch port, a Wi-Fi access
point) is dropped there, which costs another keyframe.

## Decision

- Use ngtcp2's own controllers unpatched: CUBIC by default, BBR through
  `[network] congestion_control`. The choice is per host and fixed when
  a connection is accepted.
- Pace video in libgdp, in front of QUIC's datagram queue, at a multiple
  of the rate controller's target (never below a floor), and at the
  path's measured rate once spectre's frame-train timing gives an
  estimate. Never faster than the estimate.

## Consequences

- No vendored or patched QUIC stack to maintain.
- The congestion controller can't follow the network profile, which is
  only known after accept.
- Pacing above the path's measured rate was tried and loses: a backlog
  drains at the pacing rate, and anything faster than the path fills a
  shallow router queue until it drops.

## Rejected alternatives

- **Patching or vendoring ngtcp2** for a controller of ghost's own.
- **A pacing gain above 1** over the estimate, or letting a frame's first
  part go faster to probe the path: both lost far more datagrams in
  shaped tests.
