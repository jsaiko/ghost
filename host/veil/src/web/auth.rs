// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Who may use the admin UI, and how they stay signed in
// (docs/design/veil.md#admin-web-ui): a PAM login against Veil's own
// stack, membership of [web] admin_group checked through NSS on every
// request, and the same server-side session as the browser client's
// (portal_sessions), whose id travels in a Secure, HttpOnly,
// SameSite=Strict cookie. Every form and every htmx request carries the
// session's CSRF token.
use std::future::Future;
use std::net::{IpAddr, SocketAddr};
use std::pin::Pin;
use std::time::Duration;

use axum::http::{header, HeaderMap, HeaderValue};
use pamconv::Authtok;
use rand::RngCore;

use crate::db::constant_time_eq;
use crate::hosts::{hex, sha256_hex};

/// A user's sign-in to Veil: the browser client (portal.rs) and, for an
/// administrator, the admin UI. Valid on every path.
pub const PORTAL_COOKIE: &str = "veil_session";
/// The header every state-changing portal request carries: the session's
/// CSRF token, which the page learns from /api/session or /api/login.
pub const CSRF_HEADER: &str = "x-veil-csrf";

/// Checks passwords, groups and admin rights. PAM (through ghostauth)
/// and NSS in veild; a fake in the handler tests.
pub trait Authenticator: Send + Sync {
    /// Some(the password, as the host's first prompt will want it) when
    /// it is right. `client` becomes PAM_RHOST.
    fn authenticate_keep<'a>(
        &'a self,
        username: &'a str,
        password: &'a str,
        client: IpAddr,
    ) -> Pin<Box<dyn Future<Output = Option<Authtok>> + Send + 'a>>;
    fn groups(&self, username: &str) -> Vec<String>;
    fn is_admin(&self, username: &str) -> bool;
}

/// The production Authenticator: PAM ("veild"), then the same root policy
/// as the lobby; admin rights from the user's groups through NSS.
pub struct PamAuth {
    pub admin_group: String,
    pub permit_root_login: bool,
    pub permit_empty_passwords: bool,
}

// A web login has nowhere to show a second prompt: one echo-off answer
// (the password) is all it gives, so a stack that asks for more fails.
const PAM_TIMEOUT: Duration = Duration::from_secs(30);

impl Authenticator for PamAuth {
    fn authenticate_keep<'a>(
        &'a self,
        username: &'a str,
        password: &'a str,
        client: IpAddr,
    ) -> Pin<Box<dyn Future<Output = Option<Authtok>> + Send + 'a>> {
        Box::pin(async move {
            let run = async {
                let mut auth = pamconv::AuthSession::start("veild", username.to_string(), client, self.permit_empty_passwords).await;
                let mut answered = false;
                while let Some(req) = auth.next_prompt().await {
                    if !req.echo && !answered {
                        answered = true;
                        req.respond(password.to_string());
                    } else {
                        req.respond(String::new());
                    }
                }
                auth.finish().await.ok().map(|verdict| verdict.authtok)
            };
            // The stack may not have asked for the password at all (then
            // the host will ask the user itself); otherwise keep it.
            let authtok = tokio::time::timeout(PAM_TIMEOUT, run).await.ok().flatten()?;
            match nix::unistd::User::from_name(username) {
                Ok(Some(user)) if self.permit_root_login || !user.uid.is_root() => {
                    Some(authtok.unwrap_or_else(|| Authtok::new(password.as_bytes().to_vec())))
                }
                _ => None,
            }
        })
    }

    fn groups(&self, username: &str) -> Vec<String> {
        match nix::unistd::User::from_name(username) {
            Ok(Some(user)) => crate::lobby::group_names(username, user.gid),
            _ => Vec::new(),
        }
    }

    fn is_admin(&self, username: &str) -> bool {
        self.groups(username).iter().any(|g| *g == self.admin_group)
    }
}

/// A new session id (what the cookie holds) and its CSRF token. The
/// database keeps only the id's hash.
pub fn new_session() -> (String, String) {
    let mut id = [0u8; 32];
    let mut csrf = [0u8; 24];
    rand::rng().fill_bytes(&mut id);
    rand::rng().fill_bytes(&mut csrf);
    (hex(&id), hex(&csrf))
}

pub fn session_key(id: &str) -> String {
    sha256_hex(id.as_bytes())
}

pub fn portal_cookie_value(headers: &HeaderMap) -> Option<String> {
    named_cookie(headers, PORTAL_COOKIE)
}

pub(super) fn named_cookie(headers: &HeaderMap, name: &str) -> Option<String> {
    headers
        .get_all(header::COOKIE)
        .iter()
        .filter_map(|v| v.to_str().ok())
        .flat_map(|v| v.split(';'))
        .filter_map(|kv| kv.trim().split_once('='))
        .find(|(k, _)| *k == name)
        .map(|(_, v)| v.to_string())
        .filter(|v| v.len() == 64 && v.bytes().all(|b| b.is_ascii_hexdigit()))
}

pub fn set_portal_cookie(id: &str, max_age: u64) -> HeaderValue {
    HeaderValue::from_str(&format!("{PORTAL_COOKIE}={id}; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age={max_age}"))
        .expect("hex is a valid header value")
}

pub fn clear_portal_cookie() -> HeaderValue {
    HeaderValue::from_static("veil_session=; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=0")
}

pub fn csrf_ok(expected: &str, given: Option<&str>) -> bool {
    given.is_some_and(|g| constant_time_eq(expected.as_bytes(), g.as_bytes()))
}

/// The client's address for penalties and the audit log. Behind the
/// plain-HTTP listener a reverse proxy is the peer; when that peer is on
/// loopback, the last X-Forwarded-For entry (the one the proxy added) is
/// the client.
pub fn client_ip(peer: SocketAddr, headers: &HeaderMap, behind_proxy: bool) -> IpAddr {
    let peer_ip = peer.ip().to_canonical();
    if behind_proxy && peer_ip.is_loopback() {
        if let Some(ip) = headers
            .get("x-forwarded-for")
            .and_then(|v| v.to_str().ok())
            .and_then(|v| v.rsplit(',').next())
            .and_then(|v| v.trim().parse::<IpAddr>().ok())
        {
            return ip.to_canonical();
        }
    }
    peer_ip
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cookies_are_found_among_others() {
        let mut h = HeaderMap::new();
        let id = "a".repeat(64);
        h.insert(header::COOKIE, HeaderValue::from_str(&format!("x=1; {PORTAL_COOKIE}={id}; y=2")).unwrap());
        assert_eq!(portal_cookie_value(&h), Some(id));
        let mut h = HeaderMap::new();
        h.insert(header::COOKIE, HeaderValue::from_static("veil_session=short"));
        assert_eq!(portal_cookie_value(&h), None);
    }

    #[test]
    fn forwarded_for_is_believed_only_from_a_local_proxy() {
        let mut h = HeaderMap::new();
        h.insert("x-forwarded-for", HeaderValue::from_static("203.0.113.9, 198.51.100.7"));
        let local: SocketAddr = "127.0.0.1:5000".parse().unwrap();
        let remote: SocketAddr = "192.0.2.1:5000".parse().unwrap();
        assert_eq!(client_ip(local, &h, true), "198.51.100.7".parse::<IpAddr>().unwrap());
        assert_eq!(client_ip(local, &h, false), "127.0.0.1".parse::<IpAddr>().unwrap());
        assert_eq!(client_ip(remote, &h, true), "192.0.2.1".parse::<IpAddr>().unwrap());
    }
}
