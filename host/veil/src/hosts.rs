// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The host channel, veild's side (docs/design/veil.md#the-host-channel,
// host/proto/broker.proto). Each joined ghostd keeps one QUIC connection
// to Veil (ALPN `gdp-host/1`, mutual TLS). Its first stream is the control
// stream: a Join (from `ghostd join`, before Veil has a pin for the host)
// or a HostHello, then the host's events. Veil opens one more bidi stream
// per login it hands to the host (lobby.rs).
use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{anyhow, bail, Context, Result};
use ipc::broker::{
    host_envelope::Msg, HostEnvelope, HostError, HostErrorCode, HostWelcome, JoinAccepted, Left, LoginStreamOpen,
    RunningSession,
};
use ipc::framing::{read_frame, read_frame_max, write_frame};
use quinn::VarInt;
use rand::RngCore;
use sha2::{Digest, Sha256};
use tokio::sync::{OwnedSemaphorePermit, Semaphore};
use tokio::time::timeout;
use tracing::{info, warn};

use crate::config::DeviceMode;
use crate::db::{Db, Placement, TokenError};

// A host has no reason to send a big first frame.
const HELLO_MAX_FRAME: u32 = 16 * 1024;
const HELLO_TIMEOUT: Duration = Duration::from_secs(10);

/// Logins one Veil account may have in flight at once, across every
/// host: each browser flow and each lobby relay holds one of these as
/// well as the host's login-stream place, so one account can't fill a
/// host's `max_login_streams` with abandoned logins and lock everyone
/// else out of it.
pub const MAX_LOGINS_PER_USER: usize = 4;

/// Logins in flight per Veil account (see MAX_LOGINS_PER_USER).
#[derive(Default)]
pub struct UserLogins(Mutex<HashMap<String, usize>>);

/// One account's place among its MAX_LOGINS_PER_USER; given back on
/// drop.
pub struct UserLoginSlot {
    logins: Arc<UserLogins>,
    username: String,
}

impl UserLogins {
    /// A place for `username`, or None when the account already has
    /// MAX_LOGINS_PER_USER logins in flight.
    fn claim(self: &Arc<Self>, username: &str) -> Option<UserLoginSlot> {
        let mut map = self.0.lock().unwrap_or_else(|p| p.into_inner());
        let count = map.entry(username.to_string()).or_insert(0);
        if *count >= MAX_LOGINS_PER_USER {
            return None;
        }
        *count += 1;
        Some(UserLoginSlot { logins: self.clone(), username: username.to_string() })
    }
}

impl Drop for UserLoginSlot {
    fn drop(&mut self) {
        let mut map = self.logins.0.lock().unwrap_or_else(|p| p.into_inner());
        if let Some(count) = map.get_mut(&self.username) {
            *count -= 1;
            if *count == 0 {
                map.remove(&self.username);
            }
        }
    }
}

/// A connected host's channel, as the lobby uses it.
#[derive(Clone)]
pub struct HostLink {
    pub conn: quinn::Connection,
    generation: u64,
    logins: Arc<Semaphore>,
    user_logins: Arc<UserLogins>,
}

/// A login stream on a host's channel: the stream pair, the place in
/// that host's `max_login_streams` and the account's place among its
/// MAX_LOGINS_PER_USER. Both are held until `release` or drop.
pub struct LoginStream {
    pub send: quinn::SendStream,
    pub recv: quinn::RecvStream,
    permit: Option<OwnedSemaphorePermit>,
    user_slot: Option<UserLoginSlot>,
}

/// Why a login stream couldn't be opened.
#[derive(Debug)]
pub enum OpenLoginError {
    /// `veil_user` already has MAX_LOGINS_PER_USER logins in flight.
    TooManyLogins,
    Other(anyhow::Error),
}

impl std::fmt::Display for OpenLoginError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            OpenLoginError::TooManyLogins => write!(f, "too many logins in progress for this account"),
            OpenLoginError::Other(e) => write!(f, "{e:#}"),
        }
    }
}

impl std::error::Error for OpenLoginError {}

impl HostLink {
    /// Opens a login stream for a client at `forwarded_for`, on behalf of
    /// the Veil account `veil_user`, and sends its LoginStreamOpen. Waits
    /// for a free slot when this host already has `max_login_streams`
    /// logins in flight; refuses outright when the account has
    /// MAX_LOGINS_PER_USER.
    pub async fn open_login(
        &self,
        forwarded_for: &str,
        client_id: &str,
        veil_user: &str,
    ) -> std::result::Result<LoginStream, OpenLoginError> {
        let user_slot = self.user_logins.claim(veil_user).ok_or(OpenLoginError::TooManyLogins)?;
        let open = async {
            let permit = self.logins.clone().acquire_owned().await.context("host channel closing")?;
            let (mut send, recv) = self.conn.open_bi().await.context("opening a login stream")?;
            write_frame(
                &mut send,
                &LoginStreamOpen { forwarded_for: forwarded_for.to_string(), client_id: client_id.to_string() },
            )
            .await?;
            Ok::<_, anyhow::Error>((send, recv, permit))
        };
        let (send, recv, permit) = open.await.map_err(OpenLoginError::Other)?;
        Ok(LoginStream { send, recv, permit: Some(permit), user_slot: Some(user_slot) })
    }
}

impl LoginStream {
    /// Gives back the host's and the account's places now, for a login
    /// that is decided (ended, refused or redirected) while the stream
    /// object lives on.
    pub fn release(&mut self) {
        self.permit = None;
        self.user_slot = None;
    }
}

/// The desktops a connected host offers, as its last Snapshot said, so a
/// client can choose one together with the host (DeviceList).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct HostTypes {
    /// (id, display name), in the host's order.
    pub available: Vec<(String, String)>,
    pub default_type: String,
}

pub struct Hosts {
    db: Arc<Db>,
    default_mode: DeviceMode,
    max_login_streams: usize,
    online: Mutex<HashMap<String, HostLink>>,
    types: Mutex<HashMap<String, HostTypes>>,
    generations: AtomicU64,
    user_logins: Arc<UserLogins>,
}

impl Hosts {
    pub fn new(db: Arc<Db>, default_mode: DeviceMode, max_login_streams: u32) -> Hosts {
        Hosts {
            db,
            default_mode,
            max_login_streams: max_login_streams as usize,
            online: Mutex::new(HashMap::new()),
            types: Mutex::new(HashMap::new()),
            generations: AtomicU64::new(1),
            user_logins: Arc::new(UserLogins::default()),
        }
    }

    fn online(&self) -> std::sync::MutexGuard<'_, HashMap<String, HostLink>> {
        self.online.lock().unwrap_or_else(|p| p.into_inner())
    }

    pub fn link(&self, device_id: &str) -> Option<HostLink> {
        self.online().get(device_id).cloned()
    }

    pub fn is_online(&self, device_id: &str) -> bool {
        self.online().contains_key(device_id)
    }

    /// What `device_id` offers; empty when it is offline or hasn't said.
    pub fn session_types(&self, device_id: &str) -> HostTypes {
        self.types.lock().unwrap_or_else(|p| p.into_inner()).get(device_id).cloned().unwrap_or_default()
    }

    fn set_session_types(&self, device_id: &str, types: HostTypes) {
        self.types.lock().unwrap_or_else(|p| p.into_inner()).insert(device_id.to_string(), types);
    }

    /// The device was removed or disabled in the admin UI: drop its
    /// channel now rather than at its next reconnect.
    pub fn disconnect(&self, device_id: &str, code: HostErrorCode, reason: &str) {
        if let Some(link) = self.online().remove(device_id) {
            self.types.lock().unwrap_or_else(|p| p.into_inner()).remove(device_id);
            link.conn.close(VarInt::from_u32(code as u32), reason.as_bytes());
        }
    }

    /// Runs one host connection to its end.
    pub async fn serve(self: Arc<Self>, conn: quinn::Connection) {
        let remote = gdpnet::canonical(conn.remote_address());
        let pin = gdpnet::peer_fingerprint(conn.peer_identity());
        match self.clone().serve_inner(&conn, pin).await {
            Ok(()) => conn.close(VarInt::from_u32(0), b""),
            Err(e) => {
                let code = match e.downcast_ref::<Refusal>() {
                    Some(r) => r.code as u32,
                    None => HostErrorCode::HostErrorMalformed as u32,
                };
                warn!(%remote, error = %format!("{e:#}"), "hosts: channel ended");
                conn.close(VarInt::from_u32(code), b"");
            }
        }
    }

    async fn serve_inner(self: Arc<Self>, conn: &quinn::Connection, pin: Option<String>) -> Result<()> {
        let remote = gdpnet::canonical(conn.remote_address());
        let (mut send, mut recv) = timeout(HELLO_TIMEOUT, conn.accept_bi())
            .await
            .context("host opened no control stream")?
            .context("accepting the control stream")?;
        let Some(pin) = pin else {
            return Err(refuse(&mut send, HostErrorCode::HostErrorCertMismatch, "no client certificate").await);
        };
        let first: HostEnvelope = timeout(HELLO_TIMEOUT, read_frame_max(&mut recv, HELLO_MAX_FRAME))
            .await
            .context("host sent no hello")??;
        match first.msg {
            Some(Msg::Join(join)) => {
                let device_id = self.join(&mut send, join, &pin, remote.ip()).await?;
                // The joining ghostd exits once it has its id; wait for
                // that (briefly) so the reply isn't cut off by our close.
                let _ = send.finish();
                let _ = timeout(Duration::from_secs(5), conn.closed()).await;
                info!(%remote, device_id, "hosts: joined");
                Ok(())
            }
            Some(Msg::Hello(hello)) => {
                let device = match self.db.device(&hello.device_id)? {
                    None => return Err(refuse(&mut send, HostErrorCode::HostErrorUnknownDevice, "unknown device").await),
                    Some(d) if d.cert_sha256 != pin => {
                        return Err(refuse(&mut send, HostErrorCode::HostErrorCertMismatch, "certificate does not match").await)
                    }
                    Some(d) if !d.enabled => {
                        return Err(refuse(&mut send, HostErrorCode::HostErrorDisabled, "device disabled").await)
                    }
                    Some(d) => d,
                };
                write_frame(&mut send, &HostEnvelope { msg: Some(Msg::Welcome(HostWelcome {})) }).await?;
                let generation = self.generations.fetch_add(1, Ordering::Relaxed);
                let link = HostLink {
                    conn: conn.clone(),
                    generation,
                    logins: Arc::new(Semaphore::new(self.max_login_streams)),
                    user_logins: self.user_logins.clone(),
                };
                if let Some(old) = self.online().insert(device.id.clone(), link) {
                    old.conn.close(VarInt::from_u32(0), b"replaced by a newer connection");
                }
                self.db.touch_device(&device.id)?;
                info!(%remote, device_id = %device.id, name = %device.name, version = %hello.version, "hosts: host online");
                let result = self.events(&device.id, &mut send, &mut recv).await;
                self.went_offline(&device.id, generation);
                result
            }
            _ => Err(refuse(&mut send, HostErrorCode::HostErrorMalformed, "expected Join or HostHello").await),
        }
    }

    async fn join(
        &self,
        send: &mut quinn::SendStream,
        join: ipc::broker::Join,
        pin: &str,
        source: std::net::IpAddr,
    ) -> Result<String> {
        let Some((token_id, secret)) = join.token.split_once('.') else {
            return Err(refuse(send, HostErrorCode::HostErrorBadToken, "malformed join token").await);
        };
        match self.db.consume_join_token(token_id, &sha256_hex(secret.as_bytes()))? {
            Ok(()) => {}
            Err(e) => {
                let why = match e {
                    TokenError::Unknown => "unknown join token",
                    TokenError::Used => "join token already used",
                    TokenError::Expired => "join token expired",
                };
                self.db.audit("host", "join refused", None, &format!("{} ({why})", join.hostname), Some(&source.to_string()));
                return Err(refuse(send, HostErrorCode::HostErrorBadToken, why).await);
            }
        }
        let client_address = if join.client_address.is_empty() { source.to_string() } else { join.client_address };
        let device_id = self.db.join_device(&new_id(), &join.hostname, &client_address, pin, self.default_mode)?;
        self.db.set_token_device(token_id, &device_id)?;
        self.db.audit(
            "host",
            "joined",
            Some(&device_id),
            &format!("{} as {client_address}, version {}", join.hostname, join.version),
            Some(&source.to_string()),
        );
        write_frame(send, &HostEnvelope { msg: Some(Msg::JoinAccepted(JoinAccepted { device_id: device_id.clone() })) })
            .await?;
        Ok(device_id)
    }

    // The control stream after HostWelcome: events until the host goes.
    async fn events(&self, device_id: &str, send: &mut quinn::SendStream, recv: &mut quinn::RecvStream) -> Result<()> {
        loop {
            let env: HostEnvelope = match read_frame(recv).await {
                Ok(env) => env,
                // The host closing its stream or connection is how it goes
                // offline normally (a ghostd restart).
                Err(ipc::framing::FrameError::Io(_)) => return Ok(()),
                Err(e) => return Err(e.into()),
            };
            let placement = |uid: u32, username: String, session_type: String, started_at: i64, viewer: bool| Placement {
                device_id: device_id.to_string(),
                username,
                uid,
                session_type,
                started_at,
                viewer_attached: viewer,
            };
            match env.msg {
                Some(Msg::Snapshot(snap)) => {
                    let sessions: Vec<Placement> = snap
                        .sessions
                        .iter()
                        .cloned()
                        .map(|RunningSession { uid, username, session_type, started_at_unix, viewer_attached }| {
                            placement(uid, username, session_type, started_at_unix, viewer_attached)
                        })
                        .collect();
                    info!(device_id, sessions = sessions.len(), types = snap.available_types.len(), "hosts: snapshot");
                    self.db.replace_placements(device_id, &sessions)?;
                    self.set_session_types(
                        device_id,
                        HostTypes {
                            available: snap.available_types.into_iter().map(|t| (t.id, t.name)).collect(),
                            default_type: snap.default_type,
                        },
                    );
                }
                Some(Msg::SessionStarted(s)) => {
                    info!(device_id, username = %s.username, session_type = %s.session_type, "hosts: session started");
                    self.db.upsert_placement(&placement(s.uid, s.username, s.session_type, s.started_at_unix, false))?;
                }
                Some(Msg::SessionEnded(s)) => {
                    info!(device_id, username = %s.username, reason = %s.reason, "hosts: session ended");
                    self.db.remove_placement(device_id, &s.username)?;
                }
                Some(Msg::ViewerAttached(v)) => self.db.set_viewer(device_id, &v.username, true)?,
                Some(Msg::ViewerDetached(v)) => self.db.set_viewer(device_id, &v.username, false)?,
                Some(Msg::LocalLoginTakeover(t)) => {
                    info!(device_id, username = %t.username, "hosts: the user logged in at the host itself");
                    self.db.audit(&t.username, "local login", Some(device_id), "ended the ghost session", None);
                }
                Some(Msg::Leave(_)) => {
                    self.db.remove_device(device_id)?;
                    self.db.audit("host", "left", Some(device_id), "ghostd leave", None);
                    info!(device_id, "hosts: host left");
                    write_frame(send, &HostEnvelope { msg: Some(Msg::Left(Left {})) }).await?;
                    let _ = send.finish();
                    let _ = send.stopped().await;
                    return Ok(());
                }
                other => bail!("unexpected message on the control stream: {other:?}"),
            }
        }
    }

    fn went_offline(&self, device_id: &str, generation: u64) {
        let mut online = self.online();
        if online.get(device_id).is_some_and(|l| l.generation == generation) {
            online.remove(device_id);
            drop(online);
            self.types.lock().unwrap_or_else(|p| p.into_inner()).remove(device_id);
            let _ = self.db.touch_device(device_id);
            // What it was running is unknown now; its next Snapshot says.
            if let Err(e) = self.db.replace_placements(device_id, &[]) {
                warn!(device_id, error = %e, "hosts: clearing an offline host's sessions failed");
            }
            info!(device_id, "hosts: host offline");
        }
    }

    /// Issues a join token: what the admin pastes into `ghostd join` on
    /// the host, `<id>.<secret>:sha256:<Veil's lobby fingerprint>`. Only
    /// the secret's hash is stored.
    pub fn issue_join_token(&self, ttl: Duration, created_by: &str, lobby_fingerprint: &str) -> Result<String> {
        let mut id = [0u8; 6];
        let mut secret = [0u8; 24];
        rand::rng().fill_bytes(&mut id);
        rand::rng().fill_bytes(&mut secret);
        let id = hex(&id);
        let secret = hex(&secret);
        self.db.add_join_token(&id, &sha256_hex(secret.as_bytes()), ttl.as_secs() as i64, created_by)?;
        self.db.audit(created_by, "join token issued", None, &format!("token {id}, valid {}", preauth::duration::format(ttl)), None);
        Ok(format!("{id}.{secret}:sha256:{lobby_fingerprint}"))
    }
}

/// A HostError sent to the host, also the connection's close code.
#[derive(Debug)]
struct Refusal {
    code: HostErrorCode,
    message: String,
}

impl std::fmt::Display for Refusal {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "refused ({}): {}", self.code.as_str_name(), self.message)
    }
}

impl std::error::Error for Refusal {}

// Sends the host a HostError and returns the error that ends its
// connection with the same code.
async fn refuse(send: &mut quinn::SendStream, code: HostErrorCode, message: &str) -> anyhow::Error {
    let env = HostEnvelope { msg: Some(Msg::Error(HostError { code: code as i32, message: message.to_string() })) };
    if write_frame(send, &env).await.is_ok() {
        let _ = send.finish();
        let _ = timeout(Duration::from_secs(2), send.stopped()).await;
    }
    anyhow!(Refusal { code, message: message.to_string() })
}

fn new_id() -> String {
    let mut id = [0u8; 8];
    rand::rng().fill_bytes(&mut id);
    hex(&id)
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

pub fn sha256_hex(data: &[u8]) -> String {
    hex(&Sha256::digest(data))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_hosts_session_types_are_remembered_until_it_goes() {
        let hosts = Hosts::new(Arc::new(Db::in_memory()), DeviceMode::Auto, 4);
        assert_eq!(hosts.session_types("d1"), HostTypes::default(), "nothing known about a host that hasn't said");
        let types = HostTypes {
            available: vec![("gnome".into(), "GNOME".into()), ("kde".into(), "KDE Plasma".into())],
            default_type: "gnome".into(),
        };
        hosts.set_session_types("d1", types.clone());
        assert_eq!(hosts.session_types("d1"), types);
        assert_eq!(hosts.session_types("d2"), HostTypes::default(), "per host");
        // A newer Snapshot replaces the list rather than adding to it.
        hosts.set_session_types("d1", HostTypes { available: vec![("kde".into(), "KDE Plasma".into())], default_type: "kde".into() });
        assert_eq!(hosts.session_types("d1").available.len(), 1);
    }
}
