// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The PAM session stack. ghostd decided the login on its own stack before
// spawning ghostseat.
//
// packaging/system/pam.d/ghostseat still has an auth stack, for two
// reasons: Context::open_session() resolves pam_setcred through it, and
// it is where the keyring modules pick up the password to unlock the
// user's wallet at session open, as in a display manager's stack
// (docs/design/login-and-sessions.md#wallet-and-keyring-unlock). That only
// works within one PAM handle, which is why ghostd hands the password over
// rather than unlocking anything itself. Nothing here can fail the login:
// every auth line but pam_permit is optional, and an authenticate()
// failure is only logged.
use std::ffi::{CStr, CString};

use pam_client2::{Context, ConversationHandler, ErrorCode, Flag, SessionToken};
use tracing::{info, warn};
use zeroize::Zeroizing;

/// Answers the auth stack's password prompt (pam_unix's, which is what
/// sets PAM_AUTHTOK for the keyring modules after it) with the password
/// ghostd passed on, and nothing else. Emptied after authenticate() so
/// the copy doesn't live as long as the session does.
pub struct Conversation {
    authtok: Option<Zeroizing<Vec<u8>>>,
}

impl ConversationHandler for Conversation {
    fn prompt_echo_on(&mut self, _prompt: &CStr) -> Result<CString, ErrorCode> {
        Err(ErrorCode::CONV_ERR)
    }

    fn prompt_echo_off(&mut self, _prompt: &CStr) -> Result<CString, ErrorCode> {
        let authtok = self.authtok.as_ref().ok_or(ErrorCode::CONV_ERR)?;
        CString::new(authtok.to_vec()).map_err(|_| ErrorCode::CONV_ERR)
    }

    fn text_info(&mut self, msg: &CStr) {
        info!(pam_message = %msg.to_string_lossy(), "ghostseat: PAM info");
    }

    fn error_msg(&mut self, msg: &CStr) {
        warn!(pam_message = %msg.to_string_lossy(), "ghostseat: PAM error");
    }
}

/// A logind session held open by a leaked `pam_client2::Session`. Kept
/// alive for the life of the process; `close()` is the only way to end it
/// (SIGTERM or a CLOSE request -- see main.rs).
pub struct OpenedSession {
    pub session_id: String,
    pub opened_at: i64,
    /// StatusReply.session_env (host/proto/ghostseat.proto).
    pub session_env: Vec<String>,
    context: Context<Conversation>,
    token: SessionToken,
}

impl OpenedSession {
    /// Drops the PAM session (`pam_close_session` + `pam_setcred(DELETE_CRED)`,
    /// via `Session`'s own `Drop`) and then the PAM handle itself
    /// (`pam_end`, via `Context`'s `Drop`). Runs on a blocking thread
    /// (`pam_close_session` can also talk to logind, and libpam's C calls
    /// are all synchronous); unlike the open, nothing here needs the main
    /// thread.
    pub async fn close(self) {
        let mut context = self.context;
        let token = self.token;
        let _ = tokio::task::spawn_blocking(move || {
            drop(context.unleak_session(token));
        })
        .await;
    }
}

/// Synchronous (libpam's calls are C), and run on the main thread, which
/// pam_loginuid needs (main.rs, open_session_with_deadline).
/// `Err` carries a human-readable reason for the login-failure log line --
/// the caller only needs to log and exit, never branch on it.
pub fn open_session(
    username: &str,
    client_ip: &str,
    authtok: Option<Zeroizing<Vec<u8>>>,
) -> Result<OpenedSession, String> {
    let have_authtok = authtok.is_some();
    let mut context = Context::new("ghostseat", Some(username), Conversation { authtok })
        .map_err(|e| format!("pam_start: {e}"))?;

    // -> pam_systemd sets Remote=yes for a non-loopback address.
    context.set_rhost(Some(client_ip)).map_err(|e| format!("setting PAM_RHOST: {e}"))?;

    // pam_systemd takes Type/Class from these before any PAM_TTY or
    // display heuristics, so the session is Type=wayland Class=user.
    context.putenv("XDG_SESSION_TYPE=wayland").map_err(|e| format!("putenv XDG_SESSION_TYPE: {e}"))?;
    context.putenv("XDG_SESSION_CLASS=user").map_err(|e| format!("putenv XDG_SESSION_CLASS: {e}"))?;

    // Only with a password: without one there is nothing for the keyring
    // modules to unlock with, and pam_unix would just fail on the
    // conversation.
    if have_authtok {
        if let Err(e) = context.authenticate(Flag::SILENT) {
            warn!(username, error = %e, "ghostseat: keyring auth pass failed; the wallet will stay locked");
        }
        context.conversation_mut().authtok = None;
    }

    let session = context.open_session(Flag::NONE).map_err(|e| format!("pam_open_session: {e}"))?;

    // pam_systemd silently no-ops (PAM_SUCCESS, no session) when the
    // calling process is already inside a session or user slice --
    // XDG_SESSION_ID appearing in the PAM environment afterwards is the
    // only reliable signal a session actually got created.
    let session_id = match session.getenv("XDG_SESSION_ID") {
        Some(id) => id.to_string(),
        None => {
            drop(session); // closes the no-op session cleanly, if there was one
            return Err(
                "no XDG_SESSION_ID after pam_open_session (pam_systemd did not create a \
                 session -- is ghostd itself running inside a user session or slice?)"
                    .to_string(),
            );
        }
    };

    // The XDG_* variables describe this logind session, not the user
    // manager every session of the uid shares, so they stay out.
    let session_env = session
        .envlist()
        .iter()
        .filter_map(|item| item.as_cstr().to_str().ok().map(str::to_string))
        .filter(|entry| !entry.starts_with("XDG_"))
        .collect();

    let opened_at = ipc::unix_now();
    let token = session.leak();
    Ok(OpenedSession { session_id, opened_at, session_env, context, token })
}
