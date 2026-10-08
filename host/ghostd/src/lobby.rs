// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The lobby (gdp-spec.md §4): one bidirectional stream, opened by spectre
// as QUIC stream 0, carrying LobbyHello -> AuthChallenge/AuthResponse (PAM
// in a ghostauth instance, the pamconv crate) -> SessionList (the user's running session plus the
// offered session types, profiles.rs) -> SessionOpen (session.rs) ->
// Redirect or LobbyError.
//
// The same exchange also arrives on login streams Veil opens over this
// host's channel (broker.rs, gdp-spec.md §13.4), with Veil standing in for
// the client. The only differences: the client's address comes from the
// stream's LoginStreamOpen.forwarded_for rather than the socket, and the
// stream, not a connection, is what ends.
use std::net::{IpAddr, SocketAddr};
use std::sync::Arc;
use std::time::Duration;

use anyhow::Result;
use quinn::VarInt;
use tokio::time::{timeout_at, Instant};
use tracing::{error, info, warn};

use ipc::broker::LoginStreamOpen;
use ipc::framing::{read_frame, read_frame_max, write_frame, FrameError};
use ipc::lobby::{
    lobby_envelope::Msg, AuthChallenge, LobbyEnvelope, LobbyError, LobbyErrorCode, Redirect,
    SessionInfo, SessionList, SessionType,
};

use crate::local_login::LocalSessionActive;
use pamconv::{AuthSession, Authtok};
use preauth::{Offense, Penalties, StartupSlot};
use crate::profiles;
use crate::session::{self, SessionManager, WraithStartupError};

const GDP_WIRE_VERSION: u32 = 1; // gdp-spec.md §4.3, LobbyHello.protocol_version

// Frames from a client that hasn't authenticated yet (LobbyHello,
// AuthResponse) are held to this rather than the protocol's 1 MiB -- the
// same cap wraith applies to its own unauthenticated peers.
const PRE_AUTH_MAX_FRAME: u32 = 16 * 1024;

// From PAM's answer to SessionOpen: how long a client gets to pick a
// session type. The ticket ghostauth minted is good for this long, so a
// later SessionOpen would be refused by ghostseat anyway; without the
// deadline the connection and its task would stay around for nothing.
const SESSION_OPEN_TIMEOUT: Duration = Duration::from_secs(authticket::TTL_SECS as u64);

// Authenticated lobby connections one uid may hold between PAM's answer
// and SessionOpen. The MaxStartups slot is released at authentication,
// so this is what bounds an account that opens connections and never
// sends SessionOpen.
const MAX_OPEN_LOGINS_PER_UID: u32 = 4;

pub struct LobbyContext {
    // Arc'd on its own so ensure_session can hand a clone to the
    // control-listener task it spawns for the life of the session.
    pub sessions: Arc<SessionManager>,
    // Priority order: an earlier directory's profile for a given filename
    // stem wins (main.rs puts sessions.dir before the shipped datadir).
    pub profile_dirs: Vec<std::path::PathBuf>,
    pub default_session_type: String,
    pub policy: LoginPolicy,
    pub penalties: Penalties,
    /// Authenticated connections waiting for SessionOpen, per uid
    /// (MAX_OPEN_LOGINS_PER_UID).
    pub open_logins: std::sync::Mutex<std::collections::HashMap<u32, u32>>,
}

/// A uid's place among its MAX_OPEN_LOGINS_PER_UID; given back on drop.
struct OpenLogin<'a> {
    ctx: &'a LobbyContext,
    uid: u32,
}

impl LobbyContext {
    fn claim_open_login(&self, uid: u32) -> Option<OpenLogin<'_>> {
        let mut map = self.open_logins.lock().unwrap_or_else(|p| p.into_inner());
        let count = map.entry(uid).or_insert(0);
        if *count >= MAX_OPEN_LOGINS_PER_UID {
            return None;
        }
        *count += 1;
        Some(OpenLogin { ctx: self, uid })
    }
}

impl Drop for OpenLogin<'_> {
    fn drop(&mut self) {
        let mut map = self.ctx.open_logins.lock().unwrap_or_else(|p| p.into_inner());
        if let Some(count) = map.get_mut(&self.uid) {
            *count -= 1;
            if *count == 0 {
                map.remove(&self.uid);
            }
        }
    }
}

/// sshd's login policy options ([auth] in ghostd.toml,
/// docs/design/preauth.md).
pub struct LoginPolicy {
    pub permit_root_login: bool,
    pub permit_empty_passwords: bool,
    /// From accepting the connection to PAM's answer; None is no limit
    /// (sshd's LoginGraceTime 0).
    pub login_grace_time: Option<Duration>,
}

/// Who is logging in: the client's address, and whether Veil relayed the
/// login (then the address is Veil's word for it, LoginStreamOpen's
/// forwarded_for, with no port).
#[derive(Clone, Copy, Debug)]
pub struct Peer {
    pub addr: SocketAddr,
    pub via_veil: bool,
}

impl Peer {
    fn ip(&self) -> IpAddr {
        self.addr.ip()
    }
}

impl std::fmt::Display for Peer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self.via_veil {
            true => write!(f, "{} (via Veil)", self.addr.ip()),
            false => write!(f, "{}", self.addr),
        }
    }
}

// Runs one lobby connection from the accept loop's Incoming, then closes
// it with the gdp-spec.md §12 error code matching how it ended
// (0 for a Redirect; the LobbyError's own code when one was sent;
// FRAME_TOO_LARGE / MALFORMED_FRAME when spectre's bytes were the
// problem) -- so the close itself says why, even for the paths where no
// LobbyError could be sent. `slot` is this connection's MaxStartups place,
// held until authentication is decided.
pub async fn handle_incoming(incoming: quinn::Incoming, ctx: Arc<LobbyContext>, slot: StartupSlot) {
    let deadline = ctx.policy.login_grace_time.map(|t| Instant::now() + t);
    let source = gdpnet::canonical(incoming.remote_address());
    let conn = match within(deadline, incoming).await {
        Some(Ok(conn)) => conn,
        Some(Err(e)) => {
            error!(%source, error = %e, "lobby: handshake failed");
            return;
        }
        // Not penalised: until the handshake completes, the source
        // address is unverified and could be someone else's.
        None => {
            warn!(%source, "lobby: handshake did not finish within the login grace time");
            return;
        }
    };

    let peer = Peer { addr: gdpnet::canonical(conn.remote_address()), via_veil: false };
    let result = match within(deadline, conn.accept_bi()).await {
        Some(Ok((send, recv))) => run_lobby(send, recv, &ctx, peer, deadline, Some(slot)).await,
        Some(Err(e)) => Err(e.into()),
        None => {
            warn!(%peer, "lobby: login grace time exceeded before authentication finished");
            ctx.penalties.penalise(peer.ip(), Offense::GraceExceeded);
            Ok(LobbyErrorCode::LobbyErrorAuthFailed as u32)
        }
    };
    let (code, reason) = close_code(&result);
    conn.close(VarInt::from_u32(code), reason.as_bytes());
    if let Err(e) = result {
        error!(%peer, error = %e, "lobby: connection handler failed");
    }
}

/// Runs one login Veil relays over this host's channel (broker.rs): the
/// stream's LoginStreamOpen, then the ordinary lobby. Penalties apply to
/// the client's address as Veil reports it, never to Veil's own, which
/// would lock out every user behind it. An end that sent no LobbyError
/// resets the stream with the code a direct connection would close with.
pub async fn handle_login_stream(mut send: quinn::SendStream, mut recv: quinn::RecvStream, ctx: Arc<LobbyContext>) {
    let deadline = ctx.policy.login_grace_time.map(|t| Instant::now() + t);
    let open: LoginStreamOpen = match within(deadline, read_frame_max(&mut recv, PRE_AUTH_MAX_FRAME)).await {
        Some(Ok(open)) => open,
        Some(Err(e)) => {
            warn!(error = %e, "lobby: bad login stream from Veil");
            let _ = send.reset(VarInt::from_u32(e.close_code().unwrap_or(0)));
            return;
        }
        None => {
            let _ = send.reset(VarInt::from_u32(LobbyErrorCode::LobbyErrorAuthFailed as u32));
            return;
        }
    };
    let Ok(ip) = open.forwarded_for.parse::<IpAddr>() else {
        warn!(forwarded_for = %open.forwarded_for, "lobby: Veil sent an unparsable client address");
        let _ = send.reset(VarInt::from_u32(LobbyErrorCode::LobbyErrorMalformedFrame as u32));
        return;
    };
    let peer = Peer { addr: SocketAddr::new(ip.to_canonical(), 0), via_veil: true };
    if ctx.penalties.refuses(peer.ip()) {
        info!(%peer, "lobby: refusing a penalised source");
        let _ = send_error(&mut send, LobbyErrorCode::LobbyErrorAuthFailed,
            "too many failed logins from your address; try again later".to_string()).await;
        return;
    }
    let result = run_lobby(send, recv, &ctx, peer, deadline, None).await;
    if let Err(e) = &result {
        error!(%peer, error = %e, "lobby: login stream failed");
    }
}

// The close code for how run_lobby ended: its own, or the one for a
// framing error spectre caused.
fn close_code(result: &Result<u32>) -> (u32, &'static str) {
    match result {
        Ok(code) => (*code, ""),
        Err(e) => match e.downcast_ref::<FrameError>().and_then(FrameError::close_code) {
            Some(code) => (code, "protocol violation"),
            None => (0, ""),
        },
    }
}

// `f`, bounded by `deadline` when there is one; None when it ran out.
async fn within<F: std::future::IntoFuture>(deadline: Option<Instant>, f: F) -> Option<F::Output> {
    match deadline {
        Some(deadline) => timeout_at(deadline, f).await.ok(),
        None => Some(f.await),
    }
}

// A lobby connection that got through authentication.
struct Authenticated {
    send: quinn::SendStream,
    recv: quinn::RecvStream,
    username: String,
    uid: u32,
    // For ghostseat's keyring unlock; only used if this login is the one
    // that opens the logind session.
    authtok: Option<Authtok>,
    // ghostauth's proof that this user authenticated from `remote`, which
    // ghostseat requires before it opens a session (host/authticket).
    ticket: String,
}

enum AuthOutcome {
    Authenticated(Authenticated),
    // Ended before authenticating; close with this code.
    Ended(u32),
}

// Returns the gdp-spec.md §12 code to close with. Errors are I/O or
// framing failures the caller maps to a code as above. `peer`'s address,
// already unwrapped from its v4-mapped form (gdpnet::canonical), is what
// penalties, the log and PAM_RHOST (ensure_session -> ghostseat ->
// pam_session.rs) all see.
async fn run_lobby(
    send: quinn::SendStream,
    recv: quinn::RecvStream,
    ctx: &LobbyContext,
    peer: Peer,
    deadline: Option<Instant>,
    slot: Option<StartupSlot>,
) -> Result<u32> {
    let remote = peer;
    // Set once spectre has answered a PAM prompt, which decides whether
    // an early end is sshd's "authfail" or "noauth" penalty.
    let mut attempted = false;
    let outcome = within(deadline, authenticate(send, recv, ctx, remote, &mut attempted)).await;
    drop(slot);
    let authed = match outcome {
        None => {
            warn!(%remote, "lobby: login grace time exceeded before authentication finished");
            ctx.penalties.penalise(remote.ip(), Offense::GraceExceeded);
            return Ok(LobbyErrorCode::LobbyErrorAuthFailed as u32);
        }
        Some(Err(e)) => {
            ctx.penalties.penalise(remote.ip(), if attempted { Offense::AuthFail } else { Offense::NoAuth });
            return Err(e);
        }
        Some(Ok(AuthOutcome::Ended(code))) => return Ok(code),
        Some(Ok(AuthOutcome::Authenticated(authed))) => authed,
    };
    let Authenticated { mut send, mut recv, username, uid, authtok, ticket } = authed;
    let Some(_open_login) = ctx.claim_open_login(uid) else {
        warn!(%remote, username = %username, "lobby: too many authenticated connections waiting for SessionOpen");
        return send_error(
            &mut send,
            LobbyErrorCode::LobbyErrorHostFull,
            "too many logins in progress for this account".to_string(),
        )
        .await;
    };
    let deadline = Some(Instant::now() + SESSION_OPEN_TIMEOUT);

    // One graphical login per user: someone logged in at the host itself
    // keeps it (docs/design/login-and-sessions.md). Asked only now, after
    // authentication, so a failed login never learns whether the account
    // is in use locally. ensure_session asks again under the uid lock.
    if let Err(e) = ctx.sessions.check_no_local_session(uid).await {
        info!(%remote, username = %username, reason = %e, "lobby: refusing the login, the user is logged in locally");
        let (code, message) = session_start_error(&e, ctx.sessions.port_range().max_sessions());
        return send_error(&mut send, code, message).await;
    }

    // SessionList.sessions reports this user's live session (at most one
    // per uid) so the client can resume it without asking for a type; the
    // SessionOpen that follows reuses it via ensure_session either way.
    let available = available_profiles(&ctx).await;
    let offered = |id: &str| available.iter().any(|p| p.id == id);

    // default_type: the user's last choice if it's still available, else
    // the host's configured default if available, else the first one
    // (gdp-spec.md §4.6).
    let (last_type, running) = ctx.sessions.lobby_state(uid, &username).await;
    let default_type = last_type
        .filter(|t| offered(t))
        .or_else(|| Some(ctx.default_session_type.clone()).filter(|t| offered(t)))
        .or_else(|| available.first().map(|p| p.id.clone()))
        .unwrap_or_default();

    write_frame(
        &mut send,
        &LobbyEnvelope {
            msg: Some(Msg::SessionList(SessionList {
                sessions: running
                    .iter()
                    .map(|entry| SessionInfo {
                        session_id: entry.uid.to_string(),
                        started_at_unix: entry.started_at,
                        last_active_unix: entry.last_seen,
                        session_type: entry.session_type.clone(),
                        viewer_attached: entry.viewer_attached,
                    })
                    .collect(),
                available_types: available
                    .iter()
                    .map(|p| SessionType { id: p.id.clone(), name: p.name.clone() })
                    .collect(),
                default_type: default_type.clone(),
            })),
        },
    )
    .await?;

    let Some(open_env) = within(deadline, read_frame::<LobbyEnvelope>(&mut recv)).await else {
        warn!(%remote, username = %username, "lobby: no SessionOpen within {SESSION_OPEN_TIMEOUT:?}");
        return send_error(&mut send, LobbyErrorCode::LobbyErrorSessionStartFailed, "no SessionOpen in time".to_string())
            .await;
    };
    let open_env = open_env?;
    let Some(Msg::SessionOpen(open)) = open_env.msg else {
        warn!(%remote, "lobby: expected SessionOpen, got something else");
        return Ok(LobbyErrorCode::LobbyErrorMalformedFrame as u32);
    };

    let session_type = if open.session_type.is_empty() { default_type } else { open.session_type };
    if !offered(&session_type) {
        warn!(%remote, username = %username, session_type, "lobby: unknown session type requested");
        return send_error(
            &mut send,
            LobbyErrorCode::LobbyErrorUnknownSessionType,
            format!("unknown session type {session_type:?}"),
        )
        .await;
    }

    match ctx.sessions.clone().ensure_session(uid, &username, &session_type, remote.ip(), authtok, ticket).await {
        Ok(handle) => {
            write_frame(
                &mut send,
                &LobbyEnvelope {
                    msg: Some(Msg::Redirect(Redirect {
                        // Always empty: the session lives on this host, so
                        // spectre reuses the lobby's address (gdp-spec.md
                        // §4.7). On a login Veil relayed, Veil fills it in.
                        host: String::new(),
                        port: handle.port as u32,
                        token: handle.token,
                        expiry_unix: handle.token_expiry_unix,
                        cert_sha256: handle.cert_sha256,
                    })),
                },
            )
            .await?;
            finish_and_wait(&mut send).await?;
            Ok(0)
        }
        Err(e) => {
            warn!(%remote, username = %username, error = %e, "lobby: session start failed");
            let (code, message) = session_start_error(&e, ctx.sessions.port_range().max_sessions());
            send_error(&mut send, code, message).await
        }
    }
}

// LobbyHello through PAM's answer and the login policy check (sshd's
// PermitRootLogin). Everything here runs under the login grace time.
// Explicit early ends penalise their source here; the caller does it for
// the `?` errors, using `attempted`.
async fn authenticate(
    mut send: quinn::SendStream,
    mut recv: quinn::RecvStream,
    ctx: &LobbyContext,
    remote: Peer,
    attempted: &mut bool,
) -> Result<AuthOutcome> {

    let hello_env: LobbyEnvelope = read_frame_max(&mut recv, PRE_AUTH_MAX_FRAME).await?;
    let Some(Msg::Hello(hello)) = hello_env.msg else {
        warn!(%remote, "lobby: expected LobbyHello first, got something else");
        ctx.penalties.penalise(remote.ip(), Offense::NoAuth);
        return Ok(AuthOutcome::Ended(LobbyErrorCode::LobbyErrorMalformedFrame as u32));
    };
    // `?`, not `%`: peer-supplied, so a newline in them can't forge a log line.
    info!(%remote, client_id = ?hello.client_id, username = ?hello.username, "lobby: hello");

    // An out-of-date client isn't an offense: no penalty.
    if hello.protocol_version != GDP_WIRE_VERSION {
        let code = send_error(
            &mut send,
            LobbyErrorCode::LobbyErrorVersionMismatch,
            format!("server speaks GDP wire version {GDP_WIRE_VERSION}"),
        )
        .await?;
        return Ok(AuthOutcome::Ended(code));
    }

    // One AuthChallenge per PAM prompt (gdp-spec.md §4.5).
    let mut auth = AuthSession::start("ghostd", hello.username.clone(), remote.ip(), ctx.policy.permit_empty_passwords).await;
    let auth_result = loop {
        match auth.next_prompt().await {
            Some(req) => {
                write_frame(&mut send, &challenge_envelope(req.message.clone(), req.echo))
                    .await?;
                let response_env: LobbyEnvelope = read_frame_max(&mut recv, PRE_AUTH_MAX_FRAME).await?;
                let Some(Msg::AuthResponse(response)) = response_env.msg else {
                    warn!(%remote, "lobby: expected AuthResponse, got something else");
                    let offense = if *attempted { Offense::AuthFail } else { Offense::NoAuth };
                    ctx.penalties.penalise(remote.ip(), offense);
                    return Ok(AuthOutcome::Ended(LobbyErrorCode::LobbyErrorMalformedFrame as u32));
                };
                *attempted = true;
                req.respond(response.response);
            }
            None => break auth.finish().await,
        }
    };
    let pamconv::Verdict { ticket, authtok } = match auth_result {
        Ok(verdict) => verdict,
        Err(e) => {
            warn!(%remote, username = ?hello.username, error = %e, "lobby: PAM authentication failed");
            return deny(&mut send, ctx, remote).await;
        }
    };

    // A username PAM accepted but nss doesn't know is unlikely this far in,
    // but nothing past this point can work without a uid.
    let uid = match session::resolve_uid(&hello.username) {
        Ok(uid) => uid,
        Err(e) => {
            warn!(%remote, username = ?hello.username, error = %e, "lobby: authenticated user has no uid");
            let code = send_error(
                &mut send,
                LobbyErrorCode::LobbyErrorSessionStartFailed,
                "session start failed".to_string(),
            )
            .await?;
            return Ok(AuthOutcome::Ended(code));
        }
    };

    // sshd's PermitRootLogin, checked after PAM as sshd does it, and
    // answered exactly like a wrong password so the client can't tell it
    // guessed root's password right.
    if uid == 0 && !ctx.policy.permit_root_login {
        warn!(%remote, username = ?hello.username,
            "lobby: refusing a root login (auth.permit_root_login is off)");
        return deny(&mut send, ctx, remote).await;
    }

    info!(%remote, username = ?hello.username, "lobby: PAM authentication succeeded");
    Ok(AuthOutcome::Authenticated(Authenticated { send, recv, username: hello.username, uid, authtok, ticket }))
}

// A failed authentication: the one AUTH_FAILED every such failure gets on
// the wire, then sshd's authfail penalty. (If the send itself fails,
// run_lobby penalises the error instead -- `attempted` is set by now.)
async fn deny(send: &mut quinn::SendStream, ctx: &LobbyContext, remote: Peer) -> Result<AuthOutcome> {
    let code = send_error(send, LobbyErrorCode::LobbyErrorAuthFailed, "authentication failed".to_string()).await?;
    ctx.penalties.penalise(remote.ip(), Offense::AuthFail);
    Ok(AuthOutcome::Ended(code))
}

// The LobbyError for a failed ensure_session: wraith's own code when it
// sent one (WraithStartupError), SESSION_START_FAILED otherwise. The
// message is ghostd's, written for the person at spectre; wraith's own
// text is for the log line above.
fn session_start_error(e: &anyhow::Error, max_sessions: u16) -> (LobbyErrorCode, String) {
    if e.downcast_ref::<LocalSessionActive>().is_some() {
        return (
            LobbyErrorCode::LobbyErrorLocalSessionActive,
            "you are logged in at the host itself; log out there first".to_string(),
        );
    }
    match e.downcast_ref::<WraithStartupError>().map(|w| w.code) {
        Some(LobbyErrorCode::LobbyErrorHostFull) => (
            LobbyErrorCode::LobbyErrorHostFull,
            format!("this host has no free session slots (max {max_sessions}); try again later"),
        ),
        Some(code) => (code, "session start failed".to_string()),
        None => (LobbyErrorCode::LobbyErrorSessionStartFailed, "session start failed".to_string()),
    }
}

/// The sessions.d profiles this host can start right now, in the order
/// the SessionList offers them.
pub async fn available_profiles(ctx: &LobbyContext) -> Vec<profiles::SessionProfile> {
    let profile_dirs = ctx.profile_dirs.clone();
    let profiles = tokio::task::spawn_blocking(move || profiles::load(&profile_dirs)).await.unwrap_or_else(|e| {
        warn!(error = %e, "lobby: loading session profiles panicked; offering none");
        Vec::new()
    });
    profiles.into_iter().filter(profiles::is_available).collect()
}

/// What Veil is told about this host's desktops (broker.rs's Snapshot):
/// the available types and the host's own default, without the per-user
/// "last choice" a SessionList applies.
pub async fn host_session_types(ctx: &LobbyContext) -> (Vec<(String, String)>, String) {
    let available = available_profiles(ctx).await;
    let default_type = Some(ctx.default_session_type.clone())
        .filter(|t| available.iter().any(|p| p.id == *t))
        .or_else(|| available.first().map(|p| p.id.clone()))
        .unwrap_or_default();
    (available.into_iter().map(|p| (p.id, p.name)).collect(), default_type)
}

// Sends a LobbyError, waits for spectre to have it, and yields the same
// code for the connection close that follows.
async fn send_error(send: &mut quinn::SendStream, code: LobbyErrorCode, message: String) -> Result<u32> {
    write_frame(send, &error_envelope(code, message)).await?;
    finish_and_wait(send).await?;
    Ok(code as u32)
}

// finish() only marks the end of the stream locally, and quinn closes a
// connection as soon as its last handle drops, which can beat the final
// frame to the wire. stopped() waits until the peer has acknowledged every
// byte (or reset the stream), so the connection is safe to drop after.
async fn finish_and_wait(send: &mut quinn::SendStream) -> Result<()> {
    send.finish()?;
    send.stopped().await?;
    Ok(())
}

fn challenge_envelope(prompt: String, echo_input: bool) -> LobbyEnvelope {
    LobbyEnvelope { msg: Some(Msg::AuthChallenge(AuthChallenge { prompt, echo_input })) }
}

fn error_envelope(code: LobbyErrorCode, message: String) -> LobbyEnvelope {
    LobbyEnvelope { msg: Some(Msg::Error(LobbyError { code: code as i32, message })) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use anyhow::Context;

    #[test]
    fn host_full_from_wraith_reaches_the_client() {
        let wraith: Result<()> = Err(WraithStartupError {
            code: LobbyErrorCode::LobbyErrorHostFull,
            message: "every GDP port in the given range is in use".to_string(),
        }
        .into());
        // ensure_session's callers may add context on the way up.
        let e = wraith.context("starting the session").unwrap_err();
        let (code, message) = session_start_error(&e, 64);
        assert_eq!(code, LobbyErrorCode::LobbyErrorHostFull);
        assert!(message.contains("max 64"), "{message}");
    }

    #[test]
    fn local_session_active_reaches_the_client() {
        let e: anyhow::Error = LocalSessionActive { detail: "logind session 4 on seat0".to_string() }.into();
        let e = e.context("checking for a local session");
        let (code, _) = session_start_error(&e, 64);
        assert_eq!(code, LobbyErrorCode::LobbyErrorLocalSessionActive);
    }

    #[test]
    fn uncoded_failures_stay_generic() {
        let e = anyhow::anyhow!("wraith did not connect to the control socket");
        let (code, message) = session_start_error(&e, 64);
        assert_eq!(code, LobbyErrorCode::LobbyErrorSessionStartFailed);
        assert_eq!(message, "session start failed");
    }
}
