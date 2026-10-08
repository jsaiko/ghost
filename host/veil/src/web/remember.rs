// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Remembering a browser user's password for their hosts
// (docs/design/browser-client.md#the-remembered-password), so that picking
// a host after signing in to Veil needs no second password while the
// host's own PAM still gets it and ghostseat still unlocks the wallet.
//
// The password is split between veild and the browser. At sign-in veild
// seals it with a fresh random key (ChaCha20-Poly1305) and keeps only the
// ciphertext, in memory, under the portal session; the key goes to the
// browser in the `veil_key` cookie and veild forgets it. /api/connect
// brings the two together for the one host login and wipes the result.
// Neither half alone gives the password back: not a core dump, swap or VM
// snapshot of veild (ciphertext only), not the database (nothing in it),
// not the cookie (no ciphertext).
//
// It is also bound to the address that signed in: a request from any
// other address drops it, and the page asks for the password as before.
// A host that refuses it is never offered it again for that session (no
// lockouts from retrying a drifted password). Sign-out, sign-in, veild
// restarting or `[web] remember_password` passing ends it.
use std::collections::{HashMap, HashSet};
use std::net::IpAddr;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use axum::http::{HeaderMap, HeaderValue};
use pamconv::Authtok;
use rand::RngCore;
use ring::aead::{Aad, LessSafeKey, Nonce, UnboundKey, CHACHA20_POLY1305, NONCE_LEN};
use zeroize::Zeroizing;

use crate::hosts::hex;

pub const KEY_COOKIE: &str = "veil_key";

struct Sealed {
    username: String,
    client: IpAddr,
    ciphertext: Vec<u8>,
    expires: Instant,
    /// Devices whose host refused it.
    refused: HashSet<String>,
}

/// Sealed passwords by portal session key (the hash of the session id).
#[derive(Default)]
pub struct Passwords(Mutex<HashMap<String, Sealed>>);

// Each key seals exactly one message, so a fixed nonce is safe.
const NONCE: [u8; NONCE_LEN] = [0; NONCE_LEN];

fn aead_key(raw: &[u8]) -> LessSafeKey {
    LessSafeKey::new(UnboundKey::new(&CHACHA20_POLY1305, raw).expect("a 32-byte key"))
}

/// What the ciphertext is bound to besides its key.
fn aad(session: &str, username: &str) -> Vec<u8> {
    format!("veil-remember\0{session}\0{username}").into_bytes()
}

impl Passwords {
    fn map(&self) -> std::sync::MutexGuard<'_, HashMap<String, Sealed>> {
        self.0.lock().unwrap_or_else(|p| p.into_inner())
    }

    /// Seals `authtok` for `session`; returns the key for the cookie.
    pub fn remember(&self, session: &str, username: &str, client: IpAddr, authtok: &Authtok, ttl: Duration) -> String {
        let mut raw = Zeroizing::new([0u8; 32]);
        rand::rng().fill_bytes(raw.as_mut());
        let mut buf = Zeroizing::new(authtok.as_bytes().to_vec());
        aead_key(raw.as_ref())
            .seal_in_place_append_tag(Nonce::assume_unique_for_key(NONCE), Aad::from(aad(session, username)), &mut *buf)
            .expect("sealing a short buffer");
        let now = Instant::now();
        let mut map = self.map();
        map.retain(|_, s| s.expires > now);
        map.insert(
            session.to_string(),
            Sealed { username: username.to_string(), client, ciphertext: buf.to_vec(), expires: now + ttl, refused: HashSet::new() },
        );
        hex(raw.as_ref())
    }

    /// Whether `session` from `client` has a password `device` hasn't
    /// refused. An entry seen from another address is dropped.
    pub fn usable(&self, session: &str, client: IpAddr, device: &str) -> bool {
        let mut map = self.map();
        let Some(s) = map.get(session) else { return false };
        if s.client != client || s.expires <= Instant::now() {
            map.remove(session);
            return false;
        }
        !s.refused.contains(device)
    }

    /// The password for one login to `device`, given the cookie's key.
    pub fn open(&self, session: &str, username: &str, client: IpAddr, device: &str, key: Option<&str>) -> Option<Authtok> {
        if !self.usable(session, client, device) {
            return None;
        }
        let raw = Zeroizing::new(unhex(key?)?);
        let mut map = self.map();
        let s = map.get(session)?;
        if s.username != username {
            return None;
        }
        let mut buf = Zeroizing::new(s.ciphertext.clone());
        match aead_key(&raw).open_in_place(Nonce::assume_unique_for_key(NONCE), Aad::from(aad(session, username)), &mut *buf) {
            Ok(plain) => Some(Authtok::new(plain.to_vec())),
            Err(_) => {
                // Not the key it was sealed with: it never will be.
                map.remove(session);
                None
            }
        }
    }

    /// `device`'s host refused the password: don't offer it there again.
    pub fn refused(&self, session: &str, device: &str) {
        if let Some(s) = self.map().get_mut(session) {
            s.refused.insert(device.to_string());
        }
    }

    pub fn forget(&self, session: &str) {
        self.map().remove(session);
    }
}

fn unhex(s: &str) -> Option<Vec<u8>> {
    if s.len() != 64 {
        return None;
    }
    (0..32).map(|i| u8::from_str_radix(s.get(2 * i..2 * i + 2)?, 16).ok()).collect()
}

pub fn key_cookie_value(headers: &HeaderMap) -> Option<String> {
    super::auth::named_cookie(headers, KEY_COOKIE)
}

/// Sent only to the API (/api/connect needs it, /api/logout clears it).
pub fn set_key_cookie(key: &str, max_age: u64) -> HeaderValue {
    HeaderValue::from_str(&format!("{KEY_COOKIE}={key}; Path=/api; Secure; HttpOnly; SameSite=Strict; Max-Age={max_age}"))
        .expect("hex is a valid header value")
}

pub fn clear_key_cookie() -> HeaderValue {
    HeaderValue::from_static("veil_key=; Path=/api; Secure; HttpOnly; SameSite=Strict; Max-Age=0")
}

#[cfg(test)]
mod tests {
    use super::*;

    const HOUR: Duration = Duration::from_secs(3600);

    fn ip(s: &str) -> IpAddr {
        s.parse().unwrap()
    }

    #[test]
    fn the_key_and_the_ciphertext_together_give_the_password_back() {
        let p = Passwords::default();
        let key = p.remember("s1", "alice", ip("192.0.2.1"), &Authtok::new(b"hunter2".to_vec()), HOUR);
        assert_eq!(key.len(), 64);
        let got = p.open("s1", "alice", ip("192.0.2.1"), "d1", Some(&key)).unwrap();
        assert_eq!(got.as_bytes(), b"hunter2");
        // Again for another host: it stays until the session ends.
        assert!(p.open("s1", "alice", ip("192.0.2.1"), "d2", Some(&key)).is_some());
        assert!(!p.map().get("s1").unwrap().ciphertext.windows(7).any(|w| w == b"hunter2"));
        p.forget("s1");
        assert!(p.open("s1", "alice", ip("192.0.2.1"), "d1", Some(&key)).is_none());
    }

    #[test]
    fn a_wrong_key_or_user_gives_nothing() {
        let p = Passwords::default();
        let key = p.remember("s1", "alice", ip("192.0.2.1"), &Authtok::new(b"pw".to_vec()), HOUR);
        assert!(p.open("s1", "alice", ip("192.0.2.1"), "d1", None).is_none());
        assert!(p.open("s1", "bob", ip("192.0.2.1"), "d1", Some(&key)).is_none());
        assert!(p.open("s2", "alice", ip("192.0.2.1"), "d1", Some(&key)).is_none());
        assert!(p.open("s1", "alice", ip("192.0.2.1"), "d1", Some(&"0".repeat(64))).is_none());
        // A key that fails to open it drops it.
        assert!(p.open("s1", "alice", ip("192.0.2.1"), "d1", Some(&key)).is_none());
    }

    #[test]
    fn another_address_drops_it() {
        let p = Passwords::default();
        let key = p.remember("s1", "alice", ip("192.0.2.1"), &Authtok::new(b"pw".to_vec()), HOUR);
        assert!(!p.usable("s1", ip("192.0.2.2"), "d1"));
        assert!(p.open("s1", "alice", ip("192.0.2.1"), "d1", Some(&key)).is_none(), "gone, even back at the first address");
    }

    #[test]
    fn a_refusing_host_is_not_offered_it_again() {
        let p = Passwords::default();
        let key = p.remember("s1", "alice", ip("192.0.2.1"), &Authtok::new(b"pw".to_vec()), HOUR);
        p.refused("s1", "d1");
        assert!(!p.usable("s1", ip("192.0.2.1"), "d1"));
        assert!(p.open("s1", "alice", ip("192.0.2.1"), "d1", Some(&key)).is_none());
        assert!(p.open("s1", "alice", ip("192.0.2.1"), "d2", Some(&key)).is_some());
    }

    #[test]
    fn it_expires() {
        let p = Passwords::default();
        let key = p.remember("s1", "alice", ip("192.0.2.1"), &Authtok::new(b"pw".to_vec()), Duration::ZERO);
        assert!(p.open("s1", "alice", ip("192.0.2.1"), "d1", Some(&key)).is_none());
        assert!(p.map().is_empty());
    }
}
