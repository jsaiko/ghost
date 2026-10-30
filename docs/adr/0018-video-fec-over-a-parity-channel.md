# 0018. Video FEC: Reed-Solomon parity on a channel of its own

Status: Accepted (2026-10-30)

## Context

Every lost video slice loses its frame, and every lost frame costs a
keyframe after the network profile's repair interval. On a link with
random loss that repair traffic, not capacity, limited the bitrate: on
the netem harness's `broadband` profile (0.1% loss) a session settled
near 10 Mbps of a 50 Mbit link and sent 27 keyframes in 30 s, and on
`wifi` (1.5%) p99 frame gaps reached 113 ms.

## Decision

- **Parity on datagram channel 0x04**, under the capability `fec`
  (gdp-spec.md §9.7). Data slices are unchanged.
- **Systematic Reed-Solomon over GF(2⁸), Cauchy generator**, in libgdp
  (`gdp/fec.hpp`) with its own SSSE3, AVX2 and NEON multiply-add, chosen
  at run time.
- **Blocks of at most 64 data shards, slices dealt round-robin** into
  them, so a burst spreads over every block and encode stays cheap (a
  1 MB frame's 20% parity takes 0.6 ms with AVX2).
- **Parity after the frame's data**, so a frame that arrives whole is
  decoded without waiting for it.
- **The host sizes parity from the client's slice loss before recovery**
  (`StreamStats.slices_lost`, `slices_expected`), never below the
  network profile's floor, none on a LAN until loss shows; the encoder
  gets the rate controller's target less the parity.
- **Keyframe repair stays** for frames parity couldn't save.

## Consequences

- On the harness, `broadband`: 10.3 → 16.8 Mbps settled, 27 → 2
  keyframes; `wifi`: 26 → 5 keyframes, p99 frame gap 113 → 34 ms;
  `mobile`: 15 → 2 keyframes; `lan`: no parity sent, unchanged.
- The reassembler keeps up to three frames in progress and completes
  them in order, so a late slice or parity shard of one frame still
  counts after the next one starts.
- Slices are 3 bytes shorter with `fec` in effect, so a parity datagram
  (whose header is longer) fits the same limit.
- The browser client doesn't offer `fec` yet: its sessions behave as
  before.
- A burst longer than a block's parity still costs a keyframe.

## Rejected alternatives

- **A flag bit on video datagrams for parity.** §9.1 has receivers ignore
  reserved flag bits, so an older client would take parity for data.
- **A vendored library (nanors, ISA-L).** The code is a few hundred lines;
  ISA-L is awkward to build for Windows and macOS, and either is a
  dependency to track for what a table-lookup multiply-add does.
- **GF(2¹⁶) (Leopard) for one block per frame.** Large blocks cost more to
  encode per parity shard, and round-robin blocks of 64 already spread
  bursts.
- **Retransmitting lost slices (NACK).** Costs a round trip per loss;
  parity costs none, which is what matters over the internet.
- **Contiguous blocks.** No cheaper than round-robin, and a burst then
  falls on one block.
