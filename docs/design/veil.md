# Veil

Veil is the broker: one daemon, `veild`, that a deployment's users log in
to instead of a single host. It shows each user the hosts ("devices")
they are entitled to, hands the login on to the one they pick, and can
carry the session itself. Code: `host/veil/`. Wire format: gdp-spec.md
§5 (brokered login), §13 (host channel), §14 (thin clients), §15
(browser transports).

The browser client and Wisp thin clients have their own pages:
[browser client](browser-client.md), [Wisp](wisp.md).

## Install and identity

`make install-veil` (never `make install`) installs `veild`,
`veild.service`, `/etc/pam.d/veild` and, if absent,
`/etc/ghost/veild.toml`; creates the `veil` system user and the
`ghost-admins` group; and generates, once:

- the lobby certificate, `/etc/ghost/veil-{cert,key}.pem`, key
  `root:veil 0640`. spectre pins it on first use and every joined host
  pins it at join, so replacing it means re-joining every host;
- a self-signed web certificate, `/etc/ghost/veil-web-{cert,key}.pem`
  (P-256, 825 days, names for the FQDN, short name, localhost and the
  machine's addresses), the default for `[web] cert`/`key`, so the admin
  UI works on first start behind a browser warning;
- the Wisp key, `/etc/ghost/veil-wisp.key` ([Wisp](wisp.md)).

veild runs as `veil`, not root, and never runs PAM itself: every login
goes through ghostauth, the helper systemd starts per connection to
`/run/ghost/auth.sock` as its own unprivileged user
([authentication](login-and-sessions.md#authentication)). The unit adds
`ghost` as a supplementary group, which is what the socket admits. A
Veil that isn't also a host gets ghostauth, its socket unit and its
account from `make install-veil`. Ports below 1024 come from
`CAP_NET_BIND_SERVICE`. veild is non-dumpable, since it holds passwords
in flight.

Configuration is layered as ghostd's ([configuration](../reference/configuration.md)),
always with the ownership check. `veild -t` checks it and the lobby
certificate, `veild -T` prints it, `veild token` issues a join token
from the command line, and `veild wisp-env` prints the Wisp boot
server's `.env` lines.

One UDP port (`[lobby] port`, 4442) serves three ALPNs: `gdp/1` (logins
and gateway sessions), `gdp-host/1` (host channels) and `wisp/1` (thin
clients). The pre-authentication limits ([preauth](preauth.md)) run
before the handshake, when the ALPN isn't known yet, so they cover all
three. A host holds a MaxStartups slot until its handshake completes,
and a thin client until its key checks out.

## The host channel

A joined ghostd keeps one QUIC connection to veild (`gdp-host/1`,
`host/ghostd/src/broker.rs` and `host/veil/src/hosts.rs`). ghostd dials
out, so a host behind NAT needs no inbound port for Veil's logins. Both
sides authenticate: ghostd presents its host certificate and pins
Veil's; veild checks the host's certificate against the pin it recorded
at join ([trust](trust.md)).

- **Joining.** An administrator issues a single-use join token (Add host
  in the admin UI, or `veild token`), shown as a `ghostd join <veil>
  --token <id>.<secret>:sha256:<veil fingerprint>` command. ghostd pins
  the fingerprint before sending anything; Veil stores only the secret's
  hash. `join` writes `/etc/ghost/ghostd.d/broker.toml` and asks for a
  ghostd restart. `ghostd leave` tells Veil to forget the device when it
  can reach it, and removes the file either way.
- **Events.** After every `HostWelcome`, ghostd subscribes to its
  session events and then sends a `Snapshot` of its running sessions and
  offered desktops, so nothing falls between the two. Then come
  `SessionStarted`, `SessionEnded`, `ViewerAttached`, `ViewerDetached`
  and `LocalLoginTakeover`. If ghostd's event queue overflows, it sends
  a fresh `Snapshot` instead. Veil keeps sessions in its `placements`
  table and offered desktops in memory.
- **Liveness.** ghostd pings every 10 s; veild counts a host offline
  after `[hosts] idle_timeout` (30 s). ghostd reconnects with backoff
  from 1 s to 60 s plus up to 50% jitter. A second connection for the
  same device replaces the first. On SIGTERM veild closes every
  connection properly, so hosts reconnect at once.
- **`broker_only`.** With `[broker] broker_only = true` ghostd opens no
  lobby listener, so every login comes through Veil and its entitlements
  can't be bypassed. It also lets ghostd and veild share a machine, since
  both want UDP 4442.

Replacing either side's key changes a pin and means joining again.

## The broker lobby

spectre logs in to veild as it would to a host (`host/veil/src/lobby.rs`,
gdp-spec.md §5):

1. veild runs PAM against `/etc/pam.d/veild` through ghostauth (the
   `pamconv` crate, which keeps the first echo-off answer and discards
   the ticket, since Veil opens no session), with the same pre-auth
   limits, and refuses root unless `[auth] permit_root_login`.
2. It sends a `DeviceList`: the enabled devices entitled to the user
   directly or through any of their groups (resolved with
   `getgrouplist()` at every login, so sssd groups work), each marked
   online when its channel is up, with the user's running session and
   the host's desktops. Running sessions sort first.
3. On `DeviceSelect`, veild opens a login stream on that host's channel
   and plays the client (`relay.rs`): `LobbyHello` with the same
   username; the host's first echo-off `AuthChallenge` is answered with
   the password veild holds, written from a buffer wiped afterwards; any
   other prompt goes to spectre. `SessionList` and `SessionOpen` pass
   through. A login stream holds one of the host's `max_login_streams`
   and one of the user's four logins in flight (across hosts, browser
   flows included), both given back the moment the host redirects,
   refuses or ends the login; a browser flow abandoned mid-way is swept
   after five minutes. A fifth login for one account gets `HOST_FULL`.
4. The host's `Redirect` gets `host` filled in with the device's client
   address, or is replaced by a gateway redirect
   ([below](#the-gateway)). A host's `LobbyError` is passed on;
   `AUTH_FAILED` after Veil answered the password becomes
   `HOST_AUTH_FAILED` (the passwords have drifted apart).

On the host, a login stream runs the ordinary lobby on a stream instead
of a connection. Penalties and `PAM_RHOST` use the client's address from
`LoginStreamOpen`, never Veil's, so logind's `RemoteHost` is the
client's, and the forwarded password reaches ghostseat for the wallet
unlock as in a direct login. Every login, refused or not, writes an
`audit` row.

The first frame on a `gdp/1` connection tells a login from a gateway
session: a `LobbyHello` is a login, a `SessionHello` a session. Field 1
of the one is a varint and of the other a string, so protobuf can't
decode either as the other.

## Admin web UI

veild serves the admin UI at `/admin` (`host/veil/src/web/`): axum,
askama templates (`host/veil/templates/`) and htmx, with htmx, the
stylesheet and one script (`static/admin.js`) compiled into the binary
and no build step.

- **HTTPS** is served by veild on `[web] listen` (TCP 443) from
  `[web] cert`/`key`, reloaded on SIGHUP (`systemctl reload veild`, e.g.
  from an ACME hook). It is never the lobby pair, which clients and hosts
  pin. `[web] http_listen` adds plain HTTP for a reverse proxy; there, a
  loopback peer's last `X-Forwarded-For` entry is taken as the client.
- **Sign-in** is PAM against `/etc/pam.d/veild`, through ghostauth,
  with one answer, the password (a stack that asks for more fails), the
  same root policy and the lobby's penalties, shared with it. An administrator is a member of
  `[web] admin_group` (`ghost-admins`) through NSS, checked on every
  request. The sign-in is the browser client's: one `portal_sessions`
  row and one `veil_session` cookie serve both, with the client's idle
  and absolute limits.
- **CSRF and CSP.** Every state-changing form carries the session's CSRF
  token, or the `X-CSRF-Token` header on an htmx request. A
  Content-Security-Policy allows only Veil's own scripts and styles; the
  pages have no inline script. The HTTPS listener sends
  `Strict-Transport-Security` (a year); the plain-HTTP listener behind
  a reverse proxy leaves that to the proxy.
- **Pages:** Hosts (state, client address, mode, running sessions with
  viewer and gateway throughput), a host's page (name, client address,
  mode, enabled, entitlements by user or group, remove), Add host, Thin
  Clients ([Wisp](wisp.md)), and the audit log. Disabling or removing a
  device drops its channel at once. Every change writes an `audit` row
  naming the administrator.
- **Live updates.** Each page opens the `/admin/events` WebSocket, which
  carries no data: veild checks once a second whether anything an admin
  page shows has changed and sends `changed`, and `admin.js` re-fetches
  the page and swaps its `<main>`. Not while a form on it is being
  edited, so a form is never rewritten under the administrator.

## The gateway

A device's `mode` decides whether its sessions go through Veil: `direct`
never, `gateway` always, `auto` (the default) when the client's address
is outside `[gateway] internal_networks` (RFC 1918 and `fc00::/7` by
default; loopback counts as outside). Browser sessions always go through
it.

**Redirect.** In gateway mode the lobby keeps the host's `Redirect`
(`intercept` in `host/veil/src/gateway.rs`): wraith's address (the host
channel's source address, unless the host named another), port, token and
certificate, under a fresh single-use gateway token valid 30 s. spectre
gets a `Redirect` to Veil's public address and lobby port, pinned to the
certificate it logged in against.

**Session.** `serve` checks the gateway token (a bad one closes with
`AUTH_FAILED` and is penalised, as wraith would), opens its own
connection to wraith pinning the certificate the host vouched for, sends
the `SessionHello` with the host's token and `via_gateway` set, and
relays:

- **Streams** byte for byte both ways. Every further stream one side
  opens is opened on the other in the same order, so the input stream
  keeps its place.
- **Datagrams** in both directions. quinn holds at most 64 KiB of
  datagrams per leg (`SEND_BUFFER`) and drops the oldest past that, which
  a 4K keyframe's burst exceeds. So toward the client, veild queues a
  datagram that doesn't fit and sends it when there is room, dropping it
  after 20 ms (`MAX_DATAGRAM_DELAY`). A burst on a fast leg drains without
  loss; a congested leg still turns its excess into loss, which reaches
  wraith's rate control through spectre's `StatsReport` as on a direct
  path. Every 5 s in which datagrams went missing, veild logs where.
- **The close.** Whichever leg closes first, the other gets the same
  code.

**RTT.** spectre's transport RTT ends at Veil, so after `SessionAccept`
veild sends `GatewayPath` about once a second with the wraith leg's RTT,
which spectre adds to the RTT it shows and reports (gdp-spec.md §6.10).
wraith's handshake RTT, which picks the `auto` network profile, still
sees only the Veil leg.

**Datagram size.** wraith slices video to its connection's
`max_datagram_size()`, which ngtcp2 bounds by the peer's
`max_udp_payload_size`. Veil opens the wraith leg only once the client
leg's path MTU discovery has reached `[gateway] max_udp_payload` (1452)
or stopped growing for 250 ms (at most 1 s), from an endpoint
advertising exactly the client leg's datagram size plus wraith's
per-packet overhead. wraith's slices then fit the client leg with no
protocol change. Later changes to the client leg's MTU aren't followed:
datagrams too large for it are dropped and logged.

A veild restart ends every gateway session; direct sessions don't
notice.

## Limitations

- No high availability: veild is one process with one SQLite database.
- A veild restart ends every gateway session.
- Gateway sessions have two congestion controllers in series (quinn's on
  the client leg, ngtcp2's behind it), and haven't been tested with
  spectre's feedback under loss and delay.
