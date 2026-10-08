// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// ghostlogin: the PAM account hook that lets a console login end the same
// user's ghost session
// (docs/design/login-and-sessions.md#one-graphical-login-per-user). Run by
// pam_exec, from common-account on Debian (packaging/system/pam-configs/)
// or the display managers' stacks elsewhere:
//
//   account optional pam_exec.so quiet /usr/local/lib/ghost/ghostlogin [SERVICE...]
//
// It asks ghostd (host/proto/ghostlogin.proto) to end PAM_USER's ghost
// session and waits until it is gone, so the local desktop never starts
// beside it. The account phase runs only after authentication succeeded,
// so a wrong password at the console ends nothing.
//
// It never refuses a login: every failure is logged and it exits 0. With
// ghostd unreachable it falls back to `loginctl terminate-session` on the
// user's ghostseat sessions.
//
// Only the services named as arguments count, or DEFAULT_SERVICES without
// any. On Debian the line goes into common-account, which every PAM
// service shares, and sudo inside a ghost session or ghostd's own login
// must not end it.
use std::ffi::CString;
use std::process::Command;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use ipc::framing::{read_frame, write_frame};
use ipc::ghostlogin::{local_login_envelope::Msg, LocalLoginEnvelope, LocalLoginRequest};
use ipc::paths::GHOSTLOGIN_SOCKET;
use serde::Deserialize;
use tokio::net::UnixStream;

/// The graphical display managers' PAM services.
const DEFAULT_SERVICES: &[&str] = &[
    "sddm",
    "sddm-autologin",
    "gdm-password",
    "gdm-autologin",
    "gdm-fingerprint",
    "gdm-smartcard",
    "lightdm",
    "lightdm-autologin",
];

/// ghostd's own worst case is about 20 s (session.rs's LOCAL_LOGIN_*
/// waits); past this the console login goes ahead regardless.
const GHOSTD_TIMEOUT: Duration = Duration::from_secs(25);

fn main() {
    // pam_exec throws stdout/stderr away; the journal is the only place
    // any of this can be seen.
    let ident = CString::new("ghostlogin").expect("no NUL");
    // Safety: `ident` lives until the process exits (openlog keeps the
    // pointer), and the flags are plain constants.
    unsafe { libc::openlog(ident.as_ptr(), libc::LOG_PID, libc::LOG_AUTHPRIV) };
    std::mem::forget(ident);

    let env = |name: &str| std::env::var(name).unwrap_or_default();
    let (pam_type, service, user, tty) = (env("PAM_TYPE"), env("PAM_SERVICE"), env("PAM_USER"), env("PAM_TTY"));
    if pam_type != "account" {
        log(libc::LOG_WARNING, &format!("run from the {pam_type:?} phase, not account; doing nothing"));
        return;
    }
    let args: Vec<String> = std::env::args().skip(1).collect();
    let applies = if args.is_empty() {
        DEFAULT_SERVICES.contains(&service.as_str())
    } else {
        args.iter().any(|a| *a == service)
    };
    if !applies || user.is_empty() {
        return;
    }
    if is_screen_unlock(&service, &tty) {
        return;
    }
    if let Err(e) = run(&user, &service, &tty) {
        log(libc::LOG_ERR, &format!("{user} ({service}): {e:#}; letting the login go ahead"));
    }
}

/// GNOME's lock screen checks the password through the `gdm-password`
/// service, the same one the greeter uses, so unlocking a ghost session
/// would otherwise look like a console login and end it. The greeter's
/// worker always has a tty (`/dev/tty1` on the console); the lock
/// screen's has none. Only `gdm-password`: no other
/// display manager's lock screen shares its login service.
fn is_screen_unlock(service: &str, tty: &str) -> bool {
    service == "gdm-password" && tty.is_empty()
}

fn run(user: &str, service: &str, tty: &str) -> Result<()> {
    let uid = match nix::unistd::User::from_name(user).context("looking up the user")? {
        Some(u) => u.uid.as_raw(),
        None => bail!("no such user"),
    };
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build()?;
    let result = runtime.block_on(async {
        tokio::time::timeout(GHOSTD_TIMEOUT, ask_ghostd(uid, service, tty))
            .await
            .unwrap_or_else(|_| Err(anyhow::anyhow!("ghostd did not answer within {GHOSTD_TIMEOUT:?}").into()))
    });
    match result {
        Ok(true) => log(libc::LOG_INFO, &format!("{user} ({service}): ended the user's ghost session")),
        Ok(false) => {}
        Err(GhostdError::Unreachable(e)) => {
            log(libc::LOG_WARNING, &format!("{user} ({service}): ghostd unreachable ({e:#}); ending ghostseat sessions through logind"));
            terminate_ghostseat_sessions(uid)?;
        }
        Err(GhostdError::Failed(e)) => return Err(e),
    }
    Ok(())
}

enum GhostdError {
    /// Nothing to talk to: fall back to logind.
    Unreachable(anyhow::Error),
    Failed(anyhow::Error),
}

impl From<anyhow::Error> for GhostdError {
    fn from(e: anyhow::Error) -> Self {
        GhostdError::Failed(e)
    }
}

async fn ask_ghostd(uid: u32, service: &str, tty: &str) -> std::result::Result<bool, GhostdError> {
    let mut stream = UnixStream::connect(GHOSTLOGIN_SOCKET).await.map_err(|e| {
        // Only "nobody is listening" means ghostd is down; anything else
        // (EACCES: not run as root) is a failure to report, not a reason
        // to go around ghostd.
        let unreachable = matches!(e.kind(), std::io::ErrorKind::NotFound | std::io::ErrorKind::ConnectionRefused);
        let e = anyhow::Error::new(e).context(GHOSTLOGIN_SOCKET);
        if unreachable { GhostdError::Unreachable(e) } else { GhostdError::Failed(e) }
    })?;
    let request = LocalLoginRequest { uid, service: service.to_string(), tty: tty.to_string() };
    write_frame(&mut stream, &LocalLoginEnvelope { msg: Some(Msg::Request(request)) })
        .await
        .context("sending the request")?;
    let env: LocalLoginEnvelope = read_frame(&mut stream).await.context("reading ghostd's reply")?;
    match env.msg {
        Some(Msg::Reply(reply)) if reply.error.is_empty() => Ok(reply.ended),
        Some(Msg::Reply(reply)) => Err(anyhow::anyhow!("ghostd: {}", reply.error).into()),
        _ => Err(anyhow::anyhow!("unexpected reply from ghostd").into()),
    }
}

#[derive(Deserialize)]
struct ListedSession {
    session: String,
    uid: u32,
}

/// Without ghostd: every logind session of `uid` that ghostseat opened.
/// Harder than ghostd's path (no logout, no word to the viewer), but the
/// two desktops still never overlap.
fn terminate_ghostseat_sessions(uid: u32) -> Result<()> {
    let listed = output(Command::new("loginctl").args(["list-sessions", "--json=short"]))?;
    let listed: Vec<ListedSession> = serde_json::from_str(&listed).context("parsing loginctl list-sessions")?;
    for s in listed.into_iter().filter(|s| s.uid == uid) {
        let service = output(Command::new("loginctl").args(["show-session", &s.session, "-p", "Service", "--value"]))?;
        if service.trim() == "ghostseat" {
            output(Command::new("loginctl").args(["terminate-session", &s.session]))?;
            log(libc::LOG_INFO, &format!("terminated ghost logind session {}", s.session));
        }
    }
    Ok(())
}

fn output(cmd: &mut Command) -> Result<String> {
    let out = cmd.output().with_context(|| format!("running {cmd:?}"))?;
    if !out.status.success() {
        bail!("{cmd:?} failed: {}", String::from_utf8_lossy(&out.stderr).trim());
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

fn log(priority: libc::c_int, message: &str) {
    let Ok(message) = CString::new(message) else { return };
    // Safety: "%s" with one NUL-terminated argument.
    unsafe { libc::syslog(priority, c"%s".as_ptr(), message.as_ptr()) };
}

#[cfg(test)]
mod tests {
    use super::is_screen_unlock;

    #[test]
    fn gdm_lock_screen_has_no_tty() {
        assert!(is_screen_unlock("gdm-password", ""));
        assert!(!is_screen_unlock("gdm-password", "/dev/tty1"));
        // Other services are never a screen unlock, whatever their tty.
        assert!(!is_screen_unlock("sddm", ""));
        assert!(!is_screen_unlock("lightdm", ""));
    }
}
