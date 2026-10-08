# 0007. Refinement: damage marks activity, the hash decides change

Status: Accepted (2026-09-27)

## Context

Lossless refinement sends a tile losslessly once it has stopped
changing. It must not send tiles under playing video (they would settle
on skip macroblocks and chatter), and must never leave stale lossless
pixels on the client. Compositor damage is available but imperfect:
headless GNOME's damage misses real changes (a dragged window's vacated
area, a caret a tile away from its rect), and mutter and KWin damage a
surface without subtracting what covers it.

## Decision

Every real frame hashes every tile, and the hash alone decides when a
tile has changed (and gets a clear). Damage only marks a tile active, so
it doesn't settle while damaged. Tiles are 16 px, settle after 80 ms of
wall-clock time, and loss is repaired per frame from the sent history
rather than by periodic re-sends.

## Consequences

- No stale lossless pixels from missed damage; a missed damage only lets
  a tile settle a little early.
- A terminal over a playing video stays lossy where they overlap.
  Accepted.

## Rejected alternatives

- **Damage gating the hashing.** Leaves stale lossless tiles where
  damage is missed.
- **Pixel-only video detection** (a motion rule holding neighbouring
  tiles, bounding boxes of connected motion, a stillness cap), tried in
  several variants and rolled back on 2026-09-27: it fixed the
  terminal-over-video case but couldn't tell a video's still patches from
  still content, and at 4K chattered through tens of thousands of tiles
  every few seconds.
- **8 px tiles.** Four times the bookkeeping, with no alignment to the
  codec's macroblock.
- **A per-tile churn backoff.** A tile holds about two characters, so
  ordinary typing would hold the line being typed lossy.
- **A periodic re-send of every tile.** Repair from loss reports covers
  the real divergences; a 64-bit hash collision is not one.
