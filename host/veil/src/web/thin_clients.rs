// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The admin UI's Thin Clients pages (docs/design/wisp.md): the list of
// Wisp clients, one client's report with log out, restart and shut down,
// and the spectre profile every client applies. The live state is
// crate::thin_clients'.
use askama::Template;
use axum::extract::{Path, Query, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Redirect, Response};
use axum::Form;
use ipc::unix_now as now;
use ipc::wisp::WispAction;
use serde::Deserialize;

use super::{ago, error_page, has_control, internal, render, utc, Admin, CsrfForm, Nav, NoticeQuery, WebState};
use crate::thin_clients::{ClientView, Profile, Report, CODECS};

/// Offered after "Match the client's display", as in spectre-qt's dialog.
const RESOLUTIONS: &[&str] = &["1280x720", "1366x768", "1600x900", "1920x1080", "2560x1440", "3840x2160"];

struct ClientRow {
    mac: String,
    name: String,
    hostname: String,
    online: bool,
    address: String,
    user: String,
    host_name: String,
    uptime: String,
    last_seen: String,
}

impl ClientRow {
    fn new(c: &ClientView, t: i64) -> ClientRow {
        let session = c.session.clone();
        ClientRow {
            mac: c.row.mac.clone(),
            name: c.display_name().to_string(),
            hostname: c.report.hostname.clone(),
            online: c.online,
            address: c.report.address.clone(),
            user: session.as_ref().map(|s| s.user.clone()).unwrap_or_default(),
            host_name: session.map(|s| s.host_name).unwrap_or_default(),
            uptime: if c.online && c.report.boot_time_unix > 0 { duration(t - c.report.boot_time_unix) } else { String::new() },
            last_seen: if c.online { "now".to_string() } else { ago(t, c.row.last_seen) },
        }
    }
}

/// "3 d 4 h", "2 h 5 min", "12 min".
fn duration(secs: i64) -> String {
    let secs = secs.max(0);
    let (d, h, m) = (secs / 86400, secs % 86400 / 3600, secs % 3600 / 60);
    match (d, h) {
        (0, 0) => format!("{m} min"),
        (0, _) => format!("{h} h {m} min"),
        _ => format!("{d} d {h} h"),
    }
}

fn rows(state: &WebState) -> anyhow::Result<Vec<ClientRow>> {
    let t = now();
    Ok(state.veil.thin_clients.list()?.iter().map(|c| ClientRow::new(c, t)).collect())
}

/// Feeds mod.rs's state_digest: the rows as shown plus each client's
/// report, which the detail pages show more of.
pub(super) fn digest(veil: &crate::Veil, h: &mut impl std::hash::Hasher) {
    use std::hash::Hash;
    let Ok(list) = veil.thin_clients.list() else { return };
    let t = now();
    for c in &list {
        let r = ClientRow::new(c, t);
        (&r.mac, &r.name, &r.hostname, r.online, &r.address, &r.user, &r.host_name, &r.uptime, &r.last_seen).hash(h);
        format!("{:?}", c.report).hash(h);
        c.session.as_ref().map(|s| s.started_at).hash(h);
    }
}

// Notices after a redirect, from a fixed list (as mod.rs's notice_text).
fn notice_text(key: &str) -> String {
    match key {
        "saved" => "Saved.",
        "renamed" => "Name saved.",
        "removed" => "Thin client removed.",
        "log_out" => "Ending the session. The client returns to its sign-in screen once the host ends the session.",
        "reboot" => "Restarting.",
        "power_off" => "Shutting down.",
        _ => "",
    }
    .to_string()
}

#[derive(Template)]
#[template(path = "thin_clients.html")]
struct ListPage {
    nav: Nav,
    rows: Vec<ClientRow>,
    online: usize,
    total: usize,
}

#[derive(Template)]
#[template(path = "thin_client_rows.html")]
struct RowsPartial {
    rows: Vec<ClientRow>,
}

pub(super) async fn list(State(state): State<WebState>, admin: Admin, Query(q): Query<NoticeQuery>) -> Response {
    let rows = match rows(&state) {
        Ok(rows) => rows,
        Err(e) => return internal(e),
    };
    render(&ListPage {
        online: rows.iter().filter(|r| r.online).count(),
        total: rows.len(),
        rows,
        nav: admin.nav("thin_clients", notice_text(&q.notice)),
    })
}

pub(super) async fn list_rows(State(state): State<WebState>, _admin: Admin) -> Response {
    match rows(&state) {
        Ok(rows) => render(&RowsPartial { rows }),
        Err(e) => internal(e),
    }
}

#[derive(Template)]
#[template(path = "thin_client.html")]
struct DetailPage {
    nav: Nav,
    c: ClientRow,
    report: Report,
    custom_name: String,
    booted: String,
    first_seen: String,
    session_since: String,
    memory: String,
    displays: Vec<String>,
}

pub(super) async fn detail(
    State(state): State<WebState>,
    admin: Admin,
    Path(mac): Path<String>,
    Query(q): Query<NoticeQuery>,
) -> Response {
    let view = match state.veil.thin_clients.get(&mac) {
        Ok(Some(view)) => view,
        Ok(None) => return error_page(StatusCode::NOT_FOUND, "No such thin client", "It may have been removed."),
        Err(e) => return internal(e),
    };
    let t = now();
    let report = view.report.clone();
    let displays = report
        .displays
        .iter()
        .map(|d| {
            let mut s = format!("{} {}x{}", d.connector, d.width, d.height);
            if d.refresh_mhz > 0 {
                s += &format!(" @ {:.2} Hz", d.refresh_mhz as f64 / 1000.0);
            }
            s
        })
        .collect();
    render(&DetailPage {
        nav: admin.nav("thin_clients", notice_text(&q.notice)),
        c: ClientRow::new(&view, t),
        custom_name: view.row.name.clone(),
        booted: if report.boot_time_unix > 0 { utc(report.boot_time_unix) } else { "unknown".into() },
        first_seen: utc(view.row.first_seen),
        session_since: view.session.as_ref().filter(|s| s.started_at > 0).map(|s| ago(t, s.started_at)).unwrap_or_default(),
        memory: if report.memory_bytes > 0 {
            format!("{:.1} GiB", report.memory_bytes as f64 / (1u64 << 30) as f64)
        } else {
            "unknown".into()
        },
        displays,
        report,
    })
}

#[derive(Deserialize)]
pub(super) struct NameForm {
    #[serde(default)]
    csrf: String,
    #[serde(default)]
    name: String,
}

pub(super) async fn rename(
    State(state): State<WebState>,
    admin: Admin,
    headers: HeaderMap,
    Path(mac): Path<String>,
    Form(form): Form<NameForm>,
) -> Response {
    if let Err(r) = admin.check_csrf(&headers, &form.csrf) {
        return r;
    }
    let name = form.name.trim();
    if name.len() > 64 || has_control(name) {
        return error_page(StatusCode::BAD_REQUEST, "Not saved", "The name must be at most 64 characters, without control characters.");
    }
    let veil = &state.veil;
    let before = match veil.db.thin_client(&mac) {
        Ok(Some(row)) => row.name,
        Ok(None) => return error_page(StatusCode::NOT_FOUND, "No such thin client", "It may have been removed."),
        Err(e) => return internal(e),
    };
    if before != name {
        if let Err(e) = veil.db.rename_thin_client(&mac, name) {
            return internal(e);
        }
        veil.db.audit(&admin.username, "thin client renamed", None, &format!("{mac}: {before:?} -> {name:?}"), None);
    }
    Redirect::to(&format!("/admin/thin-clients/{mac}?notice=renamed")).into_response()
}

pub(super) async fn remove(
    State(state): State<WebState>,
    admin: Admin,
    headers: HeaderMap,
    Path(mac): Path<String>,
    Form(form): Form<CsrfForm>,
) -> Response {
    if let Err(r) = admin.check_csrf(&headers, &form.csrf) {
        return r;
    }
    let thin = &state.veil.thin_clients;
    let name = thin.get(&mac).ok().flatten().map(|c| c.display_name().to_string()).unwrap_or_default();
    if thin.is_online(&mac) {
        return error_page(StatusCode::CONFLICT, "Not removed", "The client is online; it would only report back in.");
    }
    match thin.remove(&mac) {
        Ok(true) => {
            state.veil.db.audit(&admin.username, "thin client removed", None, &format!("{mac} ({name})"), None);
            Redirect::to("/admin/thin-clients?notice=removed").into_response()
        }
        Ok(false) => error_page(StatusCode::NOT_FOUND, "No such thin client", "It may have been removed already."),
        Err(e) => internal(e),
    }
}

#[derive(Deserialize)]
pub(super) struct ActionForm {
    #[serde(default)]
    csrf: String,
    #[serde(default)]
    action: String,
}

/// Log out, restart or shut down, sent to the client's agent.
pub(super) async fn act(
    State(state): State<WebState>,
    admin: Admin,
    headers: HeaderMap,
    Path(mac): Path<String>,
    Form(form): Form<ActionForm>,
) -> Response {
    if let Err(r) = admin.check_csrf(&headers, &form.csrf) {
        return r;
    }
    let (action, audit) = match form.action.as_str() {
        "log_out" => (WispAction::LogOut, "thin client logged out"),
        "reboot" => (WispAction::Reboot, "thin client restarted"),
        "power_off" => (WispAction::PowerOff, "thin client shut down"),
        _ => return error_page(StatusCode::BAD_REQUEST, "Nothing done", "Unknown action."),
    };
    let thin = &state.veil.thin_clients;
    let view = match thin.get(&mac) {
        Ok(Some(view)) => view,
        Ok(None) => return error_page(StatusCode::NOT_FOUND, "No such thin client", "It may have been removed."),
        Err(e) => return internal(e),
    };
    if action == WispAction::LogOut && view.session.is_none() {
        return error_page(StatusCode::CONFLICT, "Nothing done", "Nobody is signed in on this client.");
    }
    if !thin.command(&mac, action) {
        return error_page(StatusCode::CONFLICT, "Nothing done", "The client is offline.");
    }
    let mut detail = format!("{mac} ({})", view.display_name());
    if let Some(s) = view.session.as_ref().filter(|s| !s.user.is_empty()) {
        detail += &format!(", {} signed in", s.user);
    }
    state.veil.db.audit(&admin.username, audit, None, &detail, None);
    Redirect::to(&format!("/admin/thin-clients/{mac}?notice={}", form.action)).into_response()
}

#[derive(Template)]
#[template(path = "thin_client_settings.html")]
struct SettingsPage {
    nav: Nav,
    p: Profile,
    resolutions: Vec<String>,
    codecs: Vec<(String, String)>,
}

fn codec_choices() -> Vec<(String, String)> {
    CODECS
        .iter()
        .map(|&c| {
            let label = match c {
                "" => "Auto".to_string(),
                "h265" => "h265 (req. hardware)".to_string(),
                "av1" => "av1 (req. newer hardware)".to_string(),
                "pyrowave" => "pyrowave (wired LAN only)".to_string(),
                other => other.to_string(),
            };
            (c.to_string(), label)
        })
        .collect()
}

pub(super) async fn settings(State(state): State<WebState>, admin: Admin, Query(q): Query<NoticeQuery>) -> Response {
    render(&SettingsPage {
        nav: admin.nav("thin_clients", notice_text(&q.notice)),
        p: state.veil.thin_clients.profile(),
        resolutions: RESOLUTIONS.iter().map(|r| r.to_string()).collect(),
        codecs: codec_choices(),
    })
}

#[derive(Deserialize)]
pub(super) struct ProfileForm {
    #[serde(default)]
    csrf: String,
    #[serde(default)]
    resolution: String,
    #[serde(default)]
    view: String,
    #[serde(default)]
    preferred_decoder: String,
    #[serde(default)]
    preferred_codec: String,
    lossless_refinement: Option<String>,
    allow_pyrowave: Option<String>,
    #[serde(default)]
    network_profile: String,
    forward_gamepads: Option<String>,
    microphone: Option<String>,
    debug_logging: Option<String>,
    #[serde(default)]
    display_sleep_minutes: String,
}

pub(super) async fn save_settings(
    State(state): State<WebState>,
    admin: Admin,
    headers: HeaderMap,
    Form(form): Form<ProfileForm>,
) -> Response {
    if let Err(r) = admin.check_csrf(&headers, &form.csrf) {
        return r;
    }
    let Ok(display_sleep_minutes) = form.display_sleep_minutes.trim().parse::<u32>() else {
        return error_page(StatusCode::BAD_REQUEST, "Not saved", "Display sleep must be a whole number of minutes.");
    };
    let profile = Profile {
        resolution: form.resolution.trim().to_string(),
        view: form.view,
        preferred_decoder: form.preferred_decoder,
        preferred_codec: form.preferred_codec,
        lossless_refinement: form.lossless_refinement.is_some(),
        allow_pyrowave: form.allow_pyrowave.is_some(),
        network_profile: form.network_profile,
        forward_gamepads: form.forward_gamepads.is_some(),
        microphone: form.microphone.is_some(),
        debug_logging: form.debug_logging.is_some(),
        display_sleep_minutes,
    };
    let thin = &state.veil.thin_clients;
    let before = thin.profile();
    if let Err(e) = profile.validate() {
        return error_page(StatusCode::BAD_REQUEST, "Not saved", &format!("{e}."));
    }
    if profile != before {
        if let Err(e) = thin.set_profile(profile.clone()) {
            return internal(e);
        }
        state.veil.db.audit(&admin.username, "thin client settings changed", None, &describe_changes(&before, &profile), None);
    }
    Redirect::to("/admin/thin-clients/settings?notice=saved").into_response()
}

/// "network_profile auto -> lan, lossless_refinement on -> off", for the
/// audit log.
fn describe_changes(before: &Profile, after: &Profile) -> String {
    let (Ok(serde_json::Value::Object(a)), Ok(serde_json::Value::Object(b))) =
        (serde_json::to_value(before), serde_json::to_value(after))
    else {
        return String::new();
    };
    let show = |v: &serde_json::Value| match v {
        serde_json::Value::Bool(true) => "on".to_string(),
        serde_json::Value::Bool(false) => "off".to_string(),
        serde_json::Value::String(s) if s.is_empty() => "default".to_string(),
        serde_json::Value::String(s) => s.clone(),
        other => other.to_string(),
    };
    a.iter()
        .filter(|(k, v)| b.get(*k) != Some(*v))
        .map(|(k, v)| format!("{k} {} -> {}", show(v), b.get(k).map(show).unwrap_or_default()))
        .collect::<Vec<_>>()
        .join(", ")
}
