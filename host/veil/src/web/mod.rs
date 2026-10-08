// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The admin web UI at https://<veil>/admin (docs/design/veil.md#admin-web-ui):
// server-rendered pages (askama templates in host/veil/templates),
// htmx, live updates nudged over the /admin/events WebSocket, and no
// build step. veild serves HTTPS itself (axum-server + rustls) from [web]
// cert/key, reloaded on SIGHUP, and optionally plain HTTP on a local port
// for a reverse proxy.
pub mod auth;
pub mod portal;
pub mod remember;
mod thin_clients;

use std::net::SocketAddr;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result};
use askama::Template;
use axum::extract::ws::{Message, WebSocket};
use axum::extract::{ConnectInfo, FromRequestParts, Path, Query, State, WebSocketUpgrade};
use axum::http::request::Parts;
use axum::http::{header, HeaderMap, HeaderValue, StatusCode};
use axum::response::{Html, IntoResponse, Redirect, Response};
use axum::routing::{get, post};
use axum::{Form, Router};
use ipc::unix_now as now;
use preauth::Offense;
use serde::Deserialize;
use tracing::{info, warn};

use crate::config::DeviceMode;
use crate::db::Grantee;
use crate::Veil;
use auth::Authenticator;

#[derive(Clone)]
pub struct WebState {
    pub veil: Arc<Veil>,
    pub auth: Arc<dyn Authenticator>,
    /// Browser logins in progress (portal.rs).
    pub flows: Arc<portal::Flows>,
    /// Browser users' sealed passwords for their hosts (remember.rs).
    pub passwords: Arc<remember::Passwords>,
    /// Requests come through a reverse proxy on loopback (the plain-HTTP
    /// listener): the client is in X-Forwarded-For.
    pub behind_proxy: bool,
}

pub fn router(state: WebState) -> Router {
    Router::new()
        .route("/", get(portal::index))
        .route("/settings", get(portal::index))
        .route("/app/{file}", get(portal::app_file))
        .route("/api/login", post(portal::login))
        .route("/api/session", get(portal::session))
        .route("/api/logout", post(portal::logout))
        .route("/api/connect", post(portal::connect))
        .route("/api/flows/{id}/answer", post(portal::answer))
        .route("/api/flows/{id}/open", post(portal::open))
        .route("/gdp/ws", get(portal::websocket))
        .route("/admin", get(|| async { Redirect::to("/admin/hosts") }))
        .route("/admin/", get(|| async { Redirect::to("/admin/hosts") }))
        .route("/admin/login", get(login_page).post(login))
        .route("/admin/logout", post(logout))
        .route("/admin/hosts", get(devices))
        .route("/admin/hosts/rows", get(device_rows))
        .route("/admin/hosts/{id}", get(device).post(update_device))
        .route("/admin/hosts/{id}/remove", post(remove_device))
        .route("/admin/hosts/{id}/grant", post(grant))
        .route("/admin/hosts/{id}/revoke", post(revoke))
        .route("/admin/hosts/new", get(add_host_page).post(add_host))
        .route("/admin/thin-clients", get(thin_clients::list))
        .route("/admin/thin-clients/rows", get(thin_clients::list_rows))
        .route("/admin/thin-clients/settings", get(thin_clients::settings).post(thin_clients::save_settings))
        .route("/admin/thin-clients/{mac}", get(thin_clients::detail).post(thin_clients::rename))
        .route("/admin/thin-clients/{mac}/remove", post(thin_clients::remove))
        .route("/admin/thin-clients/{mac}/action", post(thin_clients::act))
        .route("/admin/audit", get(audit))
        .route("/admin/events", get(events))
        .route("/admin/static/{file}", get(static_file))
        .layer(axum::middleware::map_response(security_headers))
        .with_state(state)
}

/// Starts the configured listeners: HTTPS when [web] cert/key are set,
/// plain HTTP on [web] http_listen when set. Returns the TLS config so
/// SIGHUP can reload it.
pub async fn serve(veil: Arc<Veil>) -> Result<Option<axum_server::tls_rustls::RustlsConfig>> {
    let cfg = &veil.config;
    let auth: Arc<dyn Authenticator> = Arc::new(auth::PamAuth {
        admin_group: cfg.web.admin_group.clone(),
        permit_root_login: cfg.auth.permit_root_login,
        permit_empty_passwords: cfg.auth.permit_empty_passwords,
    });
    let flows = Arc::new(portal::Flows::default());
    flows.spawn_sweeper();
    let passwords = Arc::new(remember::Passwords::default());
    let mut tls = None;
    if let Some(addr) = cfg.web_listen()? {
        let rustls = axum_server::tls_rustls::RustlsConfig::from_pem_chain_file(&cfg.web.cert, &cfg.web.key)
            .await
            .with_context(|| format!("loading the web certificate {} / {}", cfg.web.cert, cfg.web.key))?;
        let app = router(WebState { veil: veil.clone(), auth: auth.clone(), flows: flows.clone(), passwords: passwords.clone(), behind_proxy: false })
            // HSTS only here: the HTTP listener below is for a reverse
            // proxy, which sets its own on the name it serves.
            .layer(axum::middleware::map_response(strict_transport_security));
        let server = axum_server::bind_rustls(addr, rustls.clone());
        tokio::spawn(async move {
            if let Err(e) = server.serve(app.into_make_service_with_connect_info::<SocketAddr>()).await {
                warn!(error = %e, "web: the HTTPS listener failed");
            }
        });
        info!(%addr, "web: admin UI on HTTPS");
        tls = Some(rustls);
    }
    if let Some(addr) = cfg.http_listen()? {
        let app = router(WebState { veil: veil.clone(), auth, flows, passwords, behind_proxy: true });
        let listener = tokio::net::TcpListener::bind(addr).await.with_context(|| format!("binding {addr}"))?;
        tokio::spawn(async move {
            if let Err(e) = axum::serve(listener, app.into_make_service_with_connect_info::<SocketAddr>()).await {
                warn!(error = %e, "web: the HTTP listener failed");
            }
        });
        info!(%addr, "web: admin UI on plain HTTP, for a reverse proxy");
    }
    Ok(tls)
}

async fn security_headers(mut response: Response) -> Response {
    let h = response.headers_mut();
    h.insert(
        header::CONTENT_SECURITY_POLICY,
        // img-src data: for the browser client's cursor images; the
        // WebTransport and WebSocket sessions are 'self' too.
        HeaderValue::from_static(
            "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data: blob:; connect-src 'self'; frame-ancestors 'none'; form-action 'self'",
        ),
    );
    h.insert(header::X_FRAME_OPTIONS, HeaderValue::from_static("DENY"));
    h.insert(header::REFERRER_POLICY, HeaderValue::from_static("no-referrer"));
    h.insert(header::X_CONTENT_TYPE_OPTIONS, HeaderValue::from_static("nosniff"));
    if !h.contains_key(header::CACHE_CONTROL) {
        h.insert(header::CACHE_CONTROL, HeaderValue::from_static("no-store"));
    }
    response
}

/// A year, without includeSubDomains (Veil's name may be under a domain
/// it doesn't own) or preload.
async fn strict_transport_security(mut response: Response) -> Response {
    response.headers_mut().insert(header::STRICT_TRANSPORT_SECURITY, HeaderValue::from_static("max-age=31536000"));
    response
}

// --- the signed-in admin ---

/// A request from a signed-in administrator: the same session as the
/// browser client's (portal_sessions), whose user is still in admin_group.
/// Nobody signed in is sent to the login page (an htmx request through
/// HX-Redirect, so the login page doesn't land inside a table body); a
/// signed-in user who isn't an administrator is told so, and keeps their
/// session.
pub struct Admin {
    pub username: String,
    pub csrf: String,
    key: String,
}

impl FromRequestParts<WebState> for Admin {
    type Rejection = Response;

    async fn from_request_parts(parts: &mut Parts, state: &WebState) -> std::result::Result<Self, Self::Rejection> {
        let to_login = || {
            if parts.headers.contains_key("hx-request") {
                (StatusCode::UNAUTHORIZED, [("hx-redirect", "/admin/login")]).into_response()
            } else {
                Redirect::to("/admin/login").into_response()
            }
        };
        let Some(id) = auth::portal_cookie_value(&parts.headers) else { return Err(to_login()) };
        let key = auth::session_key(&id);
        let web = &state.veil.config.web;
        let session = match state.veil.db.portal_session(&key, web.portal_idle.as_secs() as i64, web.portal_max.as_secs() as i64) {
            Ok(Some(s)) => s,
            Ok(None) => return Err(to_login()),
            Err(e) => return Err(internal(e)),
        };
        // Admin rights are checked on every request, not only at login:
        // taking someone out of the group takes effect at once.
        if !state.auth.is_admin(&session.username) {
            warn!(username = %session.username, "web: not an administrator; refusing the request");
            // Back to the browser client: /admin would only land here again.
            return Err(error_page_back(
                StatusCode::FORBIDDEN,
                "Not an administrator",
                "Your account isn't in the Veil administrators group.",
                "/",
            ));
        }
        Ok(Admin { username: session.username, csrf: session.csrf, key })
    }
}

impl Admin {
    /// Every state-changing request carries the session's CSRF token, in
    /// the form or (htmx) in a header.
    fn check_csrf(&self, headers: &HeaderMap, form_token: &str) -> std::result::Result<(), Response> {
        let header = headers.get("x-csrf-token").and_then(|v| v.to_str().ok());
        if auth::csrf_ok(&self.csrf, Some(form_token)) || auth::csrf_ok(&self.csrf, header) {
            Ok(())
        } else {
            warn!(username = %self.username, "web: CSRF token missing or wrong; refusing the request");
            Err(error_page(StatusCode::FORBIDDEN, "Request refused", "The form had expired. Go back, reload the page and try again."))
        }
    }

    fn nav(&self, active: &'static str, notice: String) -> Nav {
        Nav { user: self.username.clone(), csrf: self.csrf.clone(), active, notice }
    }
}

pub struct Nav {
    user: String,
    csrf: String,
    active: &'static str,
    notice: String,
}

// --- pages ---

#[derive(Template)]
#[template(path = "login.html")]
struct LoginPage {
    error: String,
    username: String,
}

#[derive(Template)]
#[template(path = "error.html")]
struct ErrorPage<'a> {
    title: &'a str,
    message: &'a str,
    /// Where Back goes.
    back: &'a str,
}

fn render<T: Template>(t: &T) -> Response {
    match t.render() {
        Ok(html) => Html(html).into_response(),
        Err(e) => internal(e),
    }
}

fn error_page(status: StatusCode, title: &str, message: &str) -> Response {
    error_page_back(status, title, message, "/admin")
}

fn error_page_back(status: StatusCode, title: &str, message: &str, back: &str) -> Response {
    let mut r = render(&ErrorPage { title, message, back });
    *r.status_mut() = status;
    r
}

pub(crate) fn internal(e: impl std::fmt::Display) -> Response {
    warn!(error = %e, "web: internal error");
    error_page(StatusCode::INTERNAL_SERVER_ERROR, "Something went wrong", "veild's log has the details.")
}

async fn login_page(admin: Option<Admin>) -> Response {
    if admin.is_some() {
        return Redirect::to("/admin/hosts").into_response();
    }
    render(&LoginPage { error: String::new(), username: String::new() })
}

impl axum::extract::OptionalFromRequestParts<WebState> for Admin {
    type Rejection = std::convert::Infallible;

    async fn from_request_parts(parts: &mut Parts, state: &WebState) -> std::result::Result<Option<Self>, Self::Rejection> {
        Ok(<Admin as FromRequestParts<WebState>>::from_request_parts(parts, state).await.ok())
    }
}

#[derive(Deserialize)]
struct LoginForm {
    username: String,
    password: String,
}

async fn login(
    State(state): State<WebState>,
    ConnectInfo(peer): ConnectInfo<SocketAddr>,
    headers: HeaderMap,
    Form(form): Form<LoginForm>,
) -> Response {
    let ip = auth::client_ip(peer, &headers, state.behind_proxy);
    let veil = &state.veil;
    let username = form.username.trim().to_string();
    // The lobby's pre-auth penalties, shared with it: failing here counts
    // against the address there.
    if veil.penalties.refuses(ip) {
        let mut r = render(&LoginPage { error: "Too many failed sign-ins from your address; try again later.".into(), username });
        *r.status_mut() = StatusCode::TOO_MANY_REQUESTS;
        return r;
    }
    let authtok = if username.is_empty() || username.len() > 256 {
        None
    } else {
        state.auth.authenticate_keep(&username, &form.password, ip).await
    };
    let Some(authtok) = authtok else {
        veil.penalties.penalise(ip, Offense::AuthFail);
        veil.db.audit(&username, "web login failed", None, "authentication failed", Some(&ip.to_string()));
        info!(%ip, username, "web: sign-in failed");
        let mut r = render(&LoginPage { error: "Wrong username or password.".into(), username });
        *r.status_mut() = StatusCode::UNAUTHORIZED;
        return r;
    };
    if !state.auth.is_admin(&username) {
        veil.db.audit(&username, "web login refused", None, "not in the admin group", Some(&ip.to_string()));
        info!(%ip, username, "web: sign-in refused, not an administrator");
        let mut r = render(&LoginPage {
            error: format!("{username} isn't an administrator (the {} group).", veil.config.web.admin_group),
            username,
        });
        *r.status_mut() = StatusCode::FORBIDDEN;
        return r;
    }
    let signed_in = match portal::start_session(&state, ip, &headers, &username, &authtok, "admin UI") {
        Ok(s) => s,
        Err(r) => return r,
    };
    let mut r = Redirect::to("/admin/hosts").into_response();
    signed_in.set_cookies(r.headers_mut());
    r
}

#[derive(Deserialize)]
struct CsrfForm {
    #[serde(default)]
    csrf: String,
}

async fn logout(State(state): State<WebState>, admin: Admin, headers: HeaderMap, Form(form): Form<CsrfForm>) -> Response {
    if let Err(r) = admin.check_csrf(&headers, &form.csrf) {
        return r;
    }
    let _ = state.veil.db.remove_portal_session(&admin.key);
    state.passwords.forget(&admin.key);
    (portal::clear_cookies(), Redirect::to("/admin/login")).into_response()
}

// --- devices ---

struct SessionRow {
    username: String,
    session_type: String,
    viewer: bool,
    since: String,
    throughput: String,
}

struct DeviceRow {
    id: String,
    name: String,
    hostname: String,
    client_address: String,
    cert_sha256: String,
    mode: &'static str,
    enabled: bool,
    online: bool,
    joined_at: String,
    last_seen: String,
    sessions: Vec<SessionRow>,
}

fn device_rows_for(veil: &Veil) -> anyhow::Result<Vec<DeviceRow>> {
    let placements = veil.db.placements()?;
    let throughput = crate::gateway::throughput(veil);
    let t = now();
    Ok(veil
        .db
        .devices()?
        .into_iter()
        .map(|d| {
            let online = veil.hosts.is_online(&d.id);
            let sessions = placements
                .iter()
                .filter(|p| p.device_id == d.id)
                .map(|p| SessionRow {
                    username: p.username.clone(),
                    session_type: p.session_type.clone(),
                    viewer: p.viewer_attached,
                    since: ago(t, p.started_at),
                    throughput: throughput
                        .get(&(d.id.clone(), p.username.clone()))
                        .map(|bps| rate(*bps))
                        .unwrap_or_default(),
                })
                .collect();
            DeviceRow {
                last_seen: if online { "now".to_string() } else { d.last_seen.map(|s| ago(t, s)).unwrap_or_else(|| "never".into()) },
                joined_at: utc(d.joined_at),
                id: d.id,
                name: d.name,
                hostname: d.hostname,
                client_address: d.client_address,
                cert_sha256: d.cert_sha256,
                mode: d.mode.as_str(),
                enabled: d.enabled,
                online,
                sessions,
            }
        })
        .collect())
}

#[derive(Template)]
#[template(path = "devices.html")]
struct DevicesPage {
    nav: Nav,
    rows: Vec<DeviceRow>,
    online: usize,
    total: usize,
    gateway_total: String,
}

#[derive(Template)]
#[template(path = "device_rows.html")]
struct DeviceRowsPartial {
    rows: Vec<DeviceRow>,
}

#[derive(Deserialize)]
struct NoticeQuery {
    #[serde(default)]
    notice: String,
}

async fn devices(State(state): State<WebState>, admin: Admin, Query(q): Query<NoticeQuery>) -> Response {
    let rows = match device_rows_for(&state.veil) {
        Ok(rows) => rows,
        Err(e) => return internal(e),
    };
    let total_bps: u64 = crate::gateway::throughput(&state.veil).values().sum();
    render(&DevicesPage {
        online: rows.iter().filter(|r| r.online).count(),
        total: rows.len(),
        rows,
        gateway_total: rate(total_bps),
        nav: admin.nav("hosts", notice_text(&q.notice)),
    })
}

/// What an admin page shows, boiled down to a number that changes when
/// the page would: the host and thin client tables as rendered (so
/// "last seen" and uptime tick over when they would on screen), the
/// reports behind the thin client pages, and the newest audit row.
fn state_digest(veil: &Veil) -> u64 {
    use std::hash::{Hash, Hasher};
    let mut h = std::collections::hash_map::DefaultHasher::new();
    if let Ok(rows) = device_rows_for(veil) {
        if let Ok(html) = (DeviceRowsPartial { rows }).render() {
            html.hash(&mut h);
        }
    }
    let throughput: u64 = crate::gateway::throughput(veil).values().sum();
    rate(throughput).hash(&mut h);
    thin_clients::digest(veil, &mut h);
    if let Ok(log) = veil.db.audit_log(1, None) {
        log.first().map(|(id, _)| *id).hash(&mut h);
    }
    h.finish()
}

/// /admin/events: a WebSocket that says "changed" whenever an admin
/// page's content would, so the open page (admin.js) re-fetches itself
/// instead of polling. It carries no data, only the nudge.
async fn events(State(state): State<WebState>, _admin: Admin, ws: WebSocketUpgrade) -> Response {
    let veil = state.veil.clone();
    ws.on_upgrade(move |socket| push_changes(socket, veil))
}

async fn push_changes(mut socket: WebSocket, veil: Arc<Veil>) {
    let mut last = state_digest(&veil);
    let mut tick = tokio::time::interval(Duration::from_secs(1));
    loop {
        tokio::select! {
            _ = tick.tick() => {
                let now = state_digest(&veil);
                if now != last {
                    last = now;
                    if socket.send(Message::Text("changed".into())).await.is_err() {
                        return;
                    }
                }
            }
            // Only here to notice the browser going away (or pinging).
            msg = socket.recv() => match msg {
                Some(Ok(Message::Close(_))) | Some(Err(_)) | None => return,
                Some(Ok(_)) => {}
            },
        }
    }
}

async fn device_rows(State(state): State<WebState>, _admin: Admin) -> Response {
    match device_rows_for(&state.veil) {
        Ok(rows) => render(&DeviceRowsPartial { rows }),
        Err(e) => internal(e),
    }
}

// Notices after a redirect come from a fixed list, so a crafted link
// can't put arbitrary text on the page.
fn notice_text(key: &str) -> String {
    match key {
        "saved" => "Saved.",
        "removed" => "Device removed.",
        "granted" => "Access granted.",
        "revoked" => "Access removed.",
        _ => "",
    }
    .to_string()
}

struct GrantRow {
    kind: &'static str,
    name: String,
}

#[derive(Template)]
#[template(path = "device.html")]
struct DevicePage {
    nav: Nav,
    d: DeviceRow,
    grants: Vec<GrantRow>,
}

async fn device(State(state): State<WebState>, admin: Admin, Path(id): Path<String>, Query(q): Query<NoticeQuery>) -> Response {
    let rows = match device_rows_for(&state.veil) {
        Ok(rows) => rows,
        Err(e) => return internal(e),
    };
    let Some(d) = rows.into_iter().find(|r| r.id == id) else {
        return error_page(StatusCode::NOT_FOUND, "No such device", "It may have been removed.");
    };
    let grants = match state.veil.db.entitlements(&id) {
        Ok(g) => g
            .into_iter()
            .map(|g| match g {
                Grantee::User(name) => GrantRow { kind: "user", name },
                Grantee::Group(name) => GrantRow { kind: "group", name },
            })
            .collect(),
        Err(e) => return internal(e),
    };
    render(&DevicePage { nav: admin.nav("hosts", notice_text(&q.notice)), d, grants })
}

#[derive(Deserialize)]
struct DeviceForm {
    #[serde(default)]
    csrf: String,
    name: String,
    client_address: String,
    mode: String,
    #[serde(default)]
    enabled: Option<String>,
}

async fn update_device(
    State(state): State<WebState>,
    admin: Admin,
    headers: HeaderMap,
    Path(id): Path<String>,
    Form(form): Form<DeviceForm>,
) -> Response {
    if let Err(r) = admin.check_csrf(&headers, &form.csrf) {
        return r;
    }
    let veil = &state.veil;
    let name = form.name.trim();
    let address = form.client_address.trim();
    let Ok(mode) = form.mode.parse::<DeviceMode>() else {
        return error_page(StatusCode::BAD_REQUEST, "Not saved", "Unknown mode.");
    };
    if name.is_empty() || name.len() > 64 || address.is_empty() || address.len() > 255 || has_control(name) || has_control(address) {
        return error_page(StatusCode::BAD_REQUEST, "Not saved", "The name and the client address must be set, without control characters.");
    }
    let enabled = form.enabled.is_some();
    let Ok(Some(before)) = veil.db.device(&id) else {
        return error_page(StatusCode::NOT_FOUND, "No such device", "It may have been removed.");
    };
    if let Err(e) = veil.db.update_device(&id, name, address, mode, enabled) {
        return internal(e);
    }
    let mut changes = Vec::new();
    if before.name != name {
        changes.push(format!("name {:?} -> {name:?}", before.name));
    }
    if before.client_address != address {
        changes.push(format!("client address {} -> {address}", before.client_address));
    }
    if before.mode != mode {
        changes.push(format!("mode {} -> {}", before.mode.as_str(), mode.as_str()));
    }
    if before.enabled != enabled {
        changes.push(if enabled { "enabled".to_string() } else { "disabled".to_string() });
    }
    if !changes.is_empty() {
        veil.db.audit(&admin.username, "device changed", Some(&id), &changes.join(", "), None);
    }
    if before.enabled && !enabled {
        veil.hosts.disconnect(&id, ipc::broker::HostErrorCode::HostErrorDisabled, "device disabled");
    }
    Redirect::to(&format!("/admin/hosts/{id}?notice=saved")).into_response()
}

async fn remove_device(
    State(state): State<WebState>,
    admin: Admin,
    headers: HeaderMap,
    Path(id): Path<String>,
    Form(form): Form<CsrfForm>,
) -> Response {
    if let Err(r) = admin.check_csrf(&headers, &form.csrf) {
        return r;
    }
    let veil = &state.veil;
    let name = veil.db.device(&id).ok().flatten().map(|d| d.name).unwrap_or_default();
    match veil.db.remove_device(&id) {
        Ok(true) => {
            veil.hosts.disconnect(&id, ipc::broker::HostErrorCode::HostErrorUnknownDevice, "device removed");
            veil.db.audit(&admin.username, "device removed", Some(&id), &name, None);
            Redirect::to("/admin/hosts?notice=removed").into_response()
        }
        Ok(false) => error_page(StatusCode::NOT_FOUND, "No such device", "It may have been removed already."),
        Err(e) => internal(e),
    }
}

#[derive(Deserialize)]
struct GrantForm {
    #[serde(default)]
    csrf: String,
    kind: String,
    name: String,
}

fn grantee(form: &GrantForm) -> Option<Grantee> {
    let name = form.name.trim();
    if name.is_empty() || name.len() > 256 || has_control(name) {
        return None;
    }
    match form.kind.as_str() {
        "user" => Some(Grantee::User(name.to_string())),
        "group" => Some(Grantee::Group(name.to_string())),
        _ => None,
    }
}

async fn grant(
    State(state): State<WebState>,
    admin: Admin,
    headers: HeaderMap,
    Path(id): Path<String>,
    Form(form): Form<GrantForm>,
) -> Response {
    if let Err(r) = admin.check_csrf(&headers, &form.csrf) {
        return r;
    }
    let Some(who) = grantee(&form) else {
        return error_page(StatusCode::BAD_REQUEST, "Not added", "Give a user or group name.");
    };
    let veil = &state.veil;
    if !matches!(veil.db.device(&id), Ok(Some(_))) {
        return error_page(StatusCode::NOT_FOUND, "No such device", "It may have been removed.");
    }
    match veil.db.grant(&id, &who) {
        Ok(true) => veil.db.audit(&admin.username, "access granted", Some(&id), &format!("{} {}", form.kind, who.name()), None),
        Ok(false) => {}
        Err(e) => return internal(e),
    }
    Redirect::to(&format!("/admin/hosts/{id}?notice=granted")).into_response()
}

async fn revoke(
    State(state): State<WebState>,
    admin: Admin,
    headers: HeaderMap,
    Path(id): Path<String>,
    Form(form): Form<GrantForm>,
) -> Response {
    if let Err(r) = admin.check_csrf(&headers, &form.csrf) {
        return r;
    }
    let Some(who) = grantee(&form) else {
        return error_page(StatusCode::BAD_REQUEST, "Not removed", "Give a user or group name.");
    };
    let veil = &state.veil;
    match veil.db.revoke(&id, &who) {
        Ok(true) => veil.db.audit(&admin.username, "access removed", Some(&id), &format!("{} {}", form.kind, who.name()), None),
        Ok(false) => {}
        Err(e) => return internal(e),
    }
    Redirect::to(&format!("/admin/hosts/{id}?notice=revoked")).into_response()
}

// --- add host ---

#[derive(Template)]
#[template(path = "add_host.html")]
struct AddHostPage {
    nav: Nav,
    command: String,
    ttl: String,
}

async fn add_host_page(admin: Admin) -> Response {
    render(&AddHostPage { nav: admin.nav("hosts", String::new()), command: String::new(), ttl: String::new() })
}

#[derive(Deserialize)]
struct AddHostForm {
    #[serde(default)]
    csrf: String,
    ttl: String,
}

async fn add_host(State(state): State<WebState>, admin: Admin, headers: HeaderMap, Form(form): Form<AddHostForm>) -> Response {
    if let Err(r) = admin.check_csrf(&headers, &form.csrf) {
        return r;
    }
    let ttl = match form.ttl.as_str() {
        "15m" => Duration::from_secs(900),
        "1d" => Duration::from_secs(86400),
        _ => state.veil.config.hosts.join_token_ttl,
    };
    let veil = &state.veil;
    let token = match veil.hosts.issue_join_token(ttl, &admin.username, veil.certs.lobby_fingerprint()) {
        Ok(token) => token,
        Err(e) => return internal(e),
    };
    let address = match veil.config.lobby.port {
        4442 => veil.config.public_address(),
        port => format!("{}:{port}", veil.config.public_address()),
    };
    render(&AddHostPage {
        nav: admin.nav("hosts", String::new()),
        command: format!("ghostd join {address} --token {token}"),
        ttl: describe_duration(ttl),
    })
}

// --- audit ---

struct AuditRow {
    at: String,
    actor: String,
    action: String,
    device: String,
    detail: String,
    client_address: String,
}

#[derive(Template)]
#[template(path = "audit.html")]
struct AuditPage {
    nav: Nav,
    entries: Vec<AuditRow>,
    older: Option<i64>,
}

#[derive(Deserialize)]
struct AuditQuery {
    before: Option<i64>,
}

const AUDIT_PAGE: u32 = 100;

async fn audit(State(state): State<WebState>, admin: Admin, Query(q): Query<AuditQuery>) -> Response {
    let veil = &state.veil;
    let rows = match veil.db.audit_log(AUDIT_PAGE, q.before) {
        Ok(rows) => rows,
        Err(e) => return internal(e),
    };
    let names: std::collections::HashMap<String, String> =
        veil.db.devices().map(|ds| ds.into_iter().map(|d| (d.id, d.name)).collect()).unwrap_or_default();
    let older = (rows.len() == AUDIT_PAGE as usize).then(|| rows.last().map(|(id, _)| *id)).flatten();
    let entries = rows
        .into_iter()
        .map(|(_, e)| AuditRow {
            at: utc(e.at),
            actor: e.actor,
            action: e.action,
            device: e.device_id.map(|id| names.get(&id).cloned().unwrap_or(id)).unwrap_or_default(),
            detail: e.detail,
            client_address: e.client_address.unwrap_or_default(),
        })
        .collect();
    render(&AuditPage { nav: admin.nav("audit", String::new()), entries, older })
}

// --- static files, compiled in ---

async fn static_file(Path(file): Path<String>) -> Response {
    let (body, content_type): (&'static [u8], &str) = match file.as_str() {
        "htmx.min.js" => (include_bytes!("../../static/htmx.min.js"), "text/javascript"),
        "admin.js" => (include_bytes!("../../static/admin.js"), "text/javascript"),
        "style.css" => (include_bytes!("../../static/style.css"), "text/css"),
        _ => return StatusCode::NOT_FOUND.into_response(),
    };
    ([(header::CONTENT_TYPE, content_type), (header::CACHE_CONTROL, "public, max-age=3600")], body).into_response()
}

// --- formatting ---

fn has_control(s: &str) -> bool {
    s.chars().any(char::is_control)
}

/// "just now", "5 s ago", "3 min ago", "2 h ago", "4 d ago".
fn ago(now: i64, then: i64) -> String {
    let secs = (now - then).max(0);
    match secs {
        0..=4 => "just now".to_string(),
        5..=59 => format!("{secs} s ago"),
        60..=3599 => format!("{} min ago", secs / 60),
        3600..=86399 => format!("{} h ago", secs / 3600),
        _ => format!("{} d ago", secs / 86400),
    }
}

/// Seconds since the epoch as "YYYY-MM-DD HH:MM:SS" (UTC).
fn utc(t: i64) -> String {
    let days = t.div_euclid(86400);
    let secs = t.rem_euclid(86400);
    // Howard Hinnant's civil_from_days.
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    format!("{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02}", secs / 3600, secs % 3600 / 60, secs % 60)
}

/// Bytes per second as "12.3 Mbit/s".
fn rate(bytes_per_sec: u64) -> String {
    let bits = bytes_per_sec as f64 * 8.0;
    if bits >= 1e9 {
        format!("{:.2} Gbit/s", bits / 1e9)
    } else if bits >= 1e6 {
        format!("{:.1} Mbit/s", bits / 1e6)
    } else if bits >= 1e3 {
        format!("{:.0} kbit/s", bits / 1e3)
    } else {
        format!("{bits:.0} bit/s")
    }
}

fn describe_duration(d: Duration) -> String {
    match d.as_secs() {
        s if s % 86400 == 0 => format!("{} day{}", s / 86400, if s == 86400 { "" } else { "s" }),
        s if s % 3600 == 0 => format!("{} hour{}", s / 3600, if s == 3600 { "" } else { "s" }),
        s => format!("{} minutes", s / 60),
    }
}

#[cfg(test)]
mod tests;
