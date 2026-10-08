// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// PAM authentication over the network, shared by ghostd's lobby and
// veild's: one AuthChallenge/AuthResponse round trip per PAM prompt
// (gdp-spec.md §4.5). Neither daemon runs PAM itself: the transaction
// runs in a ghostauth instance (host/ghostauth, host/proto/ghostauth.proto)
// that systemd starts per connection to /run/ghost/auth.sock, as the
// unprivileged `ghostauth` user. This is the client side: Start, then
// each Prompt out as a `PromptRequest` and its answer back, then Done.
// The caller names its PAM service, "ghostd" or "veild"
// (packaging/system/pam.d/).
//
// This is authentication only, authenticate() + acct_mgmt(); a host's
// logind session is opened by ghostseat, and veild opens none.
//
// The session keeps a copy of the answer to the first echo-off prompt
// (`Authtok`), since Linux-PAM won't give applications PAM_AUTHTOK and
// the helper keeps nothing. On a host it goes to ghostseat to unlock the
// user's wallet (docs/design/login-and-sessions.md#wallet-and-keyring-unlock);
// veild uses it to answer the host's own password prompt. In every stack
// ghost ships, the first echo-off prompt is pam_unix's password prompt;
// in a stack that asks something else first, the copy is the wrong
// secret and only the wallet unlock fails.
use std::net::IpAddr;

use anyhow::{Context, Result};
use ipc::framing::{read_frame_max, secret_frame_bytes, write_frame, FrameError};
use ipc::ghostauth::{auth_envelope::Msg, Answer, AuthEnvelope, Start};
use ipc::paths::AUTH_SOCKET;
use tokio::net::UnixStream;
use tokio::sync::oneshot;
use tracing::warn;
use tokio::io::AsyncWriteExt;
use zeroize::{Zeroize, Zeroizing};

// The helper's own cap on what it reads; applied to what it sends too.
const MAX_FRAME: u32 = 16 * 1024;

/// The login password, wiped on drop and never printed.
pub struct Authtok(Zeroizing<Vec<u8>>);

impl Authtok {
    /// A password from somewhere other than a PAM conversation.
    pub fn new(bytes: Vec<u8>) -> Authtok {
        Authtok(Zeroizing::new(bytes))
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.0
    }

    /// The password as the text an AuthResponse carries. It came from an
    /// AuthResponse in the first place, so it is valid UTF-8.
    pub fn to_response(&self) -> Zeroizing<String> {
        Zeroizing::new(String::from_utf8_lossy(&self.0).into_owned())
    }
}

impl std::fmt::Debug for Authtok {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Authtok(<redacted>)")
    }
}

/// A successful authentication.
pub struct Verdict {
    /// ghostauth's proof for ghostseat (host/authticket). veild has no
    /// use for it.
    pub ticket: String,
    /// The password, if the stack asked for one (see the top of this
    /// file).
    pub authtok: Option<Authtok>,
}

/// Why an authentication didn't succeed: PAM's refusal, or the helper
/// being unreachable or breaking off. Either way the client gets the
/// same AUTH_FAILED; this is for the log.
#[derive(Debug)]
pub struct AuthError(String);

impl std::fmt::Display for AuthError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for AuthError {}

pub struct PromptRequest {
    pub message: String,
    pub echo: bool,
    reply: oneshot::Sender<String>,
}

impl PromptRequest {
    /// Feed spectre's `AuthResponse` back to the waiting PAM conversation.
    /// Dropping a `PromptRequest` without calling this (e.g. because the
    /// connection died) fails the conversation instead of hanging it:
    /// the session hangs up on the helper, whose read then fails.
    pub fn respond(self, response: String) {
        let _ = self.reply.send(response);
    }
}

/// A PAM authentication transaction in progress. Drive it by alternating
/// `next_prompt()` with sending its message to spectre and calling
/// `respond()` on the answer, until `next_prompt()` returns `None`; then
/// `finish()` for the outcome.
pub struct AuthSession {
    stream: Option<UnixStream>,
    // The prompt handed out last, until the caller answers it.
    awaiting: Option<(bool, oneshot::Receiver<String>)>,
    authtok: Option<Authtok>,
    outcome: Option<Result<String, AuthError>>,
}

impl AuthSession {
    /// Connects to the helper and sends Start. `service` is the
    /// /etc/pam.d file to use; `rhost` becomes PAM_RHOST;
    /// `permit_empty_passwords` is sshd's PermitEmptyPasswords: without
    /// it, an account whose password is empty fails authentication even
    /// where the PAM stack says `nullok` (Debian's common-auth does).
    /// A connect failure is reported by `finish()`, after `next_prompt()`
    /// has returned None at once, so every caller takes one path.
    pub async fn start(service: &'static str, username: String, rhost: IpAddr, permit_empty_passwords: bool) -> AuthSession {
        let stream = match UnixStream::connect(AUTH_SOCKET).await {
            Ok(stream) => stream,
            Err(e) => {
                warn!(path = AUTH_SOCKET, error = %e, "pamconv: cannot reach ghostauth; is ghostauth.socket active?");
                return AuthSession::failed(format!("connecting to {AUTH_SOCKET}: {e}"));
            }
        };
        AuthSession::start_on(stream, service, username, rhost.to_string(), permit_empty_passwords).await
    }

    /// `start()` on a connection already made (a test's socket pair).
    pub async fn start_on(
        mut stream: UnixStream,
        service: &'static str,
        username: String,
        rhost: String,
        permit_empty_passwords: bool,
    ) -> AuthSession {
        let start = Start { service: service.to_string(), username, rhost, permit_empty_passwords };
        if let Err(e) = write_frame(&mut stream, &AuthEnvelope { msg: Some(Msg::Start(start)) }).await {
            return AuthSession::failed(format!("sending Start to ghostauth: {e}"));
        }
        AuthSession { stream: Some(stream), awaiting: None, authtok: None, outcome: None }
    }

    fn failed(why: String) -> AuthSession {
        AuthSession { stream: None, awaiting: None, authtok: None, outcome: Some(Err(AuthError(why))) }
    }

    pub async fn next_prompt(&mut self) -> Option<PromptRequest> {
        if self.outcome.is_some() {
            return None;
        }
        if let Err(e) = self.flush_answer().await {
            self.end(Err(e));
            return None;
        }
        let stream = self.stream.as_mut().expect("a session with no outcome has its stream");
        let env: AuthEnvelope = match read_frame_max(stream, MAX_FRAME).await {
            Ok(env) => env,
            Err(FrameError::Io(e)) if e.kind() == std::io::ErrorKind::UnexpectedEof => {
                self.end(Err(AuthError("ghostauth ended without a verdict".to_string())));
                return None;
            }
            Err(e) => {
                self.end(Err(AuthError(format!("reading from ghostauth: {e}"))));
                return None;
            }
        };
        match env.msg {
            Some(Msg::Prompt(prompt)) => {
                let (reply, rx) = oneshot::channel();
                self.awaiting = Some((prompt.echo, rx));
                Some(PromptRequest { message: prompt.message, echo: prompt.echo, reply })
            }
            Some(Msg::Done(done)) if done.ok => {
                self.end(Ok(done.ticket));
                None
            }
            Some(Msg::Done(done)) => {
                self.end(Err(AuthError(done.error)));
                None
            }
            other => {
                self.end(Err(AuthError(format!("unexpected frame from ghostauth: {other:?}"))));
                None
            }
        }
    }

    // The last prompt's answer, once the caller has given it, to the
    // helper. Keeps the first echo-off answer as the Authtok. A prompt
    // dropped unanswered ends the session.
    async fn flush_answer(&mut self) -> Result<(), AuthError> {
        let Some((echo, rx)) = self.awaiting.take() else { return Ok(()) };
        let response = match rx.await {
            Ok(response) => Zeroizing::new(response),
            Err(_) => return Err(AuthError("the prompt was dropped unanswered".to_string())),
        };
        if !echo && self.authtok.is_none() {
            self.authtok = Some(Authtok::new(response.as_bytes().to_vec()));
        }
        let stream = self.stream.as_mut().expect("awaiting implies a stream");
        // The answer is often the password: framed into a buffer that is
        // wiped, and the message's own copy wiped once encoded.
        let mut env = AuthEnvelope { msg: Some(Msg::Answer(Answer { response: response.to_string() })) };
        let frame = secret_frame_bytes(&env);
        if let Some(Msg::Answer(answer)) = &mut env.msg {
            answer.response.zeroize();
        }
        let frame = frame.map_err(|e| AuthError(format!("framing an answer to ghostauth: {e}")))?;
        stream.write_all(&frame).await.map_err(|e| AuthError(format!("sending an answer to ghostauth: {e}")))
    }

    fn end(&mut self, outcome: Result<String, AuthError>) {
        self.outcome = Some(outcome);
        // Hanging up is what cancels a transaction still in progress.
        self.stream = None;
        self.awaiting = None;
    }

    /// Only meaningful after `next_prompt()` has returned `None`.
    pub async fn finish(mut self) -> Result<Verdict, AuthError> {
        if self.outcome.is_none() {
            // The caller skipped the loop; run it to the verdict (every
            // outstanding prompt is dropped, which fails the transaction).
            while self.next_prompt().await.is_some() {}
        }
        let ticket = self.outcome.take().expect("ended")?;
        Ok(Verdict { ticket, authtok: self.authtok.take() })
    }
}

/// What `ghostd -t` and `veild -t` check: the helper's socket is there,
/// so a missing ghostauth.socket fails the start the way a broken
/// configuration does. Connecting would start an instance for nothing,
/// so this only looks.
pub fn check_helper() -> Result<()> {
    use std::os::unix::fs::FileTypeExt;
    match std::fs::metadata(AUTH_SOCKET) {
        Ok(meta) if meta.file_type().is_socket() => Ok(()),
        Ok(_) => anyhow::bail!("{AUTH_SOCKET} is not a socket"),
        Err(e) => Err(e).with_context(|| format!("{AUTH_SOCKET} (is ghostauth.socket enabled?)")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ipc::ghostauth::{Done, Prompt};

    async fn read(s: &mut UnixStream) -> Msg {
        read_frame_max::<AuthEnvelope>(s, MAX_FRAME).await.unwrap().msg.unwrap()
    }

    async fn send(s: &mut UnixStream, msg: Msg) {
        write_frame(s, &AuthEnvelope { msg: Some(msg) }).await.unwrap();
    }

    #[tokio::test]
    async fn two_prompts_then_a_verdict() {
        let (ours, mut helper) = UnixStream::pair().unwrap();
        let mut auth = AuthSession::start_on(ours, "ghostd", "alice".into(), "192.0.2.1".into(), false).await;
        let fake = tokio::spawn(async move {
            match read(&mut helper).await {
                Msg::Start(s) => assert_eq!((s.service.as_str(), s.username.as_str(), s.rhost.as_str()), ("ghostd", "alice", "192.0.2.1")),
                other => panic!("{other:?}"),
            }
            send(&mut helper, Msg::Prompt(Prompt { message: "Token: ".into(), echo: true })).await;
            assert!(matches!(read(&mut helper).await, Msg::Answer(a) if a.response == "1234"));
            send(&mut helper, Msg::Prompt(Prompt { message: "Password: ".into(), echo: false })).await;
            assert!(matches!(read(&mut helper).await, Msg::Answer(a) if a.response == "hunter2"));
            send(&mut helper, Msg::Done(Done { ok: true, error: String::new(), ticket: "t".into() })).await;
        });
        let p = auth.next_prompt().await.unwrap();
        assert!(p.echo);
        p.respond("1234".into());
        let p = auth.next_prompt().await.unwrap();
        assert!(!p.echo);
        p.respond("hunter2".into());
        assert!(auth.next_prompt().await.is_none());
        let verdict = auth.finish().await.unwrap();
        assert_eq!(verdict.ticket, "t");
        assert_eq!(verdict.authtok.unwrap().as_bytes(), b"hunter2");
        fake.await.unwrap();
    }

    #[tokio::test]
    async fn a_refusal_and_a_hangup() {
        let (ours, mut helper) = UnixStream::pair().unwrap();
        let mut auth = AuthSession::start_on(ours, "veild", "bob".into(), String::new(), true).await;
        let fake = tokio::spawn(async move {
            let _ = read(&mut helper).await;
            send(&mut helper, Msg::Done(Done { ok: false, error: "bad password".into(), ticket: String::new() })).await;
        });
        assert!(auth.next_prompt().await.is_none());
        assert_eq!(auth.finish().await.map(|_| ()).unwrap_err().to_string(), "bad password");
        fake.await.unwrap();

        let (ours, helper) = UnixStream::pair().unwrap();
        let mut auth = AuthSession::start_on(ours, "ghostd", "bob".into(), String::new(), false).await;
        drop(helper);
        assert!(auth.next_prompt().await.is_none());
        assert!(auth.finish().await.is_err());
    }

    #[tokio::test]
    async fn an_unanswered_prompt_hangs_up() {
        let (ours, mut helper) = UnixStream::pair().unwrap();
        let mut auth = AuthSession::start_on(ours, "ghostd", "carol".into(), String::new(), false).await;
        let _ = read(&mut helper).await;
        send(&mut helper, Msg::Prompt(Prompt { message: "Password: ".into(), echo: false })).await;
        let prompt = auth.next_prompt().await.unwrap();
        drop(prompt);
        assert!(auth.next_prompt().await.is_none());
        assert!(auth.finish().await.is_err());
        // The helper sees EOF.
        assert!(read_frame_max::<AuthEnvelope>(&mut helper, MAX_FRAME).await.is_err());
    }
}
