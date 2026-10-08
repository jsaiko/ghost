// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The broker lobby (docs/design/veil.md#the-broker-lobby, gdp-spec.md
// §5): spectre logs in to Veil exactly as it would to a host,
// against Veil's own PAM stack (through ghostauth, the pamconv crate),
// then picks one of the devices it is
// entitled to. veild opens a login stream on that host's channel
// (hosts.rs) and runs the host's lobby on the user's behalf: it answers
// the host's first echo-off prompt with the password it already holds and
// relays everything else -- further prompts, SessionList, SessionOpen --
// between the two, until the host's Redirect or LobbyError ends it.
//
// The first frame on a gdp/1 connection decides what it is: a
// LobbyEnvelope with a LobbyHello is a login, a session ControlEnvelope
// with a SessionHello is a gateway session (gateway.rs). The two can't be
// confused: field 1 of LobbyHello is a varint and field 1 of SessionHello
// a string, so protobuf refuses to decode either as the other.
use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use ipc::framing::{read_frame, read_frame_max, read_raw_frame_max, write_frame, FrameError};
use ipc::lobby::{
    lobby_envelope::Msg, AuthChallenge, Device, DeviceList, LobbyEnvelope, LobbyError, LobbyErrorCode, LobbyHello,
    Redirect, SessionType,
};
use pamconv::{AuthSession, Authtok};
use preauth::{Offense, StartupSlot};
use prost::Message;
use quinn::VarInt;
use tokio::time::{timeout_at, Instant};
use tracing::{info, warn};

use crate::hosts::OpenLoginError;
use crate::relay::{HostLogin, Step};

use crate::db::Device as DbDevice;
use crate::Veil;

const GDP_WIRE_VERSION: u32 = 1;
// As ghostd's lobby: frames before authentication are held to 16 KiB.
const PRE_AUTH_MAX_FRAME: u32 = 16 * 1024;

pub async fn serve(conn: quinn::Connection, veil: Arc<Veil>, slot: StartupSlot) {
    let grace = veil.config.auth.login_grace_time;
    let deadline = (!grace.is_zero()).then(|| Instant::now() + grace);
    let peer = gdpnet::canonical(conn.remote_address());

    let first = async {
        let (send, mut recv) = conn.accept_bi().await?;
        let frame = read_raw_frame_max(&mut recv, PRE_AUTH_MAX_FRAME).await?;
        Ok::<_, anyhow::Error>((send, recv, frame))
    };
    let (send, recv, frame) = match within(deadline, first).await {
        Some(Ok(first)) => first,
        Some(Err(e)) => {
            let code = e.downcast_ref::<FrameError>().and_then(FrameError::close_code).unwrap_or(0);
            veil.penalties.penalise(peer.ip(), Offense::NoAuth);
            conn.close(VarInt::from_u32(code), b"");
            return;
        }
        None => {
            veil.penalties.penalise(peer.ip(), Offense::GraceExceeded);
            conn.close(VarInt::from_u32(LobbyErrorCode::LobbyErrorAuthFailed as u32), b"");
            return;
        }
    };

    if let Ok(LobbyEnvelope { msg: Some(Msg::Hello(hello)) }) = LobbyEnvelope::decode(&frame[..]) {
        let result = run_lobby(send, recv, hello, &veil, peer, deadline, slot).await;
        let code = match &result {
            Ok(code) => *code,
            Err(e) => e.downcast_ref::<FrameError>().and_then(FrameError::close_code).unwrap_or(0),
        };
        if let Err(e) = result {
            warn!(%peer, error = %format!("{e:#}"), "lobby: login failed");
        }
        conn.close(VarInt::from_u32(code), b"");
        return;
    }
    if let Ok(ipc::session::ControlEnvelope { msg: Some(ipc::session::control_envelope::Msg::Hello(hello)) }) =
        ipc::session::ControlEnvelope::decode(&frame[..])
    {
        drop(slot);
        crate::gateway::serve(conn, send, recv, hello, veil, peer).await;
        return;
    }
    warn!(%peer, "lobby: first frame is neither a LobbyHello nor a SessionHello");
    veil.penalties.penalise(peer.ip(), Offense::NoAuth);
    conn.close(VarInt::from_u32(LobbyErrorCode::LobbyErrorMalformedFrame as u32), b"");
}

async fn within<F: std::future::IntoFuture>(deadline: Option<Instant>, f: F) -> Option<F::Output> {
    match deadline {
        Some(deadline) => timeout_at(deadline, f).await.ok(),
        None => Some(f.await),
    }
}

/// An authenticated Veil user.
struct User {
    name: String,
    groups: Vec<String>,
    authtok: Option<Authtok>,
}

// Returns the gdp-spec.md §12 code to close the connection with.
#[allow(clippy::too_many_arguments)]
async fn run_lobby(
    mut send: quinn::SendStream,
    mut recv: quinn::RecvStream,
    hello: LobbyHello,
    veil: &Veil,
    peer: SocketAddr,
    deadline: Option<Instant>,
    slot: StartupSlot,
) -> Result<u32> {
    // `?`, not `%`: peer-supplied, so a newline in them can't forge a log line.
    info!(%peer, client_id = ?hello.client_id, username = ?hello.username, "lobby: hello");
    if hello.protocol_version != GDP_WIRE_VERSION {
        return send_error(
            &mut send,
            LobbyErrorCode::LobbyErrorVersionMismatch,
            format!("server speaks GDP wire version {GDP_WIRE_VERSION}"),
        )
        .await;
    }

    let mut attempted = false;
    let outcome = within(deadline, authenticate(&mut send, &mut recv, &hello.username, veil, peer, &mut attempted)).await;
    drop(slot);
    let user = match outcome {
        None => {
            warn!(%peer, "lobby: login grace time exceeded before authentication finished");
            veil.penalties.penalise(peer.ip(), Offense::GraceExceeded);
            return Ok(LobbyErrorCode::LobbyErrorAuthFailed as u32);
        }
        Some(Err(e)) => {
            veil.penalties.penalise(peer.ip(), if attempted { Offense::AuthFail } else { Offense::NoAuth });
            return Err(e);
        }
        Some(Ok(Err(code))) => return Ok(code),
        Some(Ok(Ok(user))) => user,
    };
    let client = peer.ip().to_string();

    // The devices this user may use, running sessions first.
    let entitled = veil.db.entitled_devices(&user.name, &user.groups)?;
    let placements = veil.db.placements_for_user(&user.name)?;
    let mut devices: Vec<Device> = entitled
        .iter()
        .map(|d| {
            let session = placements.iter().find(|p| p.device_id == d.id);
            let types = veil.hosts.session_types(&d.id);
            // The user's last desktop on this host when it still offers it,
            // as a direct login's SessionList would have it (gdp-spec.md §4.6).
            let last = veil.db.last_type(&d.id, &user.name).ok().flatten();
            let default_type =
                last.filter(|t| types.available.iter().any(|(id, _)| id == t)).unwrap_or(types.default_type);
            Device {
                id: d.id.clone(),
                name: d.name.clone(),
                online: veil.hosts.is_online(&d.id),
                has_session: session.is_some(),
                session_type: session.map(|p| p.session_type.clone()).unwrap_or_default(),
                available_types: types.available.into_iter().map(|(id, name)| SessionType { id, name }).collect(),
                default_type,
            }
        })
        .collect();
    devices.sort_by(|a, b| {
        (b.has_session, b.online).cmp(&(a.has_session, a.online)).then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase()))
    });
    info!(%peer, username = %user.name, devices = devices.len(), "lobby: authenticated at Veil");
    write_frame(&mut send, &LobbyEnvelope { msg: Some(Msg::DeviceList(DeviceList { devices })) }).await?;

    let select: LobbyEnvelope = read_frame(&mut recv).await?;
    let Some(Msg::DeviceSelect(select)) = select.msg else {
        warn!(%peer, "lobby: expected DeviceSelect, got something else");
        return Ok(LobbyErrorCode::LobbyErrorMalformedFrame as u32);
    };
    let Some(device) = entitled.into_iter().find(|d| d.id == select.device_id) else {
        warn!(%peer, username = %user.name, device_id = %select.device_id, "lobby: device not entitled");
        veil.db.audit(&user.name, "login refused", None, &format!("not entitled to device {}", select.device_id), Some(&client));
        return send_error(&mut send, LobbyErrorCode::LobbyErrorNotEntitled, "you can't use that host".to_string()).await;
    };
    let Some(link) = veil.hosts.link(&device.id) else {
        info!(%peer, username = %user.name, device = %device.name, "lobby: device offline");
        veil.db.audit(&user.name, "login failed", Some(&device.id), "host offline", Some(&client));
        return send_error(&mut send, LobbyErrorCode::LobbyErrorHostOffline, format!("{} is offline", device.name)).await;
    };

    let relayed = relay(&mut send, &mut recv, &link, &device, &hello, user, veil, peer).await;
    match relayed {
        Ok(code) => Ok(code),
        Err(e) => {
            warn!(%peer, device = %device.name, error = %format!("{e:#}"), "lobby: relaying the login failed");
            let code = send_error(
                &mut send,
                LobbyErrorCode::LobbyErrorSessionStartFailed,
                format!("the login to {} failed", device.name),
            )
            .await
            .unwrap_or(LobbyErrorCode::LobbyErrorSessionStartFailed as u32);
            Ok(code)
        }
    }
}

// LobbyHello through PAM's answer and the root policy, as ghostd's lobby
// does it but against Veil's own stack ("veild"). Ok(Err(code)) is an
// ending that already sent its LobbyError.
async fn authenticate(
    send: &mut quinn::SendStream,
    recv: &mut quinn::RecvStream,
    username: &str,
    veil: &Veil,
    peer: SocketAddr,
    attempted: &mut bool,
) -> Result<std::result::Result<User, u32>> {
    let mut auth = AuthSession::start("veild", username.to_string(), peer.ip(), veil.config.auth.permit_empty_passwords).await;
    let result = loop {
        match auth.next_prompt().await {
            Some(req) => {
                let challenge = AuthChallenge { prompt: req.message.clone(), echo_input: req.echo };
                write_frame(send, &LobbyEnvelope { msg: Some(Msg::AuthChallenge(challenge)) }).await?;
                let env: LobbyEnvelope = read_frame_max(recv, PRE_AUTH_MAX_FRAME).await?;
                let Some(Msg::AuthResponse(response)) = env.msg else {
                    warn!(%peer, "lobby: expected AuthResponse, got something else");
                    let offense = if *attempted { Offense::AuthFail } else { Offense::NoAuth };
                    veil.penalties.penalise(peer.ip(), offense);
                    return Ok(Err(LobbyErrorCode::LobbyErrorMalformedFrame as u32));
                };
                *attempted = true;
                req.respond(response.response);
            }
            None => break auth.finish().await,
        }
    };
    // The ticket is ghostseat's concern on a host; Veil opens no session.
    let authtok = match result {
        Ok(verdict) => verdict.authtok,
        Err(e) => {
            warn!(%peer, ?username, error = %e, "lobby: PAM authentication failed");
            return deny(send, veil, username, peer).await.map(Err);
        }
    };
    let user = match nix::unistd::User::from_name(username) {
        Ok(Some(user)) => user,
        _ => {
            warn!(%peer, ?username, "lobby: authenticated user has no passwd entry");
            return deny(send, veil, username, peer).await.map(Err);
        }
    };
    if user.uid.is_root() && !veil.config.auth.permit_root_login {
        warn!(%peer, username, "lobby: refusing a root login (auth.permit_root_login is off)");
        return deny(send, veil, username, peer).await.map(Err);
    }
    Ok(Ok(User { groups: group_names(username, user.gid), name: username.to_string(), authtok }))
}

/// The names of every group `username` is in, through NSS (getgrouplist),
/// so groups from sssd/LDAP work as well as /etc/group.
pub fn group_names(username: &str, gid: nix::unistd::Gid) -> Vec<String> {
    let Ok(cname) = std::ffi::CString::new(username) else { return Vec::new() };
    let gids = nix::unistd::getgrouplist(&cname, gid).unwrap_or_else(|_| vec![gid]);
    gids.into_iter().filter_map(|g| nix::unistd::Group::from_gid(g).ok().flatten()).map(|g| g.name).collect()
}

async fn deny(send: &mut quinn::SendStream, veil: &Veil, username: &str, peer: SocketAddr) -> Result<u32> {
    veil.db.audit(username, "login failed", None, "authentication failed at Veil", Some(&peer.ip().to_string()));
    let code = send_error(send, LobbyErrorCode::LobbyErrorAuthFailed, "authentication failed".to_string()).await?;
    veil.penalties.penalise(peer.ip(), Offense::AuthFail);
    Ok(code)
}

// The relay: veild plays the client on a login stream to the host
// (relay.rs), spectre answering what Veil can't.
#[allow(clippy::too_many_arguments)]
async fn relay(
    send: &mut quinn::SendStream,
    recv: &mut quinn::RecvStream,
    link: &crate::hosts::HostLink,
    device: &DbDevice,
    hello: &LobbyHello,
    user: User,
    veil: &Veil,
    peer: SocketAddr,
) -> Result<u32> {
    let client = peer.ip().to_string();
    let mut host = match HostLogin::start(link, &client, &hello.client_id, &user.name, &user.name, user.authtok).await {
        Ok(host) => host,
        Err(OpenLoginError::TooManyLogins) => {
            warn!(%peer, username = %user.name, "lobby: too many logins in progress for this account");
            veil.db.audit(&user.name, "login refused", Some(&device.id), "too many logins in progress", Some(&client));
            return send_error(send, LobbyErrorCode::LobbyErrorHostFull, OpenLoginError::TooManyLogins.to_string()).await;
        }
        Err(OpenLoginError::Other(e)) => return Err(e),
    };
    loop {
        match host.next().await? {
            Step::Prompt { prompt, echo } => {
                let challenge = AuthChallenge { prompt, echo_input: echo };
                write_frame(send, &LobbyEnvelope { msg: Some(Msg::AuthChallenge(challenge)) }).await?;
                let answer: LobbyEnvelope = read_frame(recv).await?;
                let Some(Msg::AuthResponse(response)) = answer.msg else {
                    bail!("expected AuthResponse from the client");
                };
                host.answer(response.response).await?;
            }
            Step::SessionList(list) => {
                write_frame(send, &LobbyEnvelope { msg: Some(Msg::SessionList(list)) }).await?;
                let open: LobbyEnvelope = read_frame(recv).await?;
                let Some(Msg::SessionOpen(open)) = open.msg else {
                    bail!("expected SessionOpen from the client");
                };
                host.open(open).await?;
            }
            Step::Redirect(redirect) => {
                return redirect_client(send, redirect, device, &user.name, veil, peer).await;
            }
            Step::Refused { code, message } => {
                info!(%peer, username = %user.name, device = %device.name, code = code.as_str_name(), "lobby: the host refused the login");
                veil.db.audit(&user.name, "login failed", Some(&device.id), &format!("{}: {message}", code.as_str_name()), Some(&client));
                return send_error(send, code, message).await;
            }
            Step::Ended => {
                warn!(%peer, device = %device.name, "lobby: the host ended the login stream");
                veil.db.audit(&user.name, "login failed", Some(&device.id), "the host ended the login", Some(&client));
                return send_error(
                    send,
                    LobbyErrorCode::LobbyErrorSessionStartFailed,
                    format!("{} ended the login", device.name),
                )
                .await;
            }
        }
    }
}

// The host's Redirect, passed on: in direct mode with the host's client
// address filled in; in gateway mode replaced by one to Veil itself.
async fn redirect_client(
    send: &mut quinn::SendStream,
    mut redirect: Redirect,
    device: &DbDevice,
    username: &str,
    veil: &Veil,
    peer: SocketAddr,
) -> Result<u32> {
    let client = peer.ip().to_string();
    let via_gateway = crate::gateway::use_gateway(device.mode, peer.ip(), &veil.config)?;
    if via_gateway {
        redirect = crate::gateway::intercept(veil, redirect, device, username, peer).await?;
    } else if redirect.host.is_empty() {
        redirect.host = device.client_address.clone();
    }
    let how = if via_gateway { "gateway" } else { "direct" };
    info!(%peer, username, device = %device.name, how, "lobby: login redirected");
    veil.db.audit(username, "login", Some(&device.id), &format!("{how} session"), Some(&client));
    write_frame(send, &LobbyEnvelope { msg: Some(Msg::Redirect(redirect)) }).await?;
    finish_and_wait(send).await?;
    Ok(0)
}

async fn send_error(send: &mut quinn::SendStream, code: LobbyErrorCode, message: String) -> Result<u32> {
    let env = LobbyEnvelope { msg: Some(Msg::Error(LobbyError { code: code as i32, message })) };
    write_frame(send, &env).await?;
    finish_and_wait(send).await?;
    Ok(code as u32)
}

async fn finish_and_wait(send: &mut quinn::SendStream) -> Result<()> {
    send.finish()?;
    let _ = tokio::time::timeout(Duration::from_secs(5), send.stopped()).await.context("client never acknowledged")?;
    Ok(())
}
