// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Limits on connections that haven't authenticated yet, ported from
// OpenSSH's sshd with its option syntax and defaults
// (docs/design/preauth.md): MaxStartups' random early drop of
// unauthenticated connections, and PerSourcePenalties' refusal of an
// address after failed or abandoned logins. Both act on a quinn::Incoming
// before the QUIC handshake, so a refused client costs no crypto and no
// PAM thread. Shared by ghostd and veild.
use std::collections::HashMap;
use std::net::{IpAddr, Ipv6Addr};
use std::str::FromStr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::{bail, ensure, Context, Result};
use serde::{Deserialize, Serialize};
use tracing::info;


/// sshd_config(5)'s time format: a number with an optional s/m/h/d/w
/// suffix (seconds when bare), e.g. "120", "90s", "10m".
pub fn parse_duration(s: &str) -> Result<Duration> {
    let s = s.trim();
    let (digits, unit) = match s.char_indices().last() {
        Some((i, c)) if c.is_ascii_alphabetic() => (&s[..i], c.to_ascii_lowercase()),
        _ => (s, 's'),
    };
    let n: u64 = digits.parse().with_context(|| format!("invalid time {s:?}"))?;
    let scale = match unit {
        's' => 1,
        'm' => 60,
        'h' => 3600,
        'd' => 86400,
        'w' => 604800,
        _ => bail!("invalid time unit in {s:?} (use s, m, h, d or w)"),
    };
    Ok(Duration::from_secs(n.checked_mul(scale).with_context(|| format!("time {s:?} is too large"))?))
}

/// sshd's MaxStartups: below `start` unauthenticated connections every
/// new one is accepted; from `start` on, one is refused with probability
/// `rate`%, rising linearly to 100% at `full`. `[auth.max_startups]` in
/// ghostd.toml and veild.toml; `--max-startups` takes sshd's
/// "start:rate:full" or "N".
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct MaxStartups {
    pub start: usize,
    pub rate: u32,
    pub full: usize,
}

impl Default for MaxStartups {
    // sshd's own default, 10:30:100 (servconf.c).
    fn default() -> Self {
        MaxStartups { start: 10, rate: 30, full: 100 }
    }
}

impl FromStr for MaxStartups {
    type Err = anyhow::Error;

    fn from_str(s: &str) -> Result<Self> {
        let parts: Vec<&str> = s.split(':').collect();
        let cfg = match parts[..] {
            [n] => {
                let n = n.parse().with_context(|| format!("invalid MaxStartups {s:?}"))?;
                MaxStartups { start: n, rate: 100, full: n }
            }
            [start, rate, full] => MaxStartups {
                start: start.parse().with_context(|| format!("invalid MaxStartups start in {s:?}"))?,
                rate: rate.parse().with_context(|| format!("invalid MaxStartups rate in {s:?}"))?,
                full: full.parse().with_context(|| format!("invalid MaxStartups full in {s:?}"))?,
            },
            _ => bail!("MaxStartups must be N or start:rate:full, got {s:?}"),
        };
        cfg.validate()?;
        Ok(cfg)
    }
}

impl MaxStartups {
    pub fn validate(&self) -> Result<()> {
        ensure!(self.start >= 1, "max_startups.start must be at least 1");
        ensure!((1..=100).contains(&self.rate), "max_startups.rate must be 1-100");
        ensure!(self.full >= self.start, "max_startups.full must not be below start");
        Ok(())
    }

    /// sshd's drop_connection(): `unauthenticated` is how many are open
    /// now, `roll` a uniform 0..100.
    fn drops(&self, unauthenticated: usize, roll: u32) -> bool {
        if unauthenticated >= self.full {
            return true;
        }
        if unauthenticated < self.start {
            return false;
        }
        let span = (self.full - self.start) as u64;
        let p = u64::from(100 - self.rate) * (unauthenticated - self.start) as u64 / span + u64::from(self.rate);
        u64::from(roll) < p
    }
}

pub struct StartupLimiter {
    cfg: MaxStartups,
    unauthenticated: Arc<AtomicUsize>,
}

/// One unauthenticated connection's place in the MaxStartups count, given
/// back on drop -- which lobby.rs does as soon as PAM has an answer, or
/// the connection ends first.
pub struct StartupSlot(Arc<AtomicUsize>);

impl Drop for StartupSlot {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::Relaxed);
    }
}

impl StartupLimiter {
    pub fn new(cfg: MaxStartups) -> StartupLimiter {
        StartupLimiter { cfg, unauthenticated: Arc::new(AtomicUsize::new(0)) }
    }

    /// True once dropping has started. The accept loop then makes a client
    /// prove its address with a QUIC Retry before it takes a slot, so a
    /// flood of spoofed Initials can't fill the count on its own -- QUIC's
    /// counterpart of the SYN cookies that shield sshd's.
    pub fn under_pressure(&self) -> bool {
        self.unauthenticated.load(Ordering::Relaxed) >= self.cfg.start
    }

    pub fn try_acquire(&self) -> Option<StartupSlot> {
        let open = self.unauthenticated.load(Ordering::Relaxed);
        if self.cfg.drops(open, rand::random_range(0..100)) {
            return None;
        }
        self.unauthenticated.fetch_add(1, Ordering::Relaxed);
        Some(StartupSlot(self.unauthenticated.clone()))
    }
}

/// What a source address did to earn a penalty, named after sshd's
/// PerSourcePenalties keywords.
#[derive(Clone, Copy, Debug)]
pub enum Offense {
    /// Authentication was attempted and failed, or succeeded for an
    /// account policy refuses (root without auth.permit_root_login).
    AuthFail,
    /// The client broke off, or broke the protocol, before answering a
    /// single PAM prompt.
    NoAuth,
    /// auth.login_grace_time ran out before authentication finished.
    GraceExceeded,
}

/// sshd's PerSourcePenalties. Each offense adds its time to the source's
/// penalty; once the accumulated penalty exceeds `min` the source is
/// refused outright until it runs out, and it never exceeds `max`.
/// `[auth.penalties]` in ghostd.toml and veild.toml.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields, default)]
pub struct PenaltyConfig {
    pub enabled: bool,
    #[serde(with = "duration")]
    pub authfail: Duration,
    #[serde(with = "duration")]
    pub noauth: Duration,
    #[serde(with = "duration")]
    pub grace_exceeded: Duration,
    #[serde(with = "duration")]
    pub max: Duration,
    #[serde(with = "duration")]
    pub min: Duration,
}

impl Default for PenaltyConfig {
    // sshd's own defaults (servconf.c).
    fn default() -> Self {
        PenaltyConfig {
            enabled: true,
            authfail: Duration::from_secs(5),
            noauth: Duration::from_secs(1),
            grace_exceeded: Duration::from_secs(10),
            max: Duration::from_secs(600),
            min: Duration::from_secs(15),
        }
    }
}

impl PenaltyConfig {
    /// `--per-source-penalties`, applied over whatever ghostd.toml set:
    /// "no", "yes", or sshd_config(5)'s `keyword:time` items (authfail,
    /// noauth, grace-exceeded, max, min), separated by commas or spaces.
    /// Any items also turn penalties on.
    pub fn apply_overrides(&mut self, s: &str) -> Result<()> {
        match s.trim() {
            "no" => {
                self.enabled = false;
                return Ok(());
            }
            "yes" => {
                self.enabled = true;
                return Ok(());
            }
            _ => {}
        }
        let mut cfg = *self;
        cfg.enabled = true;
        for item in s.split(|c: char| c == ',' || c.is_whitespace()).filter(|i| !i.is_empty()) {
            let Some((key, value)) = item.split_once(':') else {
                bail!("PerSourcePenalties entry {item:?} is not keyword:time");
            };
            let value = parse_duration(value)?;
            match key {
                "authfail" => cfg.authfail = value,
                "noauth" => cfg.noauth = value,
                "grace-exceeded" => cfg.grace_exceeded = value,
                "max" => cfg.max = value,
                "min" => cfg.min = value,
                _ => bail!("unknown PerSourcePenalties keyword {key:?}"),
            }
        }
        *self = cfg;
        Ok(())
    }
}

// sshd's PerSourcePenalties max-sources4/max-sources6 (one limit here);
// past it, new sources simply aren't tracked -- its default "permissive"
// overflow mode.
const MAX_SOURCES: usize = 65536;

struct Penalty {
    expiry: Instant,
    active: bool,
}

pub struct Penalties {
    cfg: PenaltyConfig,
    table: Mutex<HashMap<IpAddr, Penalty>>,
}

/// Penalties are kept per IPv4 address and per IPv6 /64 -- one host's
/// worth of addresses under SLAAC. sshd defaults to per-address for IPv6
/// too (PerSourceNetBlockSize 32:128), which a single client can dodge by
/// hopping addresses within its own /64.
fn source_key(ip: IpAddr) -> IpAddr {
    match ip.to_canonical() {
        IpAddr::V6(v6) => IpAddr::V6(Ipv6Addr::from(v6.to_bits() & !((1u128 << 64) - 1))),
        v4 => v4,
    }
}

impl Penalties {
    pub fn new(cfg: PenaltyConfig) -> Penalties {
        Penalties { cfg, table: Mutex::new(HashMap::new()) }
    }

    pub fn refuses(&self, ip: IpAddr) -> bool {
        self.refuses_at(ip, Instant::now())
    }

    pub fn penalise(&self, ip: IpAddr, offense: Offense) {
        self.penalise_at(ip, offense, Instant::now());
    }

    fn refuses_at(&self, ip: IpAddr, now: Instant) -> bool {
        if !self.cfg.enabled {
            return false;
        }
        let table = self.table.lock().unwrap();
        table.get(&source_key(ip)).is_some_and(|p| p.active && p.expiry > now)
    }

    // srclimit.c's srclimit_penalise(), minus its expiry-ordered trees:
    // the table is only swept when it fills, which is rare enough that a
    // linear pass is fine.
    fn penalise_at(&self, ip: IpAddr, offense: Offense, now: Instant) {
        if !self.cfg.enabled {
            return;
        }
        let amount = match offense {
            Offense::AuthFail => self.cfg.authfail,
            Offense::NoAuth => self.cfg.noauth,
            Offense::GraceExceeded => self.cfg.grace_exceeded,
        };
        if amount.is_zero() {
            return;
        }
        let key = source_key(ip);
        let cap = now + self.cfg.max;
        let mut table = self.table.lock().unwrap();
        if table.len() >= MAX_SOURCES && !table.contains_key(&key) {
            table.retain(|_, p| p.expiry > now);
            if table.len() >= MAX_SOURCES {
                return;
            }
        }
        let penalty = table.entry(key).or_insert(Penalty { expiry: now, active: false });
        if penalty.expiry <= now {
            *penalty = Penalty { expiry: now, active: false };
        }
        penalty.expiry = (penalty.expiry + amount).min(cap);
        let remaining = penalty.expiry - now;
        if !penalty.active && remaining > self.cfg.min {
            penalty.active = true;
            info!(source = %key, secs = remaining.as_secs(), ?offense,
                "preauth: refusing this source's connections until its penalty runs out");
        }
    }
}

/// A Duration as ghostd.toml and veild.toml write it: a string in sshd's
/// time format ("120s", "10m", see parse_duration), or a bare integer of
/// seconds.
pub mod duration {
    use std::time::Duration;

    use serde::de::Error;
    use serde::{Deserialize, Deserializer, Serializer};

    pub fn serialize<S: Serializer>(d: &Duration, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&format(*d))
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(d: D) -> Result<Duration, D::Error> {
        #[derive(Deserialize)]
        #[serde(untagged)]
        enum Raw {
            Secs(u64),
            Text(String),
        }
        match Raw::deserialize(d)? {
            Raw::Secs(secs) => Ok(Duration::from_secs(secs)),
            Raw::Text(text) => super::parse_duration(&text).map_err(D::Error::custom),
        }
    }

    /// The largest whole unit: 600 s is "10m", 90 s stays "90s".
    pub fn format(d: Duration) -> String {
        let secs = d.as_secs();
        for (unit, scale) in [("w", 604800), ("d", 86400), ("h", 3600), ("m", 60)] {
            if secs != 0 && secs % scale == 0 {
                return format!("{}{unit}", secs / scale);
            }
        }
        format!("{secs}s")
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn durations_use_sshd_units() {
        assert_eq!(parse_duration("120").unwrap(), Duration::from_secs(120));
        assert_eq!(parse_duration("90s").unwrap(), Duration::from_secs(90));
        assert_eq!(parse_duration("10m").unwrap(), Duration::from_secs(600));
        assert_eq!(parse_duration("0").unwrap(), Duration::ZERO);
        assert!(parse_duration("5x").is_err());
        assert!(parse_duration("m").is_err());
    }

    #[test]
    fn durations_format_in_their_largest_unit() {
        assert_eq!(duration::format(Duration::from_secs(600)), "10m");
        assert_eq!(duration::format(Duration::from_secs(90)), "90s");
        assert_eq!(duration::format(Duration::from_secs(7200)), "2h");
        assert_eq!(duration::format(Duration::ZERO), "0s");
    }

    #[test]
    fn max_startups_parses_both_forms() {
        assert_eq!("10:30:100".parse::<MaxStartups>().unwrap(), MaxStartups::default());
        assert_eq!("20".parse::<MaxStartups>().unwrap(), MaxStartups { start: 20, rate: 100, full: 20 });
        assert!("0".parse::<MaxStartups>().is_err());
        assert!("10:0:100".parse::<MaxStartups>().is_err());
        assert!("10:30:5".parse::<MaxStartups>().is_err());
        assert!("10:30".parse::<MaxStartups>().is_err());
    }

    #[test]
    fn max_startups_drops_like_sshd() {
        let cfg: MaxStartups = "10:30:100".parse().unwrap();
        // Below start: never. At full: always.
        assert!(!cfg.drops(9, 0));
        assert!(cfg.drops(100, 99));
        // At start: exactly rate% (rolls 0..29 drop, 30.. don't).
        assert!(cfg.drops(10, 29));
        assert!(!cfg.drops(10, 30));
        // Halfway (55): 30 + 70 * 45/90 = 65%.
        assert!(cfg.drops(55, 64));
        assert!(!cfg.drops(55, 65));
        // Single-value form: a hard cap.
        let hard: MaxStartups = "5".parse().unwrap();
        assert!(!hard.drops(4, 0));
        assert!(hard.drops(5, 99));
    }

    #[test]
    fn startup_slots_are_returned_on_drop() {
        let limiter = StartupLimiter::new("2".parse().unwrap());
        let a = limiter.try_acquire().unwrap();
        let _b = limiter.try_acquire().unwrap();
        assert!(limiter.under_pressure());
        assert!(limiter.try_acquire().is_none());
        drop(a);
        assert!(limiter.try_acquire().is_some());
    }

    #[test]
    fn penalty_overrides_apply_over_the_current_values() {
        let overridden = |s: &str| {
            let mut cfg = PenaltyConfig { noauth: Duration::from_secs(7), ..PenaltyConfig::default() };
            cfg.apply_overrides(s).map(|()| cfg)
        };
        let cfg = overridden("authfail:30s max:1h").unwrap();
        assert_eq!(overridden("authfail:30s,max:1h").unwrap(), cfg);
        assert_eq!(cfg.authfail, Duration::from_secs(30));
        assert_eq!(cfg.max, Duration::from_secs(3600));
        assert_eq!(cfg.noauth, Duration::from_secs(7));
        assert!(!overridden("no").unwrap().enabled);
        assert!(overridden("crash:90").is_err());
        assert!(overridden("authfail").is_err());
        // Items turn a disabled config back on.
        let mut off = PenaltyConfig { enabled: false, ..PenaltyConfig::default() };
        off.apply_overrides("min:1m").unwrap();
        assert!(off.enabled);
    }

    #[test]
    fn penalties_activate_past_min_and_expire() {
        let p = Penalties::new(PenaltyConfig::default());
        let ip: IpAddr = "192.0.2.7".parse().unwrap();
        let t0 = Instant::now();
        // Three failures = 15 s, not *more* than min: still allowed.
        for _ in 0..3 {
            p.penalise_at(ip, Offense::AuthFail, t0);
        }
        assert!(!p.refuses_at(ip, t0));
        p.penalise_at(ip, Offense::AuthFail, t0);
        assert!(p.refuses_at(ip, t0));
        assert!(p.refuses_at(ip, t0 + Duration::from_secs(19)));
        assert!(!p.refuses_at(ip, t0 + Duration::from_secs(20)));
        // Other sources are unaffected.
        assert!(!p.refuses_at("192.0.2.8".parse().unwrap(), t0));
    }

    #[test]
    fn penalties_cap_at_max() {
        let p = Penalties::new(PenaltyConfig::default());
        let ip: IpAddr = "192.0.2.7".parse().unwrap();
        let t0 = Instant::now();
        for _ in 0..1000 {
            p.penalise_at(ip, Offense::AuthFail, t0);
        }
        assert!(p.refuses_at(ip, t0 + Duration::from_secs(599)));
        assert!(!p.refuses_at(ip, t0 + Duration::from_secs(600)));
    }

    #[test]
    fn ipv6_penalties_cover_the_whole_slash_64() {
        let p = Penalties::new(PenaltyConfig::default());
        let t0 = Instant::now();
        let first: IpAddr = "2001:db8:1:2::1".parse().unwrap();
        for _ in 0..4 {
            p.penalise_at(first, Offense::AuthFail, t0);
        }
        assert!(p.refuses_at("2001:db8:1:2:ffff::9".parse().unwrap(), t0));
        assert!(!p.refuses_at("2001:db8:1:3::1".parse().unwrap(), t0));
        // A v4 client seen through the dual-stack socket is keyed as v4.
        let v4: IpAddr = "192.0.2.9".parse().unwrap();
        for _ in 0..4 {
            p.penalise_at("::ffff:192.0.2.9".parse().unwrap(), Offense::AuthFail, t0);
        }
        assert!(p.refuses_at(v4, t0));
    }

    #[test]
    fn disabled_penalties_never_refuse() {
        let p = Penalties::new(PenaltyConfig { enabled: false, ..PenaltyConfig::default() });
        let ip: IpAddr = "192.0.2.7".parse().unwrap();
        let t0 = Instant::now();
        for _ in 0..10 {
            p.penalise_at(ip, Offense::AuthFail, t0);
        }
        assert!(!p.refuses_at(ip, t0));
    }
}
