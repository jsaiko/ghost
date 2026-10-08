// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// ghostd, the host agent (docs/design/login-and-sessions.md): the lobby
// and its authentication through ghostauth (lobby.rs), each user's session as its ghostseat
// reports it (session.rs), the session-type profiles it offers
// (profiles.rs; the redirect tokens come from ghostseat), console logins ending ghost sessions (local_login.rs),
// and on a host joined to Veil, the channel to it (broker.rs). It runs
// unprivileged, as the `ghost` user: everything that needs root happens
// in ghostseat. Settings come from ghostd.toml (config.rs), which the
// flags below override.
mod broker;
mod config;
mod local_login;
mod lobby;
mod profiles;
mod session;

use std::path::PathBuf;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use quinn::crypto::rustls::QuicServerConfig;
use tracing::{debug, info, warn};
use tracing_subscriber::EnvFilter;

use config::Config;
use lobby::{LobbyContext, LoginPolicy};
use preauth::{Penalties, StartupLimiter};
use session::SessionManager;

/// Every setting lives in ghostd.toml (config.rs, packaging/config/ghostd.toml);
/// a flag here overrides the file for this run, which is mostly for dev
/// runs under `systemd-run`.
#[derive(Parser)]
struct Args {
    /// Configuration file; its drop-ins are read from the same path with
    /// a `.d` extension. Missing is fine at the default path (built-in
    /// defaults), an error when named here.
    #[arg(short = 'f', long)]
    config: Option<PathBuf>,

    /// Check the configuration and the host certificate, then exit
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

    /// [sessions] port_base.
    #[arg(long)]
    session_port_base: Option<u16>,

    /// [sessions] max.
    #[arg(long)]
    max_sessions: Option<u16>,

    /// [sessions] dir.
    #[arg(long)]
    sessions_dir: Option<PathBuf>,

    /// [sessions] default_type.
    #[arg(long)]
    default_session_type: Option<String>,

    /// [lobby] cert.
    #[arg(long)]
    cert: Option<PathBuf>,

    /// [lobby] key.
    #[arg(long)]
    key: Option<PathBuf>,

    /// [auth] permit_root_login; `--permit-root-login=false` turns it off.
    #[arg(long, num_args = 0..=1, require_equals = true, default_missing_value = "true")]
    permit_root_login: Option<bool>,

    /// [auth] permit_empty_passwords; `=false` turns it off.
    #[arg(long, num_args = 0..=1, require_equals = true, default_missing_value = "true")]
    permit_empty_passwords: Option<bool>,

    /// [auth] login_grace_time: seconds, or with an s/m/h suffix.
    #[arg(long, value_parser = preauth::parse_duration)]
    login_grace_time: Option<Duration>,

    /// [auth.max_startups], as sshd's start:rate:full or N.
    #[arg(long)]
    max_startups: Option<preauth::MaxStartups>,

    /// [auth.penalties]: "no", "yes", or sshd-style keyword:time items
    /// (authfail, noauth, grace-exceeded, max, min), comma-separated.
    #[arg(long)]
    per_source_penalties: Option<String>,

    /// [log] level. RUST_LOG, when set, still wins.
    #[arg(long)]
    log_level: Option<String>,

    #[command(subcommand)]
    command: Option<Command>,
}

#[derive(Subcommand)]
enum Command {
    /// Join this host to a Veil, with a token from its admin UI, and
    /// write ghostd.d/broker.toml. Restart ghostd afterwards.
    Join {
        /// Veil's address, "host" or "host:port".
        veil: String,
        /// The token, `<id>.<secret>:sha256:<fingerprint>`, exactly as
        /// Veil's admin UI shows it.
        #[arg(long)]
        token: String,
        /// The address clients use to reach this host directly (Veil puts
        /// it in their redirect). Defaults to this machine's FQDN.
        #[arg(long)]
        address: Option<String>,
        /// Join even though broker.toml says this host already is.
        #[arg(long)]
        force: bool,
    },
    /// Tell Veil to forget this host, and remove broker.toml.
    Leave,
}

impl Args {
    fn apply(&self, config: &mut Config) -> Result<()> {
        set(&mut config.lobby.address, &self.address);
        set(&mut config.lobby.port, &self.port);
        set(&mut config.lobby.cert, &self.cert);
        set(&mut config.lobby.key, &self.key);
        set(&mut config.sessions.port_base, &self.session_port_base);
        set(&mut config.sessions.max, &self.max_sessions);
        set(&mut config.sessions.dir, &self.sessions_dir);
        set(&mut config.sessions.default_type, &self.default_session_type);
        set(&mut config.auth.permit_root_login, &self.permit_root_login);
        set(&mut config.auth.permit_empty_passwords, &self.permit_empty_passwords);
        set(&mut config.auth.login_grace_time, &self.login_grace_time);
        set(&mut config.auth.max_startups, &self.max_startups);
        set(&mut config.log.level, &self.log_level);
        if let Some(items) = &self.per_source_penalties {
            config.auth.penalties.apply_overrides(items).context("--per-source-penalties")?;
        }
        Ok(())
    }
}

// A flag overrides its config key only when it was given.
fn set<T: Clone>(key: &mut T, flag: &Option<T>) {
    if let Some(value) = flag {
        *key = value.clone();
    }
}

fn main() -> Result<()> {
    let args = Args::parse();

    let path = args.config.clone().unwrap_or_else(|| PathBuf::from(config::DEFAULT_PATH));
    let (mut config, files) = config::load(&path, args.config.is_some())?;
    args.apply(&mut config)?;
    config.validate()?;

    if args.dump {
        print!("{}", toml::to_string(&config).context("printing the configuration")?);
        return Ok(());
    }
    if args.test {
        // sshd -t checks its host keys too; and nothing can log in
        // without the ghostauth and ghostseat sockets.
        gdpnet::load_host_identity(&config.lobby.cert, &config.lobby.key)?;
        pamconv::check_helper()?;
        session::check_seat_socket()?;
        return Ok(());
    }
    if let Some(command) = &args.command {
        rustls::crypto::ring::default_provider()
            .install_default()
            .map_err(|_| anyhow::anyhow!("failed to install rustls ring CryptoProvider"))?;
        let rt = tokio::runtime::Runtime::new()?;
        return match command {
            Command::Join { veil, token, address, force } => {
                rt.block_on(broker::join(&path, &config, veil, token, address.clone(), *force))
            }
            Command::Leave => rt.block_on(broker::leave(&path, &config)),
        };
    }

    // Logging starts only now, from log.level; anything that failed
    // above went to stderr through main's Err, which the journal keeps
    // all the same. RUST_LOG, when set, wins for one-off runs.
    let filter = match std::env::var("RUST_LOG") {
        Ok(directives) if !directives.is_empty() => directives,
        _ => config.log.level.clone(),
    };
    tracing_subscriber::fmt().with_env_filter(EnvFilter::new(&filter)).init();

    if files.is_empty() {
        info!(path = %path.display(), "ghostd: no configuration file, running on built-in defaults");
    } else {
        info!(?files, "ghostd: configuration loaded");
    }
    // Holds the login password between PAM's answer and ghostseat's Open
    // (lobby.rs's Authtok): keep it out of core dumps and away from
    // ptrace, as ghostauth, ghostseat and veild do.
    if let Err(e) = nix::sys::prctl::set_dumpable(false) {
        warn!(error = %e, "ghostd: couldn't turn off core dumps");
    }
    tokio::runtime::Runtime::new()?.block_on(run(config))
}

async fn run(config: Config) -> Result<()> {
    let port_range = config.port_range()?;

    // rustls 0.23 panics in ServerConfig::builder() unless exactly one
    // CryptoProvider is compiled in or one is installed explicitly. Only
    // "ring" is enabled today, but installing it here keeps a dependency
    // that turns on aws-lc-rs from becoming a startup panic.
    rustls::crypto::ring::default_provider()
        .install_default()
        .map_err(|_| anyhow::anyhow!("failed to install rustls ring CryptoProvider"))?;

    // Persistent, so spectre's first-use pin stays valid across restarts.
    let identity = gdpnet::load_host_identity(&config.lobby.cert, &config.lobby.key)
        .context("loading the host certificate (`make install` generates it; see [lobby] cert/key)")?;
    let fingerprint = identity.fingerprint.clone();

    let mut server_crypto = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(identity.chain, identity.key)
        .context("building rustls ServerConfig")?;
    server_crypto.alpn_protocols = vec![b"gdp/1".to_vec()]; // gdp-spec.md §2.1

    let server_config = quinn::ServerConfig::with_crypto(Arc::new(
        QuicServerConfig::try_from(server_crypto).context("building QuicServerConfig")?,
    ));

    // broker_only: no lobby listener at all, so every login comes through
    // Veil.
    let endpoint = if config.broker.broker_only {
        info!(session_ports_start = port_range.start, session_ports_end = port_range.end,
            "ghostd: broker_only, no lobby listener; logins come through Veil");
        None
    } else {
        let address = config.lobby_address()?;
        let endpoint = gdpnet::bind(address, config.lobby.port, quinn::EndpointConfig::default(), Some(server_config))?;
        info!(address = %address.map_or("[::]".to_string(), |a| a.to_string()), port = config.lobby.port,
            session_ports_start = port_range.start, session_ports_end = port_range.end,
            cert_sha256 = %fingerprint, "ghostd: lobby endpoint listening");
        Some(endpoint)
    };
    // The sockets land in RUN_DIR and the last session types in
    // STATE_DIR; both must be ghostd's own before the first login.
    session::ensure_run_dir()?;
    session::ensure_state_dir()?;
    let sessions = Arc::new(SessionManager::new(
        port_range,
        PathBuf::from(format!("{}/last-types.json", session::STATE_DIR)),
    ));
    // Before the lobby accepts anything that could race it: every session
    // that outlived the previous ghostd is found again from its seat
    // socket, and a stale socket file removed.
    sessions.reconcile_at_startup().await;
    sessions.clone().spawn_events_listener()?;
    local_login::spawn_listener(sessions.clone())?;
    let ctx = Arc::new(LobbyContext {
        sessions: sessions.clone(),
        profile_dirs: vec![config.sessions.dir, PathBuf::from(concat!(env!("GHOST_DATADIR"), "/sessions.d"))],
        default_session_type: config.sessions.default_type,
        policy: LoginPolicy {
            permit_root_login: config.auth.permit_root_login,
            permit_empty_passwords: config.auth.permit_empty_passwords,
            login_grace_time: Some(config.auth.login_grace_time).filter(|t| !t.is_zero()),
        },
        penalties: Penalties::new(config.auth.penalties),
        open_logins: Default::default(),
    });
    let startups = StartupLimiter::new(config.auth.max_startups);

    if config.broker.joined() {
        tokio::spawn(broker::run(config.broker.clone(), config.lobby.cert.clone(), config.lobby.key.clone(), ctx.clone(), sessions));
    }
    let Some(endpoint) = endpoint else {
        std::future::pending::<()>().await;
        unreachable!();
    };

    // Every refusal here happens before the handshake: no crypto, no
    // ghostauth instance (docs/design/preauth.md).
    while let Some(incoming) = endpoint.accept().await {
        // Unmapped, as lobby.rs logs it (the socket is dual-stack).
        let source = gdpnet::canonical(incoming.remote_address());
        if ctx.penalties.refuses(source.ip()) {
            debug!(%source, "lobby: refusing a penalised source");
            incoming.refuse();
            continue;
        }
        // Under load, a client proves its address (one extra round trip)
        // before it may take a MaxStartups slot.
        if startups.under_pressure() && !incoming.remote_address_validated() && incoming.may_retry() {
            if let Err(e) = incoming.retry() {
                e.into_incoming().ignore();
            }
            continue;
        }
        let Some(slot) = startups.try_acquire() else {
            warn!(%source, "lobby: dropping a connection, too many unauthenticated (--max-startups)");
            incoming.refuse();
            continue;
        };
        tokio::spawn(lobby::handle_incoming(incoming, ctx.clone(), slot));
    }
    Ok(())
}
