# Transport and rate control

How a session's video and audio reach the client and how wraith decides
how many bits to spend. The wire format is gdp-spec.md §2 (transport),
§6–§10 (session); the shared implementation is libgdp
(`libgdp/src/transport/`, over ngtcp2). wraith's side:
`host/wraith/src/session/gdp_session.*`, `rate_controller.*`,
`path_rate_estimator.*`.

## The session connection

- One QUIC connection, ALPN `gdp/1`: two reliable streams (control,
  then input) and unreliable datagrams for video and audio.
- A connection becomes the session's viewer only once its
  `SessionHello` token checks out. Until then it waits in a short list
  of pending connections, two per source address, with a deadline for
  its hello, so a peer that completes the handshake and idles can't hold
  the session and one address can't keep evicting another's reconnect.
  Every failure before authentication closes with `AUTH_FAILED`.
- One viewer at a time. A second connection with a good token gets
  `ALREADY_CONNECTED`, unless its hello asks to take over; then the old
  viewer is closed with `TAKEN_OVER`.
- `SessionHello` / `SessionAccept` negotiate the codec, the capabilities
  (`clipboard`, `refine`, `gamepad`, `microphone`), the output size and
  the network profile. The codec is the first of the client's
  preferences that wraith can actually open, which it finds by trying.
  wraith decides its capabilities per session
  (`GdpSession::supported_capabilities()`).
- Video is sliced to the largest datagram the connection currently
  allows (`gdp::Connection::max_datagram_size()`). That follows QUIC's
  path MTU discovery: a little under QUIC's 1200-byte minimum at
  connect, 1432 once a 1500-byte path is confirmed. Audio datagrams are
  sent ahead of any queued video.

## Rate control

`RateController` sets the encoder's bitrate from what the transport and
spectre measure. spectre sends a `StatsReport` every 250 ms. Any of
three signals counts as saturation:

- **Queuing delay.** spectre timestamps each frame's first slice on its
  own session clock (zeroed at `SessionAccept`, as wraith's is) and
  reports the window's minimum and average of arrival minus pts. The
  clocks' offset is unknown but constant, so wraith compares the
  window's minimum against a 10 s running minimum: a standing queue
  delays every packet, jitter only spreads them. Without receiver
  timestamps it uses the QUIC RTT the same way.
- **Send backlog.** QUIC holds datagrams back for congestion control,
  and libgdp queues the rest, so a link slower than the encoder backs up
  inside wraith first. libgdp reports how long its oldest unsent
  datagram has waited (`Connection::datagram_stats()`).
- **Loss**, as QUIC declared it, but only alongside rising delay or
  skipped frames, or past 10%: a radio link drops packets at random, and
  cutting for that only starves the stream.

On saturation the target drops to 0.85x, or to 0.9x what spectre
received when the encoder was filling its target. A backlog with no
network queue behind it comes down only to what QUIC managed to send. A
cut holds for 500 ms before the next. Otherwise the target climbs at the
profile's rate, a quarter of that near the last rate that caused
trouble, and only while the encoder is really sending near its target:
an idle desktop proves nothing about headroom. Delay that survives two
cuts unchanged is taken as the path's base moving.

**Network profiles** (`SessionHello.network_profile`):

| | floor | start | delay | loss | climb | backlog | repair |
|---|---|---|---|---|---|---|---|
| `lan` | 2 Mbps | ceiling | 10 ms | 1% | 50%/s | 25 ms | immediate |
| `internet` | 1 Mbps | 8 Mbps | 25 ms | 2% | 15%/s | 50 ms | 300 ms |
| `mobile` | 0.5 Mbps | 4 Mbps | 50 ms | 5% | 8%/s | 80 ms | 600 ms |

`auto` (the default) starts as `lan` if the handshake RTT is at most
5 ms, else `internet`, and re-decides on every report: sustained loss
over 3% or delay jitter over 10 ms is `mobile` (left again below 1.5%
and 5 ms); a base RTT over 5 ms or loss over 0.8% is `internet`. A new
verdict must hold for 2 s. The ceiling is wraith.toml's
`encode.max_bitrate_mbps` (or PyroWave's, see [PyroWave](pyrowave.md)).

## The send-queue gate

Before each encode, `SessionServices::admit_frame()` asks whether the
oldest unsent datagram has waited longer than the profile's backlog
limit. If so, the frame is skipped rather than queued behind it. A 4 ms
poll has the host re-deliver the current picture
(`SessionHost::redeliver_frame()`) once the backlog clears, since a
desktop that has gone still would otherwise never send what the skipped
frame carried.

## Lost frames and keyframes

A lost frame (or one spectre discarded) corrupts every frame that
references it, so it gets a keyframe, unless one already sent or
requested comes after it, and no sooner than the profile's repair
interval after the previous repair. Every loss costs a keyframe: there
is no FEC, intra refresh or reference invalidation. Lossless
refinement's layer is repaired separately and at once
([refinement](refinement.md#loss)).

A keyframe request also has the host re-encode the current picture,
since a still desktop produces no next frame to carry it.

The periodic keyframe (`encode.gop`) defaults to every 600 frames (10 s
at 60 fps). With every loss repaired on its own, it only bounds damage a
repair missed, and on a slow link each keyframe is a burst. H.264, H.265
and AV1 streams with a 600-frame GOP decode without error past their
8-bit frame_num / POC / order-hint wrap.

## Congestion control

Under wraith's rate control, each QUIC connection has its own congestion
controller: ngtcp2's CUBIC by default, or BBR with
`[network] congestion_control = "bbr"`. ngtcp2's CUBIC takes seconds to
regrow its window after a loss, so random loss holds its window small;
ngtcp2's BBR keeps datagrams queued longer at high rates. ngtcp2 has no
hook for a controller of ghost's own. The algorithm is fixed when a
connection is accepted, before `SessionHello` names a profile, so it
can't follow the profile.

## Pacing

A keyframe sent at the host's line rate overflows the buffer of any
slower hop on the way (a 10G host behind a 2.5G switch port, a Wi-Fi
access point), and each loss costs another keyframe, whose burst loses
again. So wraith paces video through
`gdp::Connection::set_pacing_rate()`, a token bucket in libgdp in front
of the QUIC stack's datagram queue: a millisecond's worth goes at once,
the rest at the set rate. Datagrams the pacer holds count as send
backlog; audio skips it. ngtcp2 also paces on its own.

The rate is `network.pacing_multiplier` (10) times the rate
controller's target, never below `network.pacing_floor_mbps` (500).

**Path rate estimate.** spectre times how each large frame's datagrams
arrive (`StreamStats.trains`); a frame sent faster than the slowest hop
arrives spread out to that hop's rate. `PathRateEstimator` keeps the
median of the last 7 samples, each valid for 3 s, and wraith paces at
that estimate: never above the fixed rate, never below the target.
Pacing at the estimate and no faster matters, because a backlog drains
at the pacing rate and anything faster than the path fills a shallow
router queue until it drops. A faster path is found through the target
instead: the rate controller climbs while reports are clean, pacing
follows it past the estimate, and frames that arrive as fast as they
were paced raise the estimate. With no fresh sample, pacing goes back to
the fixed rate.

## Testing

`client/spectre/tools/netem-harness` runs a real wraith against
`spectre_netprobe` (a headless client that decodes nothing and prints
arrival statistics) across a veth into a network namespace, shaped by
netem per profile (`lan`, `broadband`, `wifi`, `bufferbloat`, `mobile`,
`uplink20`, `uplink50`). wraith.toml's `network.rate_trace` logs every
report's measurements and target.

## Limitations

- Every lost frame costs a keyframe; on a link with random loss, that
  repair traffic, not capacity, limits the bitrate.
- One congestion controller per host, fixed at accept time.
- One output and one viewer per session.
