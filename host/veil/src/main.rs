// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// veild: Veil, the ghost broker (docs/design/veil.md). One QUIC
// endpoint on the lobby port carries spectre's logins (lobby.rs), the
// joined hosts' channels (`gdp-host/1`, hosts.rs) and Wisp thin clients'
// reports (`wisp/1`, thin_clients.rs); its state is one SQLite file
// (db.rs). Settings come from veild.toml (config.rs), which the
// flags below override.
mod config;
mod db;
mod gateway;
mod hosts;
mod lobby;
mod lobby_tls;
mod relay;
mod thin_clients;
mod web;
mod ws;
mod wt;

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use preauth::{Penalties, StartupLimiter};
use quinn::crypto::rustls::QuicServerConfig;
use tracing::{debug, info, warn};
use tracing_subscriber::EnvFilter;

use config::Config;
use db::Db;
use hosts::Hosts;

/// Every setting lives in veild.toml (config.rs, packaging/config/veild.toml); a
/// flag here overrides the file for this run.
#[derive(Parser)]
struct Args {
    /// Configuration file; its drop-ins are read from the same path with
    /// a `.d` extension. Missing is fine at the default path (built-in
    /// defaults), an error when named here.
    #[arg(short = 'f', long)]
    config: Option<PathBuf>,

    /// Check the configuration and the lobby certificate, then exit
    /// (sshd -t).
    #[arg(short = 't')]
    test: bool,

    /// Print the effective configuration as TOML, then exit (sshd -T).
    #[arg(short = 'T')]
    dump: bool,

    /// [lobby] address.
    #[arg(long)]
    address: Option<String>,
    /// [lobby] port.
    #[arg(long)]
    port: Option<u16>,

    /// [lobby] cert.
    #[arg(long)]
    cert: Option<PathBuf>,

    /// [lobby] key.
    #[arg(long)]
    key: Option<PathBuf>,

    /// [state] database.
    #[arg(long)]
    database: Option<PathBuf>,

    /// [log] level. RUST_LOG, when set, still wins.
    #[arg(long)]
    log_level: Option<String>,

    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(Subcommand)]
enum Command {
    /// Issue a join token and print the `ghostd join` command to run on
    /// the host, as the admin UI's "Add host" page does. Run as root or as
    /// the veil user; root switches to the database's owner first.
    Token {
        /// How long the token stays valid; [hosts] join_token_ttl when
        /// not given.
        #[arg(long, value_parser = preauth::parse_duration)]
        ttl: Option<Duration>,
    },
    /// Print the Wisp boot server's .env lines for this Veil: the lobby
    /// certificate's fingerprint (VEIL_CERT_SHA256) and the thin clients'
    /// key (WISP_KEY), so `veild wisp-env >> .env` works. Run as root or
    /// as a member of the veil group.
    WispEnv,
}

impl Args {
    fn apply(&self, config: &mut Config) {
        set(&mut config.lobby.address, &self.address);
        set(&mut config.lobby.port, &self.port);
        set(&mut config.lobby.cert, &self.cert);
        set(&mut config.lobby.key, &self.key);
        set(&mut config.state.database, &self.database);
        set(&mut config.log.level, &self.log_level);
    }
}

fn set<T: Clone>(key: &mut T, flag: &Option<T>) {
    if let Some(value) = flag {
        *key = value.clone();
    }
}

fn main() -> Result<()> {
    let args = Args::parse();

    let path = args.config.clone().unwrap_or_else(|| PathBuf::from(config::DEFAULT_PATH));
    let (mut config, files) = config::load(&path, args.config.is_some())?;
    args.apply(&mut config);
    config.validate()?;

    if args.dump {
        print!("{}", toml::to_string(&config).context("printing the configuration")?);
        return Ok(());
    }
    if args.test {
        gdpnet::load_host_identity(&config.lobby.cert, &config.lobby.key)?;
        pamconv::check_helper()?;
        return Ok(());
    }
    match args.command {
        Some(Command::Token { ttl }) => return issue_token(&config, ttl),
        Some(Command::WispEnv) => return wisp_env(&config),
        None => {}
    }

    let filter = match std::env::var("RUST_LOG") {
        Ok(directives) if !directives.is_empty() => directives,
        _ => config.log.level.clone(),
    };
    tracing_subscriber::fmt().with_env_filter(EnvFilter::new(&filter)).init();

    if files.is_empty() {
        info!(path = %path.display(), "veild: no configuration file, running on built-in defaults");
    } else {
        info!(?files, "veild: configuration loaded");
    }
    // Holds passwords (in flight, and sealed for browser users): keep
    // them out of core dumps and away from ptrace.
    if let Err(e) = nix::sys::prctl::set_dumpable(false) {
        warn!(error = %e, "veild: couldn't turn off core dumps");
    }
    let runtime = tokio::runtime::Runtime::new()?;
    let result = runtime.block_on(run(config));
    // Gateway sessions and the like may still be winding down; don't
    // wait on them.
    runtime.shutdown_timeout(Duration::from_secs(1));
    result
}

/// `veild token`: writes the token straight into the database, so the
/// daemon needn't be running.
fn issue_token(config: &Config, ttl: Option<Duration>) -> Result<()> {
    become_database_owner(&config.state.database)?;
    let fingerprint = gdpnet::load_host_identity(&config.lobby.cert, &config.lobby.key)
        .context("loading the lobby certificate")?
        .fingerprint;
    let db = Arc::new(Db::open(&config.state.database)?);
    let hosts = Hosts::new(db, config.gateway.default_mode, config.hosts.max_login_streams);
    let actor = std::env::var("SUDO_USER").unwrap_or_else(|_| "cli".to_string());
    let ttl = ttl.unwrap_or(config.hosts.join_token_ttl);
    let token = hosts.issue_join_token(ttl, &actor, &fingerprint)?;
    let address = match config.lobby.port {
        4442 => config.public_address(),
        port => format!("{}:{port}", config.public_address()),
    };
    println!("On the host, as root (valid for {}, once):\n", preauth::duration::format(ttl));
    println!("  ghostd join {address} --token {token}");
    Ok(())
}

/// `veild wisp-env`.
fn wisp_env(config: &Config) -> Result<()> {
    let fingerprint = gdpnet::load_host_identity(&config.lobby.cert, &config.lobby.key)
        .context("loading the lobby certificate")?
        .fingerprint;
    let key = thin_clients::load_key(&config.thin_clients.key)
        .context("loading the Wisp key ([thin_clients] key; `make install-veil` generates it)")?;
    println!("VEIL_CERT_SHA256={fingerprint}");
    println!("WISP_KEY={key}");
    Ok(())
}

/// The database must stay the veil user's: a root-owned WAL file left
/// behind would lock veild out of it. When run as root, switch to the
/// database directory's owner before touching it.
fn become_database_owner(database: &Path) -> Result<()> {
    use std::os::unix::fs::MetadataExt;
    if !nix::unistd::geteuid().is_root() {
        return Ok(());
    }
    let dir = database.parent().filter(|d| !d.as_os_str().is_empty()).unwrap_or(Path::new("."));
    let meta = std::fs::metadata(dir).with_context(|| format!("reading {}", dir.display()))?;
    if meta.uid() == 0 {
        return Ok(());
    }
    let (uid, gid) = (nix::unistd::Uid::from_raw(meta.uid()), nix::unistd::Gid::from_raw(meta.gid()));
    nix::unistd::setgroups(&[]).context("dropping supplementary groups")?;
    nix::unistd::setgid(gid).context("setgid")?;
    nix::unistd::setuid(uid).context("setuid")?;
    if nix::unistd::geteuid().is_root() {
        bail!("could not switch to the database's owner");
    }
    Ok(())
}

/// What every connection handler shares.
pub struct Veil {
    pub config: Config,
    pub db: Arc<Db>,
    pub hosts: Arc<Hosts>,
    pub thin_clients: thin_clients::ThinClients,
    pub penalties: Penalties,
    /// What the lobby port presents (lobby_tls.rs): the lobby
    /// certificate's fingerprint for join tokens, the clients' for
    /// gateway redirects.
    pub certs: Arc<lobby_tls::LobbyCerts>,
    pub gateway: gateway::Gateway,
}

async fn run(config: Config) -> Result<()> {
    rustls::crypto::ring::default_provider()
        .install_default()
        .map_err(|_| anyhow::anyhow!("failed to install rustls ring CryptoProvider"))?;

    let web_for_clients = config
        .lobby
        .clients_use_web_cert
        .then(|| (Path::new(&config.web.cert), Path::new(&config.web.key)));
    let certs = lobby_tls::LobbyCerts::load(&config.lobby.cert, &config.lobby.key, web_for_clients)?;

    let mut server_crypto = rustls::ServerConfig::builder()
        .with_client_cert_verifier(gdpnet::AnyClientCert::new())
        .with_cert_resolver(certs.clone());
    server_crypto.alpn_protocols = vec![b"gdp/1".to_vec(), b"gdp-host/1".to_vec(), b"wisp/1".to_vec()];
    let server_config =
        quinn::ServerConfig::with_crypto(Arc::new(QuicServerConfig::try_from(server_crypto).context("building QuicServerConfig")?));

    let mut server_config = server_config;
    let mut transport = quinn::TransportConfig::default();
    // Hosts ping every 10 s (ghostd's broker.rs); this is how long Veil
    // waits before it counts one as gone.
    transport.max_idle_timeout(Some(config.hosts.idle_timeout.try_into().context("hosts.idle_timeout")?));
    // Gateway sessions (gateway.rs): quinn's datagram buffer per leg, and
    // path MTU discovery up to the configured cap.
    transport.datagram_send_buffer_size(gateway::SEND_BUFFER);
    let mut mtu = quinn::MtuDiscoveryConfig::default();
    mtu.upper_bound(config.gateway.max_udp_payload);
    transport.mtu_discovery_config(Some(mtu));
    server_config.transport_config(Arc::new(transport));
    let mut endpoint_config = quinn::EndpointConfig::default();
    endpoint_config.max_udp_payload_size(config.gateway.max_udp_payload).context("gateway.max_udp_payload")?;

    let db = Arc::new(Db::open(&config.state.database)?);
    // Sessions saved by the last run belong to hosts that aren't connected
    // yet; they would show as running on hosts that may be gone.
    db.clear_placements()?;
    let hosts = Arc::new(Hosts::new(db.clone(), config.gateway.default_mode, config.hosts.max_login_streams));
    let wisp_key = match thin_clients::load_key(&config.thin_clients.key) {
        Ok(key) => Some(key),
        Err(e) => {
            warn!(error = %format!("{e:#}"), "veild: no Wisp key, so thin clients can't connect (`make install-veil` generates it)");
            None
        }
    };
    let veil = Arc::new(Veil {
        penalties: Penalties::new(config.auth.penalties),
        thin_clients: thin_clients::ThinClients::new(db.clone(), wisp_key),
        db,
        hosts,
        certs,
        config,
        gateway: Default::default(),
    });
    let web_tls = web::serve(veil.clone()).await?;
    // Browser sessions: WebTransport on the web UI's port, over UDP.
    let web_addr = veil.config.web_listen()?;
    let webtransport = match web_addr {
        Some(addr) => Some(wt::serve(veil.clone(), addr).await?),
        None => None,
    };
    // SIGHUP: reload the web certificate (an ACME renewal hook runs
    // `systemctl reload veild`), wherever it is presented. Nothing else is
    // reloaded.
    tokio::spawn({
        let veil = veil.clone();
        async move {
            use tokio::signal::unix::{signal, SignalKind};
            let Ok(mut hup) = signal(SignalKind::hangup()) else { return };
            while hup.recv().await.is_some() {
                let Some(tls) = &web_tls else {
                    info!("veild: SIGHUP, but HTTPS is off; nothing to reload");
                    continue;
                };
                match tls.reload_from_pem_file(&veil.config.web.cert, &veil.config.web.key).await {
                    Ok(()) => info!(cert = %veil.config.web.cert, "veild: reloaded the web certificate"),
                    Err(e) => warn!(error = %e, "veild: reloading the web certificate failed; keeping the old one"),
                }
                if let (Some(endpoint), Some(addr)) = (&webtransport, web_addr) {
                    wt::reload(endpoint, &veil, addr).await;
                }
                if veil.certs.clients_use_web_cert() {
                    let (cert, key) = (Path::new(&veil.config.web.cert), Path::new(&veil.config.web.key));
                    match veil.certs.reload_clients(cert, key) {
                        Ok(()) => info!(cert_sha256 = %veil.certs.client_fingerprint(), "veild: lobby clients now get the reloaded web certificate"),
                        Err(e) => warn!(error = %format!("{e:#}"), "veild: reloading the web certificate for lobby clients failed; keeping the old one"),
                    }
                }
            }
        }
    });
    let startups = StartupLimiter::new(veil.config.auth.max_startups);

    let address = veil.config.lobby_address()?;
    let endpoint = gdpnet::bind(address, veil.config.lobby.port, endpoint_config, Some(server_config))?;
    info!(
        address = %address.map_or("[::]".to_string(), |a| a.to_string()),
        port = veil.config.lobby.port,
        cert_sha256 = %veil.certs.lobby_fingerprint(),
        clients_cert_sha256 = %veil.certs.client_fingerprint(),
        "veild: lobby listening"
    );

    // On SIGTERM/SIGINT, close every connection with a proper
    // CONNECTION_CLOSE, so joined hosts start reconnecting at once rather
    // than after their idle timeout. Closing the endpoint ends the accept
    // loop below, and veild returns from main.
    {
        let endpoint = endpoint.clone();
        tokio::spawn(async move {
            use tokio::signal::unix::{signal, SignalKind};
            let (Ok(mut term), Ok(mut int)) = (signal(SignalKind::terminate()), signal(SignalKind::interrupt())) else {
                return;
            };
            tokio::select! {
                _ = term.recv() => {}
                _ = int.recv() => {}
            }
            info!("veild: stopping");
            endpoint.close(quinn::VarInt::from_u32(0), b"veild stopping");
        });
    }

    // As ghostd's lobby (docs/design/preauth.md), every refusal
    // here happens before the handshake. The ALPN isn't known yet, so the
    // limits apply to hosts' channels and thin clients too: both share
    // their address's penalty. A host holds a MaxStartups slot until its
    // handshake is done -- with its client certificate, that already
    // proves it holds a key -- and a thin client until its key checks out.
    while let Some(incoming) = endpoint.accept().await {
        let source = gdpnet::canonical(incoming.remote_address());
        if veil.penalties.refuses(source.ip()) {
            debug!(%source, "veild: refusing a penalised source");
            incoming.refuse();
            continue;
        }
        if startups.under_pressure() && !incoming.remote_address_validated() && incoming.may_retry() {
            if let Err(e) = incoming.retry() {
                e.into_incoming().ignore();
            }
            continue;
        }
        let Some(slot) = startups.try_acquire() else {
            warn!(%source, "veild: dropping a connection, too many unauthenticated (max_startups)");
            incoming.refuse();
            continue;
        };
        tokio::spawn(accept(incoming, veil.clone(), slot));
    }
    let _ = tokio::time::timeout(Duration::from_secs(2), endpoint.wait_idle()).await;
    Ok(())
}

async fn accept(incoming: quinn::Incoming, veil: Arc<Veil>, slot: preauth::StartupSlot) {
    let source = gdpnet::canonical(incoming.remote_address());
    let grace = veil.config.auth.login_grace_time;
    let handshake = async {
        match grace.is_zero() {
            true => Some(incoming.await),
            false => tokio::time::timeout(grace, incoming).await.ok(),
        }
    };
    let conn = match handshake.await {
        Some(Ok(conn)) => conn,
        Some(Err(e)) => {
            debug!(%source, error = %e, "veild: handshake failed");
            return;
        }
        None => {
            warn!(%source, "veild: handshake did not finish within the login grace time");
            return;
        }
    };
    let alpn = conn
        .handshake_data()
        .and_then(|h| h.downcast::<quinn::crypto::rustls::HandshakeData>().ok())
        .and_then(|h| h.protocol);
    match alpn.as_deref() {
        Some(b"gdp-host/1") => {
            drop(slot);
            veil.hosts.clone().serve(conn).await
        }
        Some(b"gdp/1") => lobby::serve(conn, veil, slot).await,
        Some(b"wisp/1") => thin_clients::serve(conn, veil, slot).await,
        _ => conn.close(quinn::VarInt::from_u32(0), b"unknown ALPN"),
    }
}
