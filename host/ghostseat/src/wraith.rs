// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// wraith's side of a session, from ghostseat's end: its control socket
// (host/proto/control.proto; wraith's side is session/seat_client.cpp),
// its unit in the user's systemd manager, and the logout a console login
// asks for. Everything here runs as root, which `systemctl --user
// --machine=<user>@` needs.
use std::os::unix::fs::PermissionsExt;
use std::path::Path;
use std::process::Command;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use ipc::control::{control_envelope::Msg as ControlMsg, ControlEnvelope, SessionInit};
use ipc::framing::{read_frame, write_frame};
use ipc::lobby::LobbyErrorCode;
use ipc::paths::control_socket;
use tokio::net::{UnixListener, UnixStream};
use tokio::time::{timeout, Instant};
use tracing::{info, warn};

const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
const USER_MANAGER_WAIT: Duration = Duration::from_secs(5);
const USER_MANAGER_RETRY_INTERVAL: Duration = Duration::from_millis(200);
/// How long the control loop waits for a connected client's one request
/// frame. Anything running as the session's uid can connect to the
/// control socket, so a client that connects and sends nothing must not
/// stall SessionEnded.
pub const CONTROL_READ_TIMEOUT: Duration = Duration::from_secs(5);
// logout(): how long the desktop gets to log out on its own after
// wraith's SIGUSR1 before `systemctl stop`, then how long the stop gets.
// The console login waits on both, so they're kept well short of
// wraith's own 30 s logout grace.
const LOGOUT_WAIT: Duration = Duration::from_secs(10);
const STOP_WAIT: Duration = Duration::from_secs(5);
const POLL_INTERVAL: Duration = Duration::from_millis(200);

/// What wraith reports in SessionReady.
pub struct Ready {
    pub port: u16,
    pub cert_sha256: String,
}

/// A startup failure wraith reported with an error code of its own
/// (ControlError.code), which ghostd passes on to spectre in place of the
/// generic SESSION_START_FAILED.
#[derive(Debug)]
pub struct StartupError {
    pub code: LobbyErrorCode,
    pub message: String,
}

impl std::fmt::Display for StartupError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "wraith reported a startup error ({}): {}", self.code.as_str_name(), self.message)
    }
}

impl std::error::Error for StartupError {}

/// Binds, chmods and chowns `/run/ghost/<uid>.sock`, unlinking any stale
/// file left by a wraith that died without cleaning up first (bind fails
/// with AddrInUse otherwise). Bound before wraith.service starts, so
/// wraith's connect never races it.
pub fn bind_control_socket(uid: u32) -> Result<UnixListener> {
    let sock_path = control_socket(uid);
    let _ = std::fs::remove_file(&sock_path);
    let listener = UnixListener::bind(&sock_path)
        .with_context(|| format!("binding control socket {}", sock_path.display()))?;
    std::fs::set_permissions(&sock_path, std::fs::Permissions::from_mode(0o600))
        .with_context(|| format!("chmod {}", sock_path.display()))?;
    nix::unistd::chown(&sock_path, Some(nix::unistd::Uid::from_raw(uid)), None)
        .with_context(|| format!("chown {} to uid {uid}", sock_path.display()))?;
    Ok(listener)
}

/// Starts wraith.service and runs the SessionInit/SessionReady handshake
/// on `listener`.
pub async fn bring_up(
    uid: u32,
    username: &str,
    session_env: &[String],
    listener: &UnixListener,
    init: SessionInit,
) -> Result<Ready> {
    // start_unit shells out to systemctl and can block for seconds
    // retrying (see its own comment) -- run it on a blocking thread
    // rather than stalling this task's executor thread.
    let (username_owned, session_env) = (username.to_string(), session_env.to_vec());
    tokio::task::spawn_blocking(move || start_unit(&username_owned, &session_env))
        .await
        .context("wraith-start task panicked")??;

    let ready = timeout(CONNECT_TIMEOUT, accept_and_handshake(listener, uid, init))
        .await
        .with_context(|| format!("wraith did not connect to the control socket within {CONNECT_TIMEOUT:?}"))??;
    info!(username, port = ready.port, cert_sha256 = %ready.cert_sha256, "wraith ready");
    Ok(ready)
}

/// Whether a control connection comes from the session's uid (or root).
/// The socket is 0600 to that uid in a 0711 directory, so this is belt
/// and braces; it also states the boundary: the peer the socket admits
/// is the user, and nothing finer (login-and-sessions.md, "The control
/// socket"). A refusal is logged.
pub fn peer_is_session_user(stream: &UnixStream, uid: u32) -> bool {
    match stream.peer_cred() {
        Ok(cred) if cred.uid() == uid || cred.uid() == 0 => true,
        Ok(cred) => {
            warn!(uid, peer_uid = cred.uid(), "refusing a control connection from another uid");
            false
        }
        Err(e) => {
            warn!(uid, error = %e, "could not read a control client's credentials; refusing it");
            false
        }
    }
}

async fn accept_and_handshake(listener: &UnixListener, uid: u32, init: SessionInit) -> Result<Ready> {
    let mut stream = loop {
        let (stream, _addr) = listener.accept().await.context("accepting wraith's control connection")?;
        if peer_is_session_user(&stream, uid) {
            break stream;
        }
    };
    write_frame(&mut stream, &ControlEnvelope { msg: Some(ControlMsg::Init(init)) })
        .await
        .context("sending SessionInit to wraith")?;
    let env: ControlEnvelope = read_frame(&mut stream).await.context("reading SessionReady from wraith")?;
    match env.msg {
        Some(ControlMsg::Ready(ready)) => {
            let port = u16::try_from(ready.port).context("SessionReady.port out of range")?;
            if port == 0 {
                bail!("SessionReady.port is 0");
            }
            Ok(Ready { port, cert_sha256: ready.cert_sha256 })
        }
        Some(ControlMsg::Error(e)) => match i32::try_from(e.code).ok().and_then(|c| LobbyErrorCode::try_from(c).ok()) {
            Some(code) if code != LobbyErrorCode::LobbyErrorUnspecified => {
                Err(StartupError { code, message: e.message }.into())
            }
            _ => bail!("wraith reported a startup error: {}", e.message),
        },
        other => bail!("expected SessionReady from wraith, got {other:?}"),
    }
}

/// Control requests wraith sends after the handshake, each on its own
/// connection.
pub enum Request {
    SessionEnded { result: String, exit_code: String, exit_status: String },
    CreateDevice(UnixStream, ipc::control::CreateDevice),
    ViewerAttached,
    ViewerDetached,
}

/// Reads the one request on a freshly accepted control connection.
pub async fn read_request(mut stream: UnixStream) -> Option<Request> {
    let env: ControlEnvelope = match timeout(CONTROL_READ_TIMEOUT, read_frame(&mut stream)).await {
        Ok(Ok(env)) => env,
        Ok(Err(e)) => {
            warn!(error = %e, "reading a control frame failed");
            return None;
        }
        Err(_) => {
            warn!("control client sent nothing within {CONTROL_READ_TIMEOUT:?}, dropping it");
            return None;
        }
    };
    match env.msg {
        Some(ControlMsg::SessionEnded(e)) => {
            Some(Request::SessionEnded { result: e.result, exit_code: e.exit_code, exit_status: e.exit_status })
        }
        Some(ControlMsg::CreateDevice(req)) => Some(Request::CreateDevice(stream, req)),
        Some(ControlMsg::ViewerAttached(_)) => Some(Request::ViewerAttached),
        Some(ControlMsg::ViewerDetached(_)) => Some(Request::ViewerDetached),
        other => {
            warn!(?other, "unexpected message on the control socket");
            None
        }
    }
}

fn systemctl_user(username: &str, args: &[&str]) -> Command {
    let mut cmd = Command::new("systemctl");
    cmd.arg("--user").arg(format!("--machine={username}@")).args(args);
    cmd
}

/// `user@<uid>.service` can take a moment to come up after
/// `pam_open_session` returns (logind starts it asynchronously), so retry
/// briefly instead of failing on the first race.
/// `session_env` (the PAM session environment) goes into the user
/// manager's environment first, the way a display manager hands its PAM
/// environment to the session: wraith inherits it from there and the
/// desktop's own units see it too (pam_kwallet_init runs as
/// plasma-kwallet-pam.service and only unlocks with PAM_KWALLET5_LOGIN).
fn start_unit(username: &str, session_env: &[String]) -> Result<()> {
    if !session_env.is_empty() {
        let names: Vec<&str> = session_env.iter().map(|e| e.split('=').next().unwrap_or_default()).collect();
        info!(username, ?names, "passing the PAM session environment to the user manager");
    }
    let mut set_env = vec!["set-environment"];
    set_env.extend(session_env.iter().map(String::as_str));
    let deadline = Instant::now() + USER_MANAGER_WAIT;
    loop {
        let result = if session_env.is_empty() {
            Ok(())
        } else {
            run_command(&mut systemctl_user(username, &set_env))
        }
        .and_then(|()| run_command(&mut systemctl_user(username, &["start", "wraith.service"])));
        match result {
            Ok(()) => return Ok(()),
            Err(e) if Instant::now() < deadline => {
                warn!(username, error = %e, "user manager not ready yet, retrying");
                std::thread::sleep(USER_MANAGER_RETRY_INTERVAL);
            }
            Err(e) => return Err(e),
        }
    }
}

/// True only if systemd itself currently considers `wraith.service`
/// active for `username`. A non-"active" result (unit inactive, failed,
/// or the user manager itself not running) reads as "not running"; only
/// a real error running `systemctl` itself propagates.
pub async fn unit_active(username: &str) -> Result<bool> {
    let username_owned = username.to_string();
    tokio::task::spawn_blocking(move || {
        let status = systemctl_user(&username_owned, &["is-active", "--quiet", "wraith.service"])
            .status()
            .with_context(|| format!("running systemctl is-active for {username_owned}"))?;
        Ok(status.success())
    })
    .await
    .context("wraith-liveness-check task panicked")?
}

async fn systemctl_user_async(username: &str, args: &[&str]) -> Result<()> {
    let username = username.to_string();
    let args: Vec<String> = args.iter().map(|a| a.to_string()).collect();
    tokio::task::spawn_blocking(move || {
        let args: Vec<&str> = args.iter().map(String::as_str).collect();
        run_command(&mut systemctl_user(&username, &args))
    })
    .await
    .context("systemctl task panicked")?
}

/// Logs the desktop out for a console login
/// (docs/design/login-and-sessions.md#one-graphical-login-per-user):
/// wraith is asked to log the desktop out (SIGUSR1, which also closes the
/// viewer with ENDED_BY_LOCAL_LOGIN), stopped outright if that takes too
/// long. Returns once wraith.service is inactive, or after giving up.
pub async fn logout(username: &str) {
    match unit_active(username).await {
        Ok(true) => {}
        Ok(false) => return,
        Err(e) => {
            warn!(username, error = %format!("{e:#}"), "couldn't check wraith.service; trying the logout anyway");
        }
    }
    info!(username, "console login, asking wraith to log the desktop out");
    if let Err(e) = systemctl_user_async(username, &["kill", "--kill-whom=main", "--signal=SIGUSR1", "wraith.service"]).await {
        warn!(username, error = %format!("{e:#}"), "signalling wraith failed");
    }
    if wait_for_inactive(username, LOGOUT_WAIT).await {
        return;
    }
    warn!(username, "the desktop didn't log out within {LOGOUT_WAIT:?}; stopping wraith.service");
    if let Err(e) = systemctl_user_async(username, &["stop", "--no-block", "wraith.service"]).await {
        warn!(username, error = %format!("{e:#}"), "stopping wraith.service failed");
    }
    if !wait_for_inactive(username, STOP_WAIT).await {
        warn!(username, "wraith.service still active; closing the logind session anyway");
    }
}

/// Polls wraith.service until it is inactive; false if it still wasn't
/// after `limit`. A failed check counts as still active.
async fn wait_for_inactive(username: &str, limit: Duration) -> bool {
    let deadline = Instant::now() + limit;
    loop {
        if matches!(unit_active(username).await, Ok(false)) {
            return true;
        }
        if Instant::now() >= deadline {
            return false;
        }
        tokio::time::sleep(POLL_INTERVAL).await;
    }
}

fn run_command(cmd: &mut Command) -> Result<()> {
    let output = cmd.output().with_context(|| format!("running {cmd:?}"))?;
    if !output.status.success() {
        bail!("{cmd:?} failed: {}", String::from_utf8_lossy(&output.stderr).trim());
    }
    Ok(())
}

/// Removes the control socket file at the end of the session.
pub fn remove_control_socket(uid: u32) {
    let path = control_socket(uid);
    if let Err(e) = std::fs::remove_file(&path) {
        if e.kind() != std::io::ErrorKind::NotFound {
            warn!(path = %Path::new(&path).display(), error = %e, "removing the control socket failed");
        }
    }
}
