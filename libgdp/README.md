# libgdp

The shared GDP protocol library (`gdp-spec.md` is the wire format it
implements), linked into wraith (`host/wraith/src/session/gdp_session.{hpp,cpp}`),
spectre (`client/spectre/src/net/session_client.{hpp,cpp}`) and spectre-qt (the
lobby client).

## What's here

- `include/gdp/datagram.hpp` / `src/datagram.cpp`: the 16-byte video and
  6-byte audio datagram headers (gdp-spec.md §9, §10),
  `slice_payload_for()` (a connection's max datagram size -> video slice
  payload, `kFallbackDatagramPayload` while it's unknown), and
  `slice_video_frame()` (one coded frame -> N datagrams, into a
  caller-supplied send callback). Pure wire encode/decode, no allocation.
- `include/gdp/video_reassembler.hpp` / `src/video_reassembler.cpp`:
  `VideoFrameReassembler`, the receiving end of `slice_video_frame()` --
  one in-progress frame per stream_id, newer frame_id evicts older.
  Separate from `datagram.hpp` because it allocates.
- `include/gdp/framing.hpp` / `src/framing.cpp`: length-prefixed protobuf
  framing for the lobby/control/input streams (gdp-spec.md §3.1).
  `FrameReader` accumulates bytes from a reliable stream and yields
  complete frames as they arrive; `feed_and_drain()` is the whole usual
  on_data handler in one call. `Stream::send_message()` (transport.hpp)
  is the sending counterpart.
- `include/gdp/clock.hpp` / `src/clock.cpp`: `monotonic_us()` (the raw
  process clock, for `client_time_us` and local timing), the session
  clock (gdp-spec.md §11), and wraparound-safe `pts_diff()`.
- `include/gdp/clipboard.hpp` / `src/clipboard.cpp`: the clipboard-sync
  rules (gdp-spec.md §7.9) -- the `"clipboard"` capability
  name, the `"text/plain"` wire mime and 1 MiB − 1 KiB cap, the text mime
  preference/offer order and Latin-1 `STRING` transcode the compositors
  need, and `ClipboardEcho`, the "is this a fresh local change or just
  the peer's own content coming back" check. Shared so wraith's four
  backends and spectre's SDL path can't disagree about any of it.
- `include/gdp/refine.hpp` / `src/refine.cpp`: the lossless refinement
  container (gdp-spec.md §9.5) -- the `"refine"`
  capability name, `refine_pack_frame()` / `refine_parse_frame()`, and the
  receive-side size cap. Zstd-backed.
- `include/gdp/gamepad.hpp`: the `"gamepad"` capability name and the wire
  order of `GamepadState`'s axes and buttons (gdp-spec.md §8.5).
- `include/gdp/negotiation.hpp` / `src/negotiation.cpp`: codec and
  capability negotiation (gdp-spec.md §6.6, §6.7); `include/gdp/video_codec.hpp`
  / `src/video_codec.cpp`: the video codec tokens.
- `include/gdp/audio_format.hpp`: `AudioFormat`, the session audio format
  (gdp-spec.md §10) shared by SessionAccept, wraith's capture/encode
  pipeline, and spectre's decoder/player, plus the `"opus"` token.
- `include/gdp/version.hpp`: the ALPN string and `LobbyHello`'s wire version.
- `include/gdp/error_codes.hpp` / `src/error_codes.cpp`: gdp-spec.md §12's
  error codes and their human-readable text.
- `include/gdp/cert_fingerprint.hpp` / `src/cert_fingerprint.cpp`: SHA-256
  and the certificate fingerprint helpers host pinning uses (gdp-spec.md
  §2.3).
- `include/gdp/lobby_client.hpp` / `src/lobby_client.cpp`: `LobbyClient`,
  the client side of the lobby phase (gdp-spec.md §4), which spectre-qt
  drives.
- `include/gdp/client_connection.hpp` / `src/client_connection.cpp`:
  `ClientConnection`, the base for client-role objects (`LobbyClient`
  here, `SessionClient` in spectre): owns the `Transport` + `Connection`,
  exposes `notify_fd()`/`dispatch()`, and turns state changes into
  `on_connected()`/`on_connection_shutdown()` hooks.
- `include/gdp/transport.hpp` / `src/transport/`: QUIC over ngtcp2, with
  OpenSSL (3.5+) for TLS through `ngtcp2_crypto_ossl` -- `Transport`
  (client `connect()` / server `listen()`), `Connection` (streams,
  datagrams, and `rtt_us()` -- ngtcp2's smoothed RTT, for `StatsReport`'s
  RTT field per gdp-spec.md §7.5), `Stream`. Datagrams wait in libgdp's
  own queue (priority ones first, the rest in order behind the optional
  pacer) until ngtcp2 packs them, so `datagram_stats()` is exact:
  queued/sent/acked/lost, and how long the oldest unsent one has waited --
  the send backlog wraith's rate control watches. `max_datagram_size()`
  follows path MTU discovery, which probes up to a 1500-byte MTU.
  `Transport::set_congestion_control()` picks CUBIC (the default) or BBR
  for later connections. Each `Transport` runs one network thread
  (`net_loop.cpp`) that owns its sockets (`udp_socket.cpp`) and every
  `ngtcp2_conn` (`quic_conn.cpp`); a listener's connections share its
  socket and are found by connection ID (`server_demux.cpp`). See the
  comment at the top of `transport.cpp` for the threading model: every
  `on_*` callback the application sees fires from `Transport::dispatch()`,
  called on whatever thread the host chooses whenever it pumps its own
  event loop. `GDP_QUIC_LOG=1` in the environment prints ngtcp2's own
  trace to stderr.
- `libgdp/proto/lobby.proto`, `libgdp/proto/session.proto` (also compiled by the
  host/ Rust workspace): the actual message schemas, per
  gdp-spec.md §4, §6, §7 and §8.

## Build

Requires protobuf (`protoc` + dev headers), ngtcp2 with its OpenSSL crypto
helper, and OpenSSL 3.5+ (`apt install libngtcp2-dev
libngtcp2-crypto-ossl-dev libssl-dev` on Ubuntu 26.04). On Windows and
macOS they come from vcpkg: `ngtcp2[openssl]` and `openssl`, through the
manifests in `packaging/windows/` and `packaging/macos/`. Static and
dynamic triplets both work.

The transport is portable below `src/transport/socket_platform.hpp`: the
network loop polls with `ppoll` (Linux), `poll` (macOS) or `WSAPoll`
(Windows), and `udp_socket_posix.cpp` / `udp_socket_win.cpp` are the two
socket implementations. On Windows there is no batched receive and no
source-address pinning; only clients run there.

```sh
cmake -S .. -B ../build -G Ninja
cmake --build ../build
ctest --test-dir ../build
```

## Tests

- `tests/datagram_test.cpp`, `tests/framing_test.cpp`,
  `tests/negotiation_test.cpp`, `tests/clipboard_test.cpp`,
  `tests/refine_test.cpp`, `tests/cert_fingerprint_test.cpp` (SHA-256
  against the NIST vectors): plain-`assert()` unit tests, no external
  framework.
- `tests/transport_test.cpp`: a real loopback client/server test over
  the QUIC transport (self-signed cert generated via the `openssl` CLI at test start) --
  connects, opens both streams, round-trips a message on the control
  stream, sends a datagram, confirms the server identified which
  stream was which by QUIC stream ID rather than arrival order, and checks
  the client sees the server certificate's fingerprint (what host pinning
  compares, gdp-spec.md §2.3) exactly as `openssl` computes it. This is
  the one piece of libgdp that can't be meaningfully unit-tested any other
  way, so it's the test that actually exercises `transport.hpp`'s contract.
- `tests/ca_trust_test.cpp`: `CaTrust` (gdp-spec.md §2.3) against a
  private CA the `openssl` CLI makes at test start: a certificate it
  issued verifies for the name dialed (an address or a DNS name), and a
  wrong name, a self-signed certificate, no `CaTrust`, the system store
  alone and an unreadable CA file all don't -- with the handshake
  completing every time.
- `tests/stream_loss_test.cpp`: stream integrity under loss. A client and
  server talk through an in-process UDP relay that drops, reorders and
  duplicates packets both ways, and each pushes 8 MiB of seeded bytes at
  the other with datagrams interleaved; every byte is checked. It guards
  the send buffers the transport keeps until the peer acks them.
- `tests/lobby_client_test.cpp`: `LobbyClient` against a running ghostd.
  Not a ctest, since it needs that separate process; see its header comment
  for how to run it.

libFuzzer targets cover what untrusted bytes reach: the hand-written
parsers (`fuzz/datagram_fuzzer.cpp`, `fuzz/refine_fuzzer.cpp`), the
session control messages (`fuzz/session_control_fuzzer.cpp`), and the QUIC
server's packet handling before any TLS -- routing, accepting, Retry,
version negotiation, stateless reset (`fuzz/server_demux_fuzzer.cpp`). CI
runs each for a minute.

```sh
cmake -S .. -B ../build -G Ninja -DGDP_ENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build ../build --target gdp_datagram_fuzzer gdp_refine_fuzzer
../build/bin/gdp_refine_fuzzer -max_total_time=60 /tmp/corpus
```

(`GDP_ENABLE_FUZZING` needs clang -- libFuzzer is a clang runtime feature --
so it's off by default and best done in a separate build directory from the
regular gcc one.)

## Known gaps

- `Transport::listen()` always binds the "any" address (dual-stack where
  the host has IPv6); the bind address isn't configurable.

## License

MIT ([LICENSE](LICENSE)), unlike the rest of ghost (GPL-3.0-only), so any
client or host can link it.
