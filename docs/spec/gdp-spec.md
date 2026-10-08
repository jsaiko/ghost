# Ghost Display Protocol (GDP)

**Version 1.0.** ALPN `gdp/1`, wire version 1.

GDP carries a remote Linux desktop session over QUIC: authentication and
session selection, a low-latency video and audio stream, and the user's
input, clipboard and game controllers. This document defines every byte
that crosses the network between GDP's components and the rules for
producing and acting on them. It is normative. The protocol buffer
definitions in `libgdp/proto/` and `host/proto/` carry the same messages
with their field numbers; where they and this document disagree, this
document is right and the disagreement is a bug.

## Contents

1. [Introduction](#1-introduction)
2. [Transport](#2-transport)
3. [Framing](#3-framing)
4. [Lobby](#4-lobby)
5. [Brokered login](#5-brokered-login)
6. [Session establishment](#6-session-establishment)
7. [Session control](#7-session-control)
8. [Input stream](#8-input-stream)
9. [Video datagrams](#9-video-datagrams)
10. [Audio datagrams](#10-audio-datagrams)
11. [Session clock](#11-session-clock)
12. [Error codes](#12-error-codes)
13. [Host channel (`gdp-host/1`)](#13-host-channel-gdp-host1)
14. [Thin-client channel (`wisp/1`)](#14-thin-client-channel-wisp1)
15. [Browser transports](#15-browser-transports)
16. [Extending the protocol](#16-extending-the-protocol)
17. [Security considerations](#17-security-considerations)
- [Appendix A. Limits and constants](#appendix-a-limits-and-constants)
- [Appendix B. Implementation map](#appendix-b-implementation-map)

---

## 1. Introduction

### 1.1 Conventions

The key words MUST, MUST NOT, REQUIRED, SHOULD, SHOULD NOT, RECOMMENDED,
MAY and OPTIONAL are to be read as described in BCP 14 (RFC 2119, RFC
8174) when, and only when, they appear in all capitals.

All multi-byte integers outside protobuf messages are little-endian.
`u8`, `u16`, `u32` and `u64` are unsigned integers of that many bits. Sizes
are in bytes unless stated; KiB and MiB are powers of two. Protobuf
messages are proto3; a field "unset" or "empty" is one at its proto3
default value.

Paragraphs headed *Host behavior* or marked as notes are informative:
they describe what the reference implementation does, for context, and
place no requirement on other implementations.

### 1.2 Roles

| Role | Reference implementation | What it does |
|---|---|---|
| **client** | spectre (native), the browser client | Shows the session and sends input. |
| **host agent** | ghostd | Runs on the host. Serves the lobby: authenticates users with PAM and starts or finds their desktop session. |
| **session agent** | wraith | Runs inside one user's desktop session, as that user. Captures, encodes and streams the desktop and injects input. One per session. |
| **broker** | Veil (veild) | Optional. Authenticates users on behalf of a fleet of hosts, lists the hosts each may use, and relays logins to them. |
| **gateway** | Veil | Optional. Relays a session connection between a client and a session agent that the client cannot or should not reach directly. |
| **thin client** | Wisp (wisp-agent) | A network-booted client machine that reports its state to a broker. |

A **session** is one user's desktop on one host. A host runs at most one
session per user. The client attached to a session is its **viewer**; a
session has at most one viewer at a time, and keeps running with none.

The term **host** below means the machine, or, where the context is a
connection, the host agent or session agent at its end.

### 1.3 Protocol overview

```
 client                    host agent                 session agent
   |                          |                            |
   |== lobby connection =====>|  (gdp/1, §4)               |
   |   LobbyHello, PAM, SessionList, SessionOpen            |
   |<== Redirect{port, token, cert_sha256} ===|            |
   |                          |-- starts or finds -------->|
   |                                                       |
   |================ session connection (gdp/1, §6) ======>|
   |   control stream: SessionHello / SessionAccept, ...   |
   |   input stream:   keys, pointer, gamepads  ---------->|
   |   datagrams:      <---------- video, audio            |
```

1. **Lobby** (§4). The client connects to the host agent's lobby port,
   authenticates through the host's PAM stack, chooses a desktop and
   receives a `Redirect`: a port, a short-lived session token and the
   fingerprint of the certificate the session agent will present. The
   host agent then closes the lobby connection.
2. **Session** (§6–§11). The client opens a second connection to that
   port, presents the token, negotiates codecs and capabilities, and
   from then on receives video and audio as unreliable datagrams and sends
   input on a reliable stream. The session outlives the connection: a
   client that disconnects can go through the lobby again and reattach.

A broker (§5) inserts itself into step 1: the client authenticates to the
broker, picks a host from a list, and the broker relays the rest of the
lobby exchange to that host. With a gateway (§5.4, §15), step 2 runs
through the broker too. Neither changes what the client sends or receives
in the session phase. A session agent can also be reached without a
lobby, with a static token (§6.11).

Four protocols share the QUIC transport and the framing of §3:

| ALPN | Between | Section |
|---|---|---|
| `gdp/1` | client ↔ host agent or broker (lobby); client ↔ session agent or gateway (session) | §4–§11, §15 |
| `gdp-host/1` | host agent → broker | §13 |
| `wisp/1` | thin client → broker | §14 |

---

## 2. Transport

### 2.1 QUIC

Every GDP connection is QUIC version 1 (RFC 9000) secured with TLS 1.3
(RFC 9001). A session connection additionally REQUIRES the Unreliable
Datagram Extension (RFC 9221): both endpoints MUST advertise a non-zero
`max_datagram_frame_size`. The other connections do not use datagrams.

The client selects the protocol with ALPN (RFC 7301). A server that
receives an ALPN it does not serve MUST fail the handshake or close the
connection. `gdp/1` names both the lobby and the session phases: a lobby
port and a session port are told apart by the port, and a broker that
serves both on one port tells them apart by the first frame (§5.4).

A peer closes a GDP connection with a QUIC `CONNECTION_CLOSE` frame of
type 0x1d (application close) whose error code is one of §12's. A
transport-level failure (`CONNECTION_CLOSE` type 0x1c, an idle timeout, a
failed handshake) carries no GDP meaning.

Congestion control, pacing and idle timeouts are implementation choices.
A session connection SHOULD NOT time out while a viewer is attached and
the session is idle: the host sends nothing on an unchanging desktop
except audio, so a peer that needs traffic to stay alive SHOULD send
QUIC PINGs.

### 2.2 Streams

**Lobby connection.** One bidirectional stream, the first the client
opens (stream ID 0). Every lobby message travels on it.

**Session connection.** Two bidirectional streams, both opened by the
client, in this order:

| Stream ID | Name | Carries |
|---|---|---|
| 0 | **control** | §6, §7 |
| 4 | **input** | §8 |

The streams are identified by their IDs, not by the order in which their
data arrives. Datagrams carry video and audio (§9, §10).

**Additional streams.** A feature that needs another reliable stream
(none is defined in this version) opens it after `SessionAccept`, from
either side, and only on a session that negotiated the capability of the
same name (§6.7). The first frame (§3.1) on such a stream MUST be a
`StreamOpen`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `kind` | string | What the stream carries: a lowercase token, the name of the capability that governs it. |
| 2 | `params` | bytes | The kind's own opening parameters, if any. |

Everything after `StreamOpen` is the kind's own protocol, defined with the
feature. A peer that receives a stream whose kind it does not implement or
did not negotiate MUST reset it (`RESET_STREAM` and `STOP_SENDING`) with
`UNSUPPORTED` (§12) and MUST NOT close the connection over it. Until a
feature that uses streams is negotiated, a peer SHOULD grant no credit for
streams beyond control and input (`initial_max_streams_bidi` of 2 at the
host, 0 at the client; `initial_max_streams_uni` of 0); a feature raises
the limits with `MAX_STREAMS` when it is negotiated.

### 2.3 Certificates and host identity

A GDP certificate is trusted by pinning its **fingerprint**: the SHA-256
digest of the certificate's DER encoding, written as 64 lowercase
hexadecimal digits. (This is what `openssl x509 -noout -fingerprint
-sha256` prints, without the colons and in lower case.) Certificates are
typically self-signed. A client MUST NOT let certificate validation fail
the TLS handshake; once the handshake completes, and before it sends
anything, it MUST decide whether to trust the certificate by the rules
below, and close the connection if not.

There are three kinds of certificate:

- **Host certificate.** The host agent's long-lived identity, presented
  on its lobby port. Its private key is readable only by root. A client
  pins it trust-on-first-use, per host name and port: on first contact it
  SHOULD show the user the fingerprint and ask, and on any later mismatch
  it MUST refuse to continue and SHOULD warn that the certificate changed.
  The check happens before `LobbyHello`, so an unpinned or changed host
  never receives a username or password.
- **Session certificate.** A session agent runs as the session's user,
  who must not be able to impersonate the host to other users, so it never
  holds the host key. It generates a throwaway certificate when it starts
  and gives the host agent its fingerprint over a local channel. The host
  agent passes the fingerprint to the client in `Redirect.cert_sha256`
  (§4.7). Because that arrives over the pinned lobby connection, the client
  MUST require the session connection to present exactly that certificate
  before it sends `SessionHello`. A client MUST NOT open a session
  connection for a `Redirect` whose `cert_sha256` is empty or malformed.
- **Broker certificate.** A broker's lobby certificate, pinned like a
  host certificate. A thin client and a joined host agent are configured
  with its fingerprint (§13, §14).

The host channel (§13) is mutually authenticated: the host agent also
presents its host certificate, which the broker pins.

**CA-issued lobby certificates.** A host or broker MAY instead present a
certificate issued by a certificate authority. A client MAY trust a host
or broker certificate (never a session certificate) without a pin when
it chains to a CA the client trusts and is valid for the name the user
dialed: a DNS name against the certificate's DNS subject alternative
names, an IP address literal against its IP address names. The trusted
CAs are the platform's store, any the client's administrator added, or
both. Such a client:

- MUST NOT ask the user and SHOULD NOT record a pin for that certificate.
  A CA-issued certificate is replaced at every renewal (every few months
  for an ACME certificate), and a pin would turn each renewal into a
  false "certificate changed" warning;
- MUST accept such a certificate even when it holds a different pin for
  that host and port, for the same reason;
- MUST fall back to pinning as above for a certificate that does not
  verify, whatever the reason (an unknown CA, a name mismatch, expiry).

Trust by CA is weaker than a pin against a CA that misissues a
certificate for the name (§17), which is why a client SHOULD let its
administrator restrict it to their own CA. Session certificates are
always pinned through `Redirect.cert_sha256`, and the peers that pin out
of band (§13, §14) are unaffected. A server whose certificate is pinned
by such a peer MUST NOT replace it at renewal: a broker presents its
CA-issued certificate only on `gdp/1`, and keeps its long-lived broker
certificate for `gdp-host/1` and `wisp/1` (§13, §14).

---

## 3. Framing

### 3.1 Stream frames

Every message on a reliable stream (lobby, control, input, host channel,
thin-client channel, and the first frame of an additional stream) is one
protobuf message preceded by its length:

```
+----------------+------------------------+
| length: u32 LE | protobuf message bytes |
+----------------+------------------------+
```

`length` counts the protobuf bytes only, not itself. A frame MAY be split
across QUIC STREAM frames, and one STREAM frame MAY carry several frames;
a receiver reassembles by length alone.

`length` MUST NOT exceed 1 MiB (0x100000). A receiver that reads a larger
`length`, or a frame that does not parse as the expected message type,
MUST close the connection with `FRAME_TOO_LARGE` or `MALFORMED_FRAME`
respectively (§12). Smaller limits apply to unauthenticated peers (§4.4,
§6.3) and to some channels (§13, §14); those sections name the close code
to use instead.

Each stream carries one envelope type (a message with a single `oneof`),
so a frame needs no separate type tag:

| Stream | Envelope |
|---|---|
| lobby | `gdp.lobby.LobbyEnvelope` (§4.1) |
| control | `gdp.session.ControlEnvelope` (§6.1) |
| input | `gdp.session.InputEnvelope` (§8) |
| host channel | `ghost.broker.HostEnvelope` (§13) |
| thin-client channel | `gdp.wisp.WispEnvelope` (§14) |

A receiver MUST ignore an envelope whose `oneof` is unset or holds a case
it does not recognize (it comes from a newer peer, §16), unless the
section for that stream says the message at that point must be a
particular one.

### 3.2 Datagrams

Every QUIC DATAGRAM frame on a session connection begins with a one-byte
channel tag:

```
+-------------+--------------------------+
| channel: u8 | channel-specific payload |
+-------------+--------------------------+
```

| Channel | Direction | Contents |
|---|---|---|
| 0x00 | | Reserved; never sent. |
| 0x01 | host → client | Video (§9). |
| 0x02 | host → client | Audio (§10). |
| 0x03 | client → host | Microphone (§10.1), only under the capability `microphone`. |

A receiver MUST silently drop a datagram that is empty, carries a channel
it does not recognize or did not negotiate, or is too short for its
channel's header. Datagrams are never retransmitted, may arrive in any
order, and may be lost; every rule in §9 and §10 assumes so.

---

## 4. Lobby

The lobby authenticates a user and hands their client a session. Its
server is a host agent or, with the additions of §5, a broker.

### 4.1 Envelope

```protobuf
message LobbyEnvelope {
  oneof msg {
    LobbyHello hello = 1;             // client -> server
    AuthChallenge auth_challenge = 2; // server -> client
    AuthResponse auth_response = 3;   // client -> server
    DeviceList device_list = 4;       // broker -> client (§5)
    DeviceSelect device_select = 5;   // client -> broker (§5)
    SessionList session_list = 6;     // server -> client
    SessionOpen session_open = 7;     // client -> server
    Redirect redirect = 8;            // server -> client, last
    LobbyError error = 9;             // server -> client, last
  }
}
```

### 4.2 Exchange

```
client                              host agent
   |-- LobbyHello ------------------->|
   |<-- AuthChallenge ----------------|  \  one round per PAM prompt,
   |-- AuthResponse ----------------->|  /  zero or more rounds
   |<-- SessionList ------------------|
   |-- SessionOpen ------------------>|
   |<-- Redirect ---------------------|  then the server closes (code 0)
       or, at any point after LobbyHello:
   |<-- LobbyError -------------------|  then the server closes (same code)
```

The exchange is strictly sequential. A message other than the one
expected at that point is a protocol violation: the server closes with
`MALFORMED_FRAME` (or, before authentication succeeds, as §4.4 says).
A server that ends the exchange with a `LobbyError` SHOULD wait for the
client to acknowledge the frame before closing, so that the message is
not lost to the close, and MUST close with the same code as the
`LobbyError`. After `Redirect` it closes with code 0.

### 4.3 Messages

**`LobbyHello`** (client → server, first)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `protocol_version` | uint32 | The GDP wire version: `1`. |
| 2 | `client_id` | string | Opaque client-chosen identifier (for example host name and an install ID), for the server's logs. |
| 3 | `auth_method` | `AuthMethod` | `AUTH_METHOD_PASSWORD` (1). The only method defined. |
| 4 | `username` | string | The account to log in as. |

A server that does not support `protocol_version` MUST reply
`LobbyError{VERSION_MISMATCH}`. There is no version negotiation: a
change to the wire format changes the ALPN (§16).

**`AuthChallenge`** (server → client)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `prompt` | string | The prompt text, as the server's authentication stack gives it (for example `Password: `). |
| 2 | `echo_input` | bool | Whether the client may show what the user types. False for secrets. |

**`AuthResponse`** (client → server)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `response` | string | The user's answer to the last `AuthChallenge`. |

**`SessionList`** (server → client, after authentication)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `sessions` | repeated `SessionInfo` | The user's running sessions on this host: zero or one. |
| 2 | `available_types` | repeated `SessionType` | The desktops this host can start. |
| 3 | `default_type` | string | One of the `available_types` ids; empty only when that list is empty. |

`SessionInfo`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `session_id` | string | Opaque identifier of the session. |
| 2 | `started_at_unix` | int64 | When it started, Unix seconds. |
| 3 | `last_active_unix` | int64 | When a viewer was last seen, Unix seconds. |
| 4 | `session_type` | string | The id of the desktop it runs. |
| 5 | `viewer_attached` | bool | Another client is viewing it now (§6.4). |

`SessionType`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `id` | string | Opaque identifier, unique on this host. |
| 2 | `name` | string | Display name, for the user. |

**`SessionOpen`** (client → server)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `session_type` | string | One of `SessionList.available_types`' ids, or empty for `default_type`. |

`SessionOpen` carries no size: the client asks for one in `SessionHello`
(§6.2), on every connection, whether the session is new or not.

**`Redirect`** (server → client, success)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `host` | string | Where to open the session connection; empty means the host this lobby connection was made to (§4.7). |
| 2 | `port` | uint32 | The session connection's UDP port. Always set. |
| 3 | `token` | string | The session token to present in `SessionHello` (§4.8). Opaque to the client. |
| 4 | `expiry_unix` | int64 | When the token expires, Unix seconds. Informational for the client. |
| 5 | `cert_sha256` | string | The fingerprint of the certificate the session connection will present (§2.3). |

**`LobbyError`** (server → client, failure)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `code` | `LobbyErrorCode` | A §12 code. |
| 2 | `message` | string | Human-readable detail, for the user. Not for programmatic use. |

`DeviceList` and `DeviceSelect` are defined in §5.

### 4.4 Before authentication

A lobby server holds a connection that has not yet authenticated to
limits that bound what an anonymous peer can cost it:

- **Frame size.** `LobbyHello` and every `AuthResponse` MUST NOT exceed
  16 KiB (`length` ≤ 0x4000). A larger frame closes the connection with
  `FRAME_TOO_LARGE`. The 1 MiB limit of §3.1 applies from `SessionOpen`
  on.
- **Deadline.** Authentication MUST complete within a deadline counted
  from the connection's first packet (default 120 s). A connection that
  runs out is closed with `AUTH_FAILED` and no `LobbyError`.
- **Refusal before the handshake.** A server MAY refuse a connection
  outright, with the QUIC transport error `CONNECTION_REFUSED` and no
  GDP code, when its source address has been penalized for earlier
  failed or abandoned logins, or when too many connections are
  unauthenticated at once. A client SHOULD present this as "try again
  later".

A wrong first message, or a message other than `AuthResponse` while a
challenge is outstanding, closes with `MALFORMED_FRAME`.

### 4.5 Authentication

After `LobbyHello`, the server runs its authentication stack for
`username` and turns each prompt into one `AuthChallenge`; the client
answers each with exactly one `AuthResponse`, in order. There may be any
number of rounds, including none, and the client MUST NOT assume the
first prompt is for a password. Authentication ends when the server sends
`SessionList` (success) or `LobbyError` (failure).

Every authentication failure, including a login the host's policy
refuses (for example `root`, or an account with an empty password, unless
the host allows them), MUST be reported as `AUTH_FAILED` and nothing more
specific, so that the result does not reveal whether a password was
right. A server MAY delay or penalize repeated failures from one source.

Once the user has authenticated, the server MAY refuse the login with:

- `LOCAL_SESSION_ACTIVE`: the user is logged in at one of the host's
  own seats, and a host allows one graphical login per user. This is
  checked only after authentication, so a failed login never learns it.
- `HOST_FULL`: the host has no capacity for another session.
- `SESSION_START_FAILED`: the session could not be started.

### 4.6 Choosing a session

`SessionList.available_types` lists the desktops the host can start.
`default_type` is the server's suggestion; the reference host agent picks
the user's last-started type if it is still available, else the host's
configured default, else the first available type. The client replies with
`SessionOpen`, naming one of the offered ids or leaving `session_type`
empty to mean `default_type`. A client MUST NOT send an id it was not
offered; the server MUST answer an unknown id with
`LobbyError{UNKNOWN_SESSION_TYPE}`. The ids are opaque and the client
never sends anything a host would execute.

A host runs at most one session per user. If the user's session is
already running, `SessionOpen` reattaches to it whatever `session_type`
says; a host SHOULD log a mismatch but MUST NOT refuse the request for
it. Otherwise the host starts a session of the requested type and sends
`Redirect` once its session agent is ready to accept a connection.

When that session's `viewer_attached` is set, another client is viewing
it, and the session connection will need `take_over` (§6.4). A client
SHOULD ask its user before sending `SessionOpen`, and end the login
instead if they decline.

### 4.7 Redirect

The client opens the session connection (§6) to `Redirect.host` and
`Redirect.port`, with ALPN `gdp/1`, and requires the certificate
`Redirect.cert_sha256` (§2.3).

When `host` is empty, the client MUST reuse the host string it was given
for the lobby connection: the name as typed, not the address it resolved
to, so that name resolution and certificate pinning key on one name in
both phases. A host agent only ever starts sessions on its own machine,
and the address the client dialed is the one that reaches it (through NAT,
split-horizon DNS, a VPN or a port forward) where anything derived from
the server's own socket may not be. A host agent therefore leaves `host`
empty. A broker sets it (§5.4).

The client SHOULD open the session connection immediately: the token is
short-lived (§4.8).

### 4.8 Session token

`Redirect.token` is minted on the host side and verified by the session
agent; the client passes it through unchanged as `SessionHello.token`.
The minter and the session agent share a per-session secret of 32 random
bytes, created when the session starts (in the reference host, the
session's root process holds it and mints; the host agent only carries
the token). The token is:

```
token = base64url_nopad( nonce || expiry || tag )
tag   = HMAC-SHA256( session_secret, uid || nonce || expiry )

nonce  : 16 random bytes
expiry : the expiry time, Unix seconds, int64 LE (8 bytes)
uid    : the session user's numeric uid, u32 LE (4 bytes)
```

`base64url_nopad` is base64url (RFC 4648 §5) without padding; the encoded
token is 75 characters. The uid is not carried: the session agent knows
its own and recomputes the tag with it. The expiry travels in the clear
so that the verifier can recompute the MAC; it is enforced by the
verifier's clock, not by being secret.

A session agent MUST accept a token only if it decodes to exactly 56
bytes, its expiry is not in the past, its tag matches (compared in
constant time), and it has not accepted a token with the same nonce
before. It keeps each accepted nonce until that token expires. Spent
tokens are matched by nonce, not by string: the last base64url character
carries two bits the decoder ignores, so a token has several spellings.
The reference host agent issues tokens valid for 30 s.

Test vector:

```
uid            = 1000
session_secret = 00 01 02 ... 1f   (32 bytes, byte i = i)
nonce          = 00 01 02 ... 0f   (16 bytes, byte i = i)
expiry         = 1700000000
tag            = 1d8ee40784c4c59f398533974ed5c00928eb8462884a2e9a137fbfa48483fd72
token          = AAECAwQFBgcICQoLDA0ODwDxU2UAAAAAHY7kB4TExZ85hTOXTtXACSjrhGKISi6aE3-_pISD_XI
```

---

## 5. Brokered login

A broker authenticates users against its own accounts, which share
names with the hosts' accounts, and lets each user choose among the hosts
("devices") they are entitled to. A client speaks the same lobby protocol
to a broker as to a host; it tells them apart only by whether a
`DeviceList` arrives after authentication. One client implementation
works with both.

### 5.1 Exchange

```
client                         broker                          host agent
   |-- LobbyHello -------------->|
   |<-- AuthChallenge -----------|   broker's own authentication
   |-- AuthResponse ------------>|
   |<-- DeviceList --------------|
   |-- DeviceSelect ------------>|== login stream (§13.4) ========>|
   |                             |-- LobbyHello ------------------->|
   |                             |<-- AuthChallenge ----------------|
   |                             |-- AuthResponse (the password) -->|
   |<-- AuthChallenge -----------|<-- AuthChallenge ----------------|  only if the
   |-- AuthResponse ------------>|-- AuthResponse ----------------->|  host asks more
   |<-- SessionList -------------|<-- SessionList ------------------|
   |-- SessionOpen ------------->|-- SessionOpen ------------------>|
   |<-- Redirect ----------------|<-- Redirect ---------------------|
```

After its own authentication, the broker sends `DeviceList`. The client
answers `DeviceSelect`. From then on the broker relays the chosen host's
own lobby exchange (§4.2) on the same stream, in both directions, with
the exceptions below. So `AuthChallenge` can also arrive after
`DeviceSelect`.

### 5.2 Messages

**`DeviceList`** (broker → client)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `devices` | repeated `Device` | The hosts this user is entitled to, those with a running session first. |

`Device`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `id` | string | Opaque identifier. |
| 2 | `name` | string | Display name. |
| 3 | `online` | bool | The host's channel to the broker is up (§13). |
| 4 | `has_session` | bool | This user has a session running on it. |
| 5 | `session_type` | string | That session's type id, when `has_session`. |
| 6 | `available_types` | repeated `SessionType` | What the host would offer in `SessionList.available_types`, as last reported to the broker; empty when offline. |
| 7 | `default_type` | string | The type this user last ran on the host, if it is still in `available_types`, else the host's own default; empty when unknown. |

`available_types` and `default_type` let a client choose the desktop in
the same step as the host and answer the later `SessionList` without
asking the user again.

**`DeviceSelect`** (client → broker)

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `device_id` | string | A `Device.id` from the `DeviceList`. |

### 5.3 Relaying the host's login

The broker opens a login stream to the host (§13.4) and sends the host a
`LobbyHello` for the user. It answers the host's first prompt that does
not echo (`echo_input` false) itself, with the password the client gave
the broker, and relays every other `AuthChallenge` to the client, such
as a second factor. The broker MUST hold the password only for the one
login and MUST erase it afterwards.

The broker's own refusals are `LobbyError`s with:

- `NOT_ENTITLED`: the selected device is not in this user's `DeviceList`;
- `HOST_OFFLINE`: the selected device has no channel to the broker;
- `HOST_AUTH_FAILED`: the host answered `AUTH_FAILED` after the broker
  had answered its password prompt. The password was right for the
  broker and wrong for the host; the two account databases have drifted.

Every other `LobbyError` from the host, `LOCAL_SESSION_ACTIVE` included,
is passed to the client unchanged.

### 5.4 Brokered redirects

A broker's `Redirect` always sets `host`, and is one of two kinds:

- **Direct.** `host` is the host's address for clients, `port` and
  `token` are the host's own, and `cert_sha256` is its session agent's.
  The client connects to the session agent itself.
- **Gateway.** `host` is the broker's own public address, `port` its
  lobby port, `cert_sha256` the certificate the broker presents on
  `gdp/1` at that moment (its broker certificate, or its CA-issued one,
  §2.3), and `token` a single-use gateway token of the broker's own. The client connects to
  the broker, which relays the session (below).

The client treats both alike: it opens a session connection to `host`
and `port` with ALPN `gdp/1`, pins `cert_sha256` and sends `SessionHello`
with `token`.

A broker serving logins and gateway sessions on one port tells the two
apart by the connection's first frame. Field 1 of both `LobbyEnvelope`
and `ControlEnvelope` is a length-delimited hello, and inside it field 1
of `LobbyHello` is a varint while field 1 of `SessionHello` is a string,
so a frame decodes as at most one of them. A first frame that is neither
closes the connection with `MALFORMED_FRAME`.

**Gateway relay.** The broker checks the gateway token (an unknown, used
or expired one closes with `AUTH_FAILED`), opens a session connection of
its own to the session agent, pinning the certificate the host reported,
and relays:

- both streams byte for byte, in both directions, except that it
  replaces the client's `SessionHello` with one carrying the host's own
  token and with `via_gateway` set (§6.10);
- datagrams as they arrive, never queued: a datagram the onward leg
  cannot take at once is dropped, so congestion on either leg appears as
  ordinary loss;
- the close: when either leg closes, the broker closes the other with the
  same code.

The gateway sends `GatewayPath` messages to the client (§6.10) and sizes
its leg to the session agent so that the agent's video slices fit the
client's leg (§9.2). Nothing else in the session phase differs from a
direct session. A gateway session is not end-to-end encrypted: the
broker decrypts and re-encrypts it (§17).

---

## 6. Session establishment

### 6.1 Control envelope

```protobuf
message ControlEnvelope {
  oneof msg {
    // Opening the session.
    SessionHello hello = 1;                             // client -> host
    SessionAccept accept = 2;                           // host -> client
    SessionReject reject = 3;                           // host -> client
    // Liveness and measurement.
    StatsReport stats = 4;                              // client -> host
    Ping ping = 5;                                      // either
    Pong pong = 6;                                      // either
    GatewayPath gateway_path = 7;                       // gateway -> client
    // The picture: outputs and the pointer.
    DisplaysChanged displays_changed = 8;               // host -> client
    ResolutionChange resolution_change = 9;             // client -> host
    CursorShape cursor_shape = 10;                      // host -> client
    CursorPosition cursor_position = 11;                // host -> client
    // Steering the stream.
    KeyframeRequest keyframe_request = 12;              // client -> host
    NetworkProfileChanged network_profile_changed = 13; // host -> client
    RefinePause refine_pause = 14;                      // client -> host
    // Data and ending.
    ClipboardData clipboard = 15;                       // either
    LogoutRequest logout_request = 16;                  // client -> host
    // Support.
    DiagnosticsRequest diagnostics_request = 17;        // host -> client
    DiagnosticsReport diagnostics_report = 18;          // client -> host
  }
}
```

In this section and §7, "host" means the session agent, or a gateway
relaying to one.

### 6.2 `SessionHello`

The client MUST send `SessionHello` as its first frame on the control
stream, as soon as the connection is up.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `token` | string | `Redirect.token`, verbatim, or a static token (§6.11). |
| 2 | `take_over` | bool | Displace the session's current viewer (§6.4). |
| 3 | `via_gateway` | bool | Set by a gateway (§6.10). A client MUST send false. |
| 4 | `network_profile` | `NetworkProfile` | What kind of link this is (§6.9). |
| 5 | `displays` | repeated `DisplayDescriptor` | The output size the client wants, first entry per output. Empty: keep the session's current size. |
| 6 | `codecs` | repeated string | Video codecs the client can decode, most preferred first (§6.6). |
| 7 | `codec_formats` | repeated `CodecFormat` | Bit depths and chroma subsamplings the client decodes beyond the baseline, per codec (§7.3). |
| 8 | `primaries` | repeated `ColorPrimaries` | Primaries the client can show beyond the baseline (§7.3). |
| 9 | `transfers` | repeated `ColorTransfer` | Transfer functions the client can show beyond the baseline (§7.3). |
| 10 | `capabilities` | repeated string | Extensions the client understands (§6.7). |
| 11 | `refine_formats` | repeated `RefineFormat` | Refinement tile formats the client accepts beyond `BGR8` (§9.5). |
| 12 | `decoders` | repeated string | Informational: the client's decoder backends, for example `vulkan`, `vaapi`, `d3d11va`, `software`. |

`DisplayDescriptor`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `width` | uint32 | Pixels. |
| 2 | `height` | uint32 | Pixels. |
| 3 | `refresh_mhz` | uint32 | Refresh rate in millihertz (60000 = 60 Hz); 0 if unknown. |
| 4 | `scale` | double | Output scale factor; 0 or 1 for none. |

A session in this version has one output. A host acts on the first
`displays` entry only, and only on its width and height, applying the
same rules as `ResolutionChange` (§7.2); `SessionAccept.outputs` reports
the size it actually has. A request the host cannot meet is not an error.

### 6.3 Before authentication

Until its token has been verified, a session connection is **pending**:
it is not the viewer and receives nothing. The host holds a pending
connection to:

- **a deadline:** `SessionHello` within 5 s of the handshake completing;
- **one small frame:** the first control frame MUST be a `SessionHello`
  and MUST NOT exceed 16 KiB (`length` ≤ 0x4000); the 1 MiB limit of
  §3.1 applies once the connection is the viewer;
- **a short queue:** at most 2 pending connections per source address
  and 16 in all. A third from one address evicts that address's oldest;
  past 16, the address holding the most loses its oldest. One address
  thus never evicts another's, though a peer with many addresses (an
  IPv6 prefix) can still fill the table.

Every failure while pending (a bad, expired or spent token, the
deadline, eviction, an oversized, unparseable or wrong first frame) MUST
close the connection with `AUTH_FAILED` and nothing more specific, so
that an unauthenticated peer cannot learn from the close code how far
its input got. The host SHOULD log the actual reason. Input-stream data from a
pending connection MUST be discarded.

### 6.4 One viewer, and taking over

A connection whose token verifies while the session already has a viewer
is closed with `ALREADY_CONNECTED`, unless its `SessionHello` sets
`take_over`. The default protects the attached viewer from being
silently displaced by a stranger holding a stolen token or by a forgotten
client.

With `take_over` set, the host closes the current viewer's connection
with `TAKEN_OVER` and accepts the new one as a fresh viewer: a new
`SessionAccept`, a new keyframe, and nothing carried over from the old
connection (gamepads, clipboard state and refinement pause start again).
`take_over` is honoured only after the token has been verified. A client
SHOULD set it only when its user asked for it: before `SessionOpen`, when
the `SessionList` marks the running session `viewer_attached` (§4.6), or
after being refused with `ALREADY_CONNECTED`, which a viewer attaching in
between can still cause. A gateway forwards it unchanged.

If the session was ended because its user logged in at the host's
console, a connection whose token verifies is closed with
`ENDED_BY_LOCAL_LOGIN`.

### 6.5 Acceptance

On a verified `SessionHello`, the host negotiates the codec (§6.6) and
capabilities (§6.7) and replies with `SessionAccept`. The session is
**established** once the client has received it.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `outputs` | repeated `OutputDescriptor` | The session's outputs (§7.2). One in this version. |
| 2 | `codec` | string | The video codec of every video payload (§6.6). |
| 3 | `bitrate_ceiling_bps` | uint32 | The most the host will send on video, bits per second. Informational. |
| 4 | `audio` | `AudioConfig` | The audio stream's format (§6.8); absent when the session has no audio. |
| 5 | `microphone` | `AudioConfig` | The microphone stream's format (§6.8, §10.1); present exactly when `microphone` is in effect. |
| 6 | `capabilities` | repeated string | The capabilities in effect (§6.7). |
| 7 | `network_profile` | `NetworkProfile` | The profile the host's rate control runs with (§6.9). |
| 8 | `via_gateway` | bool | `SessionHello.via_gateway`, echoed (§6.10). |
| 9 | `encoder` | string | Informational: the encoder backend, e.g. `vaapi`, `nvenc` or `software`, with `+refine` appended when refinement is in effect. |
| 10 | `host_user` | string | Informational: the session's account name. |
| 11 | `host_name` | string | Informational: the host's name. |

`encoder`, `host_user` and `host_name` are for the client's user
interface; a client MUST tolerate them empty.

Immediately after `SessionAccept` the host:

- resets its session clock (§11) and starts `frame_id` at 0 (§9.1);
- sends a keyframe on every output, even if the desktop is not changing;
- sends the current `CursorShape` (§7.4);
- sends its current clipboard text, if `clipboard` is in effect (§7.9).

**`SessionReject`** (host → client) has one field, `reason` (1, string,
human-readable). A host MAY send it instead of `SessionAccept` to explain
a refusal and then close the connection with a §12 code. The reference
session agent never sends it: every refusal is a close code, which cannot
race the close. A client MUST handle it by ending the session and showing
`reason`.

### 6.6 Codec negotiation

`SessionHello.codecs` lists the video codecs the client can decode, most
preferred first, as lowercase tokens. A token names a bitstream format,
never an implementation: the output of a hardware encoder and of x264 are
both `h264`.

| Token | Payload (§9.4) | Notes |
|---|---|---|
| `h264` | H.264 (ITU-T H.264), Annex B byte stream | The universal floor: every host and client SHOULD support it. |
| `h265` | H.265 (ITU-T H.265), Annex B byte stream | |
| `av1` | AV1 (AOMedia), low-overhead bitstream format: a sequence of OBUs | |
| `pyrowave` | One PyroWave frame | Wired LANs only; see below. |

A peer offers or accepts only what it can actually handle; a token's
presence here is no promise that a given build supports it.

The host MUST choose the first token in `codecs` that it can encode,
echo it in `SessionAccept.codec`, and send every video payload of the
session in that codec (wrapped as §9.5 when `refine` is in effect). If
none of the tokens is one it can encode (including when the list is
empty) it MUST close with `NO_COMMON_CODEC` instead of accepting. A
client that receives a `SessionAccept.codec` it did not offer MUST close
with `NO_COMMON_CODEC`.

The codec is fixed for the life of the connection.

**PyroWave.** `pyrowave` is the intra-only wavelet codec
[PyroWave](https://github.com/Themaister/pyrowave). Its bitstream is not
yet stable upstream, so both ends MUST be built from the same upstream
revision (pinned in `packaging/build-pyrowave.sh`). Every frame stands
alone, so every payload is a keyframe and loss needs no repair, but its
rate runs to several hundred Mbit/s. Therefore:

- a client SHOULD offer it, and first, only when the host is reachable
  directly (no gateway, or the host is the client's own machine) over a
  wired interface of 1 Gbit/s or more. A client MAY offer it regardless
  when its user names it explicitly, and MAY let its user turn it off;
- a host MUST NOT accept it on a session with `via_gateway` set (§6.10),
  and SHOULD accept it only when its operator has enabled it. Its
  `bitrate_ceiling_bps` follows from the output size rather than the
  host's usual maximum.

### 6.7 Capabilities

Capabilities are optional extensions within one wire version. The client
lists every capability it understands in `SessionHello.capabilities`;
the host replies in `SessionAccept.capabilities` with those it also
supports, in the client's order, without duplicates. Exactly those are
in effect for the connection. A peer MUST ignore a capability name it
does not know, and MUST NOT send or act on a message, datagram channel
or stream that belongs to a capability not in effect. A capability is
fixed for the life of the connection: one missing from `SessionAccept`
cannot be added later.

A host offers a capability according to what it will be able to do
during the connection, not only what it can do at that instant: if a
mechanism becomes ready a moment after the client attaches, offering the
capability is still right.

| Name | Status | Defined in |
|---|---|---|
| `clipboard` | defined | §7.9 |
| `refine` | defined | §7.8, §9.5 |
| `gamepad` | defined | §8.5 |
| `hid` | defined | §8.6 |
| `microphone` | defined | §6.8, §10.1 |
| `usb` | reserved | an additional stream kind (§2.2) |

Anything that changes the meaning of bytes an existing peer already
understands is not a capability but a new ALPN (§16).

### 6.8 Audio

`SessionAccept.audio` describes the audio stream (§10), because Opus
packets do not describe their own sample rate or channel count:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `sample_rate_hz` | uint32 | Sample rate of the decoded audio. |
| 2 | `channels` | uint32 | Channel count. |
| 3 | `frame_ms` | uint32 | Duration of each packet. |
| 4 | `codec` | string | The audio codec token. `opus` is the only one defined. |

The host sends `{48000, 2, 10, "opus"}` in this version. When it has no
audio capture, it omits `audio` and sends no audio datagrams. A client
that does not support the audio codec MUST run the session without audio
rather than fail it. `SessionHello` has no audio field: there is one
format, so nothing to negotiate. More would be offered the way `codecs`
offers video.

When `microphone` is in effect, `SessionAccept.microphone` (field 5,
also an `AudioConfig`) gives the format of the microphone stream (§10.1);
the host sends `{48000, 1, 10, "opus"}` in this version, and the client
encodes exactly that. A client that cannot MUST run the session without a
microphone rather than fail it.

### 6.9 Network profile

`SessionHello.network_profile` tells the host what kind of link the
client is on, for its rate control:

| Value | Name | Meaning |
|---|---|---|
| 0 | `NETWORK_PROFILE_AUTO` | Let the host classify the link from what it measures. Also what a client that does not say sends. |
| 1 | `NETWORK_PROFILE_LAN` | A local network. |
| 2 | `NETWORK_PROFILE_INTERNET` | A wide-area path. |
| 3 | `NETWORK_PROFILE_MOBILE` | A cellular or similarly variable path. |

It is a hint; nothing on the wire changes with it. The host reports the
profile it runs with in `SessionAccept.network_profile` (the one asked
for or, for `AUTO`, the one it classified the link as) and, when it later
reclassifies an `AUTO` link, sends `NetworkProfileChanged` (§7.1) with
the new one. Both are informational. `AUTO` in `SessionAccept` means a
host that does not report it.

### 6.10 Gateways

A gateway sets `SessionHello.via_gateway` as it relays the hello,
overwriting what the client sent, and the host echoes it in
`SessionAccept.via_gateway`. So the host knows at codec choice, and every
client knows from the first reply, whether the session is relayed. The
flag is never inferred.

A client's own QUIC round-trip time ends at the gateway. After
`SessionAccept`, the gateway sends the client `GatewayPath` about once a
second:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `upstream_rtt_us` | uint32 | The round-trip time of the gateway's connection to the host, µs. |

A client that measures RTT from its transport adds `upstream_rtt_us` to
it, both for display and in `StatsReport.rtt_us` (§7.5), so the host's
rate control sees the whole path. A client that measures RTT end to end
with `Ping`/`Pong` already sees the whole path and ignores `GatewayPath`.
On a direct session none arrives.

### 6.11 Connecting without a lobby

A session agent MAY be configured to listen on a fixed UDP port with a
certificate and a **static token** of its operator's choosing, and serve
clients that connect without a lobby. The client is given the host, the
port, the certificate's fingerprint and the token out of band. It opens
an ordinary session connection (ALPN `gdp/1`), requires that fingerprint
exactly, as it would `Redirect.cert_sha256` (§2.3), and sends the token
as `SessionHello.token`.

The session agent accepts the connection if the token equals its static
token, compared in constant time. A static token is not single-use and
does not expire. Everything else in §6 to §11 applies unchanged: the
pending rules (§6.3), the one viewer and `take_over` (§6.4), and every
failure is `AUTH_FAILED`.

*Host behavior:* `wraith -l PORT -t TOKEN -c CERT -k KEY -p TYPE`. It
runs as whoever starts it, with no host agent, PAM or logind session, and
is meant for development and testing.

---

## 7. Session control

### 7.1 Message validity

After `SessionAccept`, the control stream carries the messages below. A
message received before the session is established, in the wrong
direction, or belonging to a capability not in effect MUST be ignored
(before establishment, the host instead treats anything but
`SessionHello` as §6.3 says). A second `SessionHello` on an established
connection MUST be ignored.

| Message | Direction | Condition | Section |
|---|---|---|---|
| `StatsReport` | client → host | every 250 ms | §7.5 |
| `Ping` / `Pong` | either | | §7.7 |
| `GatewayPath` | gateway → client | gateway sessions | §6.10 |
| `DisplaysChanged` | host → client | | §7.2 |
| `ResolutionChange` | client → host | | §7.2 |
| `CursorShape` | host → client | | §7.4 |
| `CursorPosition` | host → client | | §7.4 |
| `KeyframeRequest` | client → host | | §7.6 |
| `NetworkProfileChanged` | host → client | | §6.9 |
| `RefinePause` | client → host | `refine` in effect | §7.8 |
| `ClipboardData` | either | `clipboard` in effect | §7.9 |
| `LogoutRequest` | client → host | | §7.10 |
| `DiagnosticsRequest` | host → client | | §7.11 |
| `DiagnosticsReport` | client → host | after a request | §7.11 |

`NetworkProfileChanged` has one field, `profile` (1, `NetworkProfile`).

### 7.2 Outputs and resolution changes

Each output is described by an `OutputDescriptor`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `stream_id` | uint32 | The output's video `stream_id` (§9.1), 0–255. |
| 2 | `display` | `DisplayDescriptor` | Its size, refresh rate and scale. |
| 3 | `color` | `ColorDescription` | How to interpret its decoded pictures (§7.3). |

`display.width` and `display.height` are the size of the picture to
show, which may be smaller than the coded size: a codec that pads its
coded size (AV1 to a multiple of 8 pixels) is cropped to this size from
the top-left.

**`ResolutionChange`** (client → host) asks for a new size for one
output:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `stream_id` | uint32 | The output. |
| 2 | `display` | `DisplayDescriptor` | The size wanted (width and height; the rest is ignored). |

The host MUST answer every `ResolutionChange` with a `DisplaysChanged`
carrying the outputs as they are afterwards, whether it changed the size
or not. It MUST refuse (answer with the current size) a request that is
odd in either dimension (subsampled chroma needs even sizes), outside 320–8192
pixels per side, or one its compositor or encoder cannot use.

**`DisplaysChanged`** (host → client) has one field, `outputs` (1,
repeated `OutputDescriptor`): a full replacement of the output list,
never a delta. A host MAY also send it unprompted when an output changes.
When an output's size changes:

- `DisplaysChanged` MUST be sent before any video frame at the new size;
- the first frame at the new size MUST be a keyframe;
- the codec, capabilities and every other negotiated parameter stay as
  they were.

An output's `color` may change the same way, for example when a session
moves between SDR and HDR. The host MUST send the `DisplaysChanged` with
the new description before any frame encoded with it, and the first such
frame MUST be a keyframe. The new description stays within what the
client declared (§7.3).

If the host changed the size but its encoder can neither open at the new
size nor reopen at the old one, it closes the connection with
`NO_COMMON_CODEC`.

Video datagrams are not ordered with respect to the control stream, so a
client MUST be prepared to decode a keyframe at a new size or colour
description that arrives before the `DisplaysChanged` announcing it. The
bitstream says its own size, depth and subsampling; until the
`DisplaysChanged` arrives the client may show the frame with the previous
description.

### 7.3 Colour

`OutputDescriptor.color` says how to turn the decoded pictures back into
RGB and how to show them:

| # | Field | Type | Values |
|---|---|---|---|
| 1 | `matrix` | `ColorMatrix` | 0 unspecified, 1 BT.601, 2 BT.709, 3 BT.2020 non-constant luminance |
| 2 | `range` | `ColorRange` | 0 unspecified, 1 limited (16–235 luma, 16–240 chroma at 8 bits, scaled by 2^(depth−8) above), 2 full |
| 3 | `primaries` | `ColorPrimaries` | 0 unspecified, 1 BT.709 (also sRGB's), 2 BT.2020 |
| 4 | `transfer` | `ColorTransfer` | 0 unspecified, 1 sRGB, 2 PQ (SMPTE ST 2084), 3 HLG |
| 5 | `bit_depth` | uint32 | Bits per component; 0 means 8 |
| 6 | `chroma` | `Chroma` | 0 unspecified, 1 4:2:0, 2 4:2:2, 3 4:4:4 |

A host SHOULD fill every field with what its encoder actually does. A
field that is unspecified, or a `color` that is absent, means the codec's
default:

| Codec | Matrix | Range | Primaries | Transfer | Depth | Chroma |
|---|---|---|---|---|---|---|
| `h264`, `h265`, `av1` | BT.601 | limited | BT.709 | sRGB | 8 | 4:2:0 |
| `pyrowave` | BT.709 | full | BT.709 | sRGB | 8 | 4:2:0 |

The BT.601 matrix for the block-based codecs is a fixed convention of
this protocol, not signalled in their bitstreams, which is why it is
written down here; a client MUST apply `color` rather than whatever the
bitstream's VUI or sequence header says.

**Colour negotiation.** Matrix and range are arithmetic that any client
can do, so they are not negotiated: a client MUST apply every value
defined in this version. Depth, chroma, primaries and transfer depend on
the client's decoder and display, so the client declares them in
`SessionHello` (§6.2) and the host chooses within what it declared.

Every client takes the **baseline**: 8 bits and 4:2:0 in every codec it
offers, BT.709 primaries and sRGB transfer. A client declares more with:

- `codec_formats`: one `CodecFormat` per codec it decodes more of.

  | # | Field | Type | Meaning |
  |---|---|---|---|
  | 1 | `codec` | string | A token from `codecs` (§6.6). |
  | 2 | `bit_depths` | repeated uint32 | Bits per component it decodes in this codec besides 8, e.g. 10. |
  | 3 | `chroma` | repeated `Chroma` | Subsamplings it decodes in this codec besides 4:2:0. |

  A combination counts as declared when its depth and its subsampling are
  each declared for the session's codec.
- `primaries` and `transfers`: what it can show besides BT.709 and sRGB,
  natively or by mapping to its display.

The host MUST describe in `color` only a depth, chroma, primaries and
transfer that the baseline or the client's declaration covers, and MUST
send pictures that match the description. Which one it picks within
that, and when it changes it (§7.2), is the host's choice. A client that
receives a description outside what it declared MAY end the session with
`UNSUPPORTED`.

A value this version does not define is never sent: a later version that
adds one adds the declaration that covers it.

*Host behavior:* the reference host converts the sRGB desktop to 8-bit
4:2:0, the baseline, with the codec's default matrix and range.

### 7.4 Cursor

The host never draws the pointer into the video. The client draws the
last `CursorShape` it received at the pointer's position.

**`CursorShape`** (host → client):

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `width` | uint32 | Pixels. |
| 2 | `height` | uint32 | Pixels. |
| 3 | `hotspot_x` | uint32 | The hotspot, in pixels from the image's left. |
| 4 | `hotspot_y` | uint32 | The hotspot, in pixels from the image's top. |
| 5 | `argb8888` | bytes | `width × height × 4` bytes: rows top to bottom, tightly packed, each pixel a little-endian 32-bit ARGB value (bytes B, G, R, A in memory), alpha premultiplied. |

A `CursorShape` with a 0×0 image means "no cursor": the focused
application hid the pointer (as games do for mouselook), and the client
draws nothing until the next non-empty shape. Neither side may exceed
384 pixels: the host MUST NOT send a larger shape, and the client MUST
ignore one, as it MUST a shape whose `argb8888` length does not match its
size. The host sends the shape whenever it changes, and once after
`SessionAccept`.

**`CursorPosition`** (host → client):

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `x` | double | Horizontal position, 0 (left edge) to 1 (right edge) of the output. |
| 2 | `y` | double | Vertical position, 0 (top) to 1 (bottom). |

When the client sends absolute pointer motion (§8.2) it knows where the
pointer is. When it sends relative motion only the host does, since the
host applies pointer constraints, confinement and warps. While the client
sends relative motion, the host reports the resulting position with
`CursorPosition`, at most one per batch of input it processes and only
when the position changed. A locked pointer produces none.

### 7.5 Feedback: `StatsReport`

The client MUST send a `StatsReport` every 250 ms while the session is
established. It is the host's only source of loss information and the
basis of its rate control.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `rtt_us` | uint32 | The client's estimate of the round-trip time, µs, from its QUIC stack (plus `GatewayPath.upstream_rtt_us` through a gateway, §6.10). |
| 2 | `streams` | repeated `StreamStats` | One per video stream received. |

`StreamStats`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `stream_id` | uint32 | The video stream. |
| 2 | `highest_frame_id_acked` | uint32 | The highest `frame_id` the client has received completely. |
| 3 | `loss_bitmap` | uint64 | Bit *i* set: frame `highest_frame_id_acked − i` was lost (*i* in 0–63). |
| 4 | `decode_us` | uint32 | Time to decode the latest frame, µs. |
| 5 | `present_us` | uint32 | Time from decode to display of the latest frame, µs. |
| 6 | `interval_us` | uint32 | The length of this report's measurement window, µs. |
| 7 | `bytes_received` | uint64 | Video datagram bytes received in the window, channel byte and headers included. |
| 8 | `delay_min_us` | sint32 | Minimum one-way delay of first slices in the window (below). |
| 9 | `delay_avg_us` | sint32 | Mean one-way delay of first slices in the window. |
| 10 | `delay_samples` | uint32 | How many first slices the delays cover. |
| 11 | `trains` | repeated `FrameTrain` | Arrival spread of large frames (below). |

**Loss.** A frame counts as lost when it was never completely
reassembled, and also when it was reassembled but then discarded (the
client had nowhere to show it yet, its refinement container was
malformed, or the decoder rejected it). From the host's point of view the
two are the same event, and reporting both is the only way the host can
repair a frame that never took effect (§9.6). Bits for frames older than
the first one the client received are always clear. Arithmetic on
`frame_id` is modulo 2³².

**Delay.** For each frame whose first-arriving datagram arrived in the
window, the client computes that datagram's arrival time on its session
clock (§11) minus the frame's `pts`. The two clocks are zeroed at
different moments, so the values carry an unknown constant offset and may
be negative; only their changes mean anything. All three delay fields are
0 when no first slice arrived in the window.

A client that cannot measure fields 6–10 leaves them 0; the host
recognizes that by `interval_us` being 0. `interval_us` is never 0 from a
client that measures them.

**Frame trains.** A host sends each frame's datagrams back to back, so the
spread the path puts between them measures its slowest hop.
`FrameTrain`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `frame_id` | uint32 | The frame. |
| 2 | `datagrams` | uint32 | Its datagram count. |
| 3 | `bytes` | uint32 | Datagram bytes after the first to arrive, channel byte and headers included. |
| 4 | `span_us` | uint32 | Time from the first datagram's arrival to the last's, µs. |

A client reports a train for a frame completed in the window only when
every one of its datagrams arrived exactly once and there were at least
16 of them, at most 8 trains per report. Arrival times are taken where the
QUIC stack receives the datagrams, not where the application processes
them. `bytes / span_us` estimates the rate of the path's slowest hop. A
client that does not measure trains sends none.

*Host behavior:* the reference host repairs each loss newly reported in
the bitmap with a keyframe, unless a keyframe already sent or requested
follows the lost frame, and no sooner than a minimum interval after the
previous repair (which depends on the network profile). It sets the video
bitrate from delay trend, loss and its own send backlog, and paces
datagrams from the frame-train estimate.

### 7.6 `KeyframeRequest`

`KeyframeRequest` (client → host) has one field, `stream_id` (1,
uint32). It asks the host to make the stream's next frame a keyframe; a
client sends it when it cannot decode (for example after a decoder
reset). The host MAY coalesce requests.

### 7.7 `Ping` and `Pong`

`Ping` and `Pong` each have one field, `nonce` (1, uint64). A peer that
receives a `Ping` on an established session MUST answer with a `Pong`
carrying the same `nonce`, promptly. A client whose transport does not
expose a round-trip time (the browser client) measures it with these.

### 7.8 Lossless refinement

With the `refine` capability in effect, every video payload of the
session is a refinement container (§9.5): the codec's own bytes plus a
lossless tile layer that the client draws over the decoded video, so that
a region the host has sent losslessly appears bit-exact. Refinement
works with every codec. Without the capability, payloads are the bare
codec bitstream.

Refinement costs the host a full-frame readback and hashing every frame.
That is the right trade for reading text and the wrong one for a game, so
a client that offers `refine` SHOULD let its user turn the layer off
during the session with `RefinePause`.

`RefinePause` (client → host) has one field, `paused` (1, bool). It
controls whether the host builds a layer; the capability stays in effect
either way, and every payload remains a container. On `paused = true` the
host's next payload MUST carry a layer with the `RESET` flag, so that the
client's plane does not keep tiles the host no longer tracks, and every
payload after it carries an empty layer until `paused = false`. Resuming
also starts from a `RESET`. Neither needs a keyframe, and the codec stream
continues uninterrupted. A pause frees the host to stop reading frames
back. `RefinePause` is not acknowledged, repeating the current state does
nothing, and every connection starts unpaused.

### 7.9 Clipboard

With the `clipboard` capability in effect, either peer sends
`ClipboardData` when its clipboard changes:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `mime_type` | string | `text/plain`. No other type is defined in this version. |
| 2 | `data` | bytes | The clipboard text, UTF-8. |

Rules:

- A peer MUST drop a `ClipboardData` whose `mime_type` is not
  `text/plain` (a later version may define more), whose `data` is empty,
  or whose `data` exceeds 1 047 552 bytes (1 MiB − 1 KiB, so that the
  message fits one control frame, §3.1). A peer MUST NOT send any of
  those. There is no "clear the clipboard" operation: a local clipboard
  that holds nothing readable is simply not sent.
- Immediately after `SessionAccept` the host sends its current clipboard
  text once, if it has any, because the desktop's clipboard may have
  changed while no client was attached. The client MUST NOT send its
  clipboard on attach: connecting must not overwrite the desktop's
  clipboard with the client's.
- After that, each peer sends on every local clipboard change, except
  that it MUST NOT send back text it has just received from the other
  peer and applied locally. (Applying the peer's text raises a local
  change notification on every platform; it is an echo, not a new copy.)
- Only the clipboard selection is synchronized, not the primary
  selection.

A peer offers the capability only if it has a working clipboard
mechanism.

### 7.10 Logout and the end of a session

`LogoutRequest` (client → host, no fields) asks the host to end the
desktop session itself, not merely this connection (which an ordinary
close does), the way the desktop's own logout would. It is unconditional
and unacknowledged. Holding a verified token proves the connection
belongs to the session's user, so no further authorization applies; the
request can only end that user's own session.

When the desktop session ends while a viewer is attached, whether by
`LogoutRequest`, the desktop's own logout or a crash, the host closes the
connection with `SESSION_ENDED`. That close is the completion signal for
`LogoutRequest`. When the session ends because its user logged in at the
host's console, the host closes with `ENDED_BY_LOCAL_LOGIN`.

A client that closes its connection detaches; the session keeps running
and the client can reattach through the lobby. The client closes with
code 0.

### 7.11 Diagnostics

When the session's user asks the host for a support report (wraith's
`--report`, run inside the desktop), the host sends `DiagnosticsRequest`
(no fields) so the report can include the client's side of whatever is
going wrong. The client answers once with `DiagnosticsReport`: one field,
`text` (1, `bytes`), at most 512 KiB, plain text meant for a person to
read: its recent log, the decoder and GPU it is using, and what it
offered in `SessionHello`. The host adds it to the user's report
unchanged, on the host, in the user's own account; it is never sent
anywhere else. The request is unconditional, needs no capability, and a
client that does not know it ignores it (§16), so the host MUST NOT wait
for the answer indefinitely.

---

## 8. Input stream

### 8.1 Envelope

The input stream carries the user's input to the host, one event per
frame, in order:

```protobuf
message InputEnvelope {
  uint64 client_time_us = 1;  // the client's monotonic clock, µs
  oneof event {
    KeyEvent key = 2;
    PointerMotion pointer_motion = 3;
    PointerButton pointer_button = 4;
    PointerAxis pointer_axis = 5;
    TouchEvent touch = 6;
    GamepadConnect gamepad_connect = 7;
    GamepadState gamepad = 8;
    GamepadDisconnect gamepad_disconnect = 9;
    HidConnect hid_connect = 10;
    HidInput hid_input = 11;
    HidGetReportReply hid_get_report_reply = 12;
    HidSetReportReply hid_set_report_reply = 13;
  }
}
```

`client_time_us` is the time the event happened on a monotonic clock of
the client's choosing, with an arbitrary epoch. The host uses only
differences between values, to measure input latency; ordering comes
from the stream. What the host sends on the input stream is in §8.4.

A host MUST NOT act on input before the session is established, and MUST
ignore events it does not support.

### 8.2 Keyboard and pointer

**`KeyEvent`:**

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `hid_usage` | uint32 | The key, as a USB HID usage ID on the Keyboard/Keypad page (0x07). |
| 2 | `state` | `KeyState` | 1 `KEY_STATE_PRESSED`, 2 `KEY_STATE_RELEASED`. |

Keys are physical positions, not characters: the client translates its
platform's key codes to HID usages, and the host translates HID usages to
its own and applies its own keyboard layout. A client SHOULD release every
key it reported pressed before it detaches or loses focus.

**`PointerMotion`:**

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `dx` | double | Horizontal: a position or a delta, per `absolute`. |
| 2 | `dy` | double | Vertical. |
| 3 | `absolute` | bool | True: `dx`, `dy` are a position, 0 to 1 over the output, (0, 0) at the top left. False: a relative movement in pixels, positive right and down. |

Absolute motion moves the pointer to a point; relative motion moves it by
an amount and is what an application that locks or confines the pointer
needs. A client typically sends absolute motion while the pointer is free
and relative motion while it has captured the mouse.

**`PointerButton`:**

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `button` | uint32 | The button as a Linux `BTN_*` code from `input-event-codes.h`: `BTN_LEFT` 0x110, `BTN_RIGHT` 0x111, `BTN_MIDDLE` 0x112, `BTN_SIDE` 0x113, `BTN_EXTRA` 0x114, and so on. |
| 2 | `state` | `KeyState` | Pressed or released. |

**`PointerAxis`** (scrolling):

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `horizontal` | double | Distance in wheel notches, positive right. |
| 2 | `vertical` | double | Distance in wheel notches, positive down. |

1.0 is one detent of a notched wheel. A smooth-scrolling device (a
touchpad, a high-resolution wheel) sends fractions, and one message may
carry several notches or both axes. Signs follow pointer motion's
coordinate space whatever the client platform's convention (SDL's
vertical wheel is up-positive; the DOM's `deltaY` is down-positive at
about 100 pixels a notch). The client applies its own natural-scrolling
setting before sending; the host applies none. The host converts notches
to its input system's units (120 per notch in Linux's high-resolution
scroll, for example), keeping fractions where it can.

### 8.3 Touch

**`TouchEvent`:**

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `touch_id` | uint32 | Identifies one contact from down to up or cancel. |
| 2 | `phase` | `Phase` | 1 `PHASE_DOWN`, 2 `PHASE_MOVE`, 3 `PHASE_UP`, 4 `PHASE_CANCEL`. |
| 3 | `x` | double | 0 to 1 over the output. |
| 4 | `y` | double | 0 to 1 over the output. |

Defined for clients with touch screens. A host without touch injection
ignores it; the reference session agent does.

### 8.4 Host to client

Under the capability `hid` (§8.6), the host sends toward the client on
the input stream, one `HostInputEnvelope` per frame:

```protobuf
message HostInputEnvelope {
  oneof event {
    HidRejected hid_rejected = 1;
    HidOutput hid_output = 2;
    HidGetReport hid_get_report = 3;
    HidSetReport hid_set_report = 4;
  }
}
```

Without `hid` the host sends nothing on the input stream, and a client
MUST ignore anything it receives there.

### 8.5 Gamepads

Only with the `gamepad` capability in effect; a host MUST ignore all
three gamepad messages otherwise.

A client forwards up to 4 controllers, each in a slot `pad_index` from 0
to 3. A host MUST ignore a gamepad message whose `pad_index` is 4 or more.

**`GamepadConnect`** (fields `pad_index` 1, uint32; `name` 2, string): a
controller appeared in the slot. `name` is for the host's logs only and
MUST NOT be trusted for anything. A connect for an occupied slot replaces
its controller.

**`GamepadDisconnect`** (field `pad_index` 1, uint32): the slot's
controller went away. A disconnect for an empty slot is ignored.

The host keeps one virtual controller per connected slot for as long as
the client is attached, and removes them all when it detaches.

**`GamepadState`:**

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `pad_index` | uint32 | The slot. |
| 2 | `axes` | repeated float | Every axis, in the order below. |
| 3 | `buttons` | repeated bool | Every button, in the order below. |

`GamepadState` is the controller's complete state, not a change: the host
compares it with what it last applied. A host MUST pad shorter arrays
with 0 and false and MUST ignore elements beyond the counts below, so that
peers built against different revisions of the list degrade gracefully.
A `GamepadState` for a slot that is not connected is ignored.

The order is SDL3's `SDL_GamepadAxis` and `SDL_GamepadButton` order. It
is fixed here as a wire fact, independent of the SDL release either end
uses.

Axes (6). Sticks range −1 to 1, positive right and down; triggers range
0 to 1.

| Index | Axis |
|---|---|
| 0 | left stick X |
| 1 | left stick Y |
| 2 | right stick X |
| 3 | right stick Y |
| 4 | left trigger |
| 5 | right trigger |

Buttons (26). Face buttons are named by position: south is Xbox A and
PlayStation Cross.

| Index | Button | Index | Button |
|---|---|---|---|
| 0 | south | 13 | d-pad left |
| 1 | east | 14 | d-pad right |
| 2 | west | 15 | misc 1 (Xbox share button, Switch capture, ...) |
| 3 | north | 16 | right paddle 1 |
| 4 | back | 17 | left paddle 1 |
| 5 | guide | 18 | right paddle 2 |
| 6 | start | 19 | left paddle 2 |
| 7 | left stick click | 20 | touchpad click |
| 8 | right stick click | 21 | misc 2 |
| 9 | left shoulder | 22 | misc 3 |
| 10 | right shoulder | 23 | misc 4 |
| 11 | d-pad up | 24 | misc 5 |
| 12 | d-pad down | 25 | misc 6 |

What the host makes of the state is its own choice. *Host behavior:* the
reference host presents an Xbox 360 controller, with the d-pad as a hat
and buttons it has no equivalent for dropped.

### 8.6 Raw HID controllers

Only with both `gamepad` and `hid` in effect; a host MUST ignore the
messages below otherwise, and a client MUST NOT send them.

A client may forward a controller as its raw HID interface instead of as
`GamepadState` snapshots. The host creates a HID device with the
controller's own identity and report descriptor, the host's own driver
for that controller binds to it, and the two ends relay the controller's
reports unchanged. Whatever the controller and the host's driver can do
together then works in the session: touchpad, motion sensors, light bar,
rumble, and vendor protocols such as Steam Input's.

A raw controller occupies a slot (§8.5) like any other. `GamepadDisconnect`
ends it, and a `HidConnect` or `GamepadConnect` for an occupied slot
replaces what is there. The host removes raw controllers when the client
detaches, as it does gamepads.

A client forwards only game controllers this way: devices its platform's
game-controller support recognises, never keyboards, mice or security
keys. While a controller is forwarded raw, the client SHOULD NOT also
drive it through its own controller support (which may set lights or
modes of its own); the host's driver owns the device.

**`HidConnect`:** a controller appeared in the slot, raw.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `pad_index` | uint32 | The slot. |
| 2 | `name` | string | The device's product name, as its platform reports it. |
| 3 | `bus` | uint32 | How the controller is attached: 3 USB, 5 Bluetooth (Linux's `BUS_*` values). |
| 4 | `vendor_id` | uint32 | USB or Bluetooth vendor ID. |
| 5 | `product_id` | uint32 | Product ID. |
| 6 | `version` | uint32 | Release number (`bcdDevice`). |
| 7 | `uniq` | string | The device's serial number; for a Bluetooth device, its address as `aa:bb:cc:dd:ee:ff`. May be empty. |
| 8 | `report_descriptor` | bytes | The HID report descriptor, 1 to 4096 bytes. |

The host's drivers choose and parse on these, so they must be the
controller's own: `name`, `uniq` and `version` included, since drivers
and the games above them key on them (a Bluetooth driver may take the
controller's address from `uniq`).

**`HidRejected`** (host → client; fields `pad_index` 1, uint32; `reason`
2, string): the host will not, or could not, present the controller in
the slot raw, and the slot is empty again. The client MAY forward the
same controller as a gamepad (§8.5) instead. `reason` is for logs. A host
rejects a controller it cannot create a device for, one its policy
refuses (§17), or one its driver fails to bind; since that last can only
be known once the device exists, a rejection may come after reports have
been exchanged.

**`HidInput`** (fields `pad_index` 1, uint32; `data` 2, bytes): one input
report, as the controller delivered it: beginning with its report ID if
the descriptor numbers its reports, without one if not. At most 4096
bytes. The host applies input reports in order. It MAY drop them until it
has finished checking the device.

**`HidOutput`** (host → client; fields `pad_index` 1, uint32; `data` 2,
bytes): an output report for the controller, beginning with its report
ID, or with 0 when the descriptor numbers no reports. The client writes
it to the controller.

**`HidGetReport`** (host → client) and **`HidSetReport`** (host →
client): the host's driver reads or writes a report synchronously, as
USB's `GET_REPORT` and `SET_REPORT` requests do.

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `pad_index` | uint32 | The slot. |
| 2 | `request_id` | uint32 | Identifies the request; the reply carries it back. |
| 3 | `report_id` | uint32 | The report, 0 to 255. |
| 4 | `type` | `HidReportType` | 1 `HID_REPORT_TYPE_INPUT`, 2 `HID_REPORT_TYPE_OUTPUT`, 3 `HID_REPORT_TYPE_FEATURE`. |
| 5 | `data` | bytes | `HidSetReport` only: the report, beginning with its report ID (0 when unnumbered). |

The client performs the request on the controller and answers with a
**`HidGetReportReply`** (fields `pad_index` 1, `request_id` 2, `failed` 3,
bool; `data` 4, bytes: the report, beginning with its report ID) or a
**`HidSetReportReply`** (fields `pad_index` 1, `request_id` 2, `failed`
3). It answers every request, in any order, with `failed` set when the
controller refused or the request can't be made on its platform. The
host fails a request that goes unanswered for long enough; *host
behavior:* the reference host's kernel waits 5 seconds. A reply whose
`request_id` the host has no request pending for is ignored.

*Host behavior:* the reference host creates the device through Linux's
`uhid`, so the controller's ordinary kernel driver (`hid-playstation`,
`hid-steam`, `hid-nintendo`, `hid-generic`, ...) binds to it, and Steam
or SDL in the session can reach it through `hidraw` as they would a
local one.

---

## 9. Video datagrams

### 9.1 Header

Channel 0x01. Every video datagram is the channel byte, a 16-byte header
and a slice of one coded frame:

```
 0          1          5            7              9       10       14          16
+----------+----------+------------+--------------+-------+--------+-----------+---------+
| stream_id| frame_id | slice_idx  | slice_count  | flags | pts    | host_lat  | payload |
| u8       | u32 LE   | u16 LE     | u16 LE       | u8    | u32 LE | u16 LE    | ...     |
+----------+----------+------------+--------------+-------+--------+-----------+---------+
```

(Offsets are from the start of the header, after the channel byte.)

- **`stream_id`**: the output (`OutputDescriptor.stream_id`).
- **`frame_id`**: the frame's number on its stream. It starts at 0 when
  the session is established and increases by one per frame sent,
  wrapping at 2³². It never resets during a connection. Every slice of
  one frame carries the same `frame_id`.
- **`slice_idx`**, **`slice_count`**: the frame is split into
  `slice_count` datagrams (at least 1), and this is number `slice_idx`,
  from 0. Every slice of one frame MUST carry the same `slice_count`.
- **`flags`**: bit 0, `KEYFRAME`, is set on every slice of a frame that
  the decoder can start from (an IDR picture, an AV1 key frame, every
  PyroWave frame). Bits 1–7 are reserved: a sender MUST set them to 0 and
  a receiver MUST ignore them.
- **`pts`**: the frame's capture time on the host's session clock (§11),
  µs. The same on every slice of one frame.
- **`host_lat`**: how long the host held the frame, from capture to
  handing it to the transport, in units of 10 µs, rounded, at least 1,
  saturating at 0xFFFF (about 655 ms); 0 means not reported. The same on
  every slice. Informational: it is the host's share of the frame's
  latency, which the client cannot otherwise see.

### 9.2 Slicing

The sender splits a coded frame (§9.4, or the container of §9.5) into
consecutive pieces. Every slice but the last carries the same number of
payload bytes; the last carries the rest. The cut points are arbitrary
bytes, not codec unit boundaries. A receiver needs no notice of the slice
size.

A datagram MUST fit the connection's current maximum datagram size: the
path MTU less IP, UDP and QUIC overhead, capped by the peer's
`max_datagram_frame_size`. The limit rises as QUIC's path MTU discovery
confirms larger packets and can fall on a path change, so a sender reads
it per frame. Until it is known, a sender uses 1100 bytes of slice
payload, which fits within QUIC's 1200-byte minimum on any path.

### 9.3 Reassembly

A receiver keeps at most one frame in progress per `stream_id`:

- the frame's slice count is the `slice_count` of the first of its
  slices to arrive; it places each slice by `slice_idx`, ignoring one
  whose `slice_idx` is not below that count or which it already has;
- a slice of a different `frame_id` discards the frame in progress
  (nothing is retransmitted, so an incomplete older frame is lost) and
  starts a new one;
- the frame is complete when all `slice_count` slices have arrived; its
  payload is the slices concatenated in `slice_idx` order.

A receiver reports frames it never completed as lost (§7.5). After a loss
it SHOULD discard non-keyframes until a keyframe arrives, or keep decoding
with errors concealed, at its discretion; the host repairs reported loss
with a keyframe.

### 9.4 Codec payloads

Without refinement, a reassembled payload is exactly one coded frame in
the session's codec:

- `h264`, `h265`: one access unit as an Annex B byte stream (start
  codes, then NAL units). A keyframe carries its parameter sets (VPS for
  H.265, SPS and PPS) in-band before the IDR slice.
- `av1`: one temporal unit in the low-overhead bitstream format: a
  sequence of OBUs with `obu_has_size_field` set, starting with a
  temporal delimiter. A keyframe carries a sequence header.
- `pyrowave`: one PyroWave frame, sequence header first.

The coded picture may be larger than the output (padding to the codec's
block size); the client shows the top-left `display.width` ×
`display.height` (§7.2).

### 9.5 Refinement container

With `refine` in effect (§7.8), each reassembled payload is this
container (all slices concatenated, not each slice):

```
 0       1         2       3        4             5          6            8          12          16
+-------+---------+-------+--------+-------------+----------+------------+----------+-----------+
| magic | version | flags | format | clear_count | reserved | tile_count | base_len | tiles_len |
| u8    | u8      | u8    | u8     | u8          | u8       | u16 LE     | u32 LE   | u32 LE    |
+-------+---------+-------+--------+-------------+----------+------------+----------+-----------+
```

followed by, in order:

1. `base_len` bytes: the codec payload (§9.4), byte for byte what the
   session would have carried for this frame without refinement;
2. `clear_count` rectangles;
3. `tile_count` rectangles;
4. `tiles_len` bytes: one Zstandard frame (RFC 8878).

- **`magic`** is 0x52 and **`version`** is 1.
- **`flags`**: bit 0, `RESET`, drops the whole overlay plane before the
  frame's rectangles are applied. Bits 1–7 are reserved and MUST be 0.
- **`format`**: the tile pixel format, a `RefineFormat` (below).
- **`reserved`** MUST be 0.
- **Rectangles** are 8 bytes each: `x`, `y`, `width`, `height`, u16 LE
  each, in frame pixels.
- **Tile pixels**: the Zstandard frame decompresses to the pixels of
  every tile, concatenated in tile order; each tile is row-major and
  tightly packed, `width × B` bytes per row, where `B` is the format's
  bytes per pixel. The decompressed length MUST equal the sum of
  `width × height × B` over the tiles.

**Tile formats.** Tile pixels are RGB in the output's primaries and
transfer (§7.3), not YCbCr. `RefineFormat`:

| Value | Name | `B` | Pixel |
|---|---|---|---|
| 0 | `BGR8` | 3 | Blue, green, red, 8 bits each (a little-endian XRGB8888 pixel's bytes in memory order, without the unused byte). |

Every client that offers `refine` accepts `BGR8`; it declares any other
format it accepts in `SessionHello.refine_formats` (§6.2). The host MUST
use only formats the client accepts, and MAY use different ones in
different frames, for example when the output's colour description
changes. The overlay plane holds whatever was last written to each
pixel; a host that changes format SHOULD send `RESET` with it.

`base_len` may be 0. Such a **layer-only** frame carries no video: the
client applies its rectangles to the picture it is already showing and
displays it again. Its `KEYFRAME` flag is never set. A host sends these
when the desktop has stopped changing and only the layer still has work
to do, so settling costs no encode or decode.

A receiver MUST drop (and report as lost, §7.5) a frame whose container
has the wrong magic or version, sets a reserved flag bit or the reserved
byte, names a format the client did not accept, is truncated, fails to
decompress, decompresses to the wrong length, or whose tiles claim more
than 64 MiB of pixel bytes in total. The tile rectangles alone
determine what a receiver allocates, so it MUST check that bound before
decompressing.

### 9.6 Overlay semantics

The client keeps one overlay plane per output, the size of the output,
initially transparent, drawn over the decoded video. For each frame, in
order:

1. if `RESET` is set, make the whole plane transparent;
2. for each clear rectangle, make that rectangle of the plane
   transparent;
3. for each tile, set that rectangle of the plane to the tile's pixels,
   opaque.

Rectangles are clipped to the output. The plane is independent of the
video's reference structure: a keyframe does not reset it, and a `RESET`
needs no keyframe. A client SHOULD reset the plane when the output's size
changes.

What goes into the layer is the host's policy; the format only says what
the client must draw. A host MUST, however, keep the plane correct: when
content under a tile changes, it MUST clear that region in the same frame
as the change, or the client will keep showing stale pixels.

Loss can take a frame's clears with it. A host MUST recover from that
when the client reports the loss (§7.5): it may resend the lost frame's
clears and tiles in a later frame, or send `RESET` and rebuild the plane.
It MUST send `RESET` whenever it cannot know what the client's plane
holds: on a new connection, when a `RESET` it sent was itself lost, or
when a reported loss is older than what it remembers.

*Host behavior:* the reference host sends a region losslessly once it has
been unchanged for a short settle time and clears it as soon as it changes
again, so moving content stays on the lossy codec and a still desktop
converges to a bit-exact image. It coalesces neighbouring clears into
larger rectangles and, when change is too scattered for 255 clears, sends
one clear over the bounding box rather than a `RESET`. It remembers each
recent frame's layer and, on a reported loss, resends that frame's clears
and re-sends its tiles if they are still current.

---

## 10. Audio datagrams

Channel 0x02. Every audio datagram is the channel byte, a 6-byte header
and one Opus packet (RFC 6716) of `AudioConfig.frame_ms` milliseconds:

```
 0        2        6
+--------+--------+---------+
| seq    | pts    | payload |
| u16 LE | u32 LE | ...     |
+--------+--------+---------+
```

- **`seq`**: the packet's sequence number, starting at 0 when the session
  is established and increasing by one per packet, wrapping at 2¹⁶.
- **`pts`**: the capture time of the packet's first sample on the host's
  session clock (§11), µs, the same clock as video's.

An audio packet is never sliced. A receiver uses `seq` to detect loss
and reordering (Opus packet loss concealment covers gaps) and `pts` to
schedule playback and keep it in step with video. Audio datagrams carry
no channel count or rate; those come from `SessionAccept.audio` (§6.8).

### 10.1 Microphone

Under the capability `microphone`, the client sends its microphone to the
host on channel 0x03. A datagram is laid out exactly as above (channel
byte, `seq`, `pts`, one Opus packet), in the format of
`SessionAccept.microphone` (§6.8), with these differences:

- `seq` starts at 0 when the session is established, as for the host's
  audio; `pts` is the client's session clock, zeroed when it receives
  `SessionAccept`. The host does not use it to schedule playback.
- The host buffers a few packets, conceals a short gap with Opus packet
  loss concealment, and presents the result to the session's applications
  as a recording device. A late or duplicate packet is dropped.
- A client MUST NOT open its microphone, and MUST NOT send on this
  channel, unless its user asked for it. Muting stops the packets; there
  is no message for it.
- A host MUST drop a datagram on this channel from a connection that has
  not authenticated or did not negotiate `microphone` (§3.2).

---

## 11. Session clock

Both `pts` fields are the host's session clock: a monotonic clock (on
Linux, `CLOCK_MONOTONIC`) in microseconds, zeroed when the host sends
`SessionAccept`, truncated to 32 bits. It is not wall-clock time and is
never compared with it.

The client keeps its own session clock, zeroed when it receives
`SessionAccept`; the one-way delays of §7.5 are measured against it, and
carry the unknown offset between the two zero points.

32-bit values wrap every 2³² µs (about 71.6 minutes), as do 32-bit
`frame_id`s every 2³² frames. A peer MUST compare two such values only by
their difference, taken modulo 2³² and read as a signed 32-bit integer,
and only for values close in time, never with `a > b` directly. This is
sufficient for A/V sync, jitter and delay measurement, and loss
bitmaps, in sessions of any length.

---

## 12. Error codes

One table of codes serves as the `code` of `LobbyError` and as the
application error code of every `CONNECTION_CLOSE` a GDP peer sends (and,
for browsers, §15's equivalents). Where a `LobbyError` is sent, the close
that follows carries the same code. The close code is the only channel
where no message can be sent: authentication failures, malformed frames,
and the session phase, which has no error message.

| Code | Name | Meaning |
|---|---|---|
| 0 | | Normal close; nothing to report. |
| **1x** | | **The peer broke the protocol or asked for something unsupported.** |
| 10 | `VERSION_MISMATCH` | `LobbyHello.protocol_version` is not supported. |
| 11 | `MALFORMED_FRAME` | A frame did not parse, or a message arrived out of order. On a session connection only after authentication; before it, `AUTH_FAILED`. |
| 12 | `FRAME_TOO_LARGE` | A frame's length exceeded the limit (§3.1, §4.4). On a session connection only after authentication; before it, `AUTH_FAILED`. |
| 13 | `NO_COMMON_CODEC` | No codec in `SessionHello.codecs` can be encoded, or `SessionAccept.codec` was not offered (§6.6). |
| 14 | `UNKNOWN_SESSION_TYPE` | `SessionOpen.session_type` is not one of the offered types (§4.6). |
| 15 | `UNSUPPORTED` | A stream kind or feature the peer does not implement or did not negotiate. The code a stream is reset with (§2.2), and the close code for something a session cannot continue without. |
| **2x** | | **Who the user is and what they may use.** |
| 20 | `AUTH_FAILED` | Authentication failed, the host's policy refused the account, authentication did not complete in time, or a session token was invalid. Every failure before authentication on a session connection (§6.3). |
| 21 | `HOST_AUTH_FAILED` | From a broker: the broker accepted the password and the host refused it (§5.3). |
| 22 | `NOT_ENTITLED` | From a broker: the selected device is not one of this user's (§5.3). |
| 23 | `LOCAL_SESSION_ACTIVE` | The user has a graphical session at one of the host's own seats, so the host refuses the login (§4.5). |
| 24 | `ALREADY_CONNECTED` | The session already has its viewer (§6.4). Sent only to a connection whose token verified. |
| **3x** | | **The host cannot take the session right now.** |
| 30 | `HOST_OFFLINE` | From a broker: the selected device is not connected to it (§5.3). |
| 31 | `HOST_FULL` | The host has no capacity for another session. |
| 32 | `SESSION_START_FAILED` | The host could not start the session. |
| **4x** | | **The session ended on purpose.** Close codes only, never in a `LobbyError`. |
| 40 | `SESSION_ENDED` | The desktop session ended while this client was attached: its own logout, a `LogoutRequest`, or a crash (§7.10). An ordinary end, not a failure. |
| 41 | `ENDED_BY_LOCAL_LOGIN` | The session was ended because its user logged in at the host's console (§6.4, §7.10). |
| 42 | `TAKEN_OVER` | Another connection of the same user took the session over (§6.4). The session continues. |

Codes are grouped by tens, and each group has room for more. A receiver
that does not know a code MUST treat it as the kind its group names (its
tens digit: 1x protocol, 2x access, 3x availability, 4x normal end), and
SHOULD show the number rather than fail to describe it. In the
`LobbyErrorCode` enum each name carries the prefix `LOBBY_ERROR_`, and 0
is `LOBBY_ERROR_UNSPECIFIED`.

---

## 13. Host channel (`gdp-host/1`)

A host agent that has joined a broker keeps one connection to it, on the
broker's lobby port with ALPN `gdp-host/1`, for as long as it runs. The
broker uses it to learn the host's sessions and to relay logins (§5).
Clients never see it.

### 13.1 Connection

The connection is mutually authenticated with pinned certificates
(§2.3): the broker presents its broker certificate, which the host agent
pins; the host agent presents its host certificate, which the broker
checks against the fingerprint it recorded when the host joined.

The host agent keeps the connection alive with QUIC PINGs every 10 s,
and either end treats 30 s without traffic as the channel lost. The host
agent then reconnects after 1 s, doubling the delay up to 60 s, with up
to 50 % random jitter added to each delay.

Messages are framed as §3.1. The first bidirectional stream, opened by
the host agent, is the **control stream**; each message on it is a
`HostEnvelope`:

```protobuf
message HostEnvelope {
  oneof msg {
    // Joining and connecting.
    Join join = 1;                                // host -> broker
    JoinAccepted join_accepted = 2;               // broker -> host
    HostHello hello = 3;                          // host -> broker
    HostWelcome welcome = 4;                      // broker -> host
    HostError error = 5;                          // broker -> host
    // Leaving.
    Leave leave = 6;                              // host -> broker
    Left left = 7;                                // broker -> host
    // What the host tells the broker once connected.
    Snapshot snapshot = 8;                        // host -> broker
    SessionStarted session_started = 9;           // host -> broker
    SessionEnded session_ended = 10;              // host -> broker
    ViewerAttached viewer_attached = 11;          // host -> broker
    ViewerDetached viewer_detached = 12;          // host -> broker
    LocalLoginTakeover local_login_takeover = 13; // host -> broker
  }
}
```

```
host agent                        broker
   |-- Join ---------------------->|   joining: once, on its own connection
   |<-- JoinAccepted / HostError --|   then the connection closes

   |-- HostHello ----------------->|   every later connection
   |<-- HostWelcome / HostError ---|
   |-- Snapshot ------------------>|
   |-- SessionStarted, SessionEnded, ViewerAttached,
   |   ViewerDetached, LocalLoginTakeover ...   as they happen
```

### 13.2 Joining

An administrator issues a single-use join token at the broker (valid for
an hour by default), presented as one string:

```
<id>.<secret>:sha256:<broker certificate fingerprint>
```

The host agent pins the fingerprint, connects, and sends `Join`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `token` | string | `<id>.<secret>`, without the fingerprint suffix. |
| 2 | `hostname` | string | The host's name, the device's initial display name. |
| 3 | `client_address` | string | Where clients reach this host's sessions directly, for `Redirect.host` (§5.4); empty means the address the broker saw. |
| 4 | `version` | string | The host agent's version, informational. |

The broker records the certificate the host presented on this connection
as the device's pin and answers `JoinAccepted{device_id}` (field 1,
string). It stores only a SHA-256 digest of the secret. A host that joins
again with the same certificate keeps its device id, name, settings and
entitlements.

### 13.3 Connecting and events

On every later connection the host agent first sends `HostHello`
(`device_id` 1, string; `version` 2, string). The broker answers
`HostWelcome` (no fields) if the device exists, its certificate matches
the pin and it is enabled; the host agent then sends a `Snapshot`.

**Refusal.** The broker answers `HostError{code, message}` and closes the
connection with the same code:

| Code | Name | Meaning |
|---|---|---|
| 10 | `HOST_ERROR_BAD_TOKEN` | The join token is unknown, used or expired. |
| 11 | `HOST_ERROR_UNKNOWN_DEVICE` | No such device: never joined, or removed at the broker. |
| 12 | `HOST_ERROR_CERT_MISMATCH` | The host's certificate is not the one it joined with, or it presented none. |
| 20 | `HOST_ERROR_DISABLED` | The device is disabled at the broker. |
| 30 | `HOST_ERROR_MALFORMED` | Any other protocol violation. |

The groups are 1x identity, 2x state and 3x protocol, as in §12. A
`HostError` can also arrive after `HostWelcome`, when the device is
removed or disabled while connected.

**Leaving.** `Leave` (no fields), after `HostWelcome`, makes the broker
delete the device; it answers `Left` (no fields) and closes.

**Events.** Sessions are identified to the broker by username, because
the broker's accounts and the host's share names, not uids; the uid is
included for logging.

| Message | Fields | When |
|---|---|---|
| `Snapshot` | `sessions` (1, repeated `RunningSession`), `available_types` (2, repeated `HostSessionType{id, name}`), `default_type` (3, string) | After every `HostWelcome`. Replaces everything the broker knew about the host's sessions, so a lost event is corrected at the next reconnect. The desktop list is what the broker puts in `Device.available_types` and `default_type` (§5.2), refreshed only by a new `Snapshot`. |
| `SessionStarted` | `uid` (1), `username` (2), `session_type` (3), `started_at_unix` (4) | A session started. |
| `SessionEnded` | `uid` (1), `username` (2), `reason` (3, free text) | A session ended. |
| `ViewerAttached` | `uid` (1), `username` (2) | A client attached to the session. |
| `ViewerDetached` | `uid` (1), `username` (2) | The client detached. |
| `LocalLoginTakeover` | `uid` (1), `username` (2) | The user logged in at the host's console, which ended their session. A `SessionEnded` follows. |

`RunningSession`: `uid` (1, uint32), `username` (2, string),
`session_type` (3, string), `started_at_unix` (4, int64),
`viewer_attached` (5, bool).

### 13.4 Login streams

For each login it relays (§5.3), the broker opens a new bidirectional
stream. Its first frame is a `LoginStreamOpen`:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `forwarded_for` | string | The client's address as the broker saw it. |
| 2 | `client_id` | string | `LobbyHello.client_id`, for the host's logs. |

After it, the stream carries the lobby exchange of §4 exactly, with the
broker in the client's place. `LoginStreamOpen` is subject to the same
size limit and deadline as `LobbyHello` (§4.4).

The host applies its pre-authentication penalties (§4.4) to
`forwarded_for`, never to the broker's own address, and passes it to its
authentication stack as the remote host. It is believed only because it
arrives on the mutually authenticated host channel; a direct lobby
connection has no such field. Where a direct lobby connection would end
with a close and no `LobbyError`, a login stream is reset with that code
instead, and the channel stays up. A host agent accepts up to 64
concurrent login streams.

---

## 14. Thin-client channel (`wisp/1`)

A Wisp thin client reports to a broker over one connection to the
broker's lobby port with ALPN `wisp/1`, kept open for as long as the
client runs. The connection's existence is the client's online state. The
client pins the broker certificate (§2.3) and keeps the connection alive
with QUIC PINGs.

The client opens one bidirectional stream. Every message on it is a
`WispEnvelope`, framed as §3.1 with a 16 KiB limit in both directions:

```protobuf
message WispEnvelope {
  oneof msg {
    WispHello hello = 1;      // client -> broker, first
    WispWelcome welcome = 2;  // broker -> client, in answer
    WispProfile profile = 3;  // broker -> client, whenever the profile changes
    WispSession session = 4;  // client -> broker, whenever a session starts or ends
  }
}
```

**`WispHello`** (client → broker, first):

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `key` | string | The shared key of the boot environment that started this client. |
| 2 | `mac` | string | The client's identity: the MAC address of the interface with the default route, lowercase, colon-separated. |
| 3 | `hostname` | string | |
| 4 | `ips` | repeated string | The client's own addresses, without loopback. |
| 5 | `arch` | string | The machine architecture (`uname -m`). |
| 6 | `image_version` | string | The client image's version. |
| 7 | `uptime_secs` | uint64 | Seconds since boot. The client has no trustworthy wall clock; the broker dates everything itself. |
| 8 | `system` | `SystemInfo` | Hardware report. |
| 9 | `session` | `WispSession` | The session already running, if any. |

`SystemInfo`: `cpu_model` (1, string), `cpu_cores` (2, uint32),
`memory_bytes` (3, uint64), `gpus` (4, repeated `Gpu{name 1, driver 2}`),
`hw_decode` (5, repeated string: working decode paths as
`<decoder>:<codec>`, e.g. `vulkan:av1`, `software:h264`), `displays` (6,
repeated `Display{connector 1, width 2, height 3, refresh_mhz 4}`),
`nic_speed_mbps` (7, uint32; 0 unknown).

The broker MUST compare `key` in constant time. A wrong or missing key,
or no key configured, closes the connection with `AUTH_FAILED`; a first
message that is not `WispHello`, or a malformed `mac`, closes with
`MALFORMED_FRAME`. The hello is subject to the lobby's pre-authentication
deadline (§4.4). A newer connection from the same `mac` replaces an older
one, which the broker closes with code 0.

**`WispWelcome`** and **`WispProfile`** (broker → client) each carry one
`ClientProfile` (field 1): the settings every thin client applies. The
broker sends every field, so a proto3 default never stands in for a
setting:

| # | Field | Type | Meaning |
|---|---|---|---|
| 1 | `resolution` | string | `WIDTHxHEIGHT` to request (§6.2), or empty for the client's screen size. |
| 2 | `view` | string | `fit`, `actual`, or empty for the client's own choice. |
| 3 | `preferred_decoder` | string | `vulkan`, `native` or `software`. |
| 4 | `preferred_codec` | string | A §6.6 token to offer first, or empty for automatic. |
| 5 | `lossless_refinement` | bool | Start sessions with the lossless layer on; false pauses it from the start (`RefinePause`, §7.8). |
| 6 | `allow_pyrowave` | bool | Allow offering `pyrowave` (§6.6). |
| 7 | `network_profile` | string | `auto`, `lan`, `internet` or `mobile` (§6.9). |
| 8 | `forward_gamepads` | bool | Offer `gamepad` (§8.5). |
| 9 | `microphone` | bool | Offer `microphone` (§10.1). |
| 10 | `debug_logging` | bool | Run the stream client with its debug log on. |
| 11 | `display_sleep_minutes` | uint32 | Turn the display off after this many minutes without input; 0 never. |

Client settings in a profile take effect
at the next session the client starts, never in a running one;
`display_sleep_minutes` takes effect at once.

**`WispSession`** (client → broker): `user` (1, string), `host_name` (2,
string); all empty means no session.

---

## 15. Browser transports

A browser cannot open raw QUIC connections, so the browser client's
sessions run through a broker's gateway (§5.4) over one of two
transports, served at the address the browser loaded the client from,
with that site's web certificate. Everything inside them is §6–§11
unchanged: the same `SessionHello` with a gateway token, the same control
and input streams, the same datagrams. How a browser obtains the gateway
token (the broker's web sign-in) is outside this protocol.

**WebTransport** (HTTP/3, at `https://<broker>/gdp`). The session's first
bidirectional stream is the control stream, its second the input stream,
and its datagrams are §3.2's. A WebTransport server cannot always deliver
an application close code, so before closing, the broker opens one
unidirectional stream carrying the 8 ASCII bytes `GDPCLOSE` followed by
the §12 code as a u32 LE (12 bytes in all), and finishes it. The client
takes that code in preference to whatever the session's close reports.

**WebSocket** (`wss://<broker>/gdp/ws`), the fallback where WebTransport
is unavailable. Every binary message is one channel byte followed by that
channel's bytes:

| Channel | Contents |
|---|---|
| 0 | Control stream bytes, either direction. |
| 1 | Input stream bytes, either direction. |
| 2 | One datagram (§3.2, channel byte included), broker to browser. |

Stream bytes are a byte stream as on QUIC: §3.1 frames may be split
across messages or share one. A close carries the §12 code plus 4000 as
the WebSocket close code (4000–4999 is the range WebSocket reserves for
applications), in both directions. A WebSocket is reliable, so the broker
bounds latency by dropping datagrams that would queue behind a full send
buffer; the loss bitmap reports them as usual.

---

## 16. Extending the protocol

This specification is version 1.0. The ALPN `gdp/1` is the compatibility
boundary: within it, nothing defined here changes meaning, and every
number in its tables and every field number in its messages is fixed.

The ways to extend it, from the least to the most disruptive:

1. **New protobuf fields and messages.** A new field on an existing
   message, or a new `oneof` case in an envelope. Old readers skip what
   they do not know (§3.1). A new field MUST have a meaning that an old
   peer can safely ignore; anything else needs a capability.
2. **New datagram channels and video flag bits** (§3.2, §9.1). Old
   readers drop or ignore them; a sender uses them only under a
   capability.
3. **New tokens.** Codec tokens (§6.6, §6.8), capability names (§6.7),
   stream kinds (§2.2) and clipboard MIME types (§7.9) are negotiated or
   checked per session, so an old peer never receives them.
4. **New error codes**, in the group that describes them (§12).
5. **A new ALPN** (`gdp/2`) for anything that changes what existing bytes
   mean. A server may offer both ALPNs during a transition; QUIC selects
   one per connection. `LobbyHello.protocol_version` changes with it.

Registries, all lowercase tokens unless stated:

| Registry | Defined in | Values in this version |
|---|---|---|
| ALPN | §1.3 | `gdp/1`, `gdp-host/1`, `wisp/1` |
| Datagram channels | §3.2 | 0x01 video, 0x02 audio, 0x03 microphone |
| Video codecs | §6.6 | `h264`, `h265`, `av1`, `pyrowave` |
| Audio codecs | §6.8 | `opus` |
| Capabilities | §6.7 | `clipboard`, `refine`, `gamepad`, `hid`, `microphone`; `usb` reserved |
| Stream kinds | §2.2 | none; `usb` reserved |
| Clipboard MIME types | §7.9 | `text/plain` |
| Refinement tile formats | §9.5 | 0 `BGR8` |
| Error codes | §12, §13.3 | as listed |

---

## 17. Security considerations

**Server authentication.** Every server is authenticated by a pinned
certificate fingerprint or, for a lobby, optionally by a CA (§2.3). The
first connection to a host with a self-signed certificate is
trust-on-first-use: a client that accepts an unverified fingerprint can
be impersonated on that first contact, so a client SHOULD show the
fingerprint for out-of-band comparison. A CA-issued lobby certificate
removes that first-contact exposure but trusts every CA the client
trusts: any of them misissuing a certificate for the host's name enables
impersonation, which a pin would have caught. A client trusting only an
organization's own CA avoids that. Session certificates are never
trusted on first use or by CA: they are vouched for by the authenticated
lobby connection.

**User authentication.** Passwords reach the host agent only inside TLS,
after the certificate check. Every authentication failure looks the same
(`AUTH_FAILED`), and the pre-authentication limits (§4.4, §6.3) bound the
memory, time and connection slots an anonymous peer can hold. A host
SHOULD penalize sources of repeated failures.

**Session tokens.** A session token (§4.8) authorizes its bearer to attach
to the session once, before it expires. It is not bound to the TLS
connection that carries it, so a token that leaks before the client
presents it (within 30 s in the reference implementation) can be used in
the client's place; the client then fails with `AUTH_FAILED`, which
makes the theft visible. The token is delivered only over the
authenticated lobby connection, and a client SHOULD keep it out of
places other local users can read, such as a process's command line.
The one-viewer rule (§6.4) means a stolen token cannot silently displace
an attached viewer: it is refused with `ALREADY_CONNECTED` unless it
asks to take over, which the attached viewer sees as `TAKEN_OVER`. A
future revision may bind the token to the session connection with a TLS
exporter (RFC 5705). Gateway tokens (§5.4) are single-use too.

A static token (§6.11) is a reusable bearer secret: anyone who learns it
can attach to, take over or end the session until the operator changes
it. The client pins the session agent's certificate before sending it,
so it is not sent to an impostor.

**Session isolation.** A session agent holds only its own throwaway
certificate and session secret, so a user who reads their own session
agent's memory can impersonate nothing but their own session.
`LogoutRequest` and take-over need no authorization beyond the token,
because they can affect only the token holder's own session.

**Diagnostics.** A host can send `DiagnosticsRequest` whenever it likes,
and a client that answers hands the host its recent log and GPU details
(host names, addresses, driver versions). The host already learns the
client's codecs and decoders from `SessionHello`, and the request comes
from the host the user chose to connect to, but a client SHOULD still note
each request in its own log and answer at most once every 30 s, as spectre
does. The text goes no further than the host's report file in the user's
own account.

**Brokers.** A broker sees every user's password for the duration of a
relayed login (§5.3) and, for gateway sessions, the whole session in
plain text, because it decrypts and re-encrypts it. A broker is trusted
with everything it relays. `pyrowave` is never used through a gateway.
The host channel is mutually authenticated, which is what makes
`LoginStreamOpen.forwarded_for` trustworthy.

A broker that presents its CA-issued web certificate on `gdp/1` (§2.3)
shares that certificate's private key between its web server and its
lobby, so a compromise of either exposes both.

**Thin clients.** The `wisp/1` key is shared by every client of a boot
environment and identifies the environment, not a device; a thin client's
MAC address is self-reported. The channel is suited to a trusted network,
and grants nothing beyond reporting status and receiving the client
profile.

**Untrusted input.** Every length and count on the wire comes from the
peer. Receivers MUST bound what they allocate from them: frame lengths
(§3.1), slice counts (§9.3), refinement tile areas (§9.5, 64 MiB),
cursor images (§7.4, 384 pixels a side), clipboard data (§7.9, 1 MiB −
1 KiB), gamepad slots and arrays (§8.5), HID descriptors and reports
(§8.6, 4096 bytes). Gamepad names
and `client_id` are for logs only.

**Raw controllers.** A HID device's identity and descriptor decide which
of the host's drivers binds to it and what that driver makes of it, so
`hid` (§8.6) lets the client plug arbitrary HID hardware into the host
as far as the host allows. A host MUST NOT let a raw controller become a
keyboard, a pointer, a system-control device (power, sleep), a switch or
anything else beyond a game controller, and MUST check the devices its
drivers actually create, not only the descriptor it was sent: a driver
picked by vendor and product ID may create devices the descriptor never
describes. *Host behavior:* the reference host accepts only USB and
Bluetooth, binds only drivers on a short allowlist, and destroys a
device as soon as any input device it produced reports a key below
`BTN_MISC`, a key outside the gamepad ranges, relative motion, a switch
or a sound. The reference
implementation fuzzes its datagram, refinement-container and control
parsers.

**Privacy of the session.** Video, audio, input and clipboard are
encrypted by QUIC between the client and the session agent (or the
gateway). The clipboard capability sends whatever the user copies on
either side to the other; a client SHOULD let its user turn it off.

---

## Appendix A. Limits and constants

| Constant | Value | Section |
|---|---|---|
| Stream frame length, maximum | 1 MiB (1 048 576) | §3.1 |
| Pre-authentication frame length, lobby and session | 16 KiB (16 384) | §4.4, §6.3 |
| `wisp/1` frame length | 16 KiB | §14 |
| Lobby authentication deadline (default) | 120 s | §4.4 |
| Session token lifetime (reference) | 30 s | §4.8 |
| Session token, decoded | 56 bytes (16 + 8 + 32) | §4.8 |
| `SessionHello` deadline | 5 s | §6.3 |
| Pending session connections | 4 | §6.3 |
| Output size per side | 320–8192, even | §7.2 |
| Cursor image side, maximum | 384 pixels | §7.4 |
| `StatsReport` interval | 250 ms | §7.5 |
| Loss bitmap window | 64 frames | §7.5 |
| Frame train: minimum datagrams; per report | 16; 8 | §7.5 |
| `GatewayPath` interval | about 1 s | §6.10 |
| Clipboard data, maximum | 1 047 552 bytes (1 MiB − 1 KiB) | §7.9 |
| `DiagnosticsReport` text, maximum | 512 KiB | §7.11 |
| Gamepad slots; axes; buttons | 4; 6; 26 | §8.5 |
| HID report descriptor; report | 4096 bytes; 4096 bytes | §8.6 |
| HID request timeout (reference host) | 5 s | §8.6 |
| Video header | 16 bytes (+1 channel) | §9.1 |
| Slice payload before the path is known | 1100 bytes | §9.2 |
| `host_lat` unit; maximum | 10 µs; 0xFFFF | §9.1 |
| Refinement header; rectangle | 16 bytes; 8 bytes | §9.5 |
| Refinement magic; version | 0x52; 1 | §9.5 |
| Refinement clears per frame | 255 | §9.5 |
| Refinement tile pixel bytes per frame | 64 MiB | §9.5 |
| Audio header | 6 bytes (+1 channel) | §10 |
| Audio format | Opus, 48 kHz, 2 channels, 10 ms | §6.8 |
| Session clock | 32-bit µs, wraps after ~71.6 min | §11 |
| Host channel: PING; idle; reconnect | 10 s; 30 s; 1 s doubling to 60 s, +≤50 % jitter | §13.1 |
| Host channel: concurrent login streams | 64 | §13.4 |
| Join token validity (default) | 1 hour | §13.2 |
| WebSocket close code offset | 4000 | §15 |

## Appendix B. Implementation map

Informative. Where each part of this specification is implemented in
this repository, for anyone changing it. A change to the wire touches
every implementation listed for it.

| Part | Files |
|---|---|
| Messages | `libgdp/proto/lobby.proto`, `session.proto`, `wisp.proto`; `host/proto/broker.proto`; the browser client's hand-written codec in `host/veil/app/proto.js` |
| ALPN, wire version | `libgdp/include/gdp/version.hpp`; `GDP_WIRE_VERSION` in `host/ghostd/src/lobby.rs` |
| Framing | `libgdp/include/gdp/framing.hpp`; `host/ipc/src/framing.rs` |
| Datagrams, slicing, reassembly | `libgdp/include/gdp/datagram.hpp`, `video_reassembler.hpp` |
| Codec and capability negotiation | `libgdp/include/gdp/negotiation.hpp`, `video_codec.hpp`, `audio_format.hpp` |
| Refinement container | `libgdp/include/gdp/refine.hpp`; `host/veil/app/refine.js` |
| Clipboard rules | `libgdp/include/gdp/clipboard.hpp` |
| Gamepad order | `libgdp/include/gdp/gamepad.hpp` |
| Session clock | `libgdp/include/gdp/clock.hpp` |
| Error codes | `libgdp/include/gdp/error_codes.hpp` (pinned by `error_codes_test`); `LobbyErrorCode` in `lobby.proto`; `host/veil/app/session.js` |
| Fingerprints, CA checks | `libgdp/include/gdp/cert_fingerprint.hpp`; `CaTrust` in `transport.hpp`; `host/veil/src/lobby_tls.rs` (which certificate the broker presents) |
| Session token | `host/authticket/src/lib.rs` (mint, in ghostseat), `host/wraith/src/session/token.cpp` (verify) |
| Lobby server | `host/ghostd/src/lobby.rs`; `host/veil/src/lobby.rs` (broker) |
| Session agent | `host/wraith/src/session/gdp_session.cpp` |
| Diagnostics (§7.11) | `host/wraith/src/session/gdp_session.cpp`, `report_server.cpp` and `report_cli.cpp` (the host); `client/spectre/src/net/session_client.cpp` and `host/veil/app/session.js` (the clients) |
| Gateway, browser transports | `host/veil/src/gateway.rs`, `wt.rs`, `ws.rs`; `host/veil/app/transport.js` |
| Host channel | `host/ghostd/src/broker.rs`; `host/veil/src/hosts.rs` |
| Thin-client channel | `client/wisp/agent/`; `host/veil/src/thin_clients.rs` |
| Fuzzers | `libgdp/fuzz/` |
