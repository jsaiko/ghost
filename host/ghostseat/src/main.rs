// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// ghostseat: a session's root process (docs/design/login-and-sessions.md).
// Never run it by hand: systemd's ghostseat.socket accepts ghostd's
// connection to /run/ghost/seat.sock and starts one instance for it,
// with the connection as fd 0. The instance reads ghostd's Open, checks
// ghostauth's ticket, opens the user's logind session on the ghostseat
// PAM stack, starts wraith.service in the user's manager and runs the
// control-socket handshake, answers Opened, and then lives as long as the
// session does: STATUS and CLOSE from ghostd on /run/ghost/<uid>-seat.sock,
// wraith's requests on /run/ghost/<uid>.sock (devices among them), and
// its own events to ghostd on /run/ghost/events.sock.
//
// ghostd never becomes the session leader itself: logind ties a session's
// liveness to the pid that called pam_open_session, and ghostd is one
// long-lived unprivileged daemon serving every user. This process is
// ghost's equivalent of xrdp's sesexec, and the only root one.
mod devices;
mod pam_session;
mod wraith;

use std::net::IpAddr;
use std::os::fd::FromRawFd;
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use ipc::control::SessionInit;
use ipc::framing::{read_frame, read_frame_max, write_frame};
use ipc::ghostseat::{
    ghostseat_envelope::Msg, event, CloseReply, Event, GhostseatEnvelope, GhostseatError, MintTokenRequest, Open,
    Opened, SessionEnded, StatusReply, TokenMinted, ViewerAttached, ViewerDetached,
};
use ipc::paths::{seat_socket, EVENTS_SOCKET, GROUP};
use tokio::net::{UnixListener, UnixStream};
use tokio::signal::unix::{signal, SignalKind};
use tokio::time::timeout;
use rand::RngCore;
use tracing::{debug, error, info, warn};
use tracing_subscriber::EnvFilter;
use zeroize::Zeroizing;

/// ghostd's configuration file, for the one key ghostseat needs.
const GHOSTD_CONFIG: &str = "/etc/ghost/ghostd.toml";

use pam_session::{open_session, OpenedSession};
use wraith::{Request, StartupError};

// ghostd writes the Open right after connecting; anything slower is a
// client that isn't ghostd.
const OPEN_READ_TIMEOUT: Duration = Duration::from_secs(10);
// Open carries a password and a 32-byte secret; the pre-auth cap is
// plenty.
const MAX_OPEN_FRAME: u32 = 16 * 1024;
// pam_systemd's own D-Bus call to logind times out at 25s; this bounds
// everything else pam_open_session might block on (another module, or
// logind accepting but never replying). pam_open_session can't be
// cancelled, so the deadline fails the whole start.
const PAM_OPEN_TIMEOUT: Duration = Duration::from_secs(30);
// PAM_MAX_RESP_SIZE is 512; anything past that isn't a password PAM
// could have accepted.
const MAX_AUTHTOK: usize = 512;
// How long a STATUS/CLOSE client gets to send its one frame.
const SEAT_READ_TIMEOUT: Duration = Duration::from_secs(5);
// The username goes into `systemctl --user --machine=<name>@` (wraith.rs),
// which systemd parses as user@host: a name with `@` (an sssd
// fully-qualified name) would address a different machine, and `:` is
// its other separator. Only the portable character set is accepted;
// LOGIN_NAME_MAX is 256.
const MAX_USERNAME_LEN: usize = 256;
// The session secret the redirect tokens are minted with (gdp-spec.md
// §4.8): generated here, never by ghostd, which only carries the tokens.
const SESSION_SECRET_LEN: usize = 32;

fn username_byte(b: u8) -> bool {
    b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-' | b'$')
}

/// Whether a seat-socket connection is ghostd's: root, or a process
/// whose primary group is `ghost` (ghostd runs as ghost:ghost). The
/// socket is root:ghost 0660 already; this is what the mode is for,
/// stated once more where the requests are read.
fn peer_is_ghostd(stream: &UnixStream) -> bool {
    let ghost_gid = nix::unistd::Group::from_name(GROUP).ok().flatten().map(|g| g.gid.as_raw());
    match stream.peer_cred() {
        Ok(cred) if cred.uid() == 0 || Some(cred.gid()) == ghost_gid => true,
        Ok(cred) => {
            warn!(peer_uid = cred.uid(), peer_gid = cred.gid(), "ghostseat: refusing a seat-socket connection");
            false
        }
        Err(e) => {
            warn!(error = %e, "ghostseat: could not read a seat client's credentials; refusing it");
            false
        }
    }
}

/// Everything the session is, once open.
struct Seat {
    uid: u32,
    username: String,
    session_type: String,
    session_secret: Zeroizing<Vec<u8>>,
    port: u16,
    cert_sha256: String,
    opened: OpenedSession,
    viewer_attached: std::sync::atomic::AtomicBool,
}

/// Why an Open failed: the message for ghostd's log, and a gdp-spec.md
/// §12 code for its LobbyError when wraith named one.
struct StartFailure {
    message: String,
    code: u32,
}

impl From<anyhow::Error> for StartFailure {
    fn from(e: anyhow::Error) -> StartFailure {
        let code = e.downcast_ref::<StartupError>().map(|s| s.code as u32).unwrap_or(0);
        StartFailure { message: format!("{e:#}"), code }
    }
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> ExitCode {
    // stderr, never stdout: in an Accept=yes instance stdout defaults to
    // the connection itself.
    tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_env_filter(EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")))
        .init();

    // Holds the login password until the keyring unlock is done: keep it
    // out of core dumps (and away from ptrace).
    // Safety: prctl(PR_SET_DUMPABLE) takes plain integers and touches no memory.
    unsafe { libc::prctl(libc::PR_SET_DUMPABLE, 0, 0, 0, 0) };

    if !nix::unistd::geteuid().is_root() {
        error!("ghostseat: must run as root (from ghostseat.socket)");
        return ExitCode::FAILURE;
    }
    // Safety: systemd hands an Accept=yes instance its connection as fd 0
    // and nothing else in this process owns or closes that fd.
    let std_stream = unsafe { std::os::unix::net::UnixStream::from_raw_fd(0) };
    let mut stream = match std_stream.set_nonblocking(true).and_then(|()| UnixStream::from_std(std_stream)) {
        Ok(stream) => stream,
        Err(e) => {
            error!(error = %e, "ghostseat: fd 0 is not a usable socket");
            return ExitCode::FAILURE;
        }
    };

    let open: Open = match timeout(OPEN_READ_TIMEOUT, read_frame_max::<GhostseatEnvelope>(&mut stream, MAX_OPEN_FRAME)).await {
        Ok(Ok(GhostseatEnvelope { msg: Some(Msg::Open(open)) })) => open,
        Ok(Ok(other)) => {
            error!(?other, "ghostseat: expected Open");
            return ExitCode::FAILURE;
        }
        Ok(Err(e)) => {
            error!(error = %e, "ghostseat: reading Open failed");
            return ExitCode::FAILURE;
        }
        Err(_) => {
            error!("ghostseat: no Open within {OPEN_READ_TIMEOUT:?}");
            return ExitCode::FAILURE;
        }
    };

    let (uid, username) = (open.uid, open.username.clone());
    match start(open).await {
        Ok((seat, seat_listener, control_listener)) => {
            // The Open's ticket was spent in validate(); this is the one
            // token it buys.
            let (token, token_expiry_unix) = seat.mint_token();
            let reply = Opened {
                session_id: seat.opened.session_id.clone(),
                port: seat.port as u32,
                cert_sha256: seat.cert_sha256.clone(),
                token,
                token_expiry_unix,
            };
            if let Err(e) = write_frame(&mut stream, &GhostseatEnvelope { msg: Some(Msg::Opened(reply)) }).await {
                // ghostd gone mid-start: the session is up all the same;
                // ghostd finds it again through STATUS.
                warn!(uid, username, error = %e, "ghostseat: ghostd didn't take the Opened reply");
            }
            drop(stream);
            info!(uid, username, session_id = %seat.opened.session_id, port = seat.port, "ghostseat: session open");
            serve(Arc::new(seat), seat_listener, control_listener).await;
            ExitCode::SUCCESS
        }
        Err(failure) => {
            error!(uid, username, error = %failure.message, "ghostseat: session start failed");
            let reply = GhostseatError { message: failure.message, code: failure.code };
            let _ = write_frame(&mut stream, &GhostseatEnvelope { msg: Some(Msg::Error(reply)) }).await;
            ExitCode::FAILURE
        }
    }
}

/// Open through wraith's SessionReady. Anything that fails after a step
/// took effect undoes it: the logind session is closed, the sockets
/// removed.
async fn start(mut open: Open) -> Result<(Seat, UnixListener, UnixListener), StartFailure> {
    let authtok = Zeroizing::new(std::mem::take(&mut open.authtok));
    validate(&open)?;
    let uid = open.uid;
    let username = open.username.clone();

    // ghostd's uid lock keeps two logins for one user apart; this catches
    // a ghostd that lost track (its restart) rather than yanking the
    // sockets out from under a live session.
    if probe_seat(uid).await {
        return Err(anyhow::anyhow!("a session is already open for uid {uid}").into());
    }
    let seat_listener = bind_seat_socket(uid)?;
    let control_listener = match wraith::bind_control_socket(uid) {
        Ok(listener) => listener,
        Err(e) => {
            remove_seat_socket(uid);
            return Err(e.into());
        }
    };

    let authtok = if authtok.is_empty() {
        None
    } else if authtok.len() > MAX_AUTHTOK {
        warn!(uid, username, "ghostseat: password longer than PAM allows; the wallet will stay locked");
        None
    } else {
        Some(authtok)
    };
    let opened = match open_session_with_deadline(uid, &username, &open.client_ip, authtok) {
        Ok(opened) => opened,
        Err(e) => {
            remove_seat_socket(uid);
            wraith::remove_control_socket(uid);
            return Err(e.into());
        }
    };
    info!(uid, username, session_id = %opened.session_id, "ghostseat: logind session open");

    devices::start_monitor(uid);
    devices::log_availability();
    // Shared with wraith alone: ghostd, which carries the tokens minted
    // from it, never holds it, so a compromised ghostd can attach to a
    // session only with a ticket.
    let mut session_secret = Zeroizing::new(vec![0u8; SESSION_SECRET_LEN]);
    rand::rng().fill_bytes(&mut session_secret);
    let init = SessionInit {
        session_secret: session_secret.to_vec(),
        port_range_start: open.port_range_start,
        port_range_end: open.port_range_end,
        session_type: open.session_type.clone(),
        uinput_available: devices::available(),
        sessions_dir: trusted_sessions_dir(),
        uhid_available: devices::uhid_available(),
    };
    let ready = match wraith::bring_up(uid, &username, &opened.session_env, &control_listener, init).await {
        Ok(ready) => ready,
        Err(e) => {
            // No desktop, no session: the next login starts over.
            opened.close().await;
            terminate_session_leftovers();
            remove_seat_socket(uid);
            wraith::remove_control_socket(uid);
            return Err(e.into());
        }
    };
    let seat = Seat {
        uid,
        username,
        session_type: open.session_type,
        session_secret,
        port: ready.port,
        cert_sha256: ready.cert_sha256,
        opened,
        viewer_attached: Default::default(),
    };
    Ok((seat, seat_listener, control_listener))
}

impl Seat {
    /// A redirect token for this session (gdp-spec.md §4.8), valid
    /// SESSION_TOKEN_TTL_SECS from now. Only ghostseat holds the secret,
    /// so only it mints; each call must be paid for with a spent ticket.
    fn mint_token(&self) -> (String, i64) {
        let expiry = ipc::unix_now() + authticket::SESSION_TOKEN_TTL_SECS;
        (authticket::mint_session_token(self.uid, &self.session_secret, expiry), expiry)
    }
}

/// Verifies and spends a ticket for `username` from `client_ip`, as
/// `validate` does for an Open.
fn spend_ticket(ticket: &str, username: &str, client_ip: &str) -> Result<()> {
    client_ip.parse::<IpAddr>().with_context(|| format!("client_ip {client_ip:?} is not an IP address"))?;
    let key_path = Path::new(authticket::KEY_PATH);
    authticket::ensure_key(key_path).context("creating the ticket key")?;
    let key = authticket::Key::load(key_path).context("loading the ticket key")?;
    let nonce = key
        .verify(ticket, authticket::HOST_SERVICE, username, client_ip, ipc::unix_now())
        .context("authentication ticket refused")?;
    authticket::spend(Path::new(authticket::SPENT_DIR), &nonce).context("authentication ticket refused")
}

/// Everything in the Open ghostd is trusted for, checked: the ticket
/// settles that `username` authenticated from `client_ip`; the uid must
/// be that user's; the rest must be well-formed.
fn validate(open: &Open) -> Result<()> {
    if open.username.is_empty() || open.username.len() > MAX_USERNAME_LEN || !open.username.bytes().all(username_byte) {
        bail!("unusable username {:?}: [A-Za-z0-9._$-] only", open.username);
    }
    let user = nix::unistd::User::from_name(&open.username).context("looking up the user")?
        .with_context(|| format!("no such user: {}", open.username))?;
    if user.uid.as_raw() != open.uid {
        bail!("uid {} is not {}'s (that is {})", open.uid, open.username, user.uid);
    }
    spend_ticket(&open.ticket, &open.username, &open.client_ip)?;
    if open.session_type.is_empty()
        || !open.session_type.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_' || b == b'.')
    {
        bail!("unusable session type {:?}", open.session_type);
    }
    if open.port_range_start == 0
        || open.port_range_start > 0xffff
        || open.port_range_end > 0xffff
        || open.port_range_start > open.port_range_end
    {
        bail!("bad port range {}-{}", open.port_range_start, open.port_range_end);
    }
    Ok(())
}

/// The `[sessions] dir` keys ghostseat reads from ghostd.toml and its
/// drop-ins; everything else in the file is ghostd's and ignored.
#[derive(Default, serde::Deserialize)]
#[serde(default)]
struct GhostdConfig {
    sessions: SessionsConfig,
}

#[derive(serde::Deserialize)]
#[serde(default)]
struct SessionsConfig {
    dir: PathBuf,
}

impl Default for SessionsConfig {
    fn default() -> Self {
        SessionsConfig { dir: PathBuf::from("/etc/ghost/sessions.d") }
    }
}

/// SessionInit.sessions_dir: ghostd.toml's `sessions.dir`, read here from
/// the root-owned config rather than taken from ghostd's Open, because
/// wraith execs the Exec of whatever profile it finds there as the user.
/// Empty, so wraith falls back to its built-in directories, when the
/// config can't be read, the directory doesn't exist, or it is not
/// root-owned and free of group and other write permission.
fn trusted_sessions_dir() -> String {
    let dir = match tomlconf::load::<GhostdConfig>(Path::new(GHOSTD_CONFIG), false, true) {
        Ok((config, _)) => config.sessions.dir,
        Err(e) => {
            warn!(error = %format!("{e:#}"), "ghostseat: not reading sessions.dir from ghostd.toml; wraith uses its built-in directories");
            return String::new();
        }
    };
    match std::fs::metadata(&dir) {
        Ok(meta) if !meta.is_dir() => {
            warn!(dir = %dir.display(), "ghostseat: sessions.dir is not a directory; wraith uses its built-in directories");
            String::new()
        }
        Ok(meta) if meta.uid() != 0 || meta.mode() & 0o022 != 0 => {
            warn!(dir = %dir.display(), uid = meta.uid(), mode = format!("{:o}", meta.mode() & 0o7777),
                "ghostseat: sessions.dir must be root-owned and not writable by group or others; wraith uses its built-in directories");
            String::new()
        }
        Ok(_) => dir.to_string_lossy().into_owned(),
        Err(_) => String::new(),
    }
}

/// Whether an instance already answers STATUS for `uid`.
async fn probe_seat(uid: u32) -> bool {
    let Ok(mut stream) = UnixStream::connect(seat_socket(uid)).await else { return false };
    let probe = async {
        write_frame(&mut stream, &GhostseatEnvelope { msg: Some(Msg::Status(Default::default())) }).await?;
        read_frame::<GhostseatEnvelope>(&mut stream).await?;
        Ok::<(), anyhow::Error>(())
    };
    matches!(timeout(SEAT_READ_TIMEOUT, probe).await, Ok(Ok(())))
}

/// Binds `/run/ghost/<uid>-seat.sock` root:ghost 0660, for ghostd.
fn bind_seat_socket(uid: u32) -> Result<UnixListener> {
    let path = seat_socket(uid);
    let _ = std::fs::remove_file(&path);
    let listener = UnixListener::bind(&path).with_context(|| format!("binding {}", path.display()))?;
    let group = nix::unistd::Group::from_name(GROUP).context("looking up the ghost group")?
        .with_context(|| format!("no group {GROUP}; is ghostd installed?"))?;
    nix::unistd::chown(&path, None, Some(group.gid)).with_context(|| format!("chgrp {}", path.display()))?;
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o660))
        .with_context(|| format!("chmod {}", path.display()))?;
    Ok(listener)
}

fn remove_seat_socket(uid: u32) {
    let _ = std::fs::remove_file(seat_socket(uid));
}

/// Runs `open_session` on this thread, the main one, under
/// `PAM_OPEN_TIMEOUT`. Not on a blocking worker: pam_loginuid writes
/// /proc/self/loginuid, which the kernel lets only the thread-group
/// leader do (EPERM from any other thread), and this process's audit
/// identity is set there. Nothing else needs the runtime meanwhile (the
/// sockets are bound but not served until `serve`), so blocking it is
/// harmless; the deadline is a watchdog thread, since no task can fire
/// one here. A PAM stack that hangs ends the process instead: ghostd
/// sees the connection close and fails the login the same way.
fn open_session_with_deadline(
    uid: u32,
    username: &str,
    client_ip: &str,
    authtok: Option<Zeroizing<Vec<u8>>>,
) -> Result<OpenedSession> {
    let (done_tx, done_rx) = std::sync::mpsc::channel::<()>();
    let (username_owned, deadline) = (username.to_string(), PAM_OPEN_TIMEOUT);
    std::thread::spawn(move || {
        if let Err(std::sync::mpsc::RecvTimeoutError::Timeout) = done_rx.recv_timeout(deadline) {
            error!(uid, username = %username_owned,
                "ghostseat: pam_open_session did not return within {deadline:?}; giving up");
            std::process::exit(1);
        }
    });
    let result = open_session(username, client_ip, authtok);
    drop(done_tx);
    match result {
        Ok(opened) => Ok(opened),
        Err(reason) => bail!("could not open a logind session for {username} (uid {uid}): {reason}"),
    }
}

/// How the serve loop ended.
enum End {
    /// wraith.service stopped (SessionEnded from its ExecStopPost).
    DesktopEnded(String),
    /// ghostd's CLOSE; the reply goes out once the session is closed.
    Close(UnixStream),
    Sigterm,
}

/// Serves ghostd and wraith until the session ends, then closes it.
async fn serve(seat: Arc<Seat>, seat_listener: UnixListener, control_listener: UnixListener) {
    let mut sigterm = match signal(SignalKind::terminate()) {
        Ok(s) => s,
        Err(e) => {
            error!(error = %e, "ghostseat: failed to install SIGTERM handler");
            finish(seat, End::Sigterm).await;
            return;
        }
    };
    // Session modules can leave children behind (pam_kwallet5's auto_start
    // forks one), and ghostseat is their parent for the whole session.
    // Reaped only here, after pam_open_session has returned and before
    // close: a module that waitpid()s its own child during open or close
    // must not find it already taken.
    let mut sigchld = match signal(SignalKind::child()) {
        Ok(s) => Some(s),
        Err(e) => {
            warn!(error = %e, "ghostseat: failed to install SIGCHLD handler; exited children won't be reaped");
            None
        }
    };
    reap_children();

    let end = loop {
        tokio::select! {
            Some(()) = async { match sigchld.as_mut() { Some(s) => s.recv().await, None => None } } => {
                reap_children();
            }
            accepted = seat_listener.accept() => {
                match accepted {
                    Ok((stream, _)) if !peer_is_ghostd(&stream) => {}
                    Ok((stream, _)) => {
                        if let Some(end) = handle_seat_request(&seat, stream).await {
                            break end;
                        }
                    }
                    Err(e) => warn!(error = %e, "ghostseat: seat-socket accept failed"),
                }
            }
            accepted = control_listener.accept() => {
                match accepted {
                    Ok((stream, _)) if !wraith::peer_is_session_user(&stream, seat.uid) => {}
                    Ok((stream, _)) => {
                        if let Some(end) = handle_control_request(&seat, stream).await {
                            break end;
                        }
                    }
                    Err(e) => warn!(error = %e, "ghostseat: control-socket accept failed"),
                }
            }
            _ = sigterm.recv() => {
                info!("ghostseat: SIGTERM, closing the logind session");
                break End::Sigterm;
            }
        }
    };
    finish(seat, end).await;
}

/// One STATUS or CLOSE on a fresh connection from ghostd.
async fn handle_seat_request(seat: &Arc<Seat>, mut stream: UnixStream) -> Option<End> {
    let env: GhostseatEnvelope = match timeout(SEAT_READ_TIMEOUT, read_frame(&mut stream)).await {
        Ok(Ok(env)) => env,
        Ok(Err(e)) => {
            warn!(error = %e, "ghostseat: reading a seat request failed");
            return None;
        }
        Err(_) => {
            warn!("ghostseat: seat client sent nothing within {SEAT_READ_TIMEOUT:?}");
            return None;
        }
    };
    match env.msg {
        Some(Msg::Status(_)) => {
            let reply = StatusReply {
                uid: seat.uid,
                username: seat.username.clone(),
                session_id: seat.opened.session_id.clone(),
                opened_at: seat.opened.opened_at,
                session_type: seat.session_type.clone(),
                port: seat.port as u32,
                cert_sha256: seat.cert_sha256.clone(),
                viewer_attached: seat.viewer_attached.load(std::sync::atomic::Ordering::Relaxed),
            };
            if let Err(e) = write_frame(&mut stream, &GhostseatEnvelope { msg: Some(Msg::StatusReply(reply)) }).await {
                warn!(error = %e, "ghostseat: writing the STATUS reply failed");
            }
            None
        }
        Some(Msg::MintToken(MintTokenRequest { username, client_ip, ticket })) => {
            // A reattach: the same proof an Open needs, spent the same way,
            // and the ticket must be this session's user's.
            let checked = if username != seat.username {
                Err(anyhow::anyhow!("ticket is for {username:?}, this session is {:?}'s", seat.username))
            } else {
                spend_ticket(&ticket, &username, &client_ip)
            };
            let reply = match checked {
                Ok(()) => {
                    info!(uid = seat.uid, username, client_ip, "ghostseat: token minted for a reattaching login");
                    let (token, expiry_unix) = seat.mint_token();
                    Msg::TokenMinted(TokenMinted { token, expiry_unix })
                }
                Err(e) => {
                    warn!(uid = seat.uid, username, client_ip, error = %format!("{e:#}"), "ghostseat: refusing to mint a token");
                    Msg::Error(GhostseatError { message: format!("{e:#}"), code: 0 })
                }
            };
            if let Err(e) = write_frame(&mut stream, &GhostseatEnvelope { msg: Some(reply) }).await {
                warn!(error = %e, "ghostseat: writing the MintToken reply failed");
            }
            None
        }
        Some(Msg::Close(close)) => {
            if close.logout {
                wraith::logout(&seat.username).await;
            }
            Some(End::Close(stream))
        }
        other => {
            warn!(?other, "ghostseat: unexpected seat request");
            let reply = GhostseatError { message: "expected StatusRequest, CloseRequest or MintTokenRequest".to_string(), code: 0 };
            let _ = write_frame(&mut stream, &GhostseatEnvelope { msg: Some(Msg::Error(reply)) }).await;
            None
        }
    }
}

/// One request from wraith on a fresh control connection.
async fn handle_control_request(seat: &Arc<Seat>, stream: UnixStream) -> Option<End> {
    match wraith::read_request(stream).await? {
        Request::SessionEnded { result, exit_code, exit_status } => {
            info!(uid = seat.uid, username = %seat.username, result, exit_code, exit_status,
                "ghostseat: wraith reported the desktop session ended");
            Some(End::DesktopEnded(format!("wraith.service stopped ({result})")))
        }
        Request::CreateDevice(stream, req) => {
            // Its own task, alive as long as the device: the relay runs
            // in it, and devices.rs caps and replaces them by slot.
            tokio::spawn(devices::handle_create(stream, seat.uid, seat.username.clone(), req));
            None
        }
        Request::ViewerAttached => {
            info!(uid = seat.uid, username = %seat.username, "ghostseat: a viewer attached");
            seat.viewer_attached.store(true, std::sync::atomic::Ordering::Relaxed);
            send_event(seat, event::Kind::ViewerAttached(ViewerAttached {})).await;
            None
        }
        Request::ViewerDetached => {
            info!(uid = seat.uid, username = %seat.username, "ghostseat: the viewer detached");
            seat.viewer_attached.store(false, std::sync::atomic::Ordering::Relaxed);
            send_event(seat, event::Kind::ViewerDetached(ViewerDetached {})).await;
            None
        }
    }
}

/// Closes the logind session and reports the end to ghostd.
async fn finish(seat: Arc<Seat>, end: End) {
    let seat = Arc::try_unwrap(seat).unwrap_or_else(|_| panic!("device tasks hold no Seat"));
    let Seat { uid, username, opened, .. } = seat;
    let reason = match &end {
        End::DesktopEnded(reason) => reason.clone(),
        End::Close(_) => "closed by ghostd".to_string(),
        End::Sigterm => "SIGTERM".to_string(),
    };
    opened.close().await;
    terminate_session_leftovers();
    remove_seat_socket(uid);
    wraith::remove_control_socket(uid);
    info!(uid, username, reason, "ghostseat: session closed");
    if let End::Close(mut stream) = end {
        if let Err(e) = write_frame(&mut stream, &GhostseatEnvelope { msg: Some(Msg::Closed(CloseReply {})) }).await {
            warn!(uid, username, error = %e, "ghostseat: writing the CLOSE reply failed");
        }
    }
    let seat_for_event = EventSource { uid, username };
    seat_for_event.send(event::Kind::SessionEnded(SessionEnded { reason })).await;
}

/// Who an Event is about.
struct EventSource {
    uid: u32,
    username: String,
}

impl EventSource {
    /// One connection to ghostd's events socket per event, no reply. A
    /// ghostd that isn't there (restarting) misses it and catches up from
    /// STATUS.
    async fn send(&self, kind: event::Kind) {
        let event = Event { uid: self.uid, username: self.username.clone(), kind: Some(kind) };
        let send = async {
            let mut stream = UnixStream::connect(EVENTS_SOCKET).await?;
            write_frame(&mut stream, &GhostseatEnvelope { msg: Some(Msg::Event(event)) }).await
        };
        if let Err(e) = timeout(SEAT_READ_TIMEOUT, send).await.unwrap_or_else(|_| Err(anyhow::anyhow!("timed out"))) {
            debug!(uid = self.uid, error = %e, "ghostseat: event not delivered to ghostd");
        }
    }
}

async fn send_event(seat: &Seat, kind: event::Kind) {
    EventSource { uid: seat.uid, username: seat.username.clone() }.send(kind).await;
}

/// SIGTERMs whatever the session modules left in this logind session's
/// scope. The desktop itself runs under the user manager, so the only
/// things in here besides ghostseat are module children -- pam_kwallet5's
/// `ksecretd --pam-login` in particular, which never exits on its own
/// (it waits on the session bus, which the user manager keeps up for as
/// long as this session exists). With logind's default
/// KillUserProcesses=no one such process would pin the session at
/// State=closing -- and with it user@<uid>.service -- after every logout.
fn terminate_session_leftovers() {
    let Some(scope) = std::fs::read_to_string("/proc/self/cgroup")
        .ok()
        .and_then(|s| s.lines().find_map(|l| l.strip_prefix("0::").map(str::to_string)))
    else {
        return;
    };
    let procs = match std::fs::read_to_string(format!("/sys/fs/cgroup{scope}/cgroup.procs")) {
        Ok(procs) => procs,
        Err(e) => {
            warn!(%scope, error = %e, "ghostseat: can't list the session scope's processes");
            return;
        }
    };
    let me = std::process::id() as libc::pid_t;
    for pid in procs.lines().filter_map(|l| l.parse::<libc::pid_t>().ok()).filter(|&p| p != me) {
        info!(pid, "ghostseat: terminating a process left in the session scope");
        // Safety: kill() takes plain integers and touches no memory.
        unsafe { libc::kill(pid, libc::SIGTERM) };
    }
}

fn reap_children() {
    // Safety: waitpid with a null status pointer writes nothing.
    while unsafe { libc::waitpid(-1, std::ptr::null_mut(), libc::WNOHANG) } > 0 {}
}
