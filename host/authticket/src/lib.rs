// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Authentication tickets (docs/design/login-and-sessions.md#authentication):
// ghostauth's proof to ghostseat that a username just authenticated from
// an address. ghostd, which carries the ticket between the two, is an
// unprivileged process that never reads the key, so a compromised ghostd
// can open a session only for a user who really did log in.
//
// A ticket is base64url(nonce[16] || expiry:i64 LE || HMAC-SHA256 tag),
// the tag over (domain, service, username, rhost, nonce, expiry) with
// the key at KEY_PATH, as ghostd's own session tokens (gdp-spec.md §4.8)
// are built. The username and rhost travel beside it, in the Open
// request; the service is the PAM service the login ran on, so a ticket
// from a veild login (a Veil co-located with a host) opens no session
// here. ghostseat spends each ticket once (`spend`).
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt, PermissionsExt};
use std::path::Path;

use anyhow::{bail, Context, Result};
use base64::Engine;
use hmac::{Hmac, Mac};
use rand::RngCore;
use sha2::Sha256;

type HmacSha256 = Hmac<Sha256>;

/// 32 random bytes as hex, root:ghostauth 0640: ghostauth mints with it,
/// ghostseat (root) verifies. `make install` generates it; ghostseat
/// generates it on first start if it is missing (`ensure_key`).
pub const KEY_PATH: &str = "/etc/ghost/auth-ticket.key";
/// The account that may read the key besides root.
pub const KEY_GROUP: &str = "ghostauth";
/// How long a ticket is good for: the user has this long after PAM's
/// answer to pick a session type and send SessionOpen.
pub const TTL_SECS: i64 = 10 * 60;
/// The PAM service whose tickets ghostseat accepts.
pub const HOST_SERVICE: &str = "ghostd";
/// Where ghostseat records the tickets it has spent, one empty file per
/// nonce, root 0700; entries older than TTL_SECS are pruned at each
/// spend. Under /run/ghost, so a reboot (which voids every ticket by
/// the clock anyway) clears it.
pub const SPENT_DIR: &str = "/run/ghost/spent-tickets";

const NONCE_LEN: usize = 16;
const TAG_LEN: usize = 32;
const DOMAIN: &[u8] = b"ghost-auth-ticket\0";

pub struct Key(Vec<u8>);

/// How long a redirect token (gdp-spec.md §4.8) is good for: spectre-qt
/// execs spectre with it at once.
pub const SESSION_TOKEN_TTL_SECS: i64 = 30;

/// A redirect token (gdp-spec.md §4.8): base64url(nonce[16] || expiry:i64
/// LE || HMAC-SHA256(session_secret, uid:u32 LE || nonce || expiry)).
/// Minted by ghostseat, which holds the session secret, and verified by
/// wraith (session/token.cpp) with the same secret.
pub fn mint_session_token(uid: u32, session_secret: &[u8], expiry_unix: i64) -> String {
    let mut nonce = [0u8; 16];
    rand::rng().fill_bytes(&mut nonce);
    mint_session_token_with_nonce(uid, session_secret, &nonce, expiry_unix)
}

// Split out so the test can pin the output to the spec's vector.
fn mint_session_token_with_nonce(uid: u32, session_secret: &[u8], nonce: &[u8; 16], expiry_unix: i64) -> String {
    let expiry_bytes = expiry_unix.to_le_bytes();
    let mut mac = HmacSha256::new_from_slice(session_secret).expect("HMAC-SHA256 accepts any key length");
    mac.update(&uid.to_le_bytes());
    mac.update(nonce);
    mac.update(&expiry_bytes);
    let tag = mac.finalize().into_bytes();
    let mut packed = Vec::with_capacity(nonce.len() + expiry_bytes.len() + tag.len());
    packed.extend_from_slice(nonce);
    packed.extend_from_slice(&expiry_bytes);
    packed.extend_from_slice(&tag);
    base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(packed)
}

impl Key {
    /// Reads KEY_PATH (or `path`): 64 hex characters, whitespace around
    /// them ignored.
    pub fn load(path: &Path) -> Result<Key> {
        let text = std::fs::read_to_string(path).with_context(|| format!("reading {}", path.display()))?;
        Key::parse(&text).with_context(|| format!("parsing {}", path.display()))
    }

    fn parse(text: &str) -> Result<Key> {
        let hex = text.trim();
        if hex.len() != 64 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
            bail!("expected 64 hex characters");
        }
        let bytes = (0..32).map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16)).collect::<Result<Vec<u8>, _>>()?;
        Ok(Key(bytes))
    }

    /// For tests and for callers that already hold the bytes.
    pub fn from_bytes(bytes: Vec<u8>) -> Key {
        Key(bytes)
    }

    pub fn mint(&self, service: &str, username: &str, rhost: &str, expiry_unix: i64) -> String {
        let mut nonce = [0u8; NONCE_LEN];
        rand::rng().fill_bytes(&mut nonce);
        let tag = self.tag(service, username, rhost, &nonce, expiry_unix);
        let mut packed = Vec::with_capacity(NONCE_LEN + 8 + TAG_LEN);
        packed.extend_from_slice(&nonce);
        packed.extend_from_slice(&expiry_unix.to_le_bytes());
        packed.extend_from_slice(&tag);
        base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(packed)
    }

    /// Ok when `ticket` was minted with this key for exactly this
    /// service, username and rhost and hasn't expired at `now_unix`.
    /// Returns the nonce, for `spend`.
    pub fn verify(&self, ticket: &str, service: &str, username: &str, rhost: &str, now_unix: i64) -> Result<Nonce> {
        let packed = base64::engine::general_purpose::URL_SAFE_NO_PAD.decode(ticket).context("ticket is not base64url")?;
        if packed.len() != NONCE_LEN + 8 + TAG_LEN {
            bail!("ticket has the wrong length");
        }
        let (nonce, rest) = packed.split_at(NONCE_LEN);
        let (expiry, tag) = rest.split_at(8);
        let expiry_unix = i64::from_le_bytes(expiry.try_into().expect("8 bytes"));
        self.mac(service, username, rhost, nonce, expiry_unix).verify_slice(tag).context("ticket signature does not match")?;
        if now_unix >= expiry_unix {
            bail!("ticket expired {} s ago", now_unix - expiry_unix);
        }
        Ok(Nonce(nonce.try_into().expect("NONCE_LEN bytes")))
    }

    fn mac(&self, service: &str, username: &str, rhost: &str, nonce: &[u8], expiry_unix: i64) -> HmacSha256 {
        let mut mac = HmacSha256::new_from_slice(&self.0).expect("HMAC-SHA256 accepts any key length");
        mac.update(DOMAIN);
        mac.update(&(service.len() as u32).to_le_bytes());
        mac.update(service.as_bytes());
        mac.update(&(username.len() as u32).to_le_bytes());
        mac.update(username.as_bytes());
        mac.update(&(rhost.len() as u32).to_le_bytes());
        mac.update(rhost.as_bytes());
        mac.update(nonce);
        mac.update(&expiry_unix.to_le_bytes());
        mac
    }

    fn tag(&self, service: &str, username: &str, rhost: &str, nonce: &[u8], expiry_unix: i64) -> Vec<u8> {
        self.mac(service, username, rhost, nonce, expiry_unix).finalize().into_bytes().to_vec()
    }
}

/// A verified ticket's nonce: what `spend` records.
pub struct Nonce([u8; NONCE_LEN]);

impl Nonce {
    fn hex(&self) -> String {
        self.0.iter().map(|b| format!("{b:02x}")).collect()
    }
}

/// Records `nonce` as spent in `dir` (SPENT_DIR), creating the directory
/// root-only if needed, and fails if it was spent already: a ticket opens
/// one session. Entries older than TTL_SECS are pruned first, since a
/// ticket that old no longer verifies. Root only, after `verify`.
pub fn spend(dir: &Path, nonce: &Nonce) -> Result<()> {
    std::fs::DirBuilder::new().mode(0o700).recursive(true).create(dir).with_context(|| format!("creating {}", dir.display()))?;
    let cutoff = std::time::SystemTime::now() - std::time::Duration::from_secs(TTL_SECS as u64);
    for entry in std::fs::read_dir(dir).with_context(|| format!("listing {}", dir.display()))?.flatten() {
        if entry.metadata().and_then(|m| m.modified()).map_or(false, |t| t < cutoff) {
            let _ = std::fs::remove_file(entry.path());
        }
    }
    let path = dir.join(nonce.hex());
    match std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&path) {
        Ok(_) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => bail!("ticket already used"),
        Err(e) => Err(e).with_context(|| format!("recording the ticket in {}", dir.display())),
    }
}

/// Creates KEY_PATH (or `path`) if it is missing: 32 random bytes as hex,
/// root:KEY_GROUP 0640, written to a temporary file and renamed so two
/// instances racing here both end up reading one key. Root only.
pub fn ensure_key(path: &Path) -> Result<()> {
    if path.exists() {
        return Ok(());
    }
    let mut bytes = [0u8; 32];
    rand::rng().fill_bytes(&mut bytes);
    let hex: String = bytes.iter().map(|b| format!("{b:02x}")).collect();
    let tmp = path.with_extension(format!("key.{}.tmp", std::process::id()));
    {
        use std::io::Write;
        let mut file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o640)
            .open(&tmp)
            .with_context(|| format!("creating {}", tmp.display()))?;
        file.write_all(hex.as_bytes()).and_then(|()| file.write_all(b"\n")).and_then(|()| file.sync_all())
            .with_context(|| format!("writing {}", tmp.display()))?;
    }
    let result = (|| -> Result<()> {
        let group = nix::unistd::Group::from_name(KEY_GROUP).context("looking up the key group")?
            .with_context(|| format!("no group {KEY_GROUP}; is ghostauth installed?"))?;
        nix::unistd::chown(&tmp, None, Some(group.gid)).context("chgrp")?;
        std::fs::set_permissions(&tmp, std::fs::Permissions::from_mode(0o640)).context("chmod")?;
        // A key another instance just put down wins: never replace one.
        match std::fs::hard_link(&tmp, path) {
            Ok(()) => Ok(()),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => Ok(()),
            Err(e) => Err(e).with_context(|| format!("linking {}", path.display())),
        }
    })();
    let _ = std::fs::remove_file(&tmp);
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key() -> Key {
        Key::from_bytes((0u8..32).collect())
    }

    #[test]
    fn round_trip() {
        let t = key().mint("ghostd", "alice", "192.0.2.1", 1_700_000_100);
        assert!(key().verify(&t, "ghostd", "alice", "192.0.2.1", 1_700_000_000).is_ok());
    }

    #[test]
    fn binds_service_user_host_and_key() {
        let t = key().mint("ghostd", "alice", "192.0.2.1", 1_700_000_100);
        assert!(key().verify(&t, "veild", "alice", "192.0.2.1", 1_700_000_000).is_err());
        assert!(key().verify(&t, "ghostd", "bob", "192.0.2.1", 1_700_000_000).is_err());
        assert!(key().verify(&t, "ghostd", "alice", "192.0.2.2", 1_700_000_000).is_err());
        assert!(Key::from_bytes(vec![1; 32]).verify(&t, "ghostd", "alice", "192.0.2.1", 1_700_000_000).is_err());
    }

    #[test]
    fn expires_and_rejects_garbage() {
        let t = key().mint("ghostd", "alice", "", 1_700_000_100);
        assert!(key().verify(&t, "ghostd", "alice", "", 1_700_000_100).is_err());
        assert!(key().verify("not a ticket", "ghostd", "alice", "", 0).is_err());
        assert!(key().verify("", "ghostd", "alice", "", 0).is_err());
    }

    #[test]
    fn spends_once() {
        let dir = std::env::temp_dir().join(format!("authticket-spend-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let t = key().mint("ghostd", "alice", "", 1_700_000_100);
        let nonce = key().verify(&t, "ghostd", "alice", "", 1_700_000_000).unwrap();
        assert!(spend(&dir, &nonce).is_ok());
        assert!(spend(&dir, &nonce).unwrap_err().to_string().contains("already used"));
        let other = key().verify(&key().mint("ghostd", "alice", "", 1_700_000_100), "ghostd", "alice", "", 1_700_000_000).unwrap();
        assert!(spend(&dir, &other).is_ok());
        let _ = std::fs::remove_dir_all(&dir);
    }

    // gdp-spec.md §4.8's test vector; host/wraith/tests/token_test.cpp
    // verifies this exact string with the same uid and secret.
    #[test]
    fn session_token_matches_the_spec_vector() {
        let secret: Vec<u8> = (0u8..32).collect();
        let mut nonce = [0u8; 16];
        for (i, b) in nonce.iter_mut().enumerate() {
            *b = i as u8;
        }
        let token = mint_session_token_with_nonce(1000, &secret, &nonce, 1_700_000_000);
        assert_eq!(token, "AAECAwQFBgcICQoLDA0ODwDxU2UAAAAAHY7kB4TExZ85hTOXTtXACSjrhGKISi6aE3-_pISD_XI");
        assert_ne!(mint_session_token_with_nonce(1001, &secret, &nonce, 1_700_000_000), token);
        assert_ne!(mint_session_token_with_nonce(1000, b"other", &nonce, 1_700_000_000), token);
        assert_ne!(mint_session_token_with_nonce(1000, &secret, &nonce, 1_700_000_001), token);
        let a = mint_session_token(1000, &secret, 1_700_000_000);
        assert_ne!(a, mint_session_token(1000, &secret, 1_700_000_000));
        assert_eq!(a.len(), token.len());
    }

    #[test]
    fn key_files_are_hex() {
        assert!(Key::parse(&"ab".repeat(32)).is_ok());
        assert!(Key::parse(&format!("  {}\n", "ab".repeat(32))).is_ok());
        assert!(Key::parse(&"ab".repeat(31)).is_err());
        assert!(Key::parse(&"zz".repeat(32)).is_err());
    }
}
