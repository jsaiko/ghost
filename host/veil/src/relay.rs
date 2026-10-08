// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// A login on a host, with veild in the client's place
// (docs/design/veil.md#the-broker-lobby): the lobby exchange on a login
// stream over the host's channel, one step at a time. The QUIC lobby
// (lobby.rs) drives it with spectre on the other end; the browser portal
// (web/portal.rs) drives it across HTTP requests.
use anyhow::{bail, Result};
use ipc::framing::{read_frame, secret_frame_bytes, write_frame, FrameError};
use ipc::lobby::{
    lobby_envelope::Msg, AuthMethod, AuthResponse, LobbyEnvelope, LobbyErrorCode, LobbyHello, Redirect, SessionList,
    SessionOpen,
};
use pamconv::Authtok;
use zeroize::Zeroize;

use crate::hosts::{HostLink, LoginStream, OpenLoginError};

const GDP_WIRE_VERSION: u32 = 1;

/// What the host wants next.
pub enum Step {
    /// A prompt beyond the password Veil answered itself (a second
    /// factor); answer with `HostLogin::answer`.
    Prompt { prompt: String, echo: bool },
    /// Authentication is done; pick a session type with `HostLogin::open`.
    SessionList(SessionList),
    /// The host's redirect to the session. The login is over.
    Redirect(Redirect),
    /// The host refused. The login is over.
    Refused { code: LobbyErrorCode, message: String },
    /// The host ended the stream without a word. The login is over.
    Ended,
}

pub struct HostLogin {
    stream: LoginStream,
    authtok: Option<Authtok>,
    // Whether Veil answered the host's password prompt itself, which is
    // what makes a refusal from the host password drift
    // (HOST_AUTH_FAILED) rather than the user's own mistake.
    answered_password: bool,
}

impl HostLogin {
    /// Opens a login stream on `link` for the host account `username`,
    /// from a client at `forwarded_for`, on behalf of the Veil account
    /// `veil_user` (whose logins in flight are capped, hosts.rs).
    /// `authtok` answers the host's first echo-off prompt, and is wiped
    /// once used or once the host is past authentication.
    pub async fn start(
        link: &HostLink,
        forwarded_for: &str,
        client_id: &str,
        veil_user: &str,
        username: &str,
        authtok: Option<Authtok>,
    ) -> std::result::Result<HostLogin, OpenLoginError> {
        let mut stream = link.open_login(forwarded_for, client_id, veil_user).await?;
        let hello = LobbyHello {
            protocol_version: GDP_WIRE_VERSION,
            client_id: client_id.to_string(),
            auth_method: AuthMethod::Password as i32,
            username: username.to_string(),
        };
        write_frame(&mut stream.send, &LobbyEnvelope { msg: Some(Msg::Hello(hello)) })
            .await
            .map_err(OpenLoginError::Other)?;
        Ok(HostLogin { stream, authtok, answered_password: false })
    }

    /// Reads until the host needs something from the user, or is done.
    /// Once it is done (Ended, Refused, Redirect, or an error) the
    /// stream's places on the host and for the account are given back,
    /// whatever the caller does with this object afterwards.
    pub async fn next(&mut self) -> Result<Step> {
        let step = self.next_step().await;
        if !matches!(step, Ok(Step::Prompt { .. }) | Ok(Step::SessionList(_))) {
            self.stream.release();
        }
        step
    }

    async fn next_step(&mut self) -> Result<Step> {
        loop {
            let env: LobbyEnvelope = match read_frame(&mut self.stream.recv).await {
                Ok(env) => env,
                // The host reset the stream (its close code for an ending
                // that sent no LobbyError) or the channel dropped.
                Err(FrameError::Io(_)) => return Ok(Step::Ended),
                Err(e) => return Err(e.into()),
            };
            match env.msg {
                Some(Msg::AuthChallenge(c)) if !c.echo_input && self.authtok.is_some() => {
                    // The first echo-off prompt gets the password Veil
                    // holds; it never outlives this answer.
                    let authtok = self.authtok.take().expect("checked above");
                    write_secret_response(&mut self.stream.send, authtok).await?;
                    self.answered_password = true;
                }
                Some(Msg::AuthChallenge(c)) => return Ok(Step::Prompt { prompt: c.prompt, echo: c.echo_input }),
                Some(Msg::SessionList(list)) => {
                    // Past authentication: the password has done its job.
                    self.authtok = None;
                    return Ok(Step::SessionList(list));
                }
                Some(Msg::Redirect(r)) => {
                    let _ = self.stream.send.finish();
                    return Ok(Step::Redirect(r));
                }
                Some(Msg::Error(e)) => {
                    let _ = self.stream.send.finish();
                    let code = LobbyErrorCode::try_from(e.code).unwrap_or(LobbyErrorCode::LobbyErrorUnspecified);
                    return Ok(match code {
                        LobbyErrorCode::LobbyErrorAuthFailed if self.answered_password => Step::Refused {
                            code: LobbyErrorCode::LobbyErrorHostAuthFailed,
                            message: "the host rejected your password; it may differ from your Veil password".into(),
                        },
                        _ => Step::Refused { code, message: e.message },
                    });
                }
                other => bail!("unexpected message from the host: {other:?}"),
            }
        }
    }

    pub async fn answer(&mut self, response: String) -> Result<()> {
        let env = LobbyEnvelope { msg: Some(Msg::AuthResponse(AuthResponse { response })) };
        write_frame(&mut self.stream.send, &env).await
    }

    pub async fn open(&mut self, open: SessionOpen) -> Result<()> {
        write_frame(&mut self.stream.send, &LobbyEnvelope { msg: Some(Msg::SessionOpen(open)) }).await
    }
}

// Writes the password as an AuthResponse from a buffer that is wiped
// afterwards, rather than through write_frame's ordinary Vec.
async fn write_secret_response(send: &mut quinn::SendStream, authtok: Authtok) -> Result<()> {
    let mut env = LobbyEnvelope {
        msg: Some(Msg::AuthResponse(AuthResponse { response: authtok.to_response().to_string() })),
    };
    let body = secret_frame_bytes(&env);
    if let Some(Msg::AuthResponse(r)) = &mut env.msg {
        r.response.zeroize();
    }
    drop(authtok);
    send.write_all(&body?).await?;
    Ok(())
}
