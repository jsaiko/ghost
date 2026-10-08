// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Wisp thin clients (docs/design/wisp.md, gdp-spec.md §14).
// Every Wisp client's wisp-agent keeps one connection to Veil's lobby port
// (ALPN `wisp/1`) while it is up: a WispHello with the boot server's shared
// key and the client's report, then session changes as they happen. Veil
// answers with the spectre profile every client applies, pushes a new one
// whenever an administrator saves it, and passes on an administrator's log
// out, restart or shut down. The connection is the client's
// online state; the database keeps the last report, so offline clients
// stay listed with their names.
use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{bail, ensure, Context, Result};
use ipc::framing::{read_frame_max, write_frame, FrameError};
use ipc::lobby::LobbyErrorCode;
use ipc::wisp::{
    wisp_envelope::Msg, ClientProfile, WispAction, WispCommand, WispEnvelope, WispHello, WispProfile, WispWelcome,
};
use preauth::{Offense, StartupSlot};
use quinn::VarInt;
use serde::{Deserialize, Serialize};
use tokio::sync::{mpsc, watch};
use tokio::time::{timeout_at, Instant};
use tracing::{debug, info, warn};

use crate::db::{constant_time_eq, Db, ThinClientRow};
use crate::Veil;

// A hello is a few short lists; anything after it smaller still.
const MAX_FRAME: u32 = 16 * 1024;
// The settings row the profile lives in.
const PROFILE_KEY: &str = "thin_client_profile";

/// The settings every thin client applies: spectre's, at its next spectre
/// launch (SpectreSettings, client/spectre-settings/spectre_settings.h,
/// minus what Wisp always does), and the client's own, at once. Stored as
/// JSON in `settings`.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct Profile {
    /// "WIDTHxHEIGHT"; empty: each client's own screen.
    pub resolution: String,
    /// "fit", "actual", or empty: whatever spectre's menu last picked.
    pub view: String,
    /// "vulkan", "native" or "software".
    pub preferred_decoder: String,
    /// A gdp/video_codec.hpp token, or empty: auto.
    pub preferred_codec: String,
    /// Start sessions lossless; false passes spectre -R off. The session
    /// menu's Lossless Refinement row switches it either way.
    pub lossless_refinement: bool,
    pub allow_pyrowave: bool,
    /// "auto", "lan", "internet" or "mobile".
    pub network_profile: String,
    pub forward_gamepads: bool,
    /// Send the client's microphone to the host (spectre -M).
    pub microphone: bool,
    /// Run spectre with SPECTRE_LOG=debug.
    pub debug_logging: bool,
    /// Turn the display off after this many minutes without input; 0:
    /// never.
    pub display_sleep_minutes: u32,
}

/// SpectreSettings' own defaults.
impl Default for Profile {
    fn default() -> Self {
        Profile {
            resolution: String::new(),
            view: String::new(),
            preferred_decoder: "vulkan".into(),
            preferred_codec: String::new(),
            lossless_refinement: true,
            allow_pyrowave: true,
            network_profile: "auto".into(),
            forward_gamepads: true,
            microphone: false,
            debug_logging: false,
            display_sleep_minutes: 20,
        }
    }
}

pub const VIEWS: &[&str] = &["", "fit", "actual"];
pub const DECODERS: &[&str] = &["vulkan", "native", "software"];
/// gdp::all_video_codec_tokens() (libgdp/src/video_codec.cpp), and auto.
pub const CODECS: &[&str] = &["", "pyrowave", "av1", "h265", "h264"];
pub const NETWORK_PROFILES: &[&str] = &["auto", "lan", "internet", "mobile"];

impl Profile {
    pub fn validate(&self) -> Result<()> {
        if !self.resolution.is_empty() {
            let ok = self.resolution.split_once('x').is_some_and(|(w, h)| {
                let dim = |s: &str| s.parse::<u32>().is_ok_and(|n| (320..=16384).contains(&n));
                dim(w) && dim(h)
            });
            ensure!(ok, "resolution must be WIDTHxHEIGHT, e.g. 1920x1080");
        }
        ensure!(VIEWS.contains(&self.view.as_str()), "unknown view {:?}", self.view);
        ensure!(DECODERS.contains(&self.preferred_decoder.as_str()), "unknown decoder {:?}", self.preferred_decoder);
        ensure!(CODECS.contains(&self.preferred_codec.as_str()), "unknown codec {:?}", self.preferred_codec);
        ensure!(self.display_sleep_minutes <= 1440, "display sleep must be at most 1440 minutes (a day)");
        ensure!(
            NETWORK_PROFILES.contains(&self.network_profile.as_str()),
            "unknown network profile {:?}",
            self.network_profile
        );
        Ok(())
    }

    fn to_proto(&self) -> ClientProfile {
        ClientProfile {
            resolution: self.resolution.clone(),
            view: self.view.clone(),
            preferred_decoder: self.preferred_decoder.clone(),
            preferred_codec: self.preferred_codec.clone(),
            lossless_refinement: self.lossless_refinement,
            allow_pyrowave: self.allow_pyrowave,
            network_profile: self.network_profile.clone(),
            forward_gamepads: self.forward_gamepads,
            microphone: self.microphone,
            debug_logging: self.debug_logging,
            display_sleep_minutes: self.display_sleep_minutes,
        }
    }
}

/// What a client said about itself in its last hello, kept as JSON in
/// `thin_clients.last_report`.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Report {
    pub hostname: String,
    pub ips: Vec<String>,
    /// The address Veil saw the connection come from.
    pub address: String,
    pub arch: String,
    pub image_version: String,
    pub boot_time_unix: i64,
    pub cpu_model: String,
    pub cpu_cores: u32,
    pub memory_bytes: u64,
    pub gpus: Vec<Gpu>,
    pub hw_decode: Vec<String>,
    pub displays: Vec<Display>,
    pub nic_speed_mbps: u32,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Gpu {
    pub name: String,
    pub driver: String,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Display {
    pub connector: String,
    pub width: u32,
    pub height: u32,
    pub refresh_mhz: u32,
}

impl Report {
    fn from_hello(hello: &WispHello, address: IpAddr) -> Report {
        let system = hello.system.clone().unwrap_or_default();
        Report {
            hostname: hello.hostname.clone(),
            ips: hello.ips.clone(),
            address: address.to_string(),
            arch: hello.arch.clone(),
            image_version: hello.image_version.clone(),
            // From the uptime, on Veil's clock: the client's may be hours off.
            boot_time_unix: if hello.uptime_secs > 0 { ipc::unix_now() - hello.uptime_secs as i64 } else { 0 },
            cpu_model: system.cpu_model,
            cpu_cores: system.cpu_cores,
            memory_bytes: system.memory_bytes,
            gpus: system.gpus.into_iter().map(|g| Gpu { name: g.name, driver: g.driver }).collect(),
            hw_decode: system.hw_decode,
            displays: system
                .displays
                .into_iter()
                .map(|d| Display { connector: d.connector, width: d.width, height: d.height, refresh_mhz: d.refresh_mhz })
                .collect(),
            nic_speed_mbps: system.nic_speed_mbps,
        }
    }
}

/// A session the client is running.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Session {
    pub user: String,
    pub host_name: String,
    pub started_at: i64,
}

impl Session {
    /// Dated now, on Veil's clock: the client's may be hours off.
    fn from_proto(s: &ipc::wisp::WispSession) -> Option<Session> {
        (!s.user.is_empty() || !s.host_name.is_empty()).then(|| Session {
            user: s.user.clone(),
            host_name: s.host_name.clone(),
            started_at: ipc::unix_now(),
        })
    }
}

struct Live {
    generation: u64,
    conn: quinn::Connection,
    session: Option<Session>,
    /// To the connection's push task.
    commands: mpsc::UnboundedSender<WispAction>,
}

/// One client as the admin UI shows it.
#[derive(Clone, Debug)]
pub struct ClientView {
    pub row: ThinClientRow,
    pub report: Report,
    pub online: bool,
    pub session: Option<Session>,
}

pub struct ThinClients {
    db: Arc<Db>,
    /// The shared key ([thin_clients] key); None: none configured, and
    /// every client is refused.
    key: Option<String>,
    online: Mutex<HashMap<String, Live>>,
    profile: watch::Sender<Profile>,
    generations: AtomicU64,
}

impl ThinClients {
    pub fn new(db: Arc<Db>, key: Option<String>) -> ThinClients {
        let profile = match db.setting(PROFILE_KEY) {
            Ok(Some(json)) => serde_json::from_str(&json).unwrap_or_else(|e| {
                warn!(error = %e, "thin clients: the stored profile doesn't parse; using the defaults");
                Profile::default()
            }),
            Ok(None) => Profile::default(),
            Err(e) => {
                warn!(error = %e, "thin clients: reading the profile failed; using the defaults");
                Profile::default()
            }
        };
        ThinClients {
            db,
            key,
            online: Mutex::new(HashMap::new()),
            profile: watch::Sender::new(profile),
            generations: AtomicU64::new(1),
        }
    }

    fn online(&self) -> std::sync::MutexGuard<'_, HashMap<String, Live>> {
        self.online.lock().unwrap_or_else(|p| p.into_inner())
    }

    pub fn profile(&self) -> Profile {
        self.profile.borrow().clone()
    }

    /// Stores the profile and pushes it to every connected client.
    pub fn set_profile(&self, profile: Profile) -> Result<()> {
        profile.validate()?;
        self.db.set_setting(PROFILE_KEY, &serde_json::to_string(&profile)?)?;
        self.profile.send_replace(profile);
        Ok(())
    }

    pub fn is_online(&self, mac: &str) -> bool {
        self.online().contains_key(mac)
    }

    /// Every client Veil has heard from, online ones first, then by name.
    pub fn list(&self) -> Result<Vec<ClientView>> {
        let rows = self.db.thin_clients()?;
        let mut views: Vec<ClientView> = rows.into_iter().map(|row| self.view(row)).collect();
        views.sort_by(|a, b| {
            b.online.cmp(&a.online).then_with(|| a.display_name().to_lowercase().cmp(&b.display_name().to_lowercase()))
        });
        Ok(views)
    }

    pub fn get(&self, mac: &str) -> Result<Option<ClientView>> {
        Ok(self.db.thin_client(mac)?.map(|row| self.view(row)))
    }

    fn view(&self, row: ThinClientRow) -> ClientView {
        let report = serde_json::from_str(&row.last_report).unwrap_or_default();
        let (online, session) = match self.online().get(&row.mac) {
            Some(live) => (true, live.session.clone()),
            None => (false, None),
        };
        ClientView { row, report, online, session }
    }

    /// Drops a client's row. Refused while it is online: its next report
    /// would only bring it back.
    pub fn remove(&self, mac: &str) -> Result<bool> {
        if self.is_online(mac) {
            bail!("{mac} is online");
        }
        self.db.remove_thin_client(mac)
    }

    /// Sends an administrator's action to a connected client. False when
    /// it is offline. Fire and forget: the client's session report or its
    /// connection dropping is the answer.
    pub fn command(&self, mac: &str, action: WispAction) -> bool {
        self.online().get(mac).is_some_and(|live| live.commands.send(action).is_ok())
    }

    fn went_offline(&self, mac: &str, generation: u64) {
        let mut online = self.online();
        if online.get(mac).is_some_and(|l| l.generation == generation) {
            online.remove(mac);
            drop(online);
            let _ = self.db.touch_thin_client(mac);
            info!(mac, "thin clients: offline");
        }
    }

    fn set_session(&self, mac: &str, generation: u64, session: Option<Session>) {
        if let Some(live) = self.online().get_mut(mac).filter(|l| l.generation == generation) {
            live.session = session;
        }
    }
}

impl ClientView {
    /// The admin-given name, or the client's own hostname.
    pub fn display_name(&self) -> &str {
        if !self.row.name.is_empty() {
            &self.row.name
        } else if !self.report.hostname.is_empty() {
            &self.report.hostname
        } else {
            &self.row.mac
        }
    }
}

/// Reads [thin_clients] key: 64 hex digits, as `make install-veil` writes
/// them.
pub fn load_key(path: &Path) -> Result<String> {
    let text = std::fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
    let key = text.trim().to_ascii_lowercase();
    ensure!(
        key.len() == 64 && key.bytes().all(|b| b.is_ascii_hexdigit()),
        "{} must hold 64 hex digits",
        path.display()
    );
    Ok(key)
}

/// "52:54:00:12:34:56", lowercase; None for anything else.
fn normalize_mac(mac: &str) -> Option<String> {
    let mac = mac.trim().to_ascii_lowercase();
    let parts: Vec<&str> = mac.split(':').collect();
    let ok = parts.len() == 6 && parts.iter().all(|p| p.len() == 2 && p.bytes().all(|b| b.is_ascii_hexdigit()));
    ok.then_some(mac)
}

/// Runs one `wisp/1` connection to its end. `slot` is the connection's
/// place under [auth] max_startups, released as soon as the key checks
/// out: an agent stays connected for as long as its client is up, and a
/// few of them holding slots would refuse every user's sign-in.
pub async fn serve(conn: quinn::Connection, veil: Arc<Veil>, slot: StartupSlot) {
    let peer = gdpnet::canonical(conn.remote_address());
    let code = match serve_inner(&conn, &veil, peer, slot).await {
        Ok(()) => 0,
        Err(e) => {
            let code = match e.downcast_ref::<Closed>() {
                Some(Closed(code)) => *code as u32,
                None => e.downcast_ref::<FrameError>().and_then(FrameError::close_code).unwrap_or(0),
            };
            if code == 0 {
                debug!(%peer, error = %format!("{e:#}"), "thin clients: connection ended");
            } else {
                warn!(%peer, error = %format!("{e:#}"), "thin clients: connection refused");
            }
            code
        }
    };
    conn.close(VarInt::from_u32(code), b"");
}

/// Ends the connection with this gdp-spec.md §12 code.
#[derive(Debug)]
struct Closed(LobbyErrorCode);

impl std::fmt::Display for Closed {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "closing with {}", self.0.as_str_name())
    }
}

impl std::error::Error for Closed {}

async fn serve_inner(conn: &quinn::Connection, veil: &Veil, peer: SocketAddr, slot: StartupSlot) -> Result<()> {
    let thin = &veil.thin_clients;
    // The same deadline as a login's, from the handshake.
    let grace = veil.config.auth.login_grace_time;
    let deadline = Instant::now() + if grace.is_zero() { Duration::from_secs(120) } else { grace };
    let first = async {
        let (send, mut recv) = conn.accept_bi().await?;
        let hello: WispEnvelope = read_frame_max(&mut recv, MAX_FRAME).await?;
        Ok::<_, anyhow::Error>((send, recv, hello))
    };
    let (mut send, mut recv, hello) = match timeout_at(deadline, first).await {
        Ok(Ok(first)) => first,
        Ok(Err(e)) => {
            veil.penalties.penalise(peer.ip(), Offense::NoAuth);
            return Err(e.context("reading the hello"));
        }
        Err(_) => {
            veil.penalties.penalise(peer.ip(), Offense::GraceExceeded);
            bail!(Closed(LobbyErrorCode::LobbyErrorAuthFailed));
        }
    };
    let Some(Msg::Hello(hello)) = hello.msg else {
        veil.penalties.penalise(peer.ip(), Offense::NoAuth);
        return Err(anyhow::anyhow!(Closed(LobbyErrorCode::LobbyErrorMalformedFrame)).context("expected WispHello"));
    };
    // A wrong key costs the address nothing: after a key rotation, every
    // client still on the old one would otherwise lock its own greeter's
    // sign-ins out too. The agent's backoff paces its retries, and the key
    // is too long to guess.
    let Some(key) = &thin.key else {
        return Err(anyhow::anyhow!(Closed(LobbyErrorCode::LobbyErrorAuthFailed))
            .context("no [thin_clients] key configured, so no thin client can connect"));
    };
    if !constant_time_eq(hello.key.trim().to_ascii_lowercase().as_bytes(), key.as_bytes()) {
        return Err(anyhow::anyhow!(Closed(LobbyErrorCode::LobbyErrorAuthFailed))
            .context(format!("wrong Wisp key from {} ({})", hello.mac, hello.hostname)));
    }
    drop(slot);
    let Some(mac) = normalize_mac(&hello.mac) else {
        return Err(anyhow::anyhow!(Closed(LobbyErrorCode::LobbyErrorMalformedFrame))
            .context(format!("bad MAC address {:?}", hello.mac)));
    };

    let report = Report::from_hello(&hello, peer.ip());
    thin.db.thin_client_seen(&mac, &serde_json::to_string(&report)?)?;
    let mut profile = thin.profile.subscribe();
    let welcome = WispWelcome { profile: Some(profile.borrow_and_update().to_proto()) };
    write_frame(&mut send, &WispEnvelope { msg: Some(Msg::Welcome(welcome)) }).await?;

    let generation = thin.generations.fetch_add(1, Ordering::Relaxed);
    let session = hello.session.as_ref().and_then(Session::from_proto);
    let (commands, mut command_rx) = mpsc::unbounded_channel();
    let live = Live { generation, conn: conn.clone(), session, commands };
    if let Some(old) = thin.online().insert(mac.clone(), live) {
        old.conn.close(VarInt::from_u32(0), b"replaced by a newer connection");
    }
    info!(%peer, mac, hostname = %hello.hostname, image = %hello.image_version, "thin clients: online");

    // Profile pushes and commands go out from their own task, so the read
    // below is never cancelled halfway through a frame.
    let pusher = tokio::spawn(async move {
        loop {
            let msg = tokio::select! {
                changed = profile.changed() => {
                    if changed.is_err() {
                        break;
                    }
                    Msg::Profile(WispProfile { profile: Some(profile.borrow_and_update().to_proto()) })
                }
                Some(action) = command_rx.recv() => Msg::Command(WispCommand { action: action as i32 }),
            };
            if write_frame(&mut send, &WispEnvelope { msg: Some(msg) }).await.is_err() {
                break;
            }
        }
    });
    let result = async {
        loop {
            let env: WispEnvelope = match read_frame_max(&mut recv, MAX_FRAME).await {
                Ok(env) => env,
                // The agent going away (a reboot, a shutdown) ends its
                // stream or connection.
                Err(FrameError::Io(_)) => return Ok(()),
                Err(e) => return Err(anyhow::Error::from(e)),
            };
            match env.msg {
                Some(Msg::Session(s)) => {
                    let session = Session::from_proto(&s);
                    match &session {
                        Some(s) => info!(mac, user = %s.user, host = %s.host_name, "thin clients: session started"),
                        None => info!(mac, "thin clients: session ended"),
                    }
                    thin.set_session(&mac, generation, session);
                }
                other => bail!("unexpected message from a thin client: {other:?}"),
            }
        }
    }
    .await;
    pusher.abort();
    thin.went_offline(&mac, generation);
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn macs_are_normalized_or_refused() {
        assert_eq!(normalize_mac(" 52:54:00:AB:cd:0F ").as_deref(), Some("52:54:00:ab:cd:0f"));
        assert_eq!(normalize_mac("525400abcd0f"), None);
        assert_eq!(normalize_mac("52:54:00:ab:cd"), None);
        assert_eq!(normalize_mac("52:54:00:ab:cd:zz"), None);
    }

    #[test]
    fn profiles_are_checked() {
        Profile::default().validate().unwrap();
        let ok = Profile { resolution: "1920x1080".into(), view: "actual".into(), preferred_codec: "av1".into(), ..Default::default() };
        ok.validate().unwrap();
        for bad in [
            Profile { resolution: "1920".into(), ..Default::default() },
            Profile { resolution: "1x1".into(), ..Default::default() },
            Profile { view: "zoom".into(), ..Default::default() },
            Profile { preferred_decoder: "vaapi".into(), ..Default::default() },
            Profile { preferred_codec: "vp9".into(), ..Default::default() },
            Profile { network_profile: "".into(), ..Default::default() },
            Profile { display_sleep_minutes: 1441, ..Default::default() },
        ] {
            assert!(bad.validate().is_err(), "{bad:?}");
        }
    }

    #[test]
    fn the_profile_survives_a_restart_and_reaches_subscribers() {
        let db = Arc::new(Db::in_memory());
        let thin = ThinClients::new(db.clone(), None);
        assert_eq!(thin.profile(), Profile::default());
        let mut rx = thin.profile.subscribe();
        let changed = Profile { lossless_refinement: false, network_profile: "lan".into(), ..Default::default() };
        thin.set_profile(changed.clone()).unwrap();
        assert!(rx.has_changed().unwrap());
        assert_eq!(*rx.borrow_and_update(), changed);
        assert!(thin.set_profile(Profile { view: "nope".into(), ..Default::default() }).is_err());
        assert_eq!(ThinClients::new(db, None).profile(), changed);
    }

    #[test]
    fn keys_must_be_64_hex_digits() {
        let dir = std::env::temp_dir().join(format!("veil-wisp-key-{}", std::process::id()));
        std::fs::write(&dir, format!("{}\n", "AB".repeat(32))).unwrap();
        assert_eq!(load_key(&dir).unwrap(), "ab".repeat(32));
        std::fs::write(&dir, "abc").unwrap();
        assert!(load_key(&dir).is_err());
        std::fs::remove_file(&dir).unwrap();
    }
}
