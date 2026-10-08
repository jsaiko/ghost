// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The browser client's login (docs/design/browser-client.md): the
// brokered login, driven over a small JSON API instead of a QUIC lobby
// stream. The page at / (host/veil/app/) works in two layers.
//
// Signing in to Veil: PAM checks the password once, a row in
// portal_sessions and a `veil_session` cookie say who is signed in, and
// the password is kept only sealed, its key in the `veil_key` cookie
// (remember.rs). Every state-changing call carries the session's CSRF
// token in X-Veil-CSRF.
//
//   POST /api/login                 {username, password}  -> devices + csrf
//   GET  /api/session                                      -> devices + csrf
//   POST /api/logout                                       -> signed_out
//
// Signing in to a host: with no password given, the remembered one goes
// to the host for the user's own account; a device marked `remembered:
// false` (none kept, or that host refused it) needs one typed.
//
//   POST /api/connect               {device_id, password?} -> next step
//   POST /api/flows/{id}/answer     {response}             -> next step
//   POST /api/flows/{id}/open       {session_type}         -> next step
//
// where a step is a prompt (a second factor the host asks for), the
// host's session list, an error, or "ready": a gateway token and the URLs
// to open the session at (WebTransport, or the WebSocket fallback). A
// browser can't speak GDP's QUIC, so its sessions always go through
// Veil's gateway, whatever the device's mode.
//
// A flow lives in veild's memory only, for at most five minutes, and its
// id is the only handle to it (it also has to be its user's). The password
// for a login, typed or unsealed, goes to the host's first prompt and is
// wiped there.
use std::collections::HashMap;
use std::net::{IpAddr, SocketAddr};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use axum::extract::{ConnectInfo, FromRequestParts, Path, State, WebSocketUpgrade};
use axum::http::request::Parts;
use axum::http::{header, HeaderMap, StatusCode};
use axum::response::{AppendHeaders, IntoResponse, Response};
use axum::Json;
use ipc::lobby::{LobbyErrorCode, SessionOpen};
use pamconv::Authtok;
use preauth::Offense;
use rand::RngCore;
use serde::{Deserialize, Serialize};
use serde_json::json;
use tracing::{info, warn};

use super::auth;
use super::remember;
use super::WebState;
use crate::db::Device;
use crate::hosts::{hex, OpenLoginError};
use crate::relay::{HostLogin, Step};

const FLOW_TTL: Duration = Duration::from_secs(300);

/// How often abandoned flows are swept (`Flows::sweep`); until then an
/// abandoned flow holds its login stream, and with it a place on the
/// host and one of the user's MAX_LOGINS_PER_USER.
const FLOW_SWEEP_INTERVAL: Duration = Duration::from_secs(30);

/// One host login in progress: opened by /api/connect, advanced by the
/// answer and open calls, gone when it ends or after FLOW_TTL.
pub struct Flow {
    /// Who is signed in to Veil: the only one who may advance this flow.
    username: String,
    /// The host account being logged in to (the user's own unless they
    /// typed another).
    host_user: String,
    client: IpAddr,
    /// The portal session's key, and whether the password came from what
    /// it remembers.
    session: String,
    remembered: bool,
    host: HostLogin,
    device: Device,
    expires: Instant,
}

#[derive(Default)]
pub struct Flows(Mutex<HashMap<String, Arc<tokio::sync::Mutex<Flow>>>>);

impl Flows {
    fn insert(&self, flow: Flow) -> String {
        let mut raw = [0u8; 16];
        rand::rng().fill_bytes(&mut raw);
        let id = hex(&raw);
        self.sweep();
        self.0.lock().unwrap_or_else(|p| p.into_inner()).insert(id.clone(), Arc::new(tokio::sync::Mutex::new(flow)));
        id
    }

    /// Drops every flow past its FLOW_TTL (a flow in use right now is
    /// left for the next sweep), which closes its login stream on the
    /// host. Run from `insert` and from the timer `spawn_sweeper` starts.
    pub fn sweep(&self) {
        let now = Instant::now();
        self.0.lock().unwrap_or_else(|p| p.into_inner()).retain(|_, f| f.try_lock().map_or(true, |f| f.expires > now));
    }

    /// Sweeps on a timer, so a flow the user walked away from doesn't
    /// hold its places until someone else starts a login.
    pub fn spawn_sweeper(self: &Arc<Self>) {
        let flows = self.clone();
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(FLOW_SWEEP_INTERVAL);
            loop {
                tick.tick().await;
                flows.sweep();
            }
        });
    }

    fn get(&self, id: &str) -> Option<Arc<tokio::sync::Mutex<Flow>>> {
        self.0.lock().unwrap_or_else(|p| p.into_inner()).get(id).cloned()
    }

    fn remove(&self, id: &str) {
        self.0.lock().unwrap_or_else(|p| p.into_inner()).remove(id);
    }
}

#[derive(Serialize)]
struct DeviceView {
    id: String,
    name: String,
    online: bool,
    has_session: bool,
    session_type: String,
    /// Veil can log in to it with the remembered password.
    remembered: bool,
}

fn error(code: LobbyErrorCode, message: impl Into<String>) -> Response {
    let status = match code {
        LobbyErrorCode::LobbyErrorAuthFailed => StatusCode::UNAUTHORIZED,
        LobbyErrorCode::LobbyErrorNotEntitled => StatusCode::FORBIDDEN,
        _ => StatusCode::OK,
    };
    let body = json!({"step": "error", "code": code as i32, "name": code.as_str_name(), "message": message.into()});
    (status, Json(body)).into_response()
}

// --- the user's sign-in to Veil ---

/// A browser user signed in to Veil: a live row in portal_sessions behind
/// the `veil_session` cookie. It proves who they are to Veil; a host login
/// also needs the password, typed or unsealed with the `veil_key` cookie.
/// A request without one gets 401 `signed_out`.
pub struct PortalUser {
    pub username: String,
    csrf: String,
    key: String,
}

fn signed_out() -> Response {
    (
        StatusCode::UNAUTHORIZED,
        Json(json!({"step": "signed_out", "message": "You are signed out; sign in again."})),
    )
        .into_response()
}

impl FromRequestParts<WebState> for PortalUser {
    type Rejection = Response;

    async fn from_request_parts(parts: &mut Parts, state: &WebState) -> std::result::Result<Self, Self::Rejection> {
        let Some(id) = auth::portal_cookie_value(&parts.headers) else { return Err(signed_out()) };
        let key = auth::session_key(&id);
        let web = &state.veil.config.web;
        match state.veil.db.portal_session(&key, web.portal_idle.as_secs() as i64, web.portal_max.as_secs() as i64) {
            Ok(Some(s)) => Ok(PortalUser { username: s.username, csrf: s.csrf, key }),
            Ok(None) => Err(signed_out()),
            Err(e) => Err(super::internal(e)),
        }
    }
}

impl PortalUser {
    /// Every state-changing request carries the session's CSRF token in a
    /// header (SameSite=Strict already keeps the cookie off cross-site
    /// requests; this is the second lock).
    fn check_csrf(&self, headers: &HeaderMap) -> std::result::Result<(), Response> {
        let given = headers.get(auth::CSRF_HEADER).and_then(|v| v.to_str().ok());
        if auth::csrf_ok(&self.csrf, given) {
            Ok(())
        } else {
            warn!(username = %self.username, "portal: CSRF token missing or wrong; refusing the request");
            Err((StatusCode::FORBIDDEN, Json(json!({"step": "error", "code": 0, "message": "The page was out of date; reload it."}))).into_response())
        }
    }
}

/// The host list for `username` with the page's bootstrap: who, and the
/// CSRF token its later calls need.
fn devices_response(state: &WebState, username: &str, csrf: &str, portal_key: &str, client: IpAddr) -> Response {
    let veil = &state.veil;
    let groups = state.auth.groups(username);
    let devices = match veil.db.entitled_devices(username, &groups) {
        Ok(d) => d,
        Err(e) => return super::internal(e),
    };
    let placements = veil.db.placements_for_user(username).unwrap_or_default();
    let mut views: Vec<DeviceView> = devices
        .iter()
        .map(|d| {
            let session = placements.iter().find(|p| p.device_id == d.id);
            DeviceView {
                id: d.id.clone(),
                name: d.name.clone(),
                online: veil.hosts.is_online(&d.id),
                has_session: session.is_some(),
                session_type: session.map(|p| p.session_type.clone()).unwrap_or_default(),
                remembered: state.passwords.usable(portal_key, client, &d.id),
            }
        })
        .collect();
    views.sort_by(|a, b| (b.has_session, b.online).cmp(&(a.has_session, a.online)).then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase())));
    let admin = state.auth.is_admin(username);
    Json(json!({"step": "devices", "user": username, "admin": admin, "csrf": csrf, "devices": views})).into_response()
}

/// Who is signed in and what they can reach; the page calls it on load
/// and whenever it returns to the host list.
pub async fn session(State(state): State<WebState>, ConnectInfo(peer): ConnectInfo<SocketAddr>, headers: HeaderMap, user: PortalUser) -> Response {
    let ip = auth::client_ip(peer, &headers, state.behind_proxy);
    devices_response(&state, &user.username, &user.csrf, &user.key, ip)
}

#[derive(Deserialize)]
pub struct LoginRequest {
    username: String,
    password: String,
}

/// Signs in to Veil: PAM against `/etc/pam.d/veild` with the lobby's
/// penalties. The password is checked, then sealed for the user's hosts
/// (remember.rs) unless `[web] remember_password` is 0.
pub async fn login(
    State(state): State<WebState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Json(req): Json<LoginRequest>,
) -> Response {
    let veil = &state.veil;
    let ip = auth::client_ip(peer, &headers, state.behind_proxy);
    if veil.penalties.refuses(ip) {
        return (
            StatusCode::TOO_MANY_REQUESTS,
            Json(json!({"step": "error", "code": LobbyErrorCode::LobbyErrorAuthFailed as i32, "message": "Too many failed logins from your address; try again later."})),
        )
            .into_response();
    }
    let username = req.username.trim().to_string();
    let authtok = if username.is_empty() || username.len() > 256 {
        None
    } else {
        state.auth.authenticate_keep(&username, &req.password, ip).await
    };
    drop(req);
    let Some(authtok) = authtok else {
        veil.penalties.penalise(ip, Offense::AuthFail);
        veil.db.audit(&username, "login failed", None, "authentication failed at Veil (browser)", Some(&ip.to_string()));
        return error(LobbyErrorCode::LobbyErrorAuthFailed, "Wrong username or password.");
    };
    let signed_in = match start_session(&state, ip, &headers, &username, &authtok, "browser") {
        Ok(s) => s,
        Err(r) => return r,
    };
    drop(authtok);
    let mut response = devices_response(&state, &username, &signed_in.csrf, &signed_in.key, ip);
    signed_in.set_cookies(response.headers_mut());
    response
}

/// A new sign-in to Veil.
pub struct SignedIn {
    pub csrf: String,
    key: String,
    session_cookie: header::HeaderValue,
    key_cookie: header::HeaderValue,
}

impl SignedIn {
    pub fn set_cookies(&self, headers: &mut HeaderMap) {
        headers.append(header::SET_COOKIE, self.session_cookie.clone());
        headers.append(header::SET_COOKIE, self.key_cookie.clone());
    }
}

/// Opens the portal session both sign-in pages share (the browser client's
/// and the admin UI's) for a user PAM has just accepted: a new id (what
/// the browser held is retired), and the password sealed for their hosts
/// unless `[web] remember_password` is 0.
pub fn start_session(
    state: &WebState,
    ip: IpAddr,
    headers: &HeaderMap,
    username: &str,
    authtok: &Authtok,
    via: &str,
) -> std::result::Result<SignedIn, Response> {
    let veil = &state.veil;
    if let Some(old) = auth::portal_cookie_value(headers) {
        let old = auth::session_key(&old);
        let _ = veil.db.remove_portal_session(&old);
        state.passwords.forget(&old);
    }
    let (id, csrf) = auth::new_session();
    let key = auth::session_key(&id);
    veil.db.add_portal_session(&key, username, &csrf).map_err(super::internal)?;
    let web = &veil.config.web;
    let remember = web.remember_password.min(web.portal_max);
    let sealed = (!remember.is_zero()).then(|| state.passwords.remember(&key, username, ip, authtok, remember));
    info!(%ip, username, via, remembered = sealed.is_some(), "portal: signed in to Veil");
    veil.db.audit(username, "sign in", None, via, Some(&ip.to_string()));
    Ok(SignedIn {
        csrf,
        key,
        session_cookie: auth::set_portal_cookie(&id, web.portal_max.as_secs()),
        key_cookie: match &sealed {
            Some(k) => remember::set_key_cookie(k, remember.as_secs()),
            None => remember::clear_key_cookie(),
        },
    })
}

/// What ending a sign-in clears.
pub fn clear_cookies() -> AppendHeaders<[(header::HeaderName, header::HeaderValue); 2]> {
    AppendHeaders([(header::SET_COOKIE, auth::clear_portal_cookie()), (header::SET_COOKIE, remember::clear_key_cookie())])
}

pub async fn logout(State(state): State<WebState>, headers: HeaderMap, user: PortalUser) -> Response {
    if let Err(r) = user.check_csrf(&headers) {
        return r;
    }
    let _ = state.veil.db.remove_portal_session(&user.key);
    state.passwords.forget(&user.key);
    info!(username = %user.username, "portal: signed out of Veil");
    (clear_cookies(), Json(json!({"step": "signed_out"}))).into_response()
}

// --- a host login ---

#[derive(Deserialize)]
pub struct ConnectRequest {
    device_id: String,
    /// The host account to log in to; empty or absent is the user's own
    /// name. Who may reach the host is still decided by who is signed in
    /// to Veil, and the host checks this account's password itself.
    #[serde(default)]
    username: String,
    #[serde(default)]
    password: String,
}

/// A host account name the page may send: not empty, not absurd, no
/// control characters (the host's PAM is the judge of the rest).
fn valid_host_user(name: &str) -> bool {
    !name.is_empty() && name.len() <= 256 && !name.chars().any(|c| c.is_control())
}

/// Starts the login to one host with the password the user just typed, or
/// else the remembered one (own account only), which goes to the host's
/// first prompt and is gone after it.
pub async fn connect(
    State(state): State<WebState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    user: PortalUser,
    Json(req): Json<ConnectRequest>,
) -> Response {
    if let Err(r) = user.check_csrf(&headers) {
        return r;
    }
    let veil = &state.veil;
    let ip = auth::client_ip(peer, &headers, state.behind_proxy);
    if veil.penalties.refuses(ip) {
        return (
            StatusCode::TOO_MANY_REQUESTS,
            Json(json!({"step": "error", "code": LobbyErrorCode::LobbyErrorAuthFailed as i32, "message": "Too many failed logins from your address; try again later."})),
        )
            .into_response();
    }
    let client = ip.to_string();
    let groups = state.auth.groups(&user.username);
    let devices = match veil.db.entitled_devices(&user.username, &groups) {
        Ok(d) => d,
        Err(e) => return super::internal(e),
    };
    let Some(device) = devices.into_iter().find(|d| d.id == req.device_id) else {
        veil.db.audit(&user.username, "login refused", None, &format!("not entitled to device {}", req.device_id), Some(&client));
        return error(LobbyErrorCode::LobbyErrorNotEntitled, "You can't use that host.");
    };
    let host_user = match req.username.trim() {
        "" => user.username.clone(),
        name if valid_host_user(name) => name.to_string(),
        _ => return error(LobbyErrorCode::LobbyErrorAuthFailed, "That isn't a valid username."),
    };
    let Some(link) = veil.hosts.link(&device.id) else {
        veil.db.audit(&user.username, "login failed", Some(&device.id), "host offline", Some(&client));
        return error(LobbyErrorCode::LobbyErrorHostOffline, format!("{} is offline.", device.name));
    };
    // No password typed: the remembered one, for the user's own account.
    // Failing that it goes as none: a stack that wants no password asks
    // nothing, and one that does asks for it as a prompt.
    let (authtok, remembered) = if !req.password.is_empty() {
        (Some(Authtok::new(req.password.into_bytes())), false)
    } else if host_user == user.username {
        let key = remember::key_cookie_value(&headers);
        let authtok = state.passwords.open(&user.key, &user.username, ip, &device.id, key.as_deref());
        let remembered = authtok.is_some();
        (authtok, remembered)
    } else {
        (None, false)
    };
    let host = match HostLogin::start(&link, &client, "browser", &user.username, &host_user, authtok).await {
        Ok(h) => h,
        Err(OpenLoginError::TooManyLogins) => {
            warn!(username = %user.username, "portal: too many logins in progress for this account");
            return error(LobbyErrorCode::LobbyErrorHostFull, "Too many logins in progress for your account; finish or wait for them to expire.");
        }
        Err(OpenLoginError::Other(e)) => {
            warn!(error = %format!("{e:#}"), "portal: opening the login on the host failed");
            return error(LobbyErrorCode::LobbyErrorSessionStartFailed, format!("The login to {} failed.", device.name));
        }
    };
    let flow = Flow { username: user.username, host_user, client: ip, session: user.key, remembered, host, device, expires: Instant::now() + FLOW_TTL };
    let id = state.flows.insert(flow);
    let flow = state.flows.get(&id).expect("just inserted");
    let mut flow = flow.lock().await;
    advance(&state, &id, &mut flow, &headers).await
}

/// The flow `id`, if it is this user's and still live.
async fn own_flow(state: &WebState, id: &str, user: &PortalUser) -> Option<Arc<tokio::sync::Mutex<Flow>>> {
    let flow = state.flows.get(id)?;
    let ok = {
        let f = flow.lock().await;
        f.username == user.username && f.expires >= Instant::now()
    };
    ok.then_some(flow)
}

#[derive(Deserialize)]
pub struct AnswerRequest {
    response: String,
}

pub async fn answer(
    State(state): State<WebState>,
    Path(id): Path<String>,
    headers: HeaderMap,
    user: PortalUser,
    Json(req): Json<AnswerRequest>,
) -> Response {
    if let Err(r) = user.check_csrf(&headers) {
        return r;
    }
    let Some(flow) = own_flow(&state, &id, &user).await else { return gone() };
    let mut flow = flow.lock().await;
    if let Err(e) = flow.host.answer(req.response).await {
        warn!(error = %e, "portal: the host's login stream failed");
        return error(LobbyErrorCode::LobbyErrorSessionStartFailed, "The login failed.");
    }
    advance(&state, &id, &mut flow, &headers).await
}

#[derive(Deserialize)]
pub struct OpenRequest {
    #[serde(default)]
    session_type: String,
}

pub async fn open(
    State(state): State<WebState>,
    Path(id): Path<String>,
    headers: HeaderMap,
    user: PortalUser,
    Json(req): Json<OpenRequest>,
) -> Response {
    if let Err(r) = user.check_csrf(&headers) {
        return r;
    }
    let Some(flow) = own_flow(&state, &id, &user).await else { return gone() };
    let mut flow = flow.lock().await;
    let open = SessionOpen { session_type: req.session_type };
    if let Err(e) = flow.host.open(open).await {
        warn!(error = %e, "portal: the host's login stream failed");
        return error(LobbyErrorCode::LobbyErrorSessionStartFailed, "The login failed.");
    }
    advance(&state, &id, &mut flow, &headers).await
}

fn gone() -> Response {
    (
        StatusCode::GONE,
        Json(json!({"step": "error", "code": 0, "message": "This login has expired; start again."})),
    )
        .into_response()
}

// Runs the host's side of the login until it needs the user again.
async fn advance(state: &WebState, id: &str, flow: &mut Flow, headers: &HeaderMap) -> Response {
    let veil = &state.veil;
    let client = flow.client.to_string();
    let device = flow.device.clone();
    // Failures name the host account when it isn't the user's own.
    let account = if flow.host_user == flow.username { String::new() } else { format!(" (as {})", flow.host_user) };
    let step = match flow.host.next().await {
        Ok(step) => step,
        Err(e) => {
            warn!(error = %format!("{e:#}"), "portal: relaying the login failed");
            Step::Ended
        }
    };
    let finished = |state: &WebState| state.flows.remove(id);
    match step {
        Step::Prompt { prompt, echo } => Json(json!({"step": "prompt", "flow": id, "prompt": prompt, "echo": echo})).into_response(),
        Step::SessionList(list) => Json(json!({
            "step": "session_list",
            "flow": id,
            "types": list.available_types.iter().map(|t| json!({"id": t.id, "name": t.name})).collect::<Vec<_>>(),
            "default_type": list.default_type,
            "running": list.sessions.iter().map(|s| json!({"session_type": s.session_type})).collect::<Vec<_>>(),
        }))
        .into_response(),
        Step::Redirect(redirect) => {
            finished(state);
            let peer = SocketAddr::new(flow.client, 0);
            let redirect = match crate::gateway::intercept(veil, redirect, &device, &flow.host_user, peer).await {
                Ok(r) => r,
                Err(e) => {
                    warn!(error = %format!("{e:#}"), "portal: setting up the gateway failed");
                    return error(LobbyErrorCode::LobbyErrorSessionStartFailed, "The session couldn't be set up.");
                }
            };
            let detail = if flow.host_user == flow.username { "browser session".to_string() } else { format!("browser session as {}", flow.host_user) };
            veil.db.audit(&flow.username, "login", Some(&device.id), &detail, Some(&client));
            info!(username = %flow.username, host_user = %flow.host_user, device = %device.name, "portal: browser session ready");
            // The browser reaches the session where it reached this page.
            let authority = headers.get(header::HOST).and_then(|h| h.to_str().ok()).unwrap_or("localhost");
            Json(json!({
                "step": "ready",
                "token": redirect.token,
                "expires": redirect.expiry_unix,
                "device": device.name,
                "webtransport": format!("https://{authority}{}", crate::wt::PATH),
                "websocket": format!("wss://{authority}{}/ws", crate::wt::PATH),
            }))
            .into_response()
        }
        Step::Refused { code, message } => {
            finished(state);
            if code == LobbyErrorCode::LobbyErrorHostAuthFailed {
                if flow.remembered {
                    // Drifted, not guessed: no penalty, and no second try
                    // that could lock the account on the host.
                    state.passwords.refused(&flow.session, &device.id);
                } else {
                    // A cookie is no longer enough to try passwords for free.
                    veil.penalties.penalise(flow.client, Offense::AuthFail);
                }
            }
            veil.db.audit(&flow.username, "login failed", Some(&device.id), &format!("{}: {message}{account}", code.as_str_name()), Some(&client));
            error(code, message)
        }
        Step::Ended => {
            finished(state);
            veil.db.audit(&flow.username, "login failed", Some(&device.id), &format!("the host ended the login{account}"), Some(&client));
            error(LobbyErrorCode::LobbyErrorSessionStartFailed, format!("{} ended the login.", device.name))
        }
    }
}

/// The WebSocket fallback for a session (ws.rs): /gdp/ws.
pub async fn websocket(State(state): State<WebState>, ConnectInfo(peer): ConnectInfo<SocketAddr>, headers: HeaderMap, ws: WebSocketUpgrade) -> Response {
    let ip = auth::client_ip(peer, &headers, state.behind_proxy);
    if state.veil.penalties.refuses(ip) {
        return StatusCode::TOO_MANY_REQUESTS.into_response();
    }
    let veil = state.veil.clone();
    ws.max_message_size(2 * 1024 * 1024).on_upgrade(move |socket| async move {
        let leg = crate::gateway::ClientLeg::WebSocket(crate::ws::WsLeg::new(socket));
        crate::gateway::serve_client(leg, veil, SocketAddr::new(ip, 0)).await
    })
}

// --- the page itself, compiled in ---

pub async fn index() -> Response {
    asset("index.html")
}

pub async fn app_file(Path(file): Path<String>) -> Response {
    asset(&file)
}

fn asset(file: &str) -> Response {
    macro_rules! app {
        ($name:literal, $type:literal) => {
            (include_bytes!(concat!("../../app/", $name)).as_slice(), $type)
        };
    }
    let (body, content_type): (&'static [u8], &str) = match file {
        "index.html" => app!("index.html", "text/html; charset=utf-8"),
        "veil-logo.png" => app!("veil-logo.png", "image/png"),
        "app.css" => app!("app.css", "text/css"),
        "main.js" => app!("main.js", "text/javascript"),
        "proto.js" => app!("proto.js", "text/javascript"),
        "session.js" => app!("session.js", "text/javascript"),
        "transport.js" => app!("transport.js", "text/javascript"),
        "video.js" => app!("video.js", "text/javascript"),
        "refine.js" => app!("refine.js", "text/javascript"),
        "display-icon.png" => app!("display-icon.png", "image/png"),
        "fzstd.js" => app!("fzstd.js", "text/javascript"),
        "audio.js" => app!("audio.js", "text/javascript"),
        "audio-worklet.js" => app!("audio-worklet.js", "text/javascript"),
        "input.js" => app!("input.js", "text/javascript"),
        "keymap.js" => app!("keymap.js", "text/javascript"),
        "gamepad.js" => app!("gamepad.js", "text/javascript"),
        "log.js" => app!("log.js", "text/javascript"),
        _ => return StatusCode::NOT_FOUND.into_response(),
    };
    ([(header::CONTENT_TYPE, content_type), (header::CACHE_CONTROL, "no-cache")], body).into_response()
}
