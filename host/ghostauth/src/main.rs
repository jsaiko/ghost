// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// ghostauth: one PAM authenticate + acct_mgmt transaction for ghostd or
// veild (docs/design/login-and-sessions.md#authentication), run as the
// `ghostauth` user, whose one privilege is CAP_DAC_READ_SEARCH (its unit
// grants it, for /etc/shadow). Never run it
// by hand: systemd's ghostauth.socket accepts each connection to
// /run/ghost/auth.sock and starts one ghostauth@.service instance with
// the connection as fd 0; the instance handles that one login and exits.
//
// The protocol is host/proto/ghostauth.proto: the caller sends Start,
// every PAM prompt becomes a Prompt answered by an Answer, and the
// helper ends with Done, carrying a ticket (host/authticket) on success.
// libpam's conversation callbacks are synchronous, so this is plain
// blocking I/O on the one socket, with no event loop.
//
// The helper keeps no copy of any answer. The caller holds everything it
// relayed, and keeps the first echo-off answer for the wallet unlock.
use std::ffi::{CStr, CString};
use std::net::IpAddr;
use std::os::fd::FromRawFd;
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::process::ExitCode;
use std::time::{SystemTime, UNIX_EPOCH};

use ipc::framing::{read_frame_sync, write_frame_sync};
use ipc::ghostauth::{auth_envelope::Msg, AuthEnvelope, Done, Prompt, Start};
use pam_client2::{Context, ConversationHandler, ErrorCode, Flag};
use tracing::{error, info, warn};
use tracing_subscriber::EnvFilter;
use zeroize::Zeroizing;

/// Every frame from the caller, which relays an unauthenticated peer's
/// bytes: the lobbies' own pre-authentication cap.
const MAX_FRAME: u32 = 16 * 1024;

/// The /etc/pam.d files a caller may name. Anything else is refused, so a
/// caller cannot point the helper at another service's stack.
const SERVICES: [&str; 2] = ["ghostd", "veild"];

const MAX_USERNAME_LEN: usize = 256;

fn main() -> ExitCode {
    // stderr, never stdout: in an Accept=yes instance stdout defaults to
    // the connection itself.
    tracing_subscriber::fmt()
        .with_writer(std::io::stderr)
        .with_env_filter(EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")))
        .init();

    // Holds passwords on their way into libpam: keep them out of core
    // dumps (and away from ptrace).
    // Safety: prctl(PR_SET_DUMPABLE) takes plain integers and touches no memory.
    unsafe { libc::prctl(libc::PR_SET_DUMPABLE, 0, 0, 0, 0) };

    // Safety: systemd hands an Accept=yes instance its connection as fd 0
    // and nothing else in this process owns or closes that fd.
    let stream = unsafe { UnixStream::from_raw_fd(0) };
    match run(stream) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            error!(error = %e, "ghostauth: no answer sent");
            ExitCode::FAILURE
        }
    }
}

/// Err only when the caller got no Done: a bad first frame, or a socket
/// that failed before Done could be written.
fn run(mut stream: UnixStream) -> Result<(), String> {
    let first: AuthEnvelope = read_frame_sync(&mut stream, MAX_FRAME).map_err(|e| format!("reading Start: {e}"))?;
    let start = match first.msg {
        Some(Msg::Start(start)) => start,
        _ => return Err("the first frame is not a Start".into()),
    };

    let done = match validate(&start) {
        Err(reason) => {
            warn!(service = %start.service, error = %reason, "ghostauth: refusing Start");
            Done { ok: false, error: reason, ticket: String::new() }
        }
        Ok(service) => {
            let conv_stream = stream.try_clone().map_err(|e| format!("dup of the connection: {e}"))?;
            login(service, &start, conv_stream)
        }
    };
    send(&mut stream, Msg::Done(done)).map_err(|e| format!("writing Done: {e}"))
}

/// The PAM service to open, or why the Start is refused.
fn validate(start: &Start) -> Result<&'static str, String> {
    let service = SERVICES
        .into_iter()
        .find(|s| *s == start.service)
        .ok_or_else(|| format!("service {:?} is not allowed", start.service))?;
    let username = &start.username;
    if username.is_empty() {
        return Err("empty username".into());
    }
    if username.len() > MAX_USERNAME_LEN {
        return Err(format!("username longer than {MAX_USERNAME_LEN} bytes"));
    }
    // NUL is a control character too.
    if username.chars().any(char::is_control) {
        return Err("control character in the username".into());
    }
    if !start.rhost.is_empty() && start.rhost.parse::<IpAddr>().is_err() {
        return Err(format!("rhost {:?} is not an IP address", start.rhost));
    }
    Ok(service)
}

/// Runs the transaction and turns its outcome into the Done to send.
fn login(service: &'static str, start: &Start, stream: UnixStream) -> Done {
    let failed = |error: String| Done { ok: false, error, ticket: String::new() };
    if let Err(e) = authenticate(service, start, stream) {
        info!(service, username = %start.username, rhost = %start.rhost, error = %e, "ghostauth: authentication failed");
        return failed(e);
    }
    let key = match authticket::Key::load(Path::new(authticket::KEY_PATH)) {
        Ok(key) => key,
        Err(e) => {
            // A login that can't yield a ticket can't open a session.
            error!(error = format!("{e:#}"), "ghostauth: cannot load the ticket key");
            return failed(format!("ticket key unavailable: {e:#}"));
        }
    };
    let now = SystemTime::now().duration_since(UNIX_EPOCH).map_or(0, |d| d.as_secs() as i64);
    let ticket = key.mint(service, &start.username, &start.rhost, now + authticket::TTL_SECS);
    info!(service, username = %start.username, rhost = %start.rhost, "ghostauth: authenticated");
    Done { ok: true, error: String::new(), ticket }
}

fn authenticate(service: &'static str, start: &Start, stream: UnixStream) -> Result<(), String> {
    // The username is preset, so the stack only prompts for credentials.
    let mut context = Context::new(service, Some(&start.username), SocketConversation { stream })
        .map_err(|e| format!("pam_start: {e}"))?;
    if !start.rhost.is_empty() {
        context.set_rhost(Some(&start.rhost)).map_err(|e| format!("setting PAM_RHOST: {e}"))?;
    }
    // The same flag sshd passes for PermitEmptyPasswords=no. pam_unix
    // applies it after its own arguments, so it overrides `nullok`.
    let flags = if start.permit_empty_passwords { Flag::NONE } else { Flag::DISALLOW_NULL_AUTHTOK };
    context.authenticate(flags).map_err(|e| e.to_string())?;
    context.acct_mgmt(Flag::NONE).map_err(|e| e.to_string())?;
    Ok(())
}

fn send(stream: &mut UnixStream, msg: Msg) -> Result<(), String> {
    write_frame_sync(stream, &AuthEnvelope { msg: Some(msg) }).map_err(|e| e.to_string())
}

/// Each prompt is one Prompt/Answer round trip with the caller. Any
/// failure, including the caller hanging up, is PAM_CONV_ERR, which fails
/// the transaction: that is how a dropped login is cancelled.
struct SocketConversation {
    stream: UnixStream,
}

impl SocketConversation {
    fn ask(&mut self, prompt: &CStr, echo: bool) -> Result<CString, ErrorCode> {
        let message = prompt.to_string_lossy().into_owned();
        if let Err(e) = send(&mut self.stream, Msg::Prompt(Prompt { message, echo })) {
            warn!(error = %e, "ghostauth: writing Prompt failed");
            return Err(ErrorCode::CONV_ERR);
        }
        let reply: AuthEnvelope = read_frame_sync(&mut self.stream, MAX_FRAME).map_err(|e| {
            warn!(error = %e, "ghostauth: reading Answer failed");
            ErrorCode::CONV_ERR
        })?;
        let response = match reply.msg {
            Some(Msg::Answer(answer)) => Zeroizing::new(answer.response),
            _ => {
                warn!("ghostauth: expected an Answer");
                return Err(ErrorCode::CONV_ERR);
            }
        };
        to_cstring(&response).ok_or(ErrorCode::CONV_ERR)
    }
}

/// `response` as the C string libpam gets, built in one exact-size
/// buffer so no unwiped copy is left behind by a reallocation. pam-client2
/// strdup()s it for libpam and drops this one, which clears only its
/// first byte; libpam wipes and frees its own copy.
fn to_cstring(response: &str) -> Option<CString> {
    let mut bytes = Vec::with_capacity(response.len() + 1);
    bytes.extend_from_slice(response.as_bytes());
    CString::new(bytes).ok()
}

impl ConversationHandler for SocketConversation {
    fn prompt_echo_on(&mut self, prompt: &CStr) -> Result<CString, ErrorCode> {
        self.ask(prompt, true)
    }

    fn prompt_echo_off(&mut self, prompt: &CStr) -> Result<CString, ErrorCode> {
        self.ask(prompt, false)
    }

    // Kept out of the caller's protocol, as the lobbies always have.
    fn text_info(&mut self, msg: &CStr) {
        info!(pam_message = %msg.to_string_lossy(), "PAM info");
    }

    fn error_msg(&mut self, msg: &CStr) {
        warn!(pam_message = %msg.to_string_lossy(), "PAM error");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn start(service: &str, username: &str, rhost: &str) -> Start {
        Start { service: service.into(), username: username.into(), rhost: rhost.into(), permit_empty_passwords: false }
    }

    #[test]
    fn validates_start() {
        assert_eq!(validate(&start("ghostd", "alice", "192.0.2.1")), Ok("ghostd"));
        assert_eq!(validate(&start("veild", "alice", "2001:db8::1")), Ok("veild"));
        assert_eq!(validate(&start("ghostd", "alice", "")), Ok("ghostd"));
        assert!(validate(&start("sshd", "alice", "")).is_err());
        assert!(validate(&start("ghostd\0", "alice", "")).is_err());
        assert!(validate(&start("", "alice", "")).is_err());
        assert!(validate(&start("ghostd", "", "")).is_err());
        assert!(validate(&start("ghostd", "al\0ice", "")).is_err());
        assert!(validate(&start("ghostd", "al\nice", "")).is_err());
        assert!(validate(&start("ghostd", &"a".repeat(MAX_USERNAME_LEN), "")).is_ok());
        assert!(validate(&start("ghostd", &"a".repeat(MAX_USERNAME_LEN + 1), "")).is_err());
        assert!(validate(&start("ghostd", "alice", "example.com")).is_err());
    }

    fn conversation() -> (SocketConversation, UnixStream) {
        let (ours, caller) = UnixStream::pair().unwrap();
        (SocketConversation { stream: ours }, caller)
    }

    fn reply(caller: &mut UnixStream, msg: Msg) {
        write_frame_sync(caller, &AuthEnvelope { msg: Some(msg) }).unwrap();
    }

    fn prompt_sent(caller: &mut UnixStream) -> Prompt {
        match read_frame_sync::<AuthEnvelope>(caller, MAX_FRAME).unwrap().msg {
            Some(Msg::Prompt(p)) => p,
            other => panic!("expected a Prompt, got {other:?}"),
        }
    }

    #[test]
    fn relays_a_prompt() {
        let (mut conv, mut caller) = conversation();
        reply(&mut caller, Msg::Answer(ipc::ghostauth::Answer { response: "hunter2".into() }));
        let answer = conv.prompt_echo_off(c"Password: ").unwrap();
        assert_eq!(answer.as_bytes(), b"hunter2");
        let p = prompt_sent(&mut caller);
        assert_eq!((p.message.as_str(), p.echo), ("Password: ", false));
    }

    #[test]
    fn fails_the_conversation() {
        // The wrong frame type.
        let (mut conv, mut caller) = conversation();
        reply(&mut caller, Msg::Start(start("ghostd", "alice", "")));
        assert_eq!(conv.prompt_echo_on(c"Token: ").unwrap_err(), ErrorCode::CONV_ERR);

        // A NUL in the response.
        let (mut conv, mut caller) = conversation();
        reply(&mut caller, Msg::Answer(ipc::ghostauth::Answer { response: "a\0b".into() }));
        assert_eq!(conv.prompt_echo_off(c"Password: ").unwrap_err(), ErrorCode::CONV_ERR);

        // The caller hanging up.
        let (mut conv, caller) = conversation();
        drop(caller);
        assert_eq!(conv.prompt_echo_off(c"Password: ").unwrap_err(), ErrorCode::CONV_ERR);
    }

    #[test]
    fn refuses_a_bad_start() {
        let (ours, mut caller) = UnixStream::pair().unwrap();
        reply(&mut caller, Msg::Start(start("login", "alice", "")));
        run(ours).unwrap();
        match read_frame_sync::<AuthEnvelope>(&mut caller, MAX_FRAME).unwrap().msg {
            Some(Msg::Done(done)) => assert!(!done.ok && done.ticket.is_empty() && done.error.contains("login")),
            other => panic!("expected Done, got {other:?}"),
        }
    }

    #[test]
    fn needs_start_first() {
        let (ours, mut caller) = UnixStream::pair().unwrap();
        reply(&mut caller, Msg::Answer(ipc::ghostauth::Answer { response: "x".into() }));
        assert!(run(ours).is_err());
    }
}
