// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Which certificate Veil's lobby port presents (gdp-spec.md §2.3).
//
// The port serves three ALPNs. `gdp-host/1` (joined hosts) and `wisp/1`
// (thin-client agents) always get the lobby certificate: their peers pin
// its fingerprint, from the join token and the boot arguments, and it
// lives as long as the hosts' joins do. `gdp/1` (clients' logins and
// gateway sessions) gets the same one by default -- spectre pins it on
// first use -- or, with [lobby] clients_use_web_cert, the web UI's
// certificate: issued by a CA for Veil's public name, it lets clients
// trust Veil with no first-use prompt. That one is renewed every few
// months, which is why it is never shown to the peers that pin.
use std::path::Path;
use std::sync::{Arc, RwLock};

use anyhow::{Context, Result};
use rustls::server::{ClientHello, ResolvesServerCert};
use rustls::sign::CertifiedKey;

/// A certificate and key ready to present, and the fingerprint peers pin.
#[derive(Debug)]
struct Presented {
    key: Arc<CertifiedKey>,
    fingerprint: String,
}

impl Presented {
    fn load(cert: &Path, key: &Path) -> Result<Presented> {
        let identity = gdpnet::load_host_identity(cert, key)?;
        let fingerprint = identity.fingerprint.clone();
        let (chain, key_der) = identity.into_parts();
        let provider = rustls::crypto::CryptoProvider::get_default().context("no rustls CryptoProvider installed")?;
        let key = CertifiedKey::from_der(chain, key_der, provider)
            .with_context(|| format!("{} doesn't go with {}", key.display(), cert.display()))?;
        Ok(Presented { key: Arc::new(key), fingerprint })
    }
}

#[derive(Debug)]
pub struct LobbyCerts {
    lobby: Presented,
    /// The web certificate, for `gdp/1`, when [lobby] clients_use_web_cert.
    clients: Option<RwLock<Presented>>,
}

impl LobbyCerts {
    /// `web`: the web certificate and key to present to clients, or None
    /// for the lobby certificate everywhere.
    pub fn load(lobby_cert: &Path, lobby_key: &Path, web: Option<(&Path, &Path)>) -> Result<Arc<LobbyCerts>> {
        let lobby = Presented::load(lobby_cert, lobby_key)
            .context("loading the lobby certificate (`make install-veil` generates it; see [lobby] cert/key)")?;
        let clients = match web {
            Some((cert, key)) => Some(RwLock::new(
                Presented::load(cert, key)
                    .context("loading the web certificate for clients ([lobby] clients_use_web_cert)")?,
            )),
            None => None,
        };
        Ok(Arc::new(LobbyCerts { lobby, clients }))
    }

    /// The lobby certificate's fingerprint: what join tokens carry.
    pub fn lobby_fingerprint(&self) -> &str {
        &self.lobby.fingerprint
    }

    /// The fingerprint `gdp/1` peers see now: what a gateway Redirect
    /// pins.
    pub fn client_fingerprint(&self) -> String {
        match &self.clients {
            Some(clients) => clients.read().unwrap_or_else(|p| p.into_inner()).fingerprint.clone(),
            None => self.lobby.fingerprint.clone(),
        }
    }

    /// Whether clients are shown the web certificate.
    pub fn clients_use_web_cert(&self) -> bool {
        self.clients.is_some()
    }

    /// SIGHUP: picks up a renewed web certificate. Keeps the old one on
    /// failure. Does nothing when clients get the lobby certificate.
    pub fn reload_clients(&self, cert: &Path, key: &Path) -> Result<()> {
        let Some(clients) = &self.clients else { return Ok(()) };
        let fresh = Presented::load(cert, key)?;
        *clients.write().unwrap_or_else(|p| p.into_inner()) = fresh;
        Ok(())
    }

    fn for_alpn(&self, offers_gdp: bool) -> Arc<CertifiedKey> {
        match (&self.clients, offers_gdp) {
            (Some(clients), true) => clients.read().unwrap_or_else(|p| p.into_inner()).key.clone(),
            _ => self.lobby.key.clone(),
        }
    }
}

impl ResolvesServerCert for LobbyCerts {
    fn resolve(&self, hello: ClientHello<'_>) -> Option<Arc<CertifiedKey>> {
        // A GDP client offers exactly one ALPN; only gdp/1 may be shown
        // the web certificate.
        let offers_gdp = hello.alpn().is_some_and(|mut offered| offered.any(|p| p == b"gdp/1"));
        Some(self.for_alpn(offers_gdp))
    }
}

#[cfg(test)]
impl LobbyCerts {
    /// A lobby certificate for tests that build a Veil, from the openssl CLI.
    pub fn for_tests() -> Arc<LobbyCerts> {
        use std::sync::atomic::{AtomicU32, Ordering};
        static NEXT: AtomicU32 = AtomicU32::new(0);
        let dir = std::env::temp_dir()
            .join(format!("veil-test-certs-{}-{}", std::process::id(), NEXT.fetch_add(1, Ordering::Relaxed)));
        std::fs::create_dir_all(&dir).unwrap();
        let lobby = tests::presented(&dir, "lobby");
        std::fs::remove_dir_all(&dir).unwrap();
        Arc::new(LobbyCerts { lobby, clients: None })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    // A throwaway self-signed pair from the openssl CLI, loaded the way
    // veild loads its own.
    pub(super) fn presented(dir: &Path, name: &str) -> Presented {
        let _ = rustls::crypto::ring::default_provider().install_default();
        let (cert, key) = (dir.join(format!("{name}.pem")), dir.join(format!("{name}.key")));
        let status = Command::new("openssl")
            .args(["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes", "-days", "1"])
            .args(["-subj", &format!("/CN={name}"), "-keyout"])
            .arg(&key)
            .arg("-out")
            .arg(&cert)
            .output()
            .expect("running openssl")
            .status;
        assert!(status.success());
        Presented::load(&cert, &key).unwrap()
    }

    #[test]
    fn only_gdp_clients_see_the_web_certificate() {
        let dir = std::env::temp_dir().join(format!("veil-lobby-tls-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();

        let both = LobbyCerts { lobby: presented(&dir, "lobby"), clients: Some(RwLock::new(presented(&dir, "web"))) };
        assert_ne!(both.client_fingerprint(), both.lobby_fingerprint());
        assert!(Arc::ptr_eq(&both.for_alpn(true), &both.clients.as_ref().unwrap().read().unwrap().key));
        assert!(Arc::ptr_eq(&both.for_alpn(false), &both.lobby.key));

        // A reload swaps what clients see, never what hosts see.
        let before = both.client_fingerprint();
        presented(&dir, "renewed");
        both.reload_clients(&dir.join("renewed.pem"), &dir.join("renewed.key")).unwrap();
        assert_ne!(both.client_fingerprint(), before);
        assert!(Arc::ptr_eq(&both.for_alpn(false), &both.lobby.key));
        // A broken renewal keeps the old certificate.
        let current = both.client_fingerprint();
        assert!(both.reload_clients(&dir.join("missing.pem"), &dir.join("missing.key")).is_err());
        assert_eq!(both.client_fingerprint(), current);

        let lobby_only = LobbyCerts { lobby: presented(&dir, "lobby2"), clients: None };
        assert!(Arc::ptr_eq(&lobby_only.for_alpn(true), &lobby_only.lobby.key));
        assert_eq!(lobby_only.client_fingerprint(), lobby_only.lobby_fingerprint());
        assert!(lobby_only.reload_clients(&dir.join("missing.pem"), &dir.join("missing.key")).is_ok());

        std::fs::remove_dir_all(&dir).unwrap();
    }
}
