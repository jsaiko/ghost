// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// ghostd's configuration (docs/reference/configuration.md): built-in
// defaults, then /etc/ghost/ghostd.toml, then /etc/ghost/ghostd.d/*.toml in
// name order, each merged key by key over the last (the tomlconf crate),
// then command-line flags (main.rs). packaging/config/ghostd.toml is the
// shipped, fully commented copy; a test holds the defaults below to it.
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{ensure, Context, Result};
use serde::{Deserialize, Serialize};

use preauth::{MaxStartups, PenaltyConfig};
use crate::session::PortRange;

pub const DEFAULT_PATH: &str = "/etc/ghost/ghostd.toml";

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct Config {
    pub lobby: Lobby,
    pub sessions: Sessions,
    pub auth: Auth,
    pub broker: Broker,
    pub log: Log,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct Lobby {
    /// The address the lobby listens on. Empty: every address, IPv4 and
    /// IPv6 alike. An IPv4 address serves IPv4 only, an IPv6 one IPv6
    /// only.
    pub address: String,
    /// UDP port of the lobby.
    pub port: u16,
    /// The host's identity certificate (PEM), which the lobby presents and
    /// spectre pins on first use (gdp-spec.md §2.3).
    pub cert: PathBuf,
    /// Private key for `cert` (PEM). root:ghost 0640: nothing but ghostd reads it.
    pub key: PathBuf,
}

impl Default for Lobby {
    fn default() -> Self {
        Lobby {
            address: String::new(),
            port: 4442,
            cert: PathBuf::from("/etc/ghost/host-cert.pem"),
            key: PathBuf::from("/etc/ghost/host-key.pem"),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct Sessions {
    /// First UDP port of the range wraith sessions bind in; the range is
    /// `port_base` through `port_base + max - 1`, one port per concurrent
    /// session.
    pub port_base: u16,
    pub max: u16,
    /// Session-type profiles (docs/reference/session-profiles.md), checked
    /// before the shipped datadir copy.
    pub dir: PathBuf,
    /// Offered when a user has no remembered choice yet.
    pub default_type: String,
}

impl Default for Sessions {
    fn default() -> Self {
        Sessions {
            port_base: 14400,
            max: 64,
            dir: PathBuf::from("/etc/ghost/sessions.d"),
            default_type: "terminal".to_string(),
        }
    }
}

/// sshd's login policy and pre-auth limits (docs/design/preauth.md).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct Auth {
    pub permit_root_login: bool,
    pub permit_empty_passwords: bool,
    /// 0 is no limit.
    #[serde(with = "duration")]
    pub login_grace_time: Duration,
    pub max_startups: MaxStartups,
    pub penalties: PenaltyConfig,
}

impl Default for Auth {
    fn default() -> Self {
        Auth {
            permit_root_login: false,
            permit_empty_passwords: false,
            login_grace_time: Duration::from_secs(120),
            max_startups: MaxStartups::default(),
            penalties: PenaltyConfig::default(),
        }
    }
}

/// Membership of a Veil (docs/design/veil.md). `ghostd join`
/// writes the first three to ghostd.d/broker.toml; empty means this host
/// isn't joined.
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct Broker {
    /// Veil's lobby, "host" or "host:port" (default port 4442).
    pub address: String,
    /// Veil's lobby certificate fingerprint, pinned.
    pub cert_sha256: String,
    /// This host's id at Veil.
    pub device_id: String,
    /// Refuse direct logins: ghostd opens no lobby listener, and logins
    /// arrive only through Veil, so its entitlements can't be bypassed.
    pub broker_only: bool,
}

impl Broker {
    pub fn joined(&self) -> bool {
        !self.address.is_empty()
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct Log {
    /// A tracing EnvFilter directive ("info", "debug",
    /// "info,ghostd::lobby=debug"), for ghostd and the ghostseat processes
    /// it spawns. A RUST_LOG environment variable overrides it (main.rs).
    pub level: String,
}

impl Default for Log {
    fn default() -> Self {
        Log { level: "info".to_string() }
    }
}

impl Config {
    /// Checks that span more than one key, and so can't be done while
    /// deserializing. Called on the final config, after flags.
    pub fn validate(&self) -> Result<()> {
        self.port_range()?;
        self.lobby_address()?;
        self.auth.max_startups.validate()?;
        ensure!(!self.sessions.default_type.is_empty(), "sessions.default_type must not be empty");
        tracing_subscriber::EnvFilter::try_new(&self.log.level)
            .with_context(|| format!("log.level {:?} is not a valid filter", self.log.level))?;
        let b = &self.broker;
        let set = [&b.address, &b.cert_sha256, &b.device_id].iter().filter(|v| !v.is_empty()).count();
        ensure!(set == 0 || set == 3, "broker.address, broker.cert_sha256 and broker.device_id go together (`ghostd join` writes all three)");
        ensure!(set == 0 || gdpnet::is_sha256_hex(&b.cert_sha256), "broker.cert_sha256 is not a lowercase SHA-256 fingerprint");
        ensure!(!b.broker_only || b.joined(), "broker.broker_only needs a Veil to log in through (`ghostd join`)");
        Ok(())
    }

    /// `[lobby] address` parsed; None for every address.
    pub fn lobby_address(&self) -> Result<Option<std::net::IpAddr>> {
        if self.lobby.address.is_empty() {
            return Ok(None);
        }
        self.lobby.address.parse().map(Some).with_context(|| format!("lobby.address {:?} is not an IP address", self.lobby.address))
    }

    pub fn port_range(&self) -> Result<PortRange> {
        PortRange::new(self.sessions.port_base, self.sessions.max, self.lobby.port)
    }
}

/// Reads `path` and its drop-ins (tomlconf::load). As sshd's StrictModes,
/// the files must belong to root (or the ghost user) and not be writable
/// by anyone else: they decide who may log in as root.
pub fn load(path: &Path, required: bool) -> Result<(Config, Vec<PathBuf>)> {
    tomlconf::load(path, required, true)
}

pub use preauth::duration;

#[cfg(test)]
mod tests {
    use std::os::unix::fs::PermissionsExt;
    use super::*;

    fn tempdir(label: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("ghostd-config-{label}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn missing_default_file_means_defaults() {
        let dir = tempdir("missing");
        let (config, files) = load(&dir.join("ghostd.toml"), false).unwrap();
        assert_eq!(config, Config::default());
        assert!(files.is_empty());
        assert!(load(&dir.join("ghostd.toml"), true).is_err());
    }

    #[test]
    fn shipped_file_is_the_defaults() {
        // packaging/config/ghostd.toml has every key commented out at its
        // default; uncommenting all of them must change nothing.
        let shipped = include_str!("../../../packaging/config/ghostd.toml");
        let uncommented = tomlconf::uncomment_defaults(shipped);
        assert_ne!(uncommented, shipped, "expected commented-out keys in packaging/config/ghostd.toml");
        let config: Config = toml::from_str(&uncommented).unwrap();
        assert_eq!(config, Config::default());
    }

    #[test]
    fn dropins_merge_key_by_key_in_name_order() {
        let dir = tempdir("dropins");
        let main = dir.join("ghostd.toml");
        std::fs::write(&main, "[lobby]\nport = 5000\n[auth.penalties]\nauthfail = \"30s\"\n").unwrap();
        std::fs::create_dir(dir.join("ghostd.d")).unwrap();
        std::fs::write(dir.join("ghostd.d/20-late.toml"), "[auth.penalties]\nmax = \"1h\"\n").unwrap();
        std::fs::write(dir.join("ghostd.d/10-early.toml"), "[auth.penalties]\nmax = 60\nmin = 5\n").unwrap();
        std::fs::write(dir.join("ghostd.d/ignored.conf"), "not toml at all").unwrap();
        // StrictModes always applies: nothing may be group-writable, which
        // a umask of 002 would make these.
        for path in [&main, &dir.join("ghostd.d")]
            .into_iter()
            .cloned()
            .chain(std::fs::read_dir(dir.join("ghostd.d")).unwrap().map(|e| e.unwrap().path()))
        {
            let mode = if path.is_dir() { 0o700 } else { 0o600 };
            std::fs::set_permissions(&path, std::fs::Permissions::from_mode(mode)).unwrap();
        }
        let (config, files) = load(&main, true).unwrap();
        assert_eq!(files.len(), 3);
        assert_eq!(config.lobby.port, 5000);
        assert_eq!(config.auth.penalties.authfail, Duration::from_secs(30));
        assert_eq!(config.auth.penalties.max, Duration::from_secs(3600));
        assert_eq!(config.auth.penalties.min, Duration::from_secs(5));
        assert_eq!(config.auth.penalties.noauth, Duration::from_secs(1));
        assert_eq!(config.sessions, Sessions::default());
    }

    #[test]
    fn unknown_keys_and_bad_values_name_the_file() {
        let dir = tempdir("errors");
        let main = dir.join("ghostd.toml");
        for bad in [
            "[auth]\npermit_root_logon = true\n",
            "[auth]\nlogin_grace_time = \"5x\"\n",
            "[lobby]\nport = \"4442\"\n",
            "[nonsense]\n",
        ] {
            std::fs::write(&main, bad).unwrap();
            let err = format!("{:#}", load(&main, true).unwrap_err());
            assert!(err.contains("ghostd.toml"), "{bad:?}: {err}");
        }
    }

    #[test]
    fn validate_catches_cross_key_mistakes() {
        let mut config = Config::default();
        config.sessions.port_base = 4440; // range 4440-4503 covers the lobby
        assert!(config.validate().is_err());
        let mut config = Config::default();
        config.auth.max_startups = MaxStartups { start: 10, rate: 30, full: 5 };
        assert!(config.validate().is_err());
        let mut config = Config::default();
        config.log.level = "info,ghostd::lobby=debug".to_string();
        assert!(config.validate().is_ok());
        config.log.level = "ghostd=loud".to_string();
        assert!(config.validate().is_err());
        let mut config = Config::default();
        config.broker.broker_only = true;
        assert!(config.validate().is_err());
        config.broker.address = "veil.example".to_string();
        assert!(config.validate().is_err());
        config.broker.device_id = "abc".to_string();
        config.broker.cert_sha256 = "AB".repeat(32);
        assert!(config.validate().is_err());
        config.broker.cert_sha256 = "ab".repeat(32);
        assert!(config.validate().is_ok());
    }

    #[test]
    fn durations_round_trip() {
        let dumped = toml::to_string(&Config::default()).unwrap();
        assert_eq!(toml::from_str::<Config>(&dumped).unwrap(), Config::default());
    }
}
