// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// One graphical login per user at a time, local or remote
// (docs/design/login-and-sessions.md#one-graphical-login-per-user). A
// console login wins: the PAM account
// hook ghostlogin (host/ghostlogin) asks ghostd over
// GHOSTLOGIN_SOCKET to end that user's ghost session, and waits for the
// answer before the display manager starts the local desktop. A remote
// login loses: the lobby refuses it with LOCAL_SESSION_ACTIVE while the user
// has a graphical session on one of the host's seats (local_session()).
use std::os::unix::fs::PermissionsExt;
use std::process::Command;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use ipc::framing::{read_frame, write_frame};
use ipc::ghostlogin::{local_login_envelope::Msg, LocalLoginEnvelope, LocalLoginReply};
use ipc::paths::GHOSTLOGIN_SOCKET;
use serde::Deserialize;
use tokio::net::{UnixListener, UnixStream};
use tokio::time::timeout;
use tracing::{info, warn};

use crate::session::SessionManager;

const REQUEST_READ_TIMEOUT: Duration = Duration::from_secs(5);

/// A graphical session this uid has on one of the host's own seats, which
/// blocks a GDP login for it.
#[derive(Debug, Clone)]
pub struct LocalSession {
    pub id: String,
    pub seat: String,
}

/// The lobby's refusal when the user is logged in at the host itself,
/// mapped to LOCAL_SESSION_ACTIVE (lobby.rs's session_start_error).
#[derive(Debug)]
pub struct LocalSessionActive {
    pub detail: String,
}

impl std::fmt::Display for LocalSessionActive {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "the user is logged in at the host itself ({})", self.detail)
    }
}

impl std::error::Error for LocalSessionActive {}

#[derive(Deserialize)]
struct ListedSession {
    session: String,
    uid: u32,
    seat: Option<String>,
    class: String,
}

/// `uid`'s first logind session that is graphical, on a seat, not remote
/// and not on its way out; None if it has none. Seatless sessions (ghost's
/// own, SSH, the user manager's) never count, and neither do text VTs:
/// only a second compositor and keyring collide with a ghost session.
/// `closing` is skipped because a logged-out session whose stray processes
/// linger can sit in it indefinitely.
pub fn local_session(uid: u32) -> Result<Option<LocalSession>> {
    let listed = command_output(Command::new("loginctl").args(["list-sessions", "--json=short"]))?;
    let listed: Vec<ListedSession> = serde_json::from_str(&listed).context("parsing loginctl list-sessions")?;
    for s in listed {
        let Some(seat) = s.seat.filter(|seat| !seat.is_empty()) else { continue };
        if s.uid != uid || s.class != "user" {
            continue;
        }
        let props = command_output(Command::new("loginctl").args([
            "show-session",
            &s.session,
            "-p",
            "Type",
            "-p",
            "State",
            "-p",
            "Remote",
        ]))?;
        let prop = |name: &str| {
            props.lines().find_map(|line| line.strip_prefix(name).and_then(|v| v.strip_prefix('='))).unwrap_or("")
        };
        if matches!(prop("Type"), "x11" | "wayland" | "mir") && prop("State") != "closing" && prop("Remote") != "yes" {
            return Ok(Some(LocalSession { id: s.session, seat }));
        }
    }
    Ok(None)
}

fn command_output(cmd: &mut Command) -> Result<String> {
    let output = cmd.output().with_context(|| format!("running {cmd:?}"))?;
    if !output.status.success() {
        bail!("{cmd:?} failed: {}", String::from_utf8_lossy(&output.stderr).trim());
    }
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

/// Binds GHOSTLOGIN_SOCKET (ghostd's own, 0600; ghostlogin runs as root,
/// which connects to anything) and serves it for ghostd's lifetime.
pub fn spawn_listener(sessions: Arc<SessionManager>) -> Result<()> {
    let _ = std::fs::remove_file(GHOSTLOGIN_SOCKET);
    let listener =
        UnixListener::bind(GHOSTLOGIN_SOCKET).with_context(|| format!("binding {GHOSTLOGIN_SOCKET}"))?;
    std::fs::set_permissions(GHOSTLOGIN_SOCKET, std::fs::Permissions::from_mode(0o600))
        .with_context(|| format!("chmod {GHOSTLOGIN_SOCKET}"))?;
    tokio::spawn(async move {
        loop {
            match listener.accept().await {
                // Each on its own task: one user's logout may take seconds,
                // and another user's console login must not wait on it.
                Ok((stream, _)) => {
                    tokio::spawn(serve(stream, sessions.clone()));
                }
                Err(e) => warn!(error = %e, "local-login: accept failed"),
            }
        }
    });
    Ok(())
}

async fn serve(mut stream: UnixStream, sessions: Arc<SessionManager>) {
    // The socket is 0600 already, so only ghostd itself and root reach
    // it; the peer check keeps a loosened mode from mattering.
    match stream.peer_cred() {
        Ok(cred) if cred.uid() == 0 => {}
        Ok(cred) => {
            warn!(peer_uid = cred.uid(), "local-login: refusing a non-root client");
            return;
        }
        Err(e) => {
            warn!(error = %e, "local-login: could not read the client's credentials");
            return;
        }
    }

    let env: LocalLoginEnvelope = match timeout(REQUEST_READ_TIMEOUT, read_frame(&mut stream)).await {
        Ok(Ok(env)) => env,
        Ok(Err(e)) => {
            warn!(error = %e, "local-login: reading the request failed");
            return;
        }
        Err(_) => {
            warn!("local-login: client sent nothing within {REQUEST_READ_TIMEOUT:?}");
            return;
        }
    };
    let Some(Msg::Request(req)) = env.msg else {
        warn!("local-login: expected LocalLoginRequest");
        return;
    };

    info!(uid = req.uid, service = %req.service, tty = %req.tty, "local-login: console login, ending any ghost session");
    let reply = match sessions.end_for_local_login(req.uid).await {
        Ok(ended) => LocalLoginReply { ended, error: String::new() },
        Err(e) => {
            warn!(uid = req.uid, error = %format!("{e:#}"), "local-login: ending the ghost session failed");
            LocalLoginReply { ended: false, error: format!("{e:#}") }
        }
    };
    if let Err(e) = write_frame(&mut stream, &LocalLoginEnvelope { msg: Some(Msg::Reply(reply)) }).await {
        warn!(uid = req.uid, error = %e, "local-login: writing the reply failed");
    }
}
