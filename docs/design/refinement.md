# Lossless refinement

A lossless layer over the video for text and UI on a mostly still
screen. It is the `refine` capability (gdp-spec.md §7.8, container in
§9.5), not a codec: the base layer is the session codec's ordinary
stream, and each frame may carry byte-exact, Zstd-compressed tiles of the
source framebuffer at 3 bytes a pixel (`BGR8`, the only tile format
the container defines so far). The client keeps a persistent
overlay plane over the decoded video
(`client/spectre/src/present/lossless_plane.*`), so a refreshed region
stays exact until the host says it changed.

Code: `host/wraith/src/encode/refine/`. The tile policy is explained in
detail at the top of `tile_tracker.hpp`.

## Shape

`RefineEncoder` is a decorator: `create_encoder()` picks the base backend
exactly as it would without refinement, then wraps it when the session
negotiated the capability. `SessionAccept.encoder` reports
`vaapi+refine`, `software+refine` and so on. A session that didn't
negotiate it never creates the wrapper, so it pays none of the costs
below.

## Policy

Host-side only; the wire says only what the client must draw.

- **Tiles.** Every real frame, every 16x16 tile is hashed and compared
  with its previous hash. 16 px is H.264's macroblock, the base codec's
  own unit of change, so a caret or a typed character holds at most a
  15 px band lossy.
- **Moving and settled.** A tile whose content just changed gets a clear
  and rides the video. A tile inactive for the settle time (80 ms of
  wall-clock time, not a count of calls) is sent losslessly once and
  then costs nothing until it changes. Moving content never qualifies;
  a still desktop converges to bit-exact.
- **Damage decides activity, never change.** A tile the compositor's
  damage touched counts as active even when its bytes came out the same:
  decoded video leaves sky and background byte-identical for hundreds of
  milliseconds and would otherwise settle and then drop back to lossy,
  but the browser damages the whole video quad every frame. The hash
  alone decides clears, because compositor damage misses real changes
  (headless GNOME's misses the area a dragged window just left). The
  accepted cost: mutter and KWin damage a surface without subtracting
  what covers it, so a terminal over a playing video stays lossy where
  they overlap.
- **Per-frame cap.** At most 768 KiB of raw pixels (1024 tiles) per
  frame, filled in 8x8-tile blocks and resuming where the last frame
  stopped: 8 frames to paint a 1080p screen, 32 at 4K. Tiles are
  coalesced into rects (runs, then stacks of runs), as clears are, so a
  settling window is a handful of rects on the wire.
- **Bandwidth budget.** The rate controller sets the base layer's
  bitrate alone, and a screenful of settling tiles is 0.4–1.2 MiB on the
  wire at 1080p and 1.6–4.7 MiB at 4K, which on a slow link the
  controller would answer by cutting the video. So the layer gets a token
  bucket in wire bytes, refilled at `refine.bandwidth_percent` (50) of
  the current target bitrate, `refine.burst_ms` (500) deep, converted to
  a raw-byte cap before each call through a running wire/raw ratio.
  Tiles over it stay due for later calls.
- **Idle pump.** wraith calls the encoder only when it has a frame, so a
  desktop that stops changing would freeze the tracker mid-settle. While
  `Encoder::has_pending_work()` is true, `SessionServices` calls
  `Encoder::pump()` every 16 ms with the last frame; nothing is hashed or
  encoded, and the tiles that are due go out as a layer-only frame
  (`base_len` 0), which the client applies to the picture it has.
- **Order.** The client applies layers in arrival order, so with an
  asynchronous base (VA-API, NVENC, PyroWave) a layer-only packet goes out
  exactly where it was built: after every frame pushed before it and
  ahead of every frame pushed after it, whose layer may take its tiles
  back as changed. `RefineEncoder` numbers pushes and tracks frames in
  flight by that order, not by pts, which needn't increase from one push
  to the next (a re-delivered frame carries wraith's clock, not the
  compositor's); a staged layer expires by wraith's clock.
- **Per profile.** Every `[refine]` key can be overridden per network
  profile in `[refine.lan]`, `[refine.internet]` and `[refine.mobile]`.
  Built in, the slower profiles settle later (150 ms internet, 250 ms
  mobile), so text being typed is re-sent less often where bytes cost
  more. A built-in profile value beats `[refine]`, so `[refine]
  settle_ms` reaches only `lan`.

The 5 s statistics line counts `churned` tiles (cleared within a second
of being sent): the number to read when tuning.

## Pausing

The client's lossless refinement switch (spectre's menu row, the
browser client's Lossless toolbar button) sends `RefinePause`.
`RefineEncoder` then leaves the tracker alone, sends a reset layer to
wipe the client's plane, and wraps every later packet with an empty
layer; whether it wants CPU frames becomes the base encoder's answer, so
a hardware base goes back to zero-copy dmabuf input. Both input paths feed one encode session, so
neither switch needs an IDR. `SessionServices` asks for a fresh frame on
every switch, and every connection starts unpaused. A client that wants
it paused from the start (spectre `-R off`, which spectre-qt passes with
"Start with Lossless Refinement on" unticked, as a Wisp profile can)
sends `RefinePause` straight after the handshake.

## Loss

A reset (`kFlagReset`) costs a full repaint, so it is sent only when the
host can't know what the client's plane holds: a new client, a lost
reset, or a loss older than the sent history. Otherwise loss is repaired
per frame:

- `TileTracker::process()` reports what each layer committed to
  (`TileEmission`: tiles sent, clears, reset), and `RefineEncoder` keeps
  the last 512 by pts.
- When a `StatsReport` shows frames lost, `GdpSession` maps their frame
  ids to pts and calls `Encoder::frames_lost()`. `TileTracker::repair()`
  re-arms the lost tiles that are still current and re-issues the lost
  clears with the next layer, re-arming every tile under them. Clears
  are re-issued unconditionally: at worst a correct tile is wiped and
  re-sent, which costs bytes, never wrong pixels.
- The same repair covers a layer lost on the host (a failed base push or
  pack).
- The video's own keyframe is requested separately and rate-limited.
- Change too scattered for the container's 255 clears goes out as one
  bounding-box clear, never as a reset.
- spectre reports a frame it discarded as lost, and applies a frame's
  layer whether or not the decoder produced a picture for it.

There is no periodic re-send of every tile: a 64-bit hash collision is
not a realistic event.

## Cost

The tracker reads a frame only through a `TileSource`
(`encode/refine/tile_source.hpp`): one hash per tile every real frame,
and the pixels of the rects it sends.

- Where the frame is in host memory (a software or PyroWave CPU base,
  a memfd capture) it is `CpuTileSource`: about
  1 ms a 4K frame for the four-lane FNV-1a, split across `BandPool`.
- A hardware base on a screencast host keeps the frame on the GPU
  (`Encoder::takes_tiled_dmabuf()` / `push_tiled()`): `DmabufReader`'s
  GLES 3.1 compute shaders hash the captured dmabuf and copy out only the
  rects that go out (`screencast/dmabuf_tile_source.*`, about 0.25 ms a
  4K frame), and the base gets the dmabuf as is. This avoids a full
  read-back and upload per frame, which is especially slow on a VM with
  a passed-through GPU.
- The shader is a bit-exact port of the CPU hash, so a session that
  falls back mid-way (a failed GPU read drops `ScreencastHost` to
  read-back for good) sees no spurious change;
  `wraith_gpu_tile_hash_test` holds them to it. Its FNV offset bases are
  uniforms because Mesa 26 (radeonsi, ACO, gfx12) computes
  `(literal ^ x) * k` wrongly.
- Zstd (level 3) costs 0.3–0.6 ms per MiB of tiles. An idle pump tick
  hashes nothing.

## Limitations

- A terminal over a playing video stays lossy where they overlap.
