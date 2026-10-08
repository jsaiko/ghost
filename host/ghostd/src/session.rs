// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Session lifecycle, ghostd's side (docs/design/login-and-sessions.md):
// after a lobby login, find the user's open session or ask a new
// ghostseat instance to open one (host/proto/ghostseat.proto). ghostd
// runs unprivileged and holds no session state that matters: each
// session's ghostseat answers STATUS on /run/ghost/<uid>-seat.sock with
// everything a reattach needs, and reports what happens on
// /run/ghost/events.sock. What ghostd keeps is the per-uid lock, the
// console-login hold, a memory of viewers and last redirects for Veil and
// the lobby, and each user's last session type on disk.
use std::collections::{HashMap, HashSet};
use std::net::IpAddr;
use std::os::unix::fs::{DirBuilderExt, FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::{anyhow, bail, Context, Result};
use ipc::broker::{
    host_envelope::Msg as HostEvent, LocalLoginTakeover, RunningSession, SessionEnded, SessionStarted, ViewerAttached,
    ViewerDetached,
};
use ipc::framing::{frame_bytes, read_frame, secret_frame_bytes};
use ipc::ghostseat::{
    event, ghostseat_envelope::Msg as SeatMsg, CloseRequest, GhostseatEnvelope, MintTokenRequest, Open, StatusReply,
};
use ipc::lobby::LobbyErrorCode;
use ipc::paths::{seat_socket, seat_socket_uid, EVENTS_SOCKET, RUN_DIR, SEAT_SOCKET};
use ipc::unix_now as now;
use pamconv::Authtok;
use tokio::net::{UnixListener, UnixStream};
use tokio::sync::Mutex;
use tokio::io::AsyncWriteExt;
use tokio::time::timeout;
use tracing::{info, warn};
use zeroize::Zeroize;

use crate::local_login::{self, LocalSessionActive};

// ghostseat's own deadlines: 30 s for pam_open_session, then the user
// manager and wraith's handshake (5 s + 15 s); this is their sum plus
// slack, so a slow start is reported rather than raced.
const OPEN_TIMEOUT: Duration = Duration::from_secs(60);
// A STATUS answer comes straight from memory.
const STATUS_TIMEOUT: Duration = Duration::from_secs(5);
// CLOSE with logout: wraith's 10 s to log out, 5 s to stop, then
// pam_close_session.
const CLOSE_TIMEOUT: Duration = Duration::from_secs(30);
// How long the events listener waits for a connected ghostseat's frame.
const EVENT_READ_TIMEOUT: Duration = Duration::from_secs(5);
// After a console login ends a ghost session, the lobby refuses that uid
// for this long even before logind lists the new local session -- the
// display manager only opens it once the PAM account hook has returned.
// Not set when the uid already had a local session (a lock-screen unlock
// goes through the same hook), and dropped once logind lists one: from
// then on that session itself is what refuses, and its logout frees the
// uid at once.
const LOCAL_LOGIN_HOLD: Duration = Duration::from_secs(30);

/// Checks `RUN_DIR`, which tmpfiles.d creates root-owned with the sticky
/// bit and group `ghost` (packaging/system/tmpfiles.d/ghost.conf): it
/// holds every socket, ghostd creates its own there but can't unlink
/// ghostseat's, each session user needs search permission to reach their
/// own, and nobody else may list it. A dev run as a user with no such
/// directory gets one of its own, 0711.
pub fn ensure_run_dir() -> Result<()> {
    let dir = Path::new(RUN_DIR);
    match std::fs::metadata(dir) {
        Ok(meta) if meta.uid() == 0 => {
            if meta.mode() & 0o1000 == 0 {
                bail!("{} is root's without the sticky bit; ghostd could unlink ghostseat's sockets there (tmpfiles.d/ghost.conf makes it 1771)", dir.display());
            }
            nix::unistd::access(dir, nix::unistd::AccessFlags::W_OK)
                .with_context(|| format!("{} is not writable by ghostd (is it in the ghost group?)", dir.display()))?;
            Ok(())
        }
        Ok(_) | Err(_) => ensure_dir(dir, 0o711),
    }
}

/// ghostd's state that outlives a reboot: the last session types.
pub const STATE_DIR: &str = "/var/lib/ghost";

/// `STATE_DIR`, mode 0700 (StateDirectory= in the unit).
pub fn ensure_state_dir() -> Result<()> {
    ensure_dir(Path::new(STATE_DIR), 0o700)
}

fn ensure_dir(dir: &Path, mode: u32) -> Result<()> {
    std::fs::DirBuilder::new()
        .recursive(true)
        .mode(mode)
        .create(dir)
        .with_context(|| format!("creating {} (the unit's RuntimeDirectory/StateDirectory does this)", dir.display()))?;
    let meta = std::fs::metadata(dir).with_context(|| format!("reading {}", dir.display()))?;
    if meta.uid() != nix::unistd::geteuid().as_raw() {
        bail!("{} is owned by uid {}, not by ghostd (uid {})", dir.display(), meta.uid(), nix::unistd::geteuid());
    }
    std::fs::set_permissions(dir, std::fs::Permissions::from_mode(mode))
        .with_context(|| format!("chmod {}", dir.display()))
}

/// What `ghostd -t` checks: the socket a login needs ghostseat on.
/// Connecting would start an instance for nothing, so this only looks.
pub fn check_seat_socket() -> Result<()> {
    match std::fs::metadata(SEAT_SOCKET) {
        Ok(meta) if meta.file_type().is_socket() => Ok(()),
        Ok(_) => bail!("{SEAT_SOCKET} is not a socket"),
        Err(e) => Err(e).with_context(|| format!("{SEAT_SOCKET} (is ghostseat.socket enabled?)")),
    }
}

/// What a login's Redirect carries: ghostseat's answer to an Open or a
/// MintTokenRequest. ghostd never holds the session secret the token was
/// minted with.
pub struct SessionHandle {
    pub port: u16,
    /// wraith's GDP certificate fingerprint, relayed in
    /// Redirect.cert_sha256 (gdp-spec.md §2.3).
    pub cert_sha256: String,
    /// The redirect token (gdp-spec.md §4.8) and when it expires.
    pub token: String,
    pub token_expiry_unix: i64,
}

/// A user's open session, as its ghostseat describes it, plus ghostd's
/// memory of the last redirect to it.
#[derive(Debug, Clone)]
pub struct SessionEntry {
    pub uid: u32,
    pub started_at: i64,
    /// When a client was last redirected to this session.
    pub last_seen: i64,
    pub session_type: String,
    /// A client is viewing it now, as ghostseat's STATUS says.
    pub viewer_attached: bool,
}

/// The inclusive UDP port range wraith scans for its GDP listener:
/// `sessions.port_base` through base + `sessions.max` - 1, one port per
/// concurrent session. ghostd only hands it over in Open; wraith takes
/// the first free port.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PortRange {
    pub start: u16,
    pub end: u16,
}

impl PortRange {
    /// Checks the range against what wraith can bind and against the
    /// lobby's own port, so a bad ghostd.toml fails at startup (and
    /// `ghostd -t`) rather than at someone's login.
    pub fn new(base: u16, max_sessions: u16, lobby_port: u16) -> Result<PortRange> {
        if max_sessions == 0 {
            bail!("sessions.max must be at least 1");
        }
        // wraith runs as the session's user, which can't bind below 1024.
        if base < 1024 {
            bail!("sessions.port_base {base} is a privileged port; wraith can't bind below 1024");
        }
        let end = u32::from(base) + u32::from(max_sessions) - 1;
        let Ok(end) = u16::try_from(end) else {
            bail!("sessions.port_base {base} + sessions.max {max_sessions} runs past port 65535");
        };
        if (base..=end).contains(&lobby_port) {
            bail!("session port range {base}-{end} includes lobby.port {lobby_port}");
        }
        Ok(PortRange { start: base, end })
    }

    pub fn max_sessions(&self) -> u16 {
        self.end - self.start + 1
    }
}

/// A startup failure wraith reported with an error code of its own
/// (ControlError.code, relayed in GhostseatError.code), which the lobby
/// passes on to spectre in place of the generic SESSION_START_FAILED.
#[derive(Debug)]
pub struct WraithStartupError {
    pub code: LobbyErrorCode,
    pub message: String,
}

impl std::fmt::Display for WraithStartupError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "wraith reported a startup error ({}): {}", self.code.as_str_name(), self.message)
    }
}

impl std::error::Error for WraithStartupError {}

/// Per-uid serialization of `ensure_session`, plus what ghostd remembers
/// between STATUS answers. Opening a session takes seconds (ghostseat's
/// logind session, the unit start, the control-socket handshake), during
/// which no <uid>-seat.sock answers yet -- two lobby connections for the
/// same account arriving in that window would otherwise both ask for a
/// new session. Serializing per uid means the second login simply waits
/// and takes the reuse path. Different uids never contend.
pub struct SessionManager {
    port_range: PortRange,
    last_types: std::sync::Mutex<LastTypes>,
    uid_locks: std::sync::Mutex<HashMap<u32, Arc<Mutex<()>>>>,
    // uid -> when end_for_local_login() last ended its session for a
    // console login; see LOCAL_LOGIN_HOLD.
    local_logins: std::sync::Mutex<HashMap<u32, Instant>>,
    // uids with a session ghostd knows to be live, so a SessionEnded is
    // news to Veil exactly once; and the last redirect to each.
    live: std::sync::Mutex<HashSet<u32>>,
    last_seen: std::sync::Mutex<HashMap<u32, i64>>,
    // uids whose wraith has a viewer attached, from ghostseat's events.
    viewers: std::sync::Mutex<HashSet<u32>>,
    // Session events for Veil's host channel (broker.rs), when this host
    // is joined to one. Sent whether or not anything listens.
    events: tokio::sync::broadcast::Sender<HostEvent>,
}

/// Each uid's last session type, under STATE_DIR so the lobby's
/// preselection survives a reboot, which no session does.
struct LastTypes {
    path: PathBuf,
    types: HashMap<u32, String>,
}

impl LastTypes {
    fn load(path: PathBuf) -> LastTypes {
        let types = std::fs::read(&path).ok().and_then(|bytes| serde_json::from_slice(&bytes).ok()).unwrap_or_default();
        LastTypes { path, types }
    }

    /// A failure to save is only logged: it costs a preselection.
    fn record(&mut self, uid: u32, session_type: &str) {
        if self.types.get(&uid).map(String::as_str) == Some(session_type) {
            return;
        }
        self.types.insert(uid, session_type.to_string());
        if let Err(e) = write_json(&self.path, &self.types) {
            warn!(uid, error = %e, "session: failed to save the last session type");
        }
    }
}

/// Write-then-rename so a crash mid-write never leaves the file
/// truncated for the next startup to choke on; mode 0600 from creation.
fn write_json<T: serde::Serialize>(path: &Path, value: &T) -> Result<()> {
    use std::io::Write;
    let tmp_path = path.with_extension("json.tmp");
    let body = serde_json::to_vec_pretty(value)?;
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .mode(0o600)
        .open(&tmp_path)
        .with_context(|| format!("creating {}", tmp_path.display()))?;
    file.write_all(&body).with_context(|| format!("writing {}", tmp_path.display()))?;
    file.sync_all().with_context(|| format!("syncing {}", tmp_path.display()))?;
    drop(file);
    std::fs::rename(&tmp_path, path).with_context(|| format!("renaming {} -> {}", tmp_path.display(), path.display()))
}

impl SessionManager {
    pub fn new(port_range: PortRange, last_types_path: PathBuf) -> SessionManager {
        SessionManager {
            port_range,
            last_types: std::sync::Mutex::new(LastTypes::load(last_types_path)),
            uid_locks: Default::default(),
            local_logins: Default::default(),
            live: Default::default(),
            last_seen: Default::default(),
            viewers: Default::default(),
            events: tokio::sync::broadcast::channel(256).0,
        }
    }

    /// Session events as they happen, for Veil's host channel.
    pub fn subscribe(&self) -> tokio::sync::broadcast::Receiver<HostEvent> {
        self.events.subscribe()
    }

    fn emit(&self, event: HostEvent) {
        let _ = self.events.send(event);
    }

    fn lock<T>(m: &std::sync::Mutex<T>) -> std::sync::MutexGuard<'_, T> {
        m.lock().unwrap_or_else(|p| p.into_inner())
    }

    /// Every open session on the host, from each ghostseat's STATUS, for
    /// the Snapshot Veil gets after each (re)connect.
    pub async fn snapshot(&self) -> Vec<RunningSession> {
        let mut sessions = Vec::new();
        for uid in seat_sockets() {
            if let ProbeOutcome::Live(reply) = probe_seat(uid).await {
                Self::lock(&self.live).insert(uid);
                sessions.push(RunningSession {
                    uid,
                    username: reply.username,
                    session_type: reply.session_type,
                    started_at_unix: reply.opened_at,
                    viewer_attached: reply.viewer_attached,
                });
            }
        }
        sessions
    }

    pub fn port_range(&self) -> PortRange {
        self.port_range
    }

    fn uid_lock(&self, uid: u32) -> Arc<Mutex<()>> {
        // Locks are never removed: the map is bounded by the number of
        // distinct accounts that ever log in, one Arc<Mutex<()>> each.
        Self::lock(&self.uid_locks).entry(uid).or_default().clone()
    }

    fn entry_from(&self, uid: u32, reply: StatusReply) -> SessionEntry {
        let last_seen = Self::lock(&self.last_seen).get(&uid).copied().unwrap_or(reply.opened_at);
        SessionEntry {
            uid,
            started_at: reply.opened_at,
            last_seen,
            session_type: reply.session_type,
            viewer_attached: reply.viewer_attached,
        }
    }

    /// Finds `uid`'s open session or opens one: `ticket` is ghostauth's
    /// proof the login happened, which ghostseat requires either way and
    /// spends for the one redirect token it answers with. `authtok` only
    /// matters when this call opens the session: it goes to ghostseat for
    /// the keyring unlock. A login that reattaches to a live session
    /// unlocks nothing -- the keyring is already as unlocked as the
    /// session left it.
    pub async fn ensure_session(
        self: Arc<Self>,
        uid: u32,
        username: &str,
        session_type: &str,
        client_ip: IpAddr,
        authtok: Option<Authtok>,
        ticket: String,
    ) -> Result<SessionHandle> {
        let lock = self.uid_lock(uid);
        let _in_flight = lock.lock().await;

        // Again here, under the uid lock, though the lobby already checked
        // right after authentication: a console login may have ended this
        // uid's session in between (end_for_local_login takes this lock).
        self.check_no_local_session(uid).await?;

        match probe_seat(uid).await {
            ProbeOutcome::Live(reply) => {
                // One session per uid: a running session is reused
                // whatever type was requested; switching desktops means
                // ending the session first.
                if reply.session_type != session_type {
                    info!(uid, port = reply.port, requested = session_type, running = %reply.session_type,
                        "session: requested type differs from the running session; reusing anyway");
                } else {
                    info!(uid, port = reply.port, "session: reusing the running session");
                }
                let minted = mint_token(uid, username, client_ip, ticket).await?;
                Self::lock(&self.last_seen).insert(uid, now());
                Self::lock(&self.live).insert(uid);
                return Ok(SessionHandle {
                    port: reply.port as u16,
                    cert_sha256: reply.cert_sha256,
                    token: minted.token,
                    token_expiry_unix: minted.expiry_unix,
                });
            }
            ProbeOutcome::NoLeader => {}
            ProbeOutcome::Error(e) => return Err(e).context("checking for an open session"),
        }

        let open = Open {
            uid,
            username: username.to_string(),
            client_ip: client_ip.to_string(),
            ticket,
            // The one unwiped copy of the password, inside the message
            // open_seat wipes once it is encoded.
            authtok: authtok.map(|a| a.as_bytes().to_vec()).unwrap_or_default(),
            session_type: session_type.to_string(),
            port_range_start: self.port_range.start as u32,
            port_range_end: self.port_range.end as u32,
        };
        let opened = open_seat(open).await?;
        info!(uid, username, session_id = %opened.session_id, port = opened.port, "session: opened");
        Self::lock(&self.last_types).record(uid, session_type);
        Self::lock(&self.last_seen).insert(uid, now());
        Self::lock(&self.live).insert(uid);
        self.emit(HostEvent::SessionStarted(SessionStarted {
            uid,
            username: username.to_string(),
            session_type: session_type.to_string(),
            started_at_unix: now(),
        }));
        Ok(SessionHandle {
            port: opened.port as u16,
            cert_sha256: opened.cert_sha256,
            token: opened.token,
            token_expiry_unix: opened.token_expiry_unix,
        })
    }

    /// Err(LocalSessionActive) when `uid` is logged in at the host itself
    /// (docs/design/login-and-sessions.md#one-graphical-login-per-user):
    /// a graphical session on a seat, or a console login that just ended
    /// its ghost session and whose own session logind may not list yet.
    /// A failed logind query lets the login through: it is a policy
    /// check, not a security boundary.
    pub async fn check_no_local_session(&self, uid: u32) -> Result<()> {
        let recent = Self::lock(&self.local_logins).get(&uid).is_some_and(|at| at.elapsed() < LOCAL_LOGIN_HOLD);
        match tokio::task::spawn_blocking(move || local_login::local_session(uid)).await {
            Ok(Ok(Some(s))) => {
                Self::lock(&self.local_logins).remove(&uid);
                Err(LocalSessionActive { detail: format!("logind session {} on {}", s.id, s.seat) }.into())
            }
            Ok(Ok(None)) if recent => {
                Err(LocalSessionActive { detail: "a console login is in progress".to_string() }.into())
            }
            Ok(Ok(None)) => Ok(()),
            Ok(Err(e)) if !recent => {
                warn!(uid, error = %format!("{e:#}"), "session: couldn't check for a local session; allowing the login");
                Ok(())
            }
            Err(e) if !recent => {
                warn!(uid, error = %e, "session: local-session check panicked; allowing the login");
                Ok(())
            }
            _ => Err(LocalSessionActive { detail: "a console login is in progress".to_string() }.into()),
        }
    }

    /// Ends `uid`'s ghost session because its user is logging in at the
    /// console, and returns once it is gone: ghostseat logs the desktop
    /// out (wraith's SIGUSR1, a stop if that takes too long) and closes
    /// the logind session before it answers. Only after that may the
    /// local desktop start -- the two sessions share the user's config
    /// files and keyring. Ok(false) when there was no session to end.
    pub async fn end_for_local_login(&self, uid: u32) -> Result<bool> {
        // Set first, so a GDP login racing this one is refused from now on,
        // including while this waits for the uid lock.
        Self::lock(&self.local_logins).insert(uid, Instant::now());
        // Already logged in locally: this is an unlock or a re-auth, not a
        // new desktop on its way, and that session refuses GDP logins itself.
        if let Ok(Ok(Some(_))) = tokio::task::spawn_blocking(move || local_login::local_session(uid)).await {
            Self::lock(&self.local_logins).remove(&uid);
        }

        let lock = self.uid_lock(uid);
        let _in_flight = lock.lock().await;

        let username = match probe_seat(uid).await {
            ProbeOutcome::Live(reply) => reply.username,
            ProbeOutcome::NoLeader => return Ok(false),
            ProbeOutcome::Error(e) => return Err(e).context("checking for an open session"),
        };
        self.emit(HostEvent::LocalLoginTakeover(LocalLoginTakeover { uid, username: username.clone() }));
        info!(uid, username, "session: console login, closing the ghost session");
        close_seat(uid, true).await?;
        self.forget(uid, &username, "console login");
        Ok(true)
    }

    /// The session is over as far as Veil and the lobby are concerned.
    fn forget(&self, uid: u32, username: &str, reason: &str) {
        let was_live = Self::lock(&self.live).remove(&uid);
        Self::lock(&self.viewers).remove(&uid);
        Self::lock(&self.last_seen).remove(&uid);
        if was_live {
            self.emit(HostEvent::SessionEnded(SessionEnded { uid, username: username.to_string(), reason: reason.to_string() }));
        }
        info!(uid, username, reason, "session: ended");
    }

    /// What the lobby's SessionList needs about `uid`: the session type
    /// it last ran (to preselect, whether or not that session is still
    /// up), and its open session if its ghostseat answers -- the same
    /// check ensure_session's reuse path makes, so what the lobby
    /// advertises matches what a SessionOpen will actually do.
    pub async fn lobby_state(&self, uid: u32, _username: &str) -> (Option<String>, Option<SessionEntry>) {
        let last_type = Self::lock(&self.last_types).types.get(&uid).cloned();
        let entry = match probe_seat(uid).await {
            ProbeOutcome::Live(reply) => {
                Self::lock(&self.live).insert(uid);
                Some(self.entry_from(uid, reply))
            }
            _ => None,
        };
        (last_type, entry)
    }

    /// Runs once at startup: every <uid>-seat.sock in RUN_DIR is a session
    /// that outlived the previous ghostd, or a stale file. This is the
    /// only polling in the design: one STATUS per socket file at startup.
    pub async fn reconcile_at_startup(&self) {
        for uid in seat_sockets() {
            match probe_seat(uid).await {
                ProbeOutcome::Live(reply) => {
                    info!(uid, username = %reply.username, session_id = %reply.session_id, "session: found open");
                    Self::lock(&self.live).insert(uid);
                    if reply.viewer_attached {
                        Self::lock(&self.viewers).insert(uid);
                    }
                }
                ProbeOutcome::NoLeader => {
                    // The file is root's and the directory sticky, so
                    // it stays; ghostseat unlinks it when it binds the
                    // next one, and every probe of it says "no leader".
                    info!(uid, "session: stale seat socket, no session behind it");
                }
                ProbeOutcome::Error(e) => warn!(uid, error = %format!("{e:#}"), "session: STATUS failed"),
            }
        }
    }

    /// Binds EVENTS_SOCKET (ghostd's own; ghostseat connects as root) and
    /// serves it for ghostd's lifetime.
    pub fn spawn_events_listener(self: Arc<Self>) -> Result<()> {
        let _ = std::fs::remove_file(EVENTS_SOCKET);
        let listener = UnixListener::bind(EVENTS_SOCKET).with_context(|| format!("binding {EVENTS_SOCKET}"))?;
        std::fs::set_permissions(EVENTS_SOCKET, std::fs::Permissions::from_mode(0o600))
            .with_context(|| format!("chmod {EVENTS_SOCKET}"))?;
        tokio::spawn(async move {
            loop {
                match listener.accept().await {
                    Ok((stream, _)) => {
                        let mgr = self.clone();
                        tokio::spawn(async move { mgr.handle_event(stream).await });
                    }
                    Err(e) => warn!(error = %e, "session: events accept failed"),
                }
            }
        });
        Ok(())
    }

    async fn handle_event(&self, mut stream: UnixStream) {
        // Only ghostseat, which runs as root, has anything to say here.
        match stream.peer_cred() {
            Ok(cred) if cred.uid() == 0 => {}
            Ok(cred) => {
                warn!(peer_uid = cred.uid(), "session: refusing an event from a non-root client");
                return;
            }
            Err(e) => {
                warn!(error = %e, "session: could not read an event client's credentials");
                return;
            }
        }
        let env: GhostseatEnvelope = match timeout(EVENT_READ_TIMEOUT, read_frame(&mut stream)).await {
            Ok(Ok(env)) => env,
            Ok(Err(e)) => {
                warn!(error = %e, "session: reading an event failed");
                return;
            }
            Err(_) => {
                warn!("session: event client sent nothing within {EVENT_READ_TIMEOUT:?}");
                return;
            }
        };
        let Some(SeatMsg::Event(ev)) = env.msg else {
            warn!("session: expected an Event");
            return;
        };
        let (uid, username) = (ev.uid, ev.username);
        match ev.kind {
            Some(event::Kind::ViewerAttached(_)) => self.set_viewer(uid, &username, true),
            Some(event::Kind::ViewerDetached(_)) => self.set_viewer(uid, &username, false),
            Some(event::Kind::SessionEnded(ended)) => {
                // Under the uid lock: a login for this uid mid-teardown
                // waits, then finds no seat and opens a new session.
                let lock = self.uid_lock(uid);
                let _in_flight = lock.lock().await;
                self.forget(uid, &username, &ended.reason);
            }
            None => warn!(uid, "session: empty event"),
        }
    }

    fn set_viewer(&self, uid: u32, username: &str, attached: bool) {
        let changed = {
            let mut viewers = Self::lock(&self.viewers);
            if attached { viewers.insert(uid) } else { viewers.remove(&uid) }
        };
        if changed {
            let username = username.to_string();
            self.emit(match attached {
                true => HostEvent::ViewerAttached(ViewerAttached { uid, username }),
                false => HostEvent::ViewerDetached(ViewerDetached { uid, username }),
            });
        }
    }
}

/// The uids with a seat socket file in RUN_DIR, live or stale.
fn seat_sockets() -> Vec<u32> {
    let Ok(entries) = std::fs::read_dir(RUN_DIR) else { return Vec::new() };
    let mut uids: Vec<u32> = entries
        .flatten()
        .filter_map(|e| seat_socket_uid(&e.file_name().to_string_lossy()))
        .collect();
    uids.sort_unstable();
    uids
}

enum ProbeOutcome {
    Live(StatusReply),
    /// `connect()` failed with `ENOENT`/`ECONNREFUSED`: nothing is
    /// listening, whether because no session is open or because a stale
    /// socket file outlived its ghostseat.
    NoLeader,
    Error(anyhow::Error),
}

/// A round-trip, not a bare `connect()`: ghostseat binds its seat socket
/// before its session is open but answers STATUS only afterwards, so a
/// `StatusReply` is proof of a live session with wraith up.
async fn probe_seat(uid: u32) -> ProbeOutcome {
    let stream = match UnixStream::connect(seat_socket(uid)).await {
        Ok(stream) => stream,
        Err(e) if is_no_listener(&e) => return ProbeOutcome::NoLeader,
        Err(e) => return ProbeOutcome::Error(e.into()),
    };
    match seat_request(stream, SeatMsg::Status(Default::default()), "STATUS", STATUS_TIMEOUT).await {
        Ok(SeatMsg::StatusReply(reply)) => ProbeOutcome::Live(reply),
        Ok(other) => ProbeOutcome::Error(anyhow!("expected StatusReply, got {other:?}")),
        Err(e) => ProbeOutcome::Error(e),
    }
}

/// CLOSE to `uid`'s ghostseat; `logout` first asks the desktop to log
/// out. Returns once the logind session is closed. A ghostseat already
/// gone is fine.
async fn close_seat(uid: u32, logout: bool) -> Result<()> {
    let stream = match UnixStream::connect(seat_socket(uid)).await {
        Ok(stream) => stream,
        Err(e) if is_no_listener(&e) => {
            info!(uid, "session: ghostseat already gone, nothing to close");
            return Ok(());
        }
        Err(e) => return Err(e).context("connecting to ghostseat to close it"),
    };
    match seat_request(stream, SeatMsg::Close(CloseRequest { logout }), "CLOSE", CLOSE_TIMEOUT).await? {
        SeatMsg::Closed(_) => Ok(()),
        other => bail!("expected CloseReply from ghostseat, got {other:?}"),
    }
}

/// MintTokenRequest to `uid`'s ghostseat, for a login that reattaches:
/// the ticket is spent there, as an Open's is.
async fn mint_token(uid: u32, username: &str, client_ip: IpAddr, ticket: String) -> Result<ipc::ghostseat::TokenMinted> {
    let stream = UnixStream::connect(seat_socket(uid)).await.context("connecting to ghostseat for a token")?;
    let request = MintTokenRequest { username: username.to_string(), client_ip: client_ip.to_string(), ticket };
    match seat_request(stream, SeatMsg::MintToken(request), "MintToken", STATUS_TIMEOUT).await? {
        SeatMsg::TokenMinted(minted) => Ok(minted),
        other => bail!("expected TokenMinted from ghostseat, got {other:?}"),
    }
}

/// Open on SEAT_SOCKET: systemd starts a ghostseat instance for the
/// connection, which answers Opened once wraith is up. The Open carries
/// the login password (for the wallet unlock), so it is framed into a
/// buffer that is wiped and the message's own copy is wiped once encoded,
/// as pamconv frames its answers to ghostauth.
async fn open_seat(open: Open) -> Result<ipc::ghostseat::Opened> {
    let mut env = GhostseatEnvelope { msg: Some(SeatMsg::Open(open)) };
    let frame = secret_frame_bytes(&env);
    if let Some(SeatMsg::Open(open)) = &mut env.msg {
        open.authtok.zeroize();
    }
    let frame = frame.context("framing Open")?;
    let stream = UnixStream::connect(SEAT_SOCKET)
        .await
        .with_context(|| format!("connecting to {SEAT_SOCKET} (is ghostseat.socket active?)"))?;
    match seat_round_trip(stream, &frame, "Open", OPEN_TIMEOUT).await? {
        SeatMsg::Opened(opened) => Ok(opened),
        other => bail!("expected Opened from ghostseat, got {other:?}"),
    }
}

fn is_no_listener(e: &std::io::Error) -> bool {
    matches!(e.kind(), std::io::ErrorKind::NotFound | std::io::ErrorKind::ConnectionRefused)
}

/// One request/reply round-trip on a fresh ghostseat connection, under
/// `limit`. A GhostseatError reply comes back as an Err, as a
/// WraithStartupError when it carries wraith's own code; any other reply
/// is the caller's to match.
async fn seat_request(stream: UnixStream, request: SeatMsg, what: &str, limit: Duration) -> Result<SeatMsg> {
    let frame = frame_bytes(&GhostseatEnvelope { msg: Some(request) }).with_context(|| format!("framing {what}"))?;
    seat_round_trip(stream, &frame, what, limit).await
}

/// seat_request() on an already framed request, for a message the caller
/// framed into a wiped buffer (open_seat).
async fn seat_round_trip(mut stream: UnixStream, frame: &[u8], what: &str, limit: Duration) -> Result<SeatMsg> {
    let round_trip = async {
        stream.write_all(frame).await.with_context(|| format!("sending {what}"))?;
        let env: GhostseatEnvelope =
            read_frame(&mut stream).await.with_context(|| format!("reading ghostseat's {what} reply"))?;
        match env.msg {
            Some(SeatMsg::Error(e)) => {
                match i32::try_from(e.code).ok().and_then(|c| LobbyErrorCode::try_from(c).ok()) {
                    Some(code) if code != LobbyErrorCode::LobbyErrorUnspecified => {
                        Err(WraithStartupError { code, message: e.message }.into())
                    }
                    _ => bail!("ghostseat reported an error: {}", e.message),
                }
            }
            Some(msg) => Ok(msg),
            None => bail!("empty reply to {what}"),
        }
    };
    timeout(limit, round_trip).await.with_context(|| format!("ghostseat did not answer {what} within {limit:?}"))?
}

pub fn resolve_uid(username: &str) -> Result<u32> {
    match nix::unistd::User::from_name(username).context("looking up username")? {
        Some(user) => Ok(user.uid.as_raw()),
        None => bail!("no such user: {username}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn port_range_defaults() {
        let r = PortRange::new(14400, 64, 4442).unwrap();
        assert_eq!(r, PortRange { start: 14400, end: 14463 });
        assert_eq!(r.max_sessions(), 64);
    }

    #[test]
    fn port_range_single_session() {
        let r = PortRange::new(14400, 1, 4442).unwrap();
        assert_eq!(r, PortRange { start: 14400, end: 14400 });
    }

    #[test]
    fn port_range_up_to_65535() {
        let r = PortRange::new(65535, 1, 4442).unwrap();
        assert_eq!(r, PortRange { start: 65535, end: 65535 });
    }

    #[test]
    fn port_range_rejects_bad_settings() {
        assert!(PortRange::new(14400, 0, 4442).is_err());
        assert!(PortRange::new(1023, 1, 4442).is_err());
        assert!(PortRange::new(65535, 2, 4442).is_err());
    }

    #[test]
    fn port_range_excludes_lobby_port() {
        assert!(PortRange::new(4400, 64, 4442).is_err());
        assert!(PortRange::new(4443, 64, 4442).is_ok());
    }

    #[test]
    fn seat_socket_names() {
        assert_eq!(seat_socket_uid("1001-seat.sock"), Some(1001));
        assert_eq!(seat_socket_uid("1001.sock"), None);
        assert_eq!(seat_socket_uid("seat.sock"), None);
    }

    #[test]
    fn last_types_persist() {
        let dir = std::env::temp_dir().join(format!("ghostd-last-types-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("last-types.json");
        let mut lt = LastTypes::load(path.clone());
        lt.record(1000, "plasma");
        let again = LastTypes::load(path);
        assert_eq!(again.types.get(&1000).map(String::as_str), Some("plasma"));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
