// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The browser client's transport (docs/design/browser-client.md):
// WebTransport over HTTP/3, on the web UI's port over UDP, with the web
// UI's CA-valid certificate -- so a browser needs no
// serverCertificateHashes and wraith's own certificates never reach it.
// A WebTransport session at /gdp is a GDP session connection in every way
// that matters: its first bidi stream is the control stream (starting with
// the SessionHello that carries the gateway token), its second the input
// stream, and its datagrams the video and audio. gateway.rs relays it to
// the session's wraith.
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result};
use tracing::{debug, info, warn};
use wtransport::{Endpoint, Identity, ServerConfig};

use crate::gateway::{self, ClientLeg};
use crate::Veil;

pub const PATH: &str = "/gdp";

// The browser leg's transport: the gateway's datagram send buffer and
// path MTU cap (gateway.rs), and an idle timeout for a browser that
// vanished without a close.
fn transport(veil: &Veil) -> Result<wtransport::quinn::TransportConfig> {
    let mut transport = wtransport::quinn::TransportConfig::default();
    transport.datagram_send_buffer_size(gateway::SEND_BUFFER);
    transport.max_idle_timeout(Some(Duration::from_secs(30).try_into()?));
    let mut mtu = wtransport::quinn::MtuDiscoveryConfig::default();
    mtu.upper_bound(veil.config.gateway.max_udp_payload);
    transport.mtu_discovery_config(Some(mtu));
    Ok(transport)
}

async fn identity(veil: &Veil) -> Result<Identity> {
    Identity::load_pemfiles(&veil.config.web.cert, &veil.config.web.key)
        .await
        .with_context(|| format!("loading the web certificate {} for WebTransport", veil.config.web.cert))
}

async fn config(veil: &Veil, addr: SocketAddr) -> Result<ServerConfig> {
    let socket = dual_stack_socket(addr)?;
    Ok(ServerConfig::builder().with_bind_socket(socket).with_custom_transport(identity(veil).await?, transport(veil)?).build())
}

// The same dual-stack binding as the QUIC endpoints (gdpnet), for the
// UDP side of [web] listen.
fn dual_stack_socket(addr: SocketAddr) -> Result<std::net::UdpSocket> {
    use socket2::{Domain, Protocol, Socket, Type};
    let domain = if addr.is_ipv6() { Domain::IPV6 } else { Domain::IPV4 };
    let socket = Socket::new(domain, Type::DGRAM, Some(Protocol::UDP)).context("creating the WebTransport socket")?;
    if addr.is_ipv6() {
        socket.set_only_v6(false).context("clearing IPV6_V6ONLY")?;
    }
    socket.bind(&addr.into()).with_context(|| format!("binding WebTransport to {addr} (UDP)"))?;
    Ok(socket.into())
}

/// Starts the WebTransport listener; returns the endpoint so SIGHUP can
/// reload its certificate.
pub async fn serve(veil: Arc<Veil>, addr: SocketAddr) -> Result<Arc<Endpoint<wtransport::endpoint::endpoint_side::Server>>> {
    let endpoint = Arc::new(Endpoint::server(config(&veil, addr).await?).context("starting WebTransport")?);
    info!(%addr, "web: browser sessions on WebTransport (UDP)");
    let accepting = endpoint.clone();
    tokio::spawn(async move {
        loop {
            let incoming = accepting.accept().await;
            let peer = gdpnet::canonical(incoming.remote_address());
            if veil.penalties.refuses(peer.ip()) {
                incoming.refuse();
                continue;
            }
            let veil = veil.clone();
            tokio::spawn(async move {
                let request = match tokio::time::timeout(Duration::from_secs(10), incoming).await {
                    Ok(Ok(request)) => request,
                    Ok(Err(e)) => {
                        debug!(%peer, error = %e, "web: WebTransport handshake failed");
                        return;
                    }
                    Err(_) => return,
                };
                if request.path() != PATH {
                    request.not_found().await;
                    return;
                }
                match request.accept().await {
                    Ok(conn) => gateway::serve_client(ClientLeg::WebTransport(conn), veil, peer).await,
                    Err(e) => debug!(%peer, error = %e, "web: WebTransport session failed"),
                }
            });
        }
    });
    Ok(endpoint)
}

/// SIGHUP: the web certificate changed.
pub async fn reload(endpoint: &Endpoint<wtransport::endpoint::endpoint_side::Server>, veil: &Veil, addr: SocketAddr) {
    // rebind = false: the address is only recorded, the socket stays.
    let result = async {
        let config = ServerConfig::builder()
            .with_bind_address(addr)
            .with_custom_transport(identity(veil).await?, transport(veil)?)
            .build();
        endpoint.reload_config(config, false)?;
        Ok::<_, anyhow::Error>(())
    };
    if let Err(e) = result.await {
        warn!(error = %format!("{e:#}"), "web: reloading the WebTransport certificate failed");
    }
}
