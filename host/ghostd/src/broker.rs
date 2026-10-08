// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The host channel, ghostd's side (docs/design/veil.md, gdp-spec.md §13,
// host/proto/broker.proto): `ghostd join` and `ghostd leave`, and the
// long-lived connection a joined ghostd keeps to Veil. ghostd dials out,
// so a host behind NAT needs no inbound port for Veil's logins. Over it
// ghostd reports its sessions (session.rs's events) and runs every login
// Veil relays on a stream of its own (lobby.rs's handle_login_stream).
use std::net::SocketAddr;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use anyhow::{anyhow, bail, Context, Result};
use ipc::broker::{host_envelope::Msg, HostEnvelope, HostErrorCode, HostHello, HostSessionType, Join, Leave, Snapshot};
use ipc::framing::{read_frame, write_frame};
use quinn::crypto::rustls::QuicClientConfig;
use tokio::sync::broadcast::error::RecvError;
use tokio::time::timeout;
use tracing::{error, info, warn};

use crate::config::{self, Broker};
use crate::lobby::{self, LobbyContext};
use crate::session::SessionManager;

const ALPN: &[u8] = b"gdp-host/1";
const DEFAULT_PORT: u16 = 4442;
const VERSION: &str = env!("CARGO_PKG_VERSION");
// Pings keep NAT mappings open; a host silent for IDLE_TIMEOUT is offline
// at Veil.
const KEEP_ALIVE: Duration = Duration::from_secs(10);
const IDLE_TIMEOUT: Duration = Duration::from_secs(30);
const BACKOFF_MIN: Duration = Duration::from_secs(1);
const BACKOFF_MAX: Duration = Duration::from_secs(60);
// Logins Veil may run on this host at once.
const MAX_LOGIN_STREAMS: u32 = 64;
const REPLY_TIMEOUT: Duration = Duration::from_secs(15);

/// The drop-in `ghostd join` writes and `ghostd leave` removes, next to
/// the main config.
pub fn dropin_path(config_path: &Path) -> PathBuf {
    config_path.with_extension("d").join("broker.toml")
}

/// A join token as the admin UI prints it: `<id>.<secret>:sha256:<hex>`.
/// The part before ":sha256:" goes to Veil; the fingerprint is Veil's
/// lobby certificate, which ghostd pins before sending anything.
fn parse_token(token: &str) -> Result<(&str, String)> {
    let (secret, pin) = token.rsplit_once(":sha256:").context("the token has no \":sha256:<fingerprint>\" part")?;
    let pin = pin.to_ascii_lowercase();
    if !gdpnet::is_sha256_hex(&pin) || !secret.contains('.') {
        bail!("malformed token: copy the whole command from Veil's admin UI");
    }
    Ok((secret, pin))
}

/// "host" or "host:port" or "[v6]:port" -> (host for SNI, port).
fn split_address(address: &str) -> Result<(String, u16)> {
    if let Some(rest) = address.strip_prefix('[') {
        let (host, tail) = rest.split_once(']').context("unclosed [ in the address")?;
        let port = match tail.strip_prefix(':') {
            Some(p) => p.parse().context("invalid port")?,
            None if tail.is_empty() => DEFAULT_PORT,
            None => bail!("invalid address {address:?}"),
        };
        return Ok((host.to_string(), port));
    }
    match address.rsplit_once(':') {
        // A bare IPv6 address has more than one colon and no port.
        Some((host, port)) if !host.contains(':') => Ok((host.to_string(), port.parse().context("invalid port")?)),
        _ => Ok((address.to_string(), DEFAULT_PORT)),
    }
}

async fn resolve(host: &str, port: u16) -> Result<SocketAddr> {
    let mut addrs = tokio::net::lookup_host((host, port)).await.with_context(|| format!("resolving {host}"))?;
    addrs.next().with_context(|| format!("{host} has no address"))
}

/// A client endpoint presenting this host's certificate and pinning
/// Veil's.
fn endpoint(identity_cert: &Path, identity_key: &Path, pin: &str) -> Result<quinn::Endpoint> {
    let (chain, key) = gdpnet::load_host_identity(identity_cert, identity_key)
        .context("loading the host certificate ([lobby] cert/key)")?
        .into_parts();
    let mut crypto = rustls::ClientConfig::builder()
        .dangerous()
        .with_custom_certificate_verifier(gdpnet::PinnedServer::new(pin))
        .with_client_auth_cert(chain, key)
        .context("building the host channel's TLS config")?;
    crypto.alpn_protocols = vec![ALPN.to_vec()];
    let mut client = quinn::ClientConfig::new(Arc::new(QuicClientConfig::try_from(crypto)?));
    let mut transport = quinn::TransportConfig::default();
    transport.keep_alive_interval(Some(KEEP_ALIVE));
    transport.max_idle_timeout(Some(IDLE_TIMEOUT.try_into()?));
    transport.max_concurrent_bidi_streams(MAX_LOGIN_STREAMS.into());
    transport.max_concurrent_uni_streams(0u32.into());
    client.transport_config(Arc::new(transport));
    let mut endpoint = gdpnet::bind_dual_stack(0, quinn::EndpointConfig::default(), None)?;
    endpoint.set_default_client_config(client);
    Ok(endpoint)
}

async fn connect(endpoint: &quinn::Endpoint, address: &str) -> Result<quinn::Connection> {
    let (host, port) = split_address(address)?;
    let addr = resolve(&host, port).await?;
    let connecting = endpoint.connect(addr, &host).with_context(|| format!("connecting to {address}"))?;
    timeout(REPLY_TIMEOUT, connecting)
        .await
        .with_context(|| format!("{address} did not answer"))?
        .map_err(|e| match e {
            quinn::ConnectionError::TransportError(t) if t.to_string().contains("certificate") => {
                anyhow!("{address} did not present the certificate in the token: {t}")
            }
            e => anyhow!("connecting to {address}: {e}"),
        })
}

/// Reads one reply on the control stream; a HostError is an Err.
async fn reply(recv: &mut quinn::RecvStream) -> Result<Msg> {
    let env: HostEnvelope =
        timeout(REPLY_TIMEOUT, read_frame(recv)).await.context("Veil did not reply")?.context("reading Veil's reply")?;
    match env.msg {
        Some(Msg::Error(e)) => {
            let code = HostErrorCode::try_from(e.code).unwrap_or(HostErrorCode::HostErrorUnspecified);
            Err(anyhow!(HostRefused { code, message: e.message }))
        }
        Some(msg) => Ok(msg),
        None => bail!("empty reply from Veil"),
    }
}

#[derive(Debug)]
struct HostRefused {
    code: HostErrorCode,
    message: String,
}

impl std::fmt::Display for HostRefused {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "Veil refused this host: {}", self.message)
    }
}

impl std::error::Error for HostRefused {}

/// `ghostd join <veil> --token <token>`: introduces this host to Veil and
/// writes ghostd.d/broker.toml. Run as root, which owns /etc/ghost.
pub async fn join(config_path: &Path, cfg: &config::Config, address: &str, token: &str, client_address: Option<String>, force: bool) -> Result<()> {
    let dropin = dropin_path(config_path);
    if cfg.broker.joined() && !force {
        bail!(
            "this host is already joined to {} (device {}); run `ghostd leave` first, or pass --force",
            cfg.broker.address,
            cfg.broker.device_id
        );
    }
    let (secret, pin) = parse_token(token)?;
    let hostname = fqdn();
    let client_address = client_address.unwrap_or_else(|| hostname.clone());

    let endpoint = endpoint(&cfg.lobby.cert, &cfg.lobby.key, &pin)?;
    let conn = connect(&endpoint, address).await?;
    let (mut send, mut recv) = conn.open_bi().await?;
    let join = Join { token: secret.to_string(), hostname: hostname.clone(), client_address: client_address.clone(), version: VERSION.to_string() };
    write_frame(&mut send, &HostEnvelope { msg: Some(Msg::Join(join)) }).await?;
    let device_id = match reply(&mut recv).await? {
        Msg::JoinAccepted(a) => a.device_id,
        other => bail!("unexpected reply from Veil: {other:?}"),
    };
    conn.close(0u32.into(), b"");
    endpoint.wait_idle().await;

    let canonical_address = match split_address(address)? {
        (host, DEFAULT_PORT) => host,
        (host, port) if host.contains(':') => format!("[{host}]:{port}"),
        (host, port) => format!("{host}:{port}"),
    };
    let text = format!(
        "# Written by `ghostd join`; `ghostd leave` removes it.\n\
         [broker]\n\
         address = {:?}\n\
         cert_sha256 = {:?}\n\
         device_id = {:?}\n",
        canonical_address, pin, device_id
    );
    write_dropin(&dropin, &text)?;
    println!("Joined Veil at {canonical_address} as device {device_id} ({hostname}, clients reach it at {client_address}).");
    println!("Wrote {}.", dropin.display());
    println!("Restart ghostd to connect: systemctl restart ghostd");
    println!("To accept logins only through Veil, set broker_only = true under [broker] (ghostd.toml or a drop-in).");
    Ok(())
}

fn write_dropin(path: &Path, text: &str) -> Result<()> {
    let dir = path.parent().context("no drop-in directory")?;
    std::fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
    std::fs::set_permissions(dir, std::fs::Permissions::from_mode(0o755)).ok();
    let tmp = path.with_extension("toml.tmp");
    std::fs::write(&tmp, text).with_context(|| format!("writing {}", tmp.display()))?;
    std::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o644))?;
    std::fs::rename(&tmp, path).with_context(|| format!("writing {}", path.display()))?;
    Ok(())
}

/// `ghostd leave`: tells Veil to forget this host, if it can be reached,
/// and removes broker.toml either way.
pub async fn leave(config_path: &Path, cfg: &config::Config) -> Result<()> {
    let b = &cfg.broker;
    if !b.joined() {
        bail!("this host isn't joined to a Veil");
    }
    match tell_veil_leave(cfg).await {
        Ok(()) => println!("Veil at {} forgot device {}.", b.address, b.device_id),
        Err(e) => println!(
            "Couldn't reach Veil to say so ({e:#}); remove device {} in its admin UI.",
            b.device_id
        ),
    }
    let dropin = dropin_path(config_path);
    match std::fs::remove_file(&dropin) {
        Ok(()) => println!("Removed {}.", dropin.display()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => println!(
            "{} doesn't exist: remove the [broker] settings from wherever they are set (ghostd -T shows them).",
            dropin.display()
        ),
        Err(e) => return Err(e).with_context(|| format!("removing {}", dropin.display())),
    }
    println!("Restart ghostd to drop the connection: systemctl restart ghostd");
    if b.broker_only {
        println!("broker_only is still set: ghostd won't accept direct logins until you turn it off.");
    }
    Ok(())
}

async fn tell_veil_leave(cfg: &config::Config) -> Result<()> {
    let b = &cfg.broker;
    let endpoint = endpoint(&cfg.lobby.cert, &cfg.lobby.key, &b.cert_sha256)?;
    let conn = connect(&endpoint, &b.address).await?;
    let (mut send, mut recv) = conn.open_bi().await?;
    let hello = HostHello { device_id: b.device_id.clone(), version: VERSION.to_string() };
    write_frame(&mut send, &HostEnvelope { msg: Some(Msg::Hello(hello)) }).await?;
    match reply(&mut recv).await? {
        Msg::Welcome(_) => {}
        other => bail!("unexpected reply from Veil: {other:?}"),
    }
    write_frame(&mut send, &HostEnvelope { msg: Some(Msg::Leave(Leave {})) }).await?;
    match reply(&mut recv).await? {
        Msg::Left(_) => {}
        other => bail!("unexpected reply from Veil: {other:?}"),
    }
    conn.close(0u32.into(), b"");
    endpoint.wait_idle().await;
    Ok(())
}

/// The joined ghostd's channel to Veil, for as long as ghostd runs:
/// connect, HostHello, Snapshot, then events out and login streams in,
/// reconnecting with exponential backoff and jitter whenever it drops.
pub async fn run(broker: Broker, cert: PathBuf, key: PathBuf, ctx: Arc<LobbyContext>, sessions: Arc<SessionManager>) {
    let endpoint = match endpoint(&cert, &key, &broker.cert_sha256) {
        Ok(e) => e,
        Err(e) => {
            error!(error = %format!("{e:#}"), "broker: can't set up the channel to Veil; staying unconnected");
            return;
        }
    };
    let mut backoff = BACKOFF_MIN;
    loop {
        match serve(&endpoint, &broker, &ctx, &sessions).await {
            // It was up: start the backoff over.
            Ok(()) => {
                info!(veil = %broker.address, "broker: channel to Veil closed");
                backoff = BACKOFF_MIN;
            }
            Err(e) => match e.downcast_ref::<HostRefused>() {
                Some(r) if matches!(r.code, HostErrorCode::HostErrorUnknownDevice | HostErrorCode::HostErrorCertMismatch) => {
                    error!(veil = %broker.address, error = %e,
                        "broker: Veil doesn't know this host anymore; `ghostd leave` and join again");
                    backoff = BACKOFF_MAX;
                }
                _ => warn!(veil = %broker.address, error = %format!("{e:#}"), "broker: channel to Veil failed"),
            },
        }
        // Up to half again on top, so a Veil restart doesn't bring every
        // host back in the same instant.
        let jitter = backoff.mul_f64(rand::random_range(0.0..0.5));
        tokio::time::sleep(backoff + jitter).await;
        backoff = (backoff * 2).min(BACKOFF_MAX);
    }
}

/// Everything Veil rebuilds a host from: the running sessions and the
/// desktops on offer.
async fn build_snapshot(ctx: &LobbyContext, sessions: &SessionManager) -> Snapshot {
    let (types, default_type) = lobby::host_session_types(ctx).await;
    Snapshot {
        sessions: sessions.snapshot().await,
        available_types: types.into_iter().map(|(id, name)| HostSessionType { id, name }).collect(),
        default_type,
    }
}

async fn serve(endpoint: &quinn::Endpoint, broker: &Broker, ctx: &Arc<LobbyContext>, sessions: &Arc<SessionManager>) -> Result<()> {
    let conn = connect(endpoint, &broker.address).await?;
    let (mut send, mut recv) = conn.open_bi().await?;
    let hello = HostHello { device_id: broker.device_id.clone(), version: VERSION.to_string() };
    write_frame(&mut send, &HostEnvelope { msg: Some(Msg::Hello(hello)) }).await?;
    match reply(&mut recv).await? {
        Msg::Welcome(_) => {}
        other => bail!("unexpected reply from Veil: {other:?}"),
    }
    info!(veil = %broker.address, device_id = %broker.device_id, "broker: connected to Veil");

    // Subscribed before the snapshot is taken, so nothing falls between
    // the two; a session in both is harmless.
    let mut events = sessions.subscribe();
    let first = build_snapshot(ctx, sessions).await;
    write_frame(&mut send, &HostEnvelope { msg: Some(Msg::Snapshot(first)) }).await?;

    let forward = async {
        loop {
            let msg = match events.recv().await {
                Ok(msg) => msg,
                // Fell behind: start Veil over from a fresh snapshot.
                Err(RecvError::Lagged(_)) => Msg::Snapshot(build_snapshot(ctx, sessions).await),
                Err(RecvError::Closed) => return Ok::<(), anyhow::Error>(()),
            };
            write_frame(&mut send, &HostEnvelope { msg: Some(msg) }).await?;
        }
    };
    let logins = async {
        loop {
            match conn.accept_bi().await {
                Ok((send, recv)) => {
                    tokio::spawn(lobby::handle_login_stream(send, recv, ctx.clone()));
                }
                Err(quinn::ConnectionError::ApplicationClosed(c)) if c.error_code == 0u32.into() => return Ok(()),
                Err(e) => return Err::<(), anyhow::Error>(e.into()),
            }
        }
    };
    // Anything Veil says on the control stream after the welcome is a
    // HostError (the device was removed or disabled) before it closes.
    let control = async {
        match read_frame::<HostEnvelope>(&mut recv).await {
            Ok(HostEnvelope { msg: Some(Msg::Error(e)) }) => {
                let code = HostErrorCode::try_from(e.code).unwrap_or(HostErrorCode::HostErrorUnspecified);
                Err(anyhow!(HostRefused { code, message: e.message }))
            }
            _ => Ok(()),
        }
    };
    tokio::select! {
        r = forward => r,
        r = logins => r,
        r = control => r,
    }
}

fn fqdn() -> String {
    std::process::Command::new("hostname")
        .arg("-f")
        .output()
        .ok()
        .filter(|o| o.status.success())
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "localhost".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tokens_split_into_secret_and_pin() {
        let pin = "ab".repeat(32);
        let token = format!("0a1b2c.s3cr3t:sha256:{}", pin.to_uppercase());
        let (secret, p) = parse_token(&token).unwrap();
        assert_eq!(secret, "0a1b2c.s3cr3t");
        assert_eq!(p, pin);
        assert!(parse_token("0a1b2c.s3cr3t").is_err());
        assert!(parse_token("nodot:sha256:abcd").is_err());
        assert!(parse_token(&format!("nodot:sha256:{pin}")).is_err());
    }

    #[test]
    fn addresses_default_to_the_lobby_port() {
        assert_eq!(split_address("veil.example").unwrap(), ("veil.example".to_string(), 4442));
        assert_eq!(split_address("veil.example:5000").unwrap(), ("veil.example".to_string(), 5000));
        assert_eq!(split_address("192.0.2.1").unwrap(), ("192.0.2.1".to_string(), 4442));
        assert_eq!(split_address("[2001:db8::1]:5000").unwrap(), ("2001:db8::1".to_string(), 5000));
        assert_eq!(split_address("[2001:db8::1]").unwrap(), ("2001:db8::1".to_string(), 4442));
        assert_eq!(split_address("2001:db8::1").unwrap(), ("2001:db8::1".to_string(), 4442));
        assert!(split_address("veil:notaport").is_err());
    }
}
