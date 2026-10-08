// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The GDP gateway (docs/design/veil.md#the-gateway): session traffic
// through Veil, for clients that can't or shouldn't reach a host directly.
//
// When a device's mode says so, the lobby keeps the host's real Redirect
// and gives the client one to Veil itself, with a gateway token of Veil's
// own (`intercept`). The client then opens its session connection to
// Veil's lobby port; its first frame, a SessionHello, brings it here
// (`serve`). Veil checks the gateway token, opens a second connection to
// the session's wraith -- pinning the certificate the host vouched for --
// and from then on relays:
//
// - streams byte for byte, both ways, in the order they were opened, the
//   one change being the SessionHello's token (the gateway token out, the
//   host's own in);
// - datagrams: toward wraith as they arrive; toward the client held at
//   most MAX_DATAGRAM_DELAY for room and then dropped, so congestion on
//   either leg shows up as the loss wraith's rate control acts on;
// - the close: either leg ending ends the other, with the same code.
//
// Datagram size: wraith slices video to its connection's
// max_datagram_size(), which follows the max_udp_payload_size Veil's end
// advertises. Veil opens the wraith leg only once the client leg's path
// MTU is known, with an endpoint advertising exactly what fits the client
// leg, so wraith's slices fit without any protocol change.
use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use base64::Engine;
use ipc::framing::{frame_bytes, read_raw_frame_max, write_frame, MAX_FRAME_LEN};
use ipc::lobby::{LobbyErrorCode, Redirect};
use ipc::session::{control_envelope::Msg as ControlMsg, ControlEnvelope, GatewayPath, SessionHello};
use quinn::crypto::rustls::QuicClientConfig;
use quinn::VarInt;
use rand::RngCore;
use tracing::{debug, info, warn};

use crate::config::{Config, DeviceMode};
use crate::db::Device;
use crate::Veil;

/// How long a client has to use its gateway Redirect, as ghostd's tokens.
const TOKEN_TTL: Duration = Duration::from_secs(30);
/// How long to let the client leg's path MTU discovery run before sizing
/// the wraith leg after it.
const MTU_SETTLE: Duration = Duration::from_millis(1000);
const MTU_POLL: Duration = Duration::from_millis(10);
/// ...or until it has stopped growing for this long.
const MTU_STABLE: Duration = Duration::from_millis(250);
/// A 1-RTT packet around one DATAGRAM frame as wraith (ngtcp2, libgdp's
/// quic_conn.cpp) counts it: short header byte, Veil's connection ID,
/// 4-byte packet number, AEAD tag, frame type and 2-byte length.
const WRAITH_DATAGRAM_OVERHEAD: usize = 1 + CID_LEN + 4 + 16 + 3;
/// The connection IDs Veil's wraith-leg endpoints issue (quinn's default).
const CID_LEN: usize = 8;
const STATS_INTERVAL: Duration = Duration::from_secs(2);

/// What a gateway Redirect stands for, until the client comes back with it.
struct Pending {
    wraith: SocketAddr,
    token: String,
    cert_sha256: String,
    device_id: String,
    device_name: String,
    username: String,
    client: IpAddr,
    expires: Instant,
}

struct Live {
    device_id: String,
    username: String,
    bytes: AtomicU64,
    rate: AtomicU64,
}

/// Gateway tokens waiting to be used, and the sessions in flight.
#[derive(Default)]
pub struct Gateway {
    pending: Mutex<HashMap<String, Pending>>,
    live: Mutex<HashMap<u64, Arc<Live>>>,
    next_id: AtomicU64,
}

/// Whether a login to `mode`'s device from `client` goes through Veil.
pub fn use_gateway(mode: DeviceMode, client: IpAddr, config: &Config) -> Result<bool> {
    Ok(match mode {
        DeviceMode::Direct => false,
        DeviceMode::Gateway => true,
        DeviceMode::Auto => !config.internal_networks()?.iter().any(|n| n.contains(client)),
    })
}

/// Keeps the host's Redirect and returns the client's: Veil's own public
/// address and lobby port, a gateway token, and Veil's lobby certificate,
/// which the client has already pinned.
pub async fn intercept(veil: &Veil, redirect: Redirect, device: &Device, username: &str, peer: SocketAddr) -> Result<Redirect> {
    let link = veil.hosts.link(&device.id).context("the host went offline")?;
    // Veil reaches wraith where the host's channel comes from: that
    // address is what Veil knows works. A host naming another address in
    // its Redirect wins.
    let host_ip = if redirect.host.is_empty() {
        link.conn.remote_address().ip().to_canonical()
    } else {
        let mut addrs = tokio::net::lookup_host((redirect.host.as_str(), 0)).await?;
        addrs.next().context("the host's redirect address doesn't resolve")?.ip()
    };
    let port = u16::try_from(redirect.port).context("bad port in the host's redirect")?;
    let mut raw = [0u8; 32];
    rand::rng().fill_bytes(&mut raw);
    let token = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(raw);
    let now = Instant::now();
    {
        let mut pending = veil.gateway.pending.lock().unwrap_or_else(|p| p.into_inner());
        pending.retain(|_, p| p.expires > now);
        pending.insert(
            token.clone(),
            Pending {
                wraith: SocketAddr::new(host_ip, port),
                token: redirect.token,
                cert_sha256: redirect.cert_sha256,
                device_id: device.id.clone(),
                device_name: device.name.clone(),
                username: username.to_string(),
                client: peer.ip(),
                expires: now + TOKEN_TTL,
            },
        );
    }
    Ok(Redirect {
        host: veil.config.public_address(),
        port: veil.config.lobby.port.into(),
        token,
        expiry_unix: ipc::unix_now() + TOKEN_TTL.as_secs() as i64,
        // What this port shows gdp/1 now (lobby_tls.rs).
        cert_sha256: veil.certs.client_fingerprint(),
    })
}

/// Bytes per second per (device id, username) over the last few seconds,
/// for the admin UI.
pub fn throughput(veil: &Veil) -> HashMap<(String, String), u64> {
    let live = veil.gateway.live.lock().unwrap_or_else(|p| p.into_inner());
    let mut out = HashMap::new();
    for s in live.values() {
        *out.entry((s.device_id.clone(), s.username.clone())).or_default() += s.rate.load(Ordering::Relaxed);
    }
    out
}

/// One end of a gateway session facing the client: spectre's QUIC
/// connection, a browser's WebTransport session, or the WebSocket
/// fallback (ws.rs).
pub enum ClientLeg {
    Quic(quinn::Connection),
    WebTransport(wtransport::Connection),
    WebSocket(crate::ws::WsLeg),
}

/// A stream half, whichever transport it comes from.
pub type BoxRead = Box<dyn tokio::io::AsyncRead + Send + Unpin>;
pub type BoxWrite = Box<dyn tokio::io::AsyncWrite + Send + Unpin>;

enum Accepted {
    Bi(BoxWrite, BoxRead),
    Uni(BoxRead),
}

impl ClientLeg {
    fn quic(&self) -> Option<&quinn::Connection> {
        match self {
            ClientLeg::Quic(c) => Some(c),
            ClientLeg::WebTransport(c) => Some(c.quic_connection()),
            ClientLeg::WebSocket(_) => None,
        }
    }

    /// The largest datagram this leg can carry right now.
    fn max_datagram_size(&self, cap: usize) -> Option<usize> {
        match self {
            ClientLeg::Quic(c) => c.max_datagram_size(),
            ClientLeg::WebTransport(c) => c.max_datagram_size(),
            // A WebSocket carries any size; let wraith use the cap.
            ClientLeg::WebSocket(_) => Some(cap.saturating_sub(WRAITH_DATAGRAM_OVERHEAD)),
        }
    }

    async fn read_datagram(&self) -> Option<bytes::Bytes> {
        match self {
            ClientLeg::Quic(c) => c.read_datagram().await.ok(),
            ClientLeg::WebTransport(c) => c.receive_datagram().await.ok().map(|d| d.payload()),
            ClientLeg::WebSocket(w) => w.read_datagram().await,
        }
    }

    /// Sends once the leg has room for it, or fails as Stale at
    /// `deadline`. A QUIC leg waits on quinn's own buffer; a WebTransport
    /// leg (whose datagrams wtransport frames itself) checks the same
    /// buffer of the QUIC connection under it; a WebSocket queues in its
    /// own writer.
    async fn send_datagram_within(&self, d: bytes::Bytes, deadline: Instant) -> std::result::Result<(), SendFailure> {
        match self {
            ClientLeg::Quic(c) => match tokio::time::timeout_at(deadline.into(), c.send_datagram_wait(d)).await {
                Ok(Ok(())) => Ok(()),
                Ok(Err(quinn::SendDatagramError::TooLarge)) => Err(SendFailure::TooLarge),
                Ok(Err(_)) => Err(SendFailure::Closed),
                Err(_) => Err(SendFailure::Stale),
            },
            ClientLeg::WebTransport(c) => {
                // Room for the payload plus HTTP/3's quarter-stream-id.
                while c.quic_connection().datagram_send_buffer_space() < d.len() + 8 {
                    if Instant::now() >= deadline {
                        return Err(SendFailure::Stale);
                    }
                    tokio::time::sleep(Duration::from_millis(1)).await;
                }
                match c.send_datagram(d) {
                    Ok(()) => Ok(()),
                    Err(wtransport::error::SendDatagramError::TooLarge) => Err(SendFailure::TooLarge),
                    Err(_) => Err(SendFailure::Closed),
                }
            }
            ClientLeg::WebSocket(w) => {
                w.send_datagram(d);
                Ok(())
            }
        }
    }

    async fn accept(&self) -> Option<Accepted> {
        match self {
            ClientLeg::Quic(c) => tokio::select! {
                bi = c.accept_bi() => bi.ok().map(|(s, r)| Accepted::Bi(Box::new(s), Box::new(r))),
                uni = c.accept_uni() => uni.ok().map(|r| Accepted::Uni(Box::new(r))),
            },
            ClientLeg::WebTransport(c) => tokio::select! {
                bi = c.accept_bi() => bi.ok().map(|(s, r)| Accepted::Bi(Box::new(s), Box::new(r))),
                uni = c.accept_uni() => uni.ok().map(|r| Accepted::Uni(Box::new(r))),
            },
            ClientLeg::WebSocket(w) => w.accept_stream().await.map(|(s, r)| Accepted::Bi(s, r)),
        }
    }

    async fn open_bi(&self) -> Option<(BoxWrite, BoxRead)> {
        match self {
            ClientLeg::Quic(c) => c.open_bi().await.ok().map(|(s, r)| (Box::new(s) as BoxWrite, Box::new(r) as BoxRead)),
            ClientLeg::WebTransport(c) => {
                let (s, r) = c.open_bi().await.ok()?.await.ok()?;
                Some((Box::new(s), Box::new(r)))
            }
            // The fallback carries the two streams a client opens and
            // nothing else; wraith opens none.
            ClientLeg::WebSocket(_) => None,
        }
    }

    async fn open_uni(&self) -> Option<BoxWrite> {
        match self {
            ClientLeg::Quic(c) => c.open_uni().await.ok().map(|s| Box::new(s) as BoxWrite),
            ClientLeg::WebTransport(c) => Some(Box::new(c.open_uni().await.ok()?.await.ok()?)),
            ClientLeg::WebSocket(_) => None,
        }
    }

    async fn closed(&self) -> (u32, Vec<u8>) {
        match self {
            ClientLeg::Quic(c) => close_of(&c.closed().await),
            ClientLeg::WebTransport(c) => match c.closed().await {
                wtransport::error::ConnectionError::ApplicationClosed(a) => {
                    (u32::try_from(a.code().into_inner()).unwrap_or(0), a.reason().to_vec())
                }
                _ => (0, Vec::new()),
            },
            ClientLeg::WebSocket(w) => (w.closed().await, Vec::new()),
        }
    }

    fn close(&self, code: u32, reason: &[u8]) {
        match self {
            ClientLeg::Quic(c) => c.close(VarInt::from_u32(code), reason),
            ClientLeg::WebTransport(c) => c.close(wtransport::VarInt::from_u32(code), reason),
            ClientLeg::WebSocket(w) => w.close(code),
        }
    }

    /// close(), telling a browser first why: wtransport can't send
    /// WebTransport's close capsule, so the GDP code goes on a one-off
    /// unidirectional stream, "GDPCLOSE" and the code (u32 LE), which the
    /// page reads before the session's closed promise settles (app/
    /// transport.js).
    async fn finish(&self, code: u32, reason: &[u8]) {
        if let ClientLeg::WebTransport(c) = self {
            let notice = async {
                let mut stream = c.open_uni().await.ok()?.await.ok()?;
                let mut msg = b"GDPCLOSE".to_vec();
                msg.extend_from_slice(&code.to_le_bytes());
                stream.write_all(&msg).await.ok()?;
                stream.finish().await.ok()
            };
            let _ = tokio::time::timeout(Duration::from_millis(500), notice).await;
        }
        self.close(code, reason);
    }

    fn kind(&self) -> &'static str {
        match self {
            ClientLeg::Quic(_) => "quic",
            ClientLeg::WebTransport(_) => "webtransport",
            ClientLeg::WebSocket(_) => "websocket",
        }
    }
}

/// spectre's session connection to Veil. `send`/`recv` is its control
/// stream, whose SessionHello the lobby has already read.
pub async fn serve(
    client: quinn::Connection,
    send: quinn::SendStream,
    recv: quinn::RecvStream,
    hello: SessionHello,
    veil: Arc<Veil>,
    peer: SocketAddr,
) {
    serve_leg(ClientLeg::Quic(client), Box::new(send), Box::new(recv), hello, veil, peer).await
}

/// A browser's session: its first stream is the control stream, which
/// starts with the SessionHello carrying the gateway token, exactly as
/// spectre's does.
pub async fn serve_client(leg: ClientLeg, veil: Arc<Veil>, peer: SocketAddr) {
    let first = async {
        let (send, mut recv) = match leg.accept().await {
            Some(Accepted::Bi(send, recv)) => (send, recv),
            _ => anyhow::bail!("no control stream"),
        };
        let hello: ControlEnvelope = ipc::framing::read_frame_max(&mut recv, 16 * 1024).await?;
        match hello.msg {
            Some(ControlMsg::Hello(hello)) => Ok((send, recv, hello)),
            _ => anyhow::bail!("the first control message isn't a SessionHello"),
        }
    };
    match tokio::time::timeout(Duration::from_secs(10), first).await {
        Ok(Ok((send, recv, hello))) => serve_leg(leg, send, recv, hello, veil, peer).await,
        Ok(Err(e)) => {
            info!(%peer, kind = leg.kind(), error = %format!("{e:#}"), "gateway: bad session start");
            veil.penalties.penalise(peer.ip(), preauth::Offense::NoAuth);
            leg.close(LobbyErrorCode::LobbyErrorAuthFailed as u32, b"");
        }
        Err(_) => {
            veil.penalties.penalise(peer.ip(), preauth::Offense::NoAuth);
            leg.close(LobbyErrorCode::LobbyErrorAuthFailed as u32, b"");
        }
    }
}

async fn serve_leg(client: ClientLeg, send: BoxWrite, recv: BoxRead, hello: SessionHello, veil: Arc<Veil>, peer: SocketAddr) {
    let pending = {
        let mut pending = veil.gateway.pending.lock().unwrap_or_else(|p| p.into_inner());
        pending.remove(&hello.token).filter(|p| p.expires > Instant::now())
    };
    let Some(pending) = pending else {
        // As wraith (gdp-spec.md §6.3): a bad token gets AUTH_FAILED and
        // nothing else.
        info!(%peer, kind = client.kind(), "gateway: unknown or expired gateway token");
        veil.penalties.penalise(peer.ip(), preauth::Offense::AuthFail);
        client.close(LobbyErrorCode::LobbyErrorAuthFailed as u32, b"");
        return;
    };
    if pending.client != peer.ip() {
        // The redirect went to one address and the session came from
        // another (a NAT rebinding, a VPN coming up). The token is a
        // bearer secret either way; worth a line in the log.
        info!(%peer, login_from = %pending.client, "gateway: session from a different address than its login");
    }
    let id = veil.gateway.next_id.fetch_add(1, Ordering::Relaxed);
    let live = Arc::new(Live {
        device_id: pending.device_id.clone(),
        username: pending.username.clone(),
        bytes: AtomicU64::new(0),
        rate: AtomicU64::new(0),
    });
    veil.gateway.live.lock().unwrap_or_else(|p| p.into_inner()).insert(id, live.clone());
    let (device, username, kind) = (pending.device_name.clone(), pending.username.clone(), client.kind());
    info!(%peer, device, username, kind, wraith = %pending.wraith, "gateway: session starting");
    let client = Arc::new(client);
    let result = relay(&client, send, recv, hello, pending, &veil, live).await;
    veil.gateway.live.lock().unwrap_or_else(|p| p.into_inner()).remove(&id);
    match result {
        Ok(code) => info!(%peer, device, username, code, "gateway: session ended"),
        Err(e) => {
            warn!(%peer, device, username, error = %format!("{e:#}"), "gateway: session failed");
            client.close(LobbyErrorCode::LobbyErrorSessionStartFailed as u32, b"");
        }
    }
}

// Waits (briefly) for the client leg's path MTU discovery to reach the
// configured cap or stop growing, then returns the largest datagram the
// client leg can carry.
async fn settled_datagram_size(client: &ClientLeg, cap: u16) -> Result<usize> {
    if let Some(quic) = client.quic() {
        let start = Instant::now();
        let mut mtu = quic.stats().path.current_mtu;
        let mut changed = start;
        while start.elapsed() < MTU_SETTLE && mtu < cap {
            tokio::time::sleep(MTU_POLL).await;
            let now = quic.stats().path.current_mtu;
            if now != mtu {
                mtu = now;
                changed = Instant::now();
            } else if changed.elapsed() >= MTU_STABLE {
                break;
            }
        }
    }
    client.max_datagram_size(cap as usize).context("the client didn't enable datagrams")
}

fn wraith_endpoint(max_udp_payload: u16, cert_sha256: &str) -> Result<quinn::Endpoint> {
    let mut endpoint_config = quinn::EndpointConfig::default();
    endpoint_config.max_udp_payload_size(max_udp_payload).context("max_udp_payload_size")?;
    let mut crypto = rustls::ClientConfig::builder()
        .dangerous()
        .with_custom_certificate_verifier(gdpnet::PinnedServer::new(cert_sha256))
        .with_no_client_auth();
    crypto.alpn_protocols = vec![b"gdp/1".to_vec()];
    let mut client_config = quinn::ClientConfig::new(Arc::new(QuicClientConfig::try_from(crypto)?));
    let mut transport = quinn::TransportConfig::default();
    transport.keep_alive_interval(Some(Duration::from_secs(5)));
    // Veil mostly receives on this leg; its own sends (the client's
    // datagrams, rare) are capped like the client leg's.
    transport.datagram_send_buffer_size(SEND_BUFFER);
    // The path to wraith is only discovered up to what fits the client leg.
    let mut mtu = quinn::MtuDiscoveryConfig::default();
    mtu.upper_bound(max_udp_payload);
    transport.mtu_discovery_config(Some(mtu));
    client_config.transport_config(Arc::new(transport));
    let mut endpoint = gdpnet::bind_dual_stack(0, endpoint_config, None)?;
    endpoint.set_default_client_config(client_config);
    Ok(endpoint)
}

/// What one leg may hold in datagrams waiting for congestion control
/// before quinn drops the oldest. Small, so congestion shows up as loss
/// rather than queueing delay, but enough for a frame's slices to wait the
/// moment it takes the connection's driver to send them.
pub const SEND_BUFFER: usize = 64 * 1024;

async fn relay(
    client: &Arc<ClientLeg>,
    client_send: BoxWrite,
    client_recv: BoxRead,
    mut hello: SessionHello,
    pending: Pending,
    veil: &Veil,
    live: Arc<Live>,
) -> Result<u32> {
    let cap = veil.config.gateway.max_udp_payload as usize;
    let client_datagram = settled_datagram_size(client, cap as u16).await?;
    let wraith_payload = (client_datagram + WRAITH_DATAGRAM_OVERHEAD).clamp(1200, cap);
    debug!(client_datagram, wraith_payload, "gateway: sizing the wraith leg");
    let endpoint = wraith_endpoint(wraith_payload as u16, &pending.cert_sha256)?;
    let connecting = endpoint
        .connect(pending.wraith, &pending.wraith.ip().to_string())
        .context("connecting to wraith")?;
    let wraith = tokio::time::timeout(Duration::from_secs(10), connecting)
        .await
        .context("wraith did not answer")?
        .context("connecting to wraith")?;

    // The control stream: the SessionHello with the host's token in place
    // of the gateway's and via_gateway set (whatever the client sent),
    // then bytes. wraith echoes via_gateway in SessionAccept, so both ends
    // know the session is relayed (gdp-spec.md §6.10).
    let (mut wraith_send, wraith_recv) = wraith.open_bi().await?;
    hello.token = pending.token;
    hello.via_gateway = true;
    write_frame(&mut wraith_send, &ControlEnvelope { msg: Some(ControlMsg::Hello(hello)) }).await?;

    // Only a GDP client knows GatewayPath; the browser measures the whole
    // path itself (gdp-spec.md §6.10).
    let report_path = matches!(**client, ClientLeg::Quic(_));
    let tasks = [
        tokio::spawn(splice(client_recv, Box::new(wraith_send), live.clone())),
        tokio::spawn(control_to_client(Box::new(wraith_recv), client_send, wraith.clone(), report_path, live.clone())),
        tokio::spawn(client_streams(client.clone(), wraith.clone(), live.clone())),
        tokio::spawn(wraith_streams(wraith.clone(), client.clone(), live.clone())),
        tokio::spawn(datagrams_to_wraith(client.clone(), wraith.clone(), live.clone())),
        tokio::spawn(datagrams_to_client(wraith.clone(), client.clone(), live.clone())),
        tokio::spawn(rate(live.clone())),
        tokio::spawn(loss_report(wraith.clone(), client.clone())),
    ];

    // Whichever leg closes first decides the code for the other.
    let (code, reason, from) = tokio::select! {
        (code, reason) = client.closed() => (code, reason, "client"),
        e = wraith.closed() => {
            let (code, reason) = close_of(&e);
            (code, reason, "wraith")
        }
    };
    let to_wraith = wraith.stats();
    let mut fields = format!(
        "from_wraith={} wraith_mtu={}",
        to_wraith.frame_rx.datagram, to_wraith.path.current_mtu
    );
    if let Some(quic) = client.quic() {
        let s = quic.stats();
        fields += &format!(
            " to_client_sent={} to_client_lost={} client_mtu={} client_cwnd={} client_congestion={}",
            s.frame_tx.datagram, s.path.lost_packets, s.path.current_mtu, s.path.cwnd, s.path.congestion_events
        );
    }
    debug!(from, code, stats = %fields, "gateway: a leg closed");
    client.finish(code, &reason).await;
    wraith.close(VarInt::from_u32(code), &reason);
    for t in tasks {
        t.abort();
    }
    let _ = tokio::time::timeout(Duration::from_secs(1), endpoint.wait_idle()).await;
    Ok(code)
}

fn close_of(e: &quinn::ConnectionError) -> (u32, Vec<u8>) {
    match e {
        quinn::ConnectionError::ApplicationClosed(c) => {
            (u32::try_from(c.error_code.into_inner()).unwrap_or(0), c.reason.to_vec())
        }
        // Timed out, reset, or a transport error: the other end gets a
        // plain close.
        _ => (0, Vec::new()),
    }
}

/// Copies one stream's bytes to another until the source ends, then
/// finishes the destination.
async fn splice(mut from: BoxRead, mut to: BoxWrite, live: Arc<Live>) {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        match from.read(&mut buf).await {
            Ok(0) => {
                let _ = to.shutdown().await;
                return;
            }
            Ok(n) => {
                live.bytes.fetch_add(n as u64, Ordering::Relaxed);
                if to.write_all(&buf[..n]).await.is_err() {
                    return;
                }
            }
            Err(_) => return,
        }
    }
}

/// The control stream toward the client: wraith's frames as they come, and,
/// once the first (SessionAccept) has gone through and `report` is set, a
/// GatewayPath about once a second with the wraith leg's RTT. Frames are
/// relayed whole, so an injected one never lands inside another.
async fn control_to_client(
    mut from: BoxRead,
    mut to: BoxWrite,
    wraith: quinn::Connection,
    report: bool,
    live: Arc<Live>,
) {
    use tokio::io::AsyncWriteExt;
    // read_raw_frame_max isn't cancel-safe, so reading is its own task.
    let (tx, mut frames) = tokio::sync::mpsc::channel::<Vec<u8>>(16);
    let reader = tokio::spawn(async move {
        while let Ok(body) = read_raw_frame_max(&mut from, MAX_FRAME_LEN).await {
            if tx.send(body).await.is_err() {
                return;
            }
        }
    });
    let mut tick = tokio::time::interval(Duration::from_secs(1));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
    let mut accepted = false;
    loop {
        let out = tokio::select! {
            body = frames.recv() => match body {
                Some(body) => {
                    accepted = true;
                    let mut out = Vec::with_capacity(4 + body.len());
                    out.extend_from_slice(&(body.len() as u32).to_le_bytes());
                    out.extend_from_slice(&body);
                    out
                }
                None => break,
            },
            _ = tick.tick(), if report && accepted => {
                let rtt = u32::try_from(wraith.rtt().as_micros()).unwrap_or(u32::MAX);
                let msg = ControlEnvelope { msg: Some(ControlMsg::GatewayPath(GatewayPath { upstream_rtt_us: rtt })) };
                match frame_bytes(&msg) {
                    Ok(out) => out,
                    Err(_) => continue,
                }
            }
        };
        live.bytes.fetch_add(out.len() as u64, Ordering::Relaxed);
        if to.write_all(&out).await.is_err() {
            reader.abort();
            return;
        }
    }
    let _ = to.shutdown().await;
}

/// Every further stream the client opens, opened on the wraith leg in the
/// same order (stream-id order), so the input stream (gdp-spec.md §2.2:
/// the second client bidi stream) keeps its place.
async fn client_streams(client: Arc<ClientLeg>, wraith: quinn::Connection, live: Arc<Live>) {
    while let Some(accepted) = client.accept().await {
        match accepted {
            Accepted::Bi(client_send, client_recv) => {
                let Ok((wraith_send, wraith_recv)) = wraith.open_bi().await else { return };
                tokio::spawn(splice(client_recv, Box::new(wraith_send), live.clone()));
                tokio::spawn(splice(Box::new(wraith_recv), client_send, live.clone()));
            }
            Accepted::Uni(client_recv) => {
                let Ok(wraith_send) = wraith.open_uni().await else { return };
                tokio::spawn(splice(client_recv, Box::new(wraith_send), live.clone()));
            }
        }
    }
}

/// And any stream wraith opens, toward the client.
async fn wraith_streams(wraith: quinn::Connection, client: Arc<ClientLeg>, live: Arc<Live>) {
    loop {
        tokio::select! {
            bi = wraith.accept_bi() => {
                let Ok((wraith_send, wraith_recv)) = bi else { return };
                let Some((client_send, client_recv)) = client.open_bi().await else { return };
                tokio::spawn(splice(client_recv, Box::new(wraith_send), live.clone()));
                tokio::spawn(splice(Box::new(wraith_recv), client_send, live.clone()));
            }
            uni = wraith.accept_uni() => {
                let Ok(wraith_recv) = uni else { return };
                let Some(client_send) = client.open_uni().await else { return };
                tokio::spawn(splice(Box::new(wraith_recv), client_send, live.clone()));
            }
        }
    }
}

/// Datagrams from the client to wraith (rare: wraith sends the video).
async fn datagrams_to_wraith(client: Arc<ClientLeg>, wraith: quinn::Connection, live: Arc<Live>) {
    while let Some(datagram) = client.read_datagram().await {
        live.bytes.fetch_add(datagram.len() as u64, Ordering::Relaxed);
        let _ = wraith.send_datagram(datagram);
    }
}

/// Video and audio toward the client, held at most MAX_DATAGRAM_DELAY.
///
/// quinn keeps only SEND_BUFFER of datagrams waiting for the client leg's
/// congestion control and silently drops the oldest past that. A 4K
/// keyframe arrives from wraith as a burst at up to the path's rate, far
/// more than SEND_BUFFER, so even a fast leg's sender falls a moment
/// behind it. Here a datagram waits for room instead -- but never longer than
/// MAX_DATAGRAM_DELAY, after which it is dropped: a burst on a fast leg
/// drains in milliseconds and loses nothing, while a congested slow leg
/// still turns its excess into loss (and some delay), which is what
/// wraith's rate control acts on, as on a direct path.
async fn datagrams_to_client(wraith: quinn::Connection, client: Arc<ClientLeg>, live: Arc<Live>) {
    let (tx, mut rx) = tokio::sync::mpsc::channel::<(Instant, bytes::Bytes)>(DATAGRAM_QUEUE);
    let reader = tokio::spawn(async move {
        let mut full = 0u64;
        while let Ok(datagram) = wraith.read_datagram().await {
            live.bytes.fetch_add(datagram.len() as u64, Ordering::Relaxed);
            if tx.try_send((Instant::now(), datagram)).is_err() {
                full += 1;
                if full == 1 || full % 1000 == 0 {
                    warn!(dropped = full, "gateway: the queue toward the client is full; dropping datagrams");
                }
            }
        }
    });
    let mut too_large = 0u64;
    while let Some((at, datagram)) = rx.recv().await {
        if at.elapsed() > MAX_DATAGRAM_DELAY {
            continue; // stale: the leg can't keep up
        }
        match client.send_datagram_within(datagram, at + MAX_DATAGRAM_DELAY).await {
            Ok(()) => {}
            Err(SendFailure::TooLarge) => {
                too_large += 1;
                if too_large == 1 || too_large % 1000 == 0 {
                    // The client leg's path MTU shrank after the wraith leg was
                    // sized: wraith keeps slicing for the old one.
                    warn!(dropped = too_large, "gateway: dropping datagrams too large for the client leg");
                }
            }
            Err(SendFailure::Stale) => {}
            Err(SendFailure::Closed) => break,
        }
    }
    reader.abort();
}

/// How long a datagram may wait in veild for the client leg: enough for a
/// keyframe burst to drain on a fast leg, short enough that a congested
/// one shows its congestion as loss rather than as a growing queue.
const MAX_DATAGRAM_DELAY: Duration = Duration::from_millis(20);
/// Datagrams held at most (a 4K keyframe is a few thousand); the delay
/// limit normally drains the queue long before this.
const DATAGRAM_QUEUE: usize = 16 * 1024;

enum SendFailure {
    TooLarge,
    Stale,
    Closed,
}

/// Where datagrams go missing on a gateway session, every LOSS_INTERVAL in
/// which any did: in from wraith, out to the client, the difference (what
/// veild itself dropped: the client leg's send buffer full, or too large),
/// and the packets each leg's QUIC declared lost. Quiet when nothing is
/// lost.
async fn loss_report(wraith: quinn::Connection, client: Arc<ClientLeg>) {
    let Some(quic) = client.quic().cloned() else { return };
    let snapshot = || {
        let w = wraith.stats();
        let c = quic.stats();
        (w.frame_rx.datagram, c.frame_tx.datagram, w.path.lost_packets, c.path.lost_packets, c.path.cwnd, c.path.rtt)
    };
    let mut tick = tokio::time::interval(LOSS_INTERVAL);
    tick.tick().await;
    let mut last = snapshot();
    loop {
        tick.tick().await;
        let now = snapshot();
        let (rx, tx) = (now.0 - last.0, now.1 - last.1);
        let dropped = rx.saturating_sub(tx);
        let (wraith_lost, client_lost) = (now.2 - last.2, now.3 - last.3);
        if dropped > 0 || client_lost > 0 || wraith_lost > 0 {
            info!(
                from_wraith = rx,
                to_client = tx,
                dropped_in_veild = dropped,
                lost_to_client = client_lost,
                lost_to_wraith = wraith_lost,
                client_cwnd = now.4,
                client_rtt_us = now.5.as_micros() as u64,
                "gateway: datagram loss"
            );
        }
        last = now;
    }
}

const LOSS_INTERVAL: Duration = Duration::from_secs(5);

async fn rate(live: Arc<Live>) {
    let mut last = live.bytes.load(Ordering::Relaxed);
    let mut tick = tokio::time::interval(STATS_INTERVAL);
    tick.tick().await;
    loop {
        tick.tick().await;
        let now = live.bytes.load(Ordering::Relaxed);
        live.rate.store((now - last) / STATS_INTERVAL.as_secs(), Ordering::Relaxed);
        last = now;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn modes_pick_the_path() {
        let mut config = Config::default();
        let lan: IpAddr = "192.168.1.20".parse().unwrap();
        let wan: IpAddr = "203.0.113.5".parse().unwrap();
        assert!(!use_gateway(DeviceMode::Direct, wan, &config).unwrap());
        assert!(use_gateway(DeviceMode::Gateway, lan, &config).unwrap());
        // The default list is the private ranges.
        assert!(!use_gateway(DeviceMode::Auto, lan, &config).unwrap());
        assert!(!use_gateway(DeviceMode::Auto, "fd12::1".parse().unwrap(), &config).unwrap());
        assert!(use_gateway(DeviceMode::Auto, wan, &config).unwrap());
        // auto with no internal networks: everything through Veil.
        config.gateway.internal_networks.clear();
        assert!(use_gateway(DeviceMode::Auto, lan, &config).unwrap());
        config.gateway.internal_networks = vec!["192.168.1.0/24".into()];
        assert!(!use_gateway(DeviceMode::Auto, lan, &config).unwrap());
        assert!(use_gateway(DeviceMode::Auto, wan, &config).unwrap());
    }
}

// The browser client's hand-written protobuf codec (app/proto.js) against
// prost, the reference: these bytes are what proto.js encoded; prost must
// read the same values back.
#[cfg(test)]
mod browser_proto {
    use ipc::session::{control_envelope::Msg, input_envelope::Event, ControlEnvelope, InputEnvelope};
    use prost::Message;

    fn bytes(hex: &str) -> Vec<u8> {
        (0..hex.len()).step_by(2).map(|i| u8::from_str_radix(&hex[i..i + 2], 16).unwrap()).collect()
    }

    #[test]
    fn session_hello() {
        let env = ControlEnvelope::decode(&bytes("0a3d0a03746f6b20022a1308800f10b80818e0d40321000000000000f83f32046832363432036176315209636c6970626f6172646209776562636f64656373")[..]).unwrap();
        let Some(Msg::Hello(h)) = env.msg else { panic!("not a hello") };
        assert_eq!(h.token, "tok");
        assert_eq!(h.codecs, ["h264", "av1"]);
        assert_eq!(h.decoders, ["webcodecs"]);
        assert_eq!(h.capabilities, ["clipboard"]);
        assert_eq!(h.network_profile, 2);
        let d = &h.displays[0];
        assert_eq!((d.width, d.height, d.refresh_mhz, d.scale), (1920, 1080, 60000, 1.5));
    }

    #[test]
    fn stats_report() {
        let env = ControlEnvelope::decode(&bytes("223608d20912311080d0acf30e18818080808080808080013090a10f3894b4e4f4cb03408f4e48b06d500c5a0b0807101418e0da01208407")[..]).unwrap();
        let Some(Msg::Stats(s)) = env.msg else { panic!("not stats") };
        assert_eq!(s.rtt_us, 1234);
        let st = &s.streams[0];
        assert_eq!(st.highest_frame_id_acked, 4_000_000_000);
        assert_eq!(st.loss_bitmap, 0x8000_0000_0000_0001);
        assert_eq!(st.interval_us, 250_000);
        assert_eq!(st.bytes_received, 123_456_789_012);
        assert_eq!((st.delay_min_us, st.delay_avg_us, st.delay_samples), (-5000, 7000, 12));
        let t = &st.trains[0];
        assert_eq!((t.frame_id, t.datagrams, t.bytes, t.span_us), (7, 20, 28000, 900));
    }

    #[test]
    fn input_events() {
        let env = InputEnvelope::decode(&bytes("0881808080808080104221080112180000003f000080bf00000000000000000000803f0000803e1a03010001")[..]).unwrap();
        assert_eq!(env.client_time_us, 9_007_199_254_740_993);
        let Some(Event::Gamepad(g)) = env.event else { panic!("not a gamepad") };
        assert_eq!(g.pad_index, 1);
        assert_eq!(g.axes, [0.5, -1.0, 0.0, 0.0, 1.0, 0.25]);
        assert_eq!(g.buttons, [true, false, true]);
        let env = InputEnvelope::decode(&bytes("08051a1409000000000000d03f11000000000000e83f1801")[..]).unwrap();
        let Some(Event::PointerMotion(m)) = env.event else { panic!("not motion") };
        assert_eq!((m.dx, m.dy, m.absolute), (0.25, 0.75, true));
        let env = ControlEnvelope::decode(&bytes("820100")[..]).unwrap();
        assert!(matches!(env.msg, Some(Msg::LogoutRequest(_))));
    }

    #[test]
    fn diagnostics_report() {
        let env = ControlEnvelope::decode(&bytes("9201060a046c6f670a")[..]).unwrap();
        let Some(Msg::DiagnosticsReport(r)) = env.msg else { panic!("not a diagnostics report") };
        assert_eq!(r.text, b"log\n");
        // And the request as wraith sends it, which proto.js decodes as {}.
        let req = ControlEnvelope { msg: Some(Msg::DiagnosticsRequest(Default::default())) };
        assert_eq!(req.encode_to_vec(), bytes("8a0100"));
    }

    // And the other way: what prost writes, for app/proto.js to read
    // (printed, for checking app/proto.js against).
    #[test]
    fn prost_vectors() {
        use ipc::session::{AudioConfig, CursorShape, DisplayDescriptor, OutputDescriptor, Ping, SessionAccept};
        let accept = ControlEnvelope {
            msg: Some(Msg::Accept(SessionAccept {
                outputs: vec![OutputDescriptor {
                    stream_id: 0,
                    display: Some(DisplayDescriptor { width: 2560, height: 1440, refresh_mhz: 60000, scale: 1.0 }),
                    color: None,
                }],
                codec: "h264".into(),
                audio: Some(AudioConfig { sample_rate_hz: 48000, channels: 2, frame_ms: 10, codec: "opus".into() }),
                capabilities: vec!["clipboard".into(), "gamepad".into()],
                encoder: "vaapi".into(),
                host_user: "alice".into(),
                host_name: "host1".into(),
                network_profile: 1,
                ..Default::default()
            })),
        };
        let cursor = ControlEnvelope {
            msg: Some(Msg::CursorShape(CursorShape { width: 1, height: 1, hotspot_x: 0, hotspot_y: 0, argb8888: vec![1, 2, 3, 4] })),
        };
        let ping = ControlEnvelope { msg: Some(Msg::Ping(Ping { nonce: u64::MAX })) };
        for m in [accept, cursor, ping] {
            let hex: String = m.encode_to_vec().iter().map(|b| format!("{b:02x}")).collect();
            println!("{hex}");
        }
    }
}
