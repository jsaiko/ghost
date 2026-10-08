// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The admin UI's handlers against an in-memory database and a fake
// authenticator: who gets in, and that nothing changes without the
// session's CSRF token.
use std::collections::HashSet;
use std::future::Future;
use std::net::{IpAddr, SocketAddr};
use std::pin::Pin;
use std::sync::{Arc, Mutex};

use axum::body::Body;
use axum::extract::connect_info::MockConnectInfo;
use axum::http::{header, Request, StatusCode};
use http_body_util::BodyExt;
use tower::ServiceExt;

use super::auth::Authenticator;
use super::*;
use crate::config::Config;
use crate::db::Db;
use crate::hosts::Hosts;

struct FakeAuth {
    admins: Mutex<HashSet<String>>,
}

impl Authenticator for FakeAuth {
    fn authenticate_keep<'a>(
        &'a self,
        username: &'a str,
        password: &'a str,
        _client: IpAddr,
    ) -> Pin<Box<dyn Future<Output = Option<pamconv::Authtok>> + Send + 'a>> {
        Box::pin(async move {
            (matches!(username, "alice" | "bob") && password == "pw").then(|| pamconv::Authtok::new(password.into()))
        })
    }

    fn groups(&self, username: &str) -> Vec<String> {
        vec![format!("{username}-group")]
    }

    fn is_admin(&self, username: &str) -> bool {
        self.admins.lock().unwrap().contains(username)
    }
}

struct Harness {
    app: Router,
    veil: Arc<Veil>,
    auth: Arc<FakeAuth>,
}

fn harness() -> Harness {
    harness_with(Config::default())
}

fn harness_with(config: Config) -> Harness {
    let db = Arc::new(Db::in_memory());
    let hosts = Arc::new(Hosts::new(db.clone(), config.gateway.default_mode, 4));
    let veil = Arc::new(Veil {
        penalties: preauth::Penalties::new(config.auth.penalties),
        thin_clients: crate::thin_clients::ThinClients::new(db.clone(), None),
        db,
        hosts,
        certs: crate::lobby_tls::LobbyCerts::for_tests(),
        config,
        gateway: Default::default(),
    });
    let auth = Arc::new(FakeAuth { admins: Mutex::new(["alice".to_string()].into_iter().collect()) });
    let app = router(WebState { veil: veil.clone(), auth: auth.clone(), flows: Default::default(), passwords: Default::default(), behind_proxy: false })
        .layer(MockConnectInfo("192.0.2.10:40000".parse::<SocketAddr>().unwrap()));
    Harness { app, veil, auth }
}

async fn send(app: &Router, req: Request<Body>) -> (StatusCode, axum::http::HeaderMap, String) {
    let resp = app.clone().oneshot(req).await.unwrap();
    let status = resp.status();
    let headers = resp.headers().clone();
    let body = resp.into_body().collect().await.unwrap().to_bytes();
    (status, headers, String::from_utf8_lossy(&body).into_owned())
}

fn get(path: &str, cookie: Option<&str>) -> Request<Body> {
    let mut b = Request::get(path);
    if let Some(c) = cookie {
        b = b.header(header::COOKIE, c);
    }
    b.body(Body::empty()).unwrap()
}

fn post(path: &str, cookie: Option<&str>, form: &str) -> Request<Body> {
    let mut b = Request::post(path).header(header::CONTENT_TYPE, "application/x-www-form-urlencoded");
    if let Some(c) = cookie {
        b = b.header(header::COOKIE, c);
    }
    b.body(Body::from(form.to_string())).unwrap()
}

/// Signs alice in; returns the cookie header value and her CSRF token.
async fn sign_in(h: &Harness) -> (String, String) {
    let (status, headers, _) = send(&h.app, post("/admin/login", None, "username=alice&password=pw")).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    let set = headers.get(header::SET_COOKIE).unwrap().to_str().unwrap().to_string();
    assert!(set.contains("Secure") && set.contains("HttpOnly") && set.contains("SameSite=Strict"), "{set}");
    let cookie = set.split(';').next().unwrap().to_string();
    let (status, _, body) = send(&h.app, get("/admin/hosts", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    let csrf = body.split("name=\"csrf\" value=\"").nth(1).unwrap().split('"').next().unwrap().to_string();
    (cookie, csrf)
}

fn add_device(h: &Harness) -> String {
    h.veil.db.join_device("d1", "host1", "host1.lan", &"cd".repeat(32), DeviceMode::Auto).unwrap()
}

#[tokio::test]
async fn pages_need_a_session() {
    let h = harness();
    let (status, headers, _) = send(&h.app, get("/admin/hosts", None)).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    assert_eq!(headers.get(header::LOCATION).unwrap(), "/admin/login");
    // htmx gets a redirect it can follow, not a login page in a table.
    let req = Request::get("/admin/hosts/rows").header("hx-request", "true").body(Body::empty()).unwrap();
    let (status, headers, _) = send(&h.app, req).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    assert_eq!(headers.get("hx-redirect").unwrap(), "/admin/login");
    // A made-up session id is no better than none.
    let fake = format!("{}={}", auth::PORTAL_COOKIE, "0".repeat(64));
    let (status, _, _) = send(&h.app, get("/admin/hosts", Some(&fake))).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
}

#[tokio::test]
async fn toolbar_has_hosts_and_thin_clients() {
    let h = harness();
    let (cookie, _) = sign_in(&h).await;
    let (status, _, body) = send(&h.app, get("/admin/thin-clients", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains(">Hosts</a>") && body.contains(">Thin Clients</a>"), "{body}");
    // Adding a host is a button on the Hosts page, not a tab.
    assert!(!body.contains("href=\"/admin/hosts/new\""), "{body}");
    let (status, _, body) = send(&h.app, get("/admin/hosts", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains("href=\"/admin/hosts/new\""), "{body}");
    // /admin/hosts/new is still the add page, not a host with the id "new".
    let (status, _, body) = send(&h.app, get("/admin/hosts/new", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains("<h1>Add host</h1>"), "{body}");
}

#[tokio::test]
async fn only_admins_sign_in() {
    let h = harness();
    let (status, headers, _) = send(&h.app, post("/admin/login", None, "username=alice&password=wrong")).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    assert!(headers.get(header::SET_COOKIE).is_none());
    let (status, headers, body) = send(&h.app, post("/admin/login", None, "username=bob&password=pw")).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    assert!(headers.get(header::SET_COOKIE).is_none());
    assert!(body.contains("an administrator (the ghost-admins group)"), "{body}");
    sign_in(&h).await;
    let actions: Vec<String> = h.veil.db.audit_log(10, None).unwrap().into_iter().map(|(_, e)| e.action).collect();
    assert_eq!(actions, vec!["sign in", "web login refused", "web login failed"]);
}

#[tokio::test]
async fn failed_sign_ins_are_penalised() {
    let h = harness();
    // sshd's defaults: 5 s per failure, refused past 15 s.
    for _ in 0..4 {
        send(&h.app, post("/admin/login", None, "username=alice&password=wrong")).await;
    }
    let (status, _, _) = send(&h.app, post("/admin/login", None, "username=alice&password=pw")).await;
    assert_eq!(status, StatusCode::TOO_MANY_REQUESTS);
}

#[tokio::test]
async fn admin_rights_are_checked_on_every_request() {
    let h = harness();
    let (cookie, _) = sign_in(&h).await;
    h.auth.admins.lock().unwrap().clear();
    let (status, _, _) = send(&h.app, get("/admin/hosts", Some(&cookie))).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    // Still signed in to Veil, only not an administrator: the browser
    // client keeps working, and the admin UI comes back with the group.
    let (status, _, body) = send(&h.app, get("/api/session", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains(r#""admin":false"#), "{body}");
    h.auth.admins.lock().unwrap().insert("alice".into());
    let (status, _, _) = send(&h.app, get("/admin/hosts", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
}

#[tokio::test]
async fn one_sign_in_serves_the_admin_ui_and_the_browser_client() {
    let h = harness();
    // Signed in at the admin UI: the browser client knows it too, and
    // offers the way across.
    let (cookie, csrf) = sign_in(&h).await;
    let (status, _, body) = send(&h.app, get("/api/session", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    let v: serde_json::Value = serde_json::from_str(&body).unwrap();
    assert_eq!((v["user"].as_str(), v["admin"].as_bool(), v["csrf"].as_str()), (Some("alice"), Some(true), Some(csrf.as_str())), "{body}");

    // Signed in at the browser client: the admin UI takes the same cookie.
    let (portal_cookie, portal_csrf) = portal_sign_in(&h, "alice").await;
    let (status, _, page) = send(&h.app, get("/admin/hosts", Some(&portal_cookie))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(page.contains(&format!("name=\"csrf\" value=\"{portal_csrf}\"")), "one CSRF token for both");
    assert!(page.contains(r#"href="/""#), "the way back to the desktops");

    // A user who isn't an administrator is told so, and stays signed in.
    let (bob, _) = portal_sign_in(&h, "bob").await;
    let (status, _, body) = send(&h.app, get("/admin/hosts", Some(&bob))).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    assert!(body.contains("Not an administrator") && body.contains(r#"<a href="/">Back</a>"#), "{body}");
    let (status, _, body) = send(&h.app, get("/api/session", Some(&bob))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains(r#""admin":false"#), "{body}");

    // Signing out at the admin UI ends the browser client's session too.
    let (status, headers, _) = send(&h.app, post("/admin/logout", Some(&portal_cookie), &format!("csrf={portal_csrf}"))).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    assert_eq!(set_cookies(&headers).iter().filter(|c| c.contains("Max-Age=0")).count(), 2);
    let (status, _, _) = send(&h.app, get("/api/session", Some(&portal_cookie))).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
}

#[tokio::test]
async fn changes_need_the_csrf_token() {
    let h = harness();
    let id = add_device(&h);
    let (cookie, csrf) = sign_in(&h).await;
    let path = format!("/admin/hosts/{id}/grant");
    let (status, _, _) = send(&h.app, post(&path, Some(&cookie), "kind=user&name=carol")).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _, _) = send(&h.app, post(&path, Some(&cookie), "csrf=00&kind=user&name=carol")).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    assert!(h.veil.db.entitlements(&id).unwrap().is_empty());
    // Another session's token doesn't do either.
    let (_, other_csrf) = sign_in(&h).await;
    assert_ne!(other_csrf, csrf);
    let (status, _, _) = send(&h.app, post(&path, Some(&cookie), &format!("csrf={other_csrf}&kind=user&name=carol"))).await;
    assert_eq!(status, StatusCode::FORBIDDEN);

    let (status, _, _) = send(&h.app, post(&path, Some(&cookie), &format!("csrf={csrf}&kind=user&name=carol"))).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    assert_eq!(h.veil.db.entitlements(&id).unwrap(), vec![Grantee::User("carol".into())]);
    // The htmx header works in place of the form field.
    let req = Request::post(format!("/admin/hosts/{id}/revoke"))
        .header(header::CONTENT_TYPE, "application/x-www-form-urlencoded")
        .header(header::COOKIE, &cookie)
        .header("x-csrf-token", &csrf)
        .body(Body::from("kind=user&name=carol"))
        .unwrap();
    let (status, _, _) = send(&h.app, req).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    assert!(h.veil.db.entitlements(&id).unwrap().is_empty());
}

#[tokio::test]
async fn devices_can_be_edited_and_removed() {
    let h = harness();
    let id = add_device(&h);
    let (cookie, csrf) = sign_in(&h).await;
    let form = format!("csrf={csrf}&name=Studio&client_address=studio.example&mode=gateway");
    let (status, _, _) = send(&h.app, post(&format!("/admin/hosts/{id}"), Some(&cookie), &form)).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    let d = h.veil.db.device(&id).unwrap().unwrap();
    assert_eq!((d.name.as_str(), d.client_address.as_str(), d.mode, d.enabled), ("Studio", "studio.example", DeviceMode::Gateway, false));
    let (status, _, body) = send(&h.app, get(&format!("/admin/hosts/{id}"), Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains("Studio") && body.contains("disabled"));
    let form = format!("csrf={csrf}&name=Studio&client_address=studio.example&mode=bogus&enabled=1");
    let (status, _, _) = send(&h.app, post(&format!("/admin/hosts/{id}"), Some(&cookie), &form)).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _, _) = send(&h.app, post(&format!("/admin/hosts/{id}/remove"), Some(&cookie), &format!("csrf={csrf}"))).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    assert!(h.veil.db.device(&id).unwrap().is_none());
    let actions: Vec<String> = h.veil.db.audit_log(3, None).unwrap().into_iter().map(|(_, e)| e.action).collect();
    assert_eq!(actions[..2], ["device removed", "device changed"]);
}

#[tokio::test]
async fn add_host_issues_a_join_command() {
    let h = harness();
    let (cookie, csrf) = sign_in(&h).await;
    let (status, _, body) = send(&h.app, post("/admin/hosts/new", Some(&cookie), &format!("csrf={csrf}&ttl=15m"))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains("ghostd join "), "{body}");
    assert!(body.contains(&format!(":sha256:{}", h.veil.certs.lobby_fingerprint())));
    assert!(body.contains("15 minutes"));
}

#[tokio::test]
async fn logout_ends_the_session() {
    let h = harness();
    let (cookie, csrf) = sign_in(&h).await;
    let (status, headers, _) = send(&h.app, post("/admin/logout", Some(&cookie), &format!("csrf={csrf}"))).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    assert!(headers.get(header::SET_COOKIE).unwrap().to_str().unwrap().contains("Max-Age=0"));
    let (status, _, _) = send(&h.app, get("/admin/hosts", Some(&cookie))).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
}

#[tokio::test]
async fn responses_carry_security_headers() {
    let h = harness();
    let (_, headers, _) = send(&h.app, get("/admin/login", None)).await;
    assert!(headers.get(header::CONTENT_SECURITY_POLICY).unwrap().to_str().unwrap().contains("script-src 'self'"));
    assert_eq!(headers.get(header::X_FRAME_OPTIONS).unwrap(), "DENY");
    let (status, headers, _) = send(&h.app, get("/admin/static/htmx.min.js", None)).await;
    assert_eq!(status, StatusCode::OK);
    assert_eq!(headers.get(header::CONTENT_TYPE).unwrap(), "text/javascript");
    let (status, _, _) = send(&h.app, get("/admin/static/../../etc/passwd", None)).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
}

#[test]
fn utc_formats_known_instants() {
    assert_eq!(utc(0), "1970-01-01 00:00:00");
    assert_eq!(utc(1_700_000_000), "2023-11-14 22:13:20");
    assert_eq!(utc(951_782_400), "2000-02-29 00:00:00");
}

#[test]
fn rates_and_ages_read_naturally() {
    assert_eq!(rate(1_500_000), "12.0 Mbit/s");
    assert_eq!(rate(0), "0 bit/s");
    assert_eq!(ago(100, 100), "just now");
    assert_eq!(ago(200, 100), "1 min ago");
    assert_eq!(ago(100_000, 0), "1 d ago");
}

// --- the browser portal's API (portal.rs) ---

fn post_json(path: &str, body: &str) -> Request<Body> {
    Request::post(path).header(header::CONTENT_TYPE, "application/json").body(Body::from(body.to_string())).unwrap()
}

/// A portal request: JSON body, the session cookie, the CSRF header.
fn portal_req(method: &str, path: &str, cookie: Option<&str>, csrf: Option<&str>, body: &str) -> Request<Body> {
    let mut b = Request::builder().method(method).uri(path).header(header::CONTENT_TYPE, "application/json");
    if let Some(c) = cookie {
        b = b.header(header::COOKIE, c);
    }
    if let Some(t) = csrf {
        b = b.header("x-veil-csrf", t);
    }
    b.body(Body::from(body.to_string())).unwrap()
}

/// Signs `user` in to the portal; returns the cookie header value and the
/// CSRF token.
async fn portal_sign_in(h: &Harness, user: &str) -> (String, String) {
    let (status, headers, body) =
        send(&h.app, post_json("/api/login", &format!(r#"{{"username":"{user}","password":"pw"}}"#))).await;
    assert_eq!(status, StatusCode::OK, "{body}");
    let set = headers.get(header::SET_COOKIE).unwrap().to_str().unwrap().to_string();
    assert!(set.contains("Secure") && set.contains("HttpOnly") && set.contains("SameSite=Strict") && set.contains("Path=/;"), "{set}");
    let v: serde_json::Value = serde_json::from_str(&body).unwrap();
    (set.split(';').next().unwrap().to_string(), v["csrf"].as_str().unwrap().to_string())
}

#[tokio::test]
async fn portal_login_lists_entitled_devices_and_keeps_no_password() {
    let h = harness();
    let id = add_device(&h);
    h.veil.db.join_device("d2", "host2", "host2.lan", &"ef".repeat(32), DeviceMode::Auto).unwrap();
    h.veil.db.grant(&id, &Grantee::Group("alice-group".into())).unwrap();
    let (status, headers, body) = send(&h.app, post_json("/api/login", r#"{"username":"alice","password":"wrong"}"#)).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    assert!(body.contains(r#""step":"error""#), "{body}");
    assert!(headers.get(header::SET_COOKIE).is_none(), "a failed sign-in sets no cookie");

    let (status, _, body) = send(&h.app, post_json("/api/login", r#"{"username":"alice","password":"pw"}"#)).await;
    assert_eq!(status, StatusCode::OK);
    let v: serde_json::Value = serde_json::from_str(&body).unwrap();
    assert_eq!(v["step"], "devices");
    assert_eq!(v["user"], "alice");
    assert!(v.get("flow").is_none(), "signing in to Veil opens no host login: {body}");
    assert!(!body.contains("pw\""), "the password is not echoed: {body}");
    let devices = v["devices"].as_array().unwrap();
    assert_eq!(devices.len(), 1, "only the entitled device: {body}");
    assert_eq!(devices[0]["id"], id.as_str());
    assert_eq!(devices[0]["online"], false);
}

fn set_cookies(headers: &axum::http::HeaderMap) -> Vec<String> {
    headers.get_all(header::SET_COOKIE).iter().map(|v| v.to_str().unwrap().to_string()).collect()
}

#[tokio::test]
async fn portal_sign_in_remembers_the_password_with_its_key_in_a_cookie() {
    let h = harness();
    let id = add_device(&h);
    h.veil.db.grant(&id, &Grantee::Group("alice-group".into())).unwrap();
    let (_, headers, body) = send(&h.app, post_json("/api/login", r#"{"username":"alice","password":"pw"}"#)).await;
    let cookies = set_cookies(&headers);
    let key = cookies.iter().find(|c| c.starts_with("veil_key=")).expect("a key cookie");
    assert!(key.contains("Path=/api;") && key.contains("HttpOnly") && key.contains("Secure") && key.contains("Max-Age=43200"), "{key}");
    assert!(!key.contains("pw;"), "{key}");
    let v: serde_json::Value = serde_json::from_str(&body).unwrap();
    assert_eq!(v["devices"][0]["remembered"], true, "{body}");
    let session = cookies.iter().find(|c| c.starts_with("veil_session=")).unwrap().split(';').next().unwrap().to_string();
    let (_, _, body) = send(&h.app, get("/api/session", Some(&session))).await;
    let v: serde_json::Value = serde_json::from_str(&body).unwrap();
    assert_eq!(v["devices"][0]["remembered"], true, "{body}");

    // Sign-out clears both cookies, and nothing is remembered after.
    let csrf = v["csrf"].as_str().unwrap();
    let (_, headers, _) = send(&h.app, portal_req("POST", "/api/logout", Some(&session), Some(csrf), "{}")).await;
    let cleared = set_cookies(&headers);
    assert!(cleared.iter().any(|c| c.starts_with("veil_session=;") && c.contains("Max-Age=0")), "{cleared:?}");
    assert!(cleared.iter().any(|c| c.starts_with("veil_key=;") && c.contains("Max-Age=0")), "{cleared:?}");
}

#[tokio::test]
async fn remember_password_zero_keeps_nothing() {
    let mut config = Config::default();
    config.web.remember_password = std::time::Duration::ZERO;
    let h = harness_with(config);
    let id = add_device(&h);
    h.veil.db.grant(&id, &Grantee::Group("alice-group".into())).unwrap();
    let (_, headers, body) = send(&h.app, post_json("/api/login", r#"{"username":"alice","password":"pw"}"#)).await;
    let cookies = set_cookies(&headers);
    assert!(cookies.iter().any(|c| c.starts_with("veil_key=;") && c.contains("Max-Age=0")), "{cookies:?}");
    let v: serde_json::Value = serde_json::from_str(&body).unwrap();
    assert_eq!(v["devices"][0]["remembered"], false, "{body}");
}

#[tokio::test]
async fn portal_session_survives_across_requests_and_ends_on_sign_out() {
    let h = harness();
    let id = add_device(&h);
    h.veil.db.grant(&id, &Grantee::Group("alice-group".into())).unwrap();
    let (status, _, body) = send(&h.app, get("/api/session", None)).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    assert!(body.contains(r#""step":"signed_out""#), "{body}");

    let (cookie, csrf) = portal_sign_in(&h, "alice").await;
    for _ in 0..2 {
        let (status, _, body) = send(&h.app, get("/api/session", Some(&cookie))).await;
        assert_eq!(status, StatusCode::OK, "{body}");
        let v: serde_json::Value = serde_json::from_str(&body).unwrap();
        assert_eq!(v["csrf"], csrf.as_str());
        assert_eq!(v["devices"].as_array().unwrap().len(), 1);
    }

    // Sign out needs the token, then ends the session for good.
    let (status, _, _) = send(&h.app, portal_req("POST", "/api/logout", Some(&cookie), None, "{}")).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, headers, _) = send(&h.app, portal_req("POST", "/api/logout", Some(&cookie), Some(&csrf), "{}")).await;
    assert_eq!(status, StatusCode::OK);
    assert!(headers.get(header::SET_COOKIE).unwrap().to_str().unwrap().contains("Max-Age=0"));
    let (status, _, _) = send(&h.app, get("/api/session", Some(&cookie))).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
}

#[tokio::test]
async fn signing_in_again_retires_the_old_session() {
    let h = harness();
    let (old, _) = portal_sign_in(&h, "alice").await;
    let (status, headers, _) = send(
        &h.app,
        Request::post("/api/login")
            .header(header::CONTENT_TYPE, "application/json")
            .header(header::COOKIE, &old)
            .body(Body::from(r#"{"username":"bob","password":"pw"}"#))
            .unwrap(),
    )
    .await;
    assert_eq!(status, StatusCode::OK);
    let new = headers.get(header::SET_COOKIE).unwrap().to_str().unwrap().split(';').next().unwrap().to_string();
    assert_ne!(old, new);
    let (status, _, _) = send(&h.app, get("/api/session", Some(&old))).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, _, body) = send(&h.app, get("/api/session", Some(&new))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains("bob"), "{body}");
}

#[tokio::test]
async fn portal_calls_that_change_things_need_the_csrf_token() {
    let h = harness();
    let id = add_device(&h);
    h.veil.db.grant(&id, &Grantee::Group("alice-group".into())).unwrap();
    let (cookie, csrf) = portal_sign_in(&h, "alice").await;
    let body = format!(r#"{{"device_id":"{id}","password":"pw"}}"#);
    for token in [None, Some("wrong")] {
        let (status, _, _) = send(&h.app, portal_req("POST", "/api/connect", Some(&cookie), token, &body)).await;
        assert_eq!(status, StatusCode::FORBIDDEN, "{token:?}");
    }
    let flow = "0".repeat(32);
    let (status, _, _) = send(&h.app, portal_req("POST", &format!("/api/flows/{flow}/answer"), Some(&cookie), None, r#"{"response":"x"}"#)).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    // With the token the request is let through to its own checks: the
    // device is entitled but offline.
    let (_, _, out) = send(&h.app, portal_req("POST", "/api/connect", Some(&cookie), Some(&csrf), &body)).await;
    let v: serde_json::Value = serde_json::from_str(&out).unwrap();
    assert_eq!(v["code"], ipc::lobby::LobbyErrorCode::LobbyErrorHostOffline as i32, "{out}");
}

#[tokio::test]
async fn portal_connect_and_flows_need_a_session_and_an_entitlement() {
    let h = harness();
    let id = add_device(&h);
    h.veil.db.join_device("d2", "host2", "host2.lan", &"ef".repeat(32), DeviceMode::Auto).unwrap();
    h.veil.db.grant(&id, &Grantee::Group("alice-group".into())).unwrap();
    let body = format!(r#"{{"device_id":"{id}","password":"pw"}}"#);
    let (status, _, _) = send(&h.app, portal_req("POST", "/api/connect", None, None, &body)).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);
    let (status, _, _) = send(&h.app, portal_req("POST", &format!("/api/flows/{}/open", "0".repeat(32)), None, None, "{}")).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED);

    let (cookie, csrf) = portal_sign_in(&h, "alice").await;
    // Not entitled.
    let (status, _, out) =
        send(&h.app, portal_req("POST", "/api/connect", Some(&cookie), Some(&csrf), r#"{"device_id":"d2","password":"pw"}"#)).await;
    assert_eq!(status, StatusCode::FORBIDDEN, "{out}");
    // A flow id that doesn't exist.
    let (status, _, _) = send(&h.app, portal_req("POST", &format!("/api/flows/{}/open", "0".repeat(32)), Some(&cookie), Some(&csrf), "{}")).await;
    assert_eq!(status, StatusCode::GONE);
}

#[tokio::test]
async fn portal_connect_takes_an_optional_host_username() {
    let h = harness();
    let id = add_device(&h);
    h.veil.db.grant(&id, &Grantee::Group("alice-group".into())).unwrap();
    let (cookie, csrf) = portal_sign_in(&h, "alice").await;
    let connect = |body: String| portal_req("POST", "/api/connect", Some(&cookie), Some(&csrf), &body);
    // Absent, empty, or another account: all reach the host check (offline
    // here); entitlement is the signed-in user's either way.
    for user in ["", r#","username":"""#, r#","username":"  ""#, r#","username":"deploy""#] {
        let body = if user.is_empty() { format!(r#"{{"device_id":"{id}","password":"pw"}}"#) } else { format!(r#"{{"device_id":"{id}"{user},"password":"pw"}}"#) };
        let (_, _, out) = send(&h.app, connect(body)).await;
        let v: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert_eq!(v["code"], ipc::lobby::LobbyErrorCode::LobbyErrorHostOffline as i32, "{user:?}: {out}");
    }
    // A name with a control character is refused before the host is asked.
    let (status, _, out) = send(&h.app, connect(format!(r#"{{"device_id":"{id}","username":"a\nb","password":"pw"}}"#))).await;
    assert_eq!(status, StatusCode::UNAUTHORIZED, "{out}");
    assert!(out.contains("valid username"), "{out}");
}

#[tokio::test]
async fn portal_page_and_assets_are_served() {
    let h = harness();
    let (status, headers, body) = send(&h.app, get("/", None)).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains("/app/main.js"));
    assert!(headers.get(header::CONTENT_SECURITY_POLICY).unwrap().to_str().unwrap().contains("connect-src 'self'"));
    for f in ["main.js", "proto.js", "session.js", "transport.js", "video.js", "refine.js", "fzstd.js", "display-icon.png", "audio.js", "audio-worklet.js", "input.js", "keymap.js", "gamepad.js", "log.js", "app.css"] {
        let (status, _, body) = send(&h.app, get(&format!("/app/{f}"), None)).await;
        assert_eq!(status, StatusCode::OK, "{f}");
        assert!(!body.is_empty(), "{f}");
    }
    let (status, _, _) = send(&h.app, get("/app/nope.js", None)).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    // The Settings tab's own address serves the same page (main.js picks
    // the view from the path).
    let (status, headers, body) = send(&h.app, get("/settings", None)).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains("/app/main.js"));
    assert!(headers.get(header::CONTENT_SECURITY_POLICY).is_some());
}

#[tokio::test]
async fn thin_clients_are_listed_named_and_removed() {
    let h = harness();
    let report = r#"{"hostname":"wisp-525400123456","address":"192.0.2.50","cpu_model":"Test CPU","memory_bytes":4294967296,
        "displays":[{"connector":"HDMI-A-1","width":1920,"height":1080,"refresh_mhz":60000}],"hw_decode":["vaapi:h264"]}"#;
    h.veil.db.thin_client_seen("52:54:00:12:34:56", report).unwrap();
    let (cookie, csrf) = sign_in(&h).await;

    let (status, _, body) = send(&h.app, get("/admin/thin-clients", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains("wisp-525400123456") && body.contains("52:54:00:12:34:56") && body.contains("offline"), "{body}");
    let (status, _, body) = send(&h.app, get("/admin/thin-clients/rows", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains("192.0.2.50"), "{body}");

    let (status, _, body) = send(&h.app, get("/admin/thin-clients/52:54:00:12:34:56", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains("Test CPU") && body.contains("4.0 GiB") && body.contains("HDMI-A-1 1920x1080 @ 60.00 Hz"), "{body}");
    assert!(body.contains("vaapi:h264") && body.contains("Remove thin client"), "{body}");

    // Renaming needs the token, and is audited.
    let path = "/admin/thin-clients/52:54:00:12:34:56";
    let (status, _, _) = send(&h.app, post(path, Some(&cookie), "name=Front+desk")).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _, _) = send(&h.app, post(path, Some(&cookie), &format!("csrf={csrf}&name=Front+desk"))).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    assert_eq!(h.veil.db.thin_client("52:54:00:12:34:56").unwrap().unwrap().name, "Front desk");
    let (_, _, body) = send(&h.app, get("/admin/thin-clients", Some(&cookie))).await;
    assert!(body.contains(">Front desk</a>"), "{body}");

    // Power actions need the token and a connected client, and offline
    // clients aren't offered them.
    assert!(!body.contains("Shut down"), "{body}");
    let act = format!("{path}/action");
    let (status, _, _) = send(&h.app, post(&act, Some(&cookie), "action=reboot")).await;
    assert_eq!(status, StatusCode::FORBIDDEN);
    let (status, _, _) = send(&h.app, post(&act, Some(&cookie), &format!("csrf={csrf}&action=explode"))).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    let (status, _, body) = send(&h.app, post(&act, Some(&cookie), &format!("csrf={csrf}&action=power_off"))).await;
    assert_eq!(status, StatusCode::CONFLICT);
    assert!(body.contains("offline"), "{body}");

    let (status, _, _) = send(&h.app, post(&format!("{path}/remove"), Some(&cookie), &format!("csrf={csrf}"))).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    assert!(h.veil.db.thin_client("52:54:00:12:34:56").unwrap().is_none());
    let (status, _, _) = send(&h.app, get(path, Some(&cookie))).await;
    assert_eq!(status, StatusCode::NOT_FOUND);
    let actions: Vec<String> = h.veil.db.audit_log(2, None).unwrap().into_iter().map(|(_, e)| e.action).collect();
    assert_eq!(actions, vec!["thin client removed", "thin client renamed"]);
}

#[tokio::test]
async fn the_thin_client_profile_is_edited_and_checked() {
    let h = harness();
    let (cookie, csrf) = sign_in(&h).await;
    let (status, _, body) = send(&h.app, get("/admin/thin-clients/settings", Some(&cookie))).await;
    assert_eq!(status, StatusCode::OK);
    assert!(body.contains(r#"name="lossless_refinement" value="1" checked"#), "the defaults are spectre-qt's: {body}");

    // Unticked boxes are simply absent from the form.
    let form = format!(
        "csrf={csrf}&resolution=1920x1080&view=fit&preferred_decoder=software&preferred_codec=h264&network_profile=lan&forward_gamepads=1&display_sleep_minutes=45"
    );
    let (status, _, _) = send(&h.app, post("/admin/thin-clients/settings", Some(&cookie), &form)).await;
    assert_eq!(status, StatusCode::SEE_OTHER);
    let p = h.veil.thin_clients.profile();
    assert_eq!((p.resolution.as_str(), p.view.as_str(), p.preferred_decoder.as_str()), ("1920x1080", "fit", "software"));
    assert!(!p.lossless_refinement && !p.allow_pyrowave && p.forward_gamepads);
    assert_eq!(p.display_sleep_minutes, 45);
    let entry = &h.veil.db.audit_log(1, None).unwrap()[0].1;
    assert_eq!(entry.action, "thin client settings changed");
    assert!(entry.detail.contains("network_profile auto -> lan") && entry.detail.contains("lossless_refinement on -> off"), "{}", entry.detail);
    assert!(entry.detail.contains("display_sleep_minutes 20 -> 45"), "{}", entry.detail);
    let (_, _, body) = send(&h.app, get("/admin/thin-clients/settings", Some(&cookie))).await;
    assert!(body.contains(r#"<option value="1920x1080" selected>"#), "{body}");

    let bad = format!("csrf={csrf}&resolution=huge&view=&preferred_decoder=vulkan&preferred_codec=&network_profile=auto&display_sleep_minutes=20");
    let (status, _, _) = send(&h.app, post("/admin/thin-clients/settings", Some(&cookie), &bad)).await;
    assert_eq!(status, StatusCode::BAD_REQUEST);
    assert_eq!(h.veil.thin_clients.profile().resolution, "1920x1080", "unchanged");
}
