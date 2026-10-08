# 0008. Software encode is H.264 only

Status: Accepted

## Context

Hosts without a usable hardware encoder still need to serve sessions.
x264 is fast enough for desktop content; software HEVC and AV1 encoders
cost far more CPU per frame for the same latency.

## Decision

The software fallback is x264, H.264 only. H.265 and AV1 need a hardware
encoder (VA-API or NVENC); when none opens, negotiation moves on to the
client's next codec, and H.264 is every client's floor.

## Consequences

- Every session can run, at H.264, on any host.
- A host without hardware encode never offers H.265 or AV1. This is a
  decision, not a gap to close.

## Rejected alternatives

- **Software HEVC or AV1 encoders.** Too much CPU per frame for an
  interactive session on a shared host.
