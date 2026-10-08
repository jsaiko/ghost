// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Veil's state: one SQLite file,
// /var/lib/veil/veil.db by default. Every query here is small and
// indexed, so callers run them inline on their async task under one
// mutex rather than through a blocking pool.
use std::collections::HashSet;
use std::os::unix::fs::PermissionsExt;
use std::path::Path;
use std::sync::Mutex;

use anyhow::{bail, Context, Result};
use ipc::unix_now as now;
use rusqlite::{params, Connection, OptionalExtension};

use crate::config::DeviceMode;

const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS devices (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    hostname TEXT NOT NULL,
    client_address TEXT NOT NULL,
    cert_sha256 TEXT NOT NULL UNIQUE,
    enabled INTEGER NOT NULL DEFAULT 1,
    mode TEXT NOT NULL,
    joined_at INTEGER NOT NULL,
    last_seen INTEGER
);
CREATE TABLE IF NOT EXISTS entitlements (
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    kind TEXT NOT NULL CHECK (kind IN ('user', 'group')),
    name TEXT NOT NULL,
    PRIMARY KEY (device_id, kind, name)
);
CREATE TABLE IF NOT EXISTS join_tokens (
    id TEXT PRIMARY KEY,
    secret_sha256 TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    used_at INTEGER,
    device_id TEXT,
    created_by TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS placements (
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    username TEXT NOT NULL,
    uid INTEGER NOT NULL,
    session_type TEXT NOT NULL,
    started_at INTEGER NOT NULL,
    viewer_attached INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (device_id, username)
);
CREATE TABLE IF NOT EXISTS last_types (
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    username TEXT NOT NULL,
    session_type TEXT NOT NULL,
    PRIMARY KEY (device_id, username)
);
CREATE TABLE IF NOT EXISTS audit (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    at INTEGER NOT NULL,
    actor TEXT NOT NULL,
    action TEXT NOT NULL,
    device_id TEXT,
    detail TEXT NOT NULL,
    client_address TEXT
);
CREATE INDEX IF NOT EXISTS audit_at ON audit(at);
CREATE TABLE IF NOT EXISTS thin_clients (
    mac TEXT PRIMARY KEY,
    name TEXT NOT NULL DEFAULT '',
    first_seen INTEGER NOT NULL,
    last_seen INTEGER NOT NULL,
    last_report TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS portal_sessions (
    id_sha256 TEXT PRIMARY KEY,
    username TEXT NOT NULL,
    csrf TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    last_seen INTEGER NOT NULL
);
";

#[derive(Clone, Debug)]
pub struct Device {
    pub id: String,
    pub name: String,
    pub hostname: String,
    pub client_address: String,
    pub cert_sha256: String,
    pub enabled: bool,
    pub mode: DeviceMode,
    pub joined_at: i64,
    pub last_seen: Option<i64>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Placement {
    pub device_id: String,
    pub username: String,
    pub uid: u32,
    pub session_type: String,
    pub started_at: i64,
    pub viewer_attached: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Grantee {
    User(String),
    Group(String),
}

impl Grantee {
    fn kind(&self) -> &'static str {
        match self {
            Grantee::User(_) => "user",
            Grantee::Group(_) => "group",
        }
    }

    pub fn name(&self) -> &str {
        match self {
            Grantee::User(n) | Grantee::Group(n) => n,
        }
    }
}

#[derive(Clone, Debug)]
pub struct AuditEntry {
    pub at: i64,
    pub actor: String,
    pub action: String,
    pub device_id: Option<String>,
    pub detail: String,
    pub client_address: Option<String>,
}

/// A Wisp thin client, as last reported (thin_clients.rs). The row stays
/// when it goes offline, so the list keeps it and its admin-given name.
#[derive(Clone, Debug)]
pub struct ThinClientRow {
    pub mac: String,
    /// Empty until an administrator names it.
    pub name: String,
    pub first_seen: i64,
    pub last_seen: i64,
    /// thin_clients::Report as JSON.
    pub last_report: String,
}

#[derive(Clone, Debug)]
pub struct WebSession {
    pub username: String,
    pub csrf: String,
    pub last_seen: i64,
}

/// Why a join token didn't work. All three read the same to the host.
#[derive(Debug, PartialEq, Eq)]
pub enum TokenError {
    Unknown,
    Used,
    Expired,
}

pub struct Db(Mutex<Connection>);

impl Db {
    /// Opens (creating if needed) the database at `path`. The file holds
    /// every host's pin and the audit log, so it is the daemon's alone.
    pub fn open(path: &Path) -> Result<Db> {
        if let Some(dir) = path.parent() {
            if !dir.as_os_str().is_empty() && !dir.exists() {
                std::fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
            }
        }
        let conn = Connection::open(path).with_context(|| format!("opening {}", path.display()))?;
        let _ = std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o600));
        Self::init(conn)
    }

    #[cfg(test)]
    pub fn in_memory() -> Db {
        Self::init(Connection::open_in_memory().unwrap()).unwrap()
    }

    fn init(conn: Connection) -> Result<Db> {
        conn.pragma_update(None, "foreign_keys", true)?;
        conn.pragma_update(None, "journal_mode", "WAL").ok();
        conn.pragma_update(None, "busy_timeout", 5000)?;
        conn.execute_batch(SCHEMA).context("creating the schema")?;
        Ok(Db(Mutex::new(conn)))
    }

    fn conn(&self) -> std::sync::MutexGuard<'_, Connection> {
        self.0.lock().unwrap_or_else(|p| p.into_inner())
    }

    // --- join tokens ---

    pub fn add_join_token(&self, id: &str, secret_sha256: &str, ttl_secs: i64, created_by: &str) -> Result<()> {
        let t = now();
        let conn = self.conn();
        // Spent and expired tokens are kept a day for the record, then go.
        conn.execute("DELETE FROM join_tokens WHERE expires_at < ?1", params![t - 86400])?;
        conn.execute(
            "INSERT INTO join_tokens (id, secret_sha256, created_at, expires_at, created_by) VALUES (?1, ?2, ?3, ?4, ?5)",
            params![id, secret_sha256, t, t + ttl_secs, created_by],
        )?;
        Ok(())
    }

    /// Marks the token used if `secret_sha256` matches and it is neither
    /// used nor expired. Single use is enforced here, in one statement.
    pub fn consume_join_token(&self, id: &str, secret_sha256: &str) -> Result<std::result::Result<(), TokenError>> {
        let conn = self.conn();
        let row: Option<(String, i64, Option<i64>)> = conn
            .query_row(
                "SELECT secret_sha256, expires_at, used_at FROM join_tokens WHERE id = ?1",
                params![id],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
            )
            .optional()?;
        let Some((stored, expires_at, used_at)) = row else { return Ok(Err(TokenError::Unknown)) };
        if !constant_time_eq(stored.as_bytes(), secret_sha256.as_bytes()) {
            return Ok(Err(TokenError::Unknown));
        }
        if used_at.is_some() {
            return Ok(Err(TokenError::Used));
        }
        let t = now();
        if expires_at <= t {
            return Ok(Err(TokenError::Expired));
        }
        let changed =
            conn.execute("UPDATE join_tokens SET used_at = ?2 WHERE id = ?1 AND used_at IS NULL", params![id, t])?;
        Ok(if changed == 1 { Ok(()) } else { Err(TokenError::Used) })
    }

    pub fn set_token_device(&self, token_id: &str, device_id: &str) -> Result<()> {
        self.conn().execute("UPDATE join_tokens SET device_id = ?2 WHERE id = ?1", params![token_id, device_id])?;
        Ok(())
    }

    // --- devices ---

    /// Records a joined host, or updates the one already holding this
    /// certificate (a host joining again with the same key keeps its id,
    /// name, mode and entitlements). Returns the device id.
    pub fn join_device(
        &self,
        new_id: &str,
        hostname: &str,
        client_address: &str,
        cert_sha256: &str,
        mode: DeviceMode,
    ) -> Result<String> {
        let conn = self.conn();
        let existing: Option<String> = conn
            .query_row("SELECT id FROM devices WHERE cert_sha256 = ?1", params![cert_sha256], |r| r.get(0))
            .optional()?;
        if let Some(id) = existing {
            conn.execute(
                "UPDATE devices SET hostname = ?2, client_address = ?3 WHERE id = ?1",
                params![id, hostname, client_address],
            )?;
            return Ok(id);
        }
        conn.execute(
            "INSERT INTO devices (id, name, hostname, client_address, cert_sha256, enabled, mode, joined_at)
             VALUES (?1, ?2, ?2, ?3, ?4, 1, ?5, ?6)",
            params![new_id, hostname, client_address, cert_sha256, mode.as_str(), now()],
        )?;
        Ok(new_id.to_string())
    }

    pub fn device(&self, id: &str) -> Result<Option<Device>> {
        self.conn()
            .query_row(
                "SELECT id, name, hostname, client_address, cert_sha256, enabled, mode, joined_at, last_seen
                 FROM devices WHERE id = ?1",
                params![id],
                device_row,
            )
            .optional()
            .map_err(Into::into)
    }

    pub fn devices(&self) -> Result<Vec<Device>> {
        let conn = self.conn();
        let mut stmt = conn.prepare(
            "SELECT id, name, hostname, client_address, cert_sha256, enabled, mode, joined_at, last_seen
             FROM devices ORDER BY name COLLATE NOCASE, id",
        )?;
        let rows = stmt.query_map([], device_row)?.collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    pub fn remove_device(&self, id: &str) -> Result<bool> {
        Ok(self.conn().execute("DELETE FROM devices WHERE id = ?1", params![id])? == 1)
    }

    pub fn update_device(&self, id: &str, name: &str, client_address: &str, mode: DeviceMode, enabled: bool) -> Result<()> {
        let changed = self.conn().execute(
            "UPDATE devices SET name = ?2, client_address = ?3, mode = ?4, enabled = ?5 WHERE id = ?1",
            params![id, name, client_address, mode.as_str(), enabled],
        )?;
        if changed != 1 {
            bail!("no device {id}");
        }
        Ok(())
    }

    pub fn touch_device(&self, id: &str) -> Result<()> {
        self.conn().execute("UPDATE devices SET last_seen = ?2 WHERE id = ?1", params![id, now()])?;
        Ok(())
    }

    // --- entitlements ---

    pub fn entitlements(&self, device_id: &str) -> Result<Vec<Grantee>> {
        let conn = self.conn();
        let mut stmt =
            conn.prepare("SELECT kind, name FROM entitlements WHERE device_id = ?1 ORDER BY kind DESC, name")?;
        let rows = stmt
            .query_map(params![device_id], |r| {
                let kind: String = r.get(0)?;
                let name: String = r.get(1)?;
                Ok(if kind == "group" { Grantee::Group(name) } else { Grantee::User(name) })
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    pub fn grant(&self, device_id: &str, who: &Grantee) -> Result<bool> {
        Ok(self.conn().execute(
            "INSERT OR IGNORE INTO entitlements (device_id, kind, name) VALUES (?1, ?2, ?3)",
            params![device_id, who.kind(), who.name()],
        )? == 1)
    }

    pub fn revoke(&self, device_id: &str, who: &Grantee) -> Result<bool> {
        Ok(self.conn().execute(
            "DELETE FROM entitlements WHERE device_id = ?1 AND kind = ?2 AND name = ?3",
            params![device_id, who.kind(), who.name()],
        )? == 1)
    }

    /// The enabled devices `username`, or any of `groups`, is entitled to.
    pub fn entitled_devices(&self, username: &str, groups: &[String]) -> Result<Vec<Device>> {
        let conn = self.conn();
        let mut stmt = conn.prepare(
            "SELECT d.id, d.name, d.hostname, d.client_address, d.cert_sha256, d.enabled, d.mode, d.joined_at, d.last_seen,
                    e.kind, e.name
             FROM devices d JOIN entitlements e ON e.device_id = d.id
             WHERE d.enabled = 1",
        )?;
        let groups: HashSet<&str> = groups.iter().map(String::as_str).collect();
        let mut seen = HashSet::new();
        let mut out = Vec::new();
        let mut rows = stmt.query([])?;
        while let Some(row) = rows.next()? {
            let kind: String = row.get(9)?;
            let name: String = row.get(10)?;
            let matches = match kind.as_str() {
                "user" => name == username,
                _ => groups.contains(name.as_str()),
            };
            if matches {
                let device = device_row(row)?;
                if seen.insert(device.id.clone()) {
                    out.push(device);
                }
            }
        }
        Ok(out)
    }

    // --- placements ---

    pub fn replace_placements(&self, device_id: &str, sessions: &[Placement]) -> Result<()> {
        let mut conn = self.conn();
        let tx = conn.transaction()?;
        tx.execute("DELETE FROM placements WHERE device_id = ?1", params![device_id])?;
        for p in sessions {
            insert_placement(&tx, p)?;
        }
        tx.commit()?;
        Ok(())
    }

    /// Forgets every host's sessions. veild starts with no host connected,
    /// and each one's first Snapshot brings back what it is running.
    pub fn clear_placements(&self) -> Result<()> {
        self.conn().execute("DELETE FROM placements", [])?;
        Ok(())
    }

    pub fn upsert_placement(&self, p: &Placement) -> Result<()> {
        insert_placement(&self.conn(), p)
    }

    pub fn remove_placement(&self, device_id: &str, username: &str) -> Result<()> {
        self.conn()
            .execute("DELETE FROM placements WHERE device_id = ?1 AND username = ?2", params![device_id, username])?;
        Ok(())
    }

    pub fn set_viewer(&self, device_id: &str, username: &str, attached: bool) -> Result<()> {
        self.conn().execute(
            "UPDATE placements SET viewer_attached = ?3 WHERE device_id = ?1 AND username = ?2",
            params![device_id, username, attached],
        )?;
        Ok(())
    }

    pub fn placements(&self) -> Result<Vec<Placement>> {
        let conn = self.conn();
        let mut stmt = conn.prepare(
            "SELECT device_id, username, uid, session_type, started_at, viewer_attached
             FROM placements ORDER BY device_id, username",
        )?;
        let rows = stmt.query_map([], placement_row)?.collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    pub fn placements_for_user(&self, username: &str) -> Result<Vec<Placement>> {
        let conn = self.conn();
        let mut stmt = conn.prepare(
            "SELECT device_id, username, uid, session_type, started_at, viewer_attached
             FROM placements WHERE username = ?1",
        )?;
        let rows = stmt.query_map(params![username], placement_row)?.collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    /// The desktop `username` last ran on `device_id`, which outlives the
    /// placement so the next DeviceList can offer it again.
    pub fn last_type(&self, device_id: &str, username: &str) -> Result<Option<String>> {
        let conn = self.conn();
        let mut stmt = conn.prepare("SELECT session_type FROM last_types WHERE device_id = ?1 AND username = ?2")?;
        let mut rows = stmt.query(params![device_id, username])?;
        Ok(match rows.next()? {
            Some(r) => Some(r.get(0)?),
            None => None,
        })
    }

    // --- audit ---

    pub fn audit(&self, actor: &str, action: &str, device_id: Option<&str>, detail: &str, client_address: Option<&str>) {
        let result = self.conn().execute(
            "INSERT INTO audit (at, actor, action, device_id, detail, client_address) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
            params![now(), actor, action, device_id, detail, client_address],
        );
        if let Err(e) = result {
            tracing::warn!(error = %e, action, "db: writing an audit row failed");
        }
    }

    pub fn audit_log(&self, limit: u32, before_id: Option<i64>) -> Result<Vec<(i64, AuditEntry)>> {
        let conn = self.conn();
        let mut stmt = conn.prepare(
            "SELECT id, at, actor, action, device_id, detail, client_address FROM audit
             WHERE ?1 IS NULL OR id < ?1 ORDER BY id DESC LIMIT ?2",
        )?;
        let rows = stmt
            .query_map(params![before_id, limit], |r| {
                Ok((
                    r.get(0)?,
                    AuditEntry {
                        at: r.get(1)?,
                        actor: r.get(2)?,
                        action: r.get(3)?,
                        device_id: r.get(4)?,
                        detail: r.get(5)?,
                        client_address: r.get(6)?,
                    },
                ))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    // --- thin clients ---

    /// Records a thin client's hello: a new row, or the existing one's
    /// report and last-seen time (its name is kept).
    pub fn thin_client_seen(&self, mac: &str, report: &str) -> Result<()> {
        let t = now();
        self.conn().execute(
            "INSERT INTO thin_clients (mac, first_seen, last_seen, last_report) VALUES (?1, ?2, ?2, ?3)
             ON CONFLICT(mac) DO UPDATE SET last_seen = ?2, last_report = ?3",
            params![mac, t, report],
        )?;
        Ok(())
    }

    pub fn touch_thin_client(&self, mac: &str) -> Result<()> {
        self.conn().execute("UPDATE thin_clients SET last_seen = ?2 WHERE mac = ?1", params![mac, now()])?;
        Ok(())
    }

    pub fn thin_client(&self, mac: &str) -> Result<Option<ThinClientRow>> {
        self.conn()
            .query_row(
                "SELECT mac, name, first_seen, last_seen, last_report FROM thin_clients WHERE mac = ?1",
                params![mac],
                thin_client_row,
            )
            .optional()
            .map_err(Into::into)
    }

    pub fn thin_clients(&self) -> Result<Vec<ThinClientRow>> {
        let conn = self.conn();
        let mut stmt = conn.prepare("SELECT mac, name, first_seen, last_seen, last_report FROM thin_clients ORDER BY mac")?;
        let rows = stmt.query_map([], thin_client_row)?.collect::<rusqlite::Result<Vec<_>>>()?;
        Ok(rows)
    }

    pub fn rename_thin_client(&self, mac: &str, name: &str) -> Result<bool> {
        Ok(self.conn().execute("UPDATE thin_clients SET name = ?2 WHERE mac = ?1", params![mac, name])? == 1)
    }

    pub fn remove_thin_client(&self, mac: &str) -> Result<bool> {
        Ok(self.conn().execute("DELETE FROM thin_clients WHERE mac = ?1", params![mac])? == 1)
    }

    // --- settings: Veil-wide values the admin UI edits, as JSON ---

    pub fn setting(&self, key: &str) -> Result<Option<String>> {
        self.conn()
            .query_row("SELECT value FROM settings WHERE key = ?1", params![key], |r| r.get(0))
            .optional()
            .map_err(Into::into)
    }

    pub fn set_setting(&self, key: &str, value: &str) -> Result<()> {
        self.conn().execute(
            "INSERT INTO settings (key, value) VALUES (?1, ?2) ON CONFLICT(key) DO UPDATE SET value = ?2",
            params![key, value],
        )?;
        Ok(())
    }

    // --- portal sessions: a browser user's sign-in to Veil itself ---

    pub fn add_portal_session(&self, id_sha256: &str, username: &str, csrf: &str) -> Result<()> {
        let t = now();
        self.conn().execute(
            "INSERT INTO portal_sessions (id_sha256, username, csrf, created_at, last_seen) VALUES (?1, ?2, ?3, ?4, ?4)",
            params![id_sha256, username, csrf, t],
        )?;
        Ok(())
    }

    /// The session, if it was used within `idle_secs` and is younger than
    /// `max_secs`; touches it. Sessions past either limit are deleted on
    /// the way.
    pub fn portal_session(&self, id_sha256: &str, idle_secs: i64, max_secs: i64) -> Result<Option<WebSession>> {
        let t = now();
        let conn = self.conn();
        conn.execute("DELETE FROM portal_sessions WHERE last_seen < ?1 OR created_at < ?2", params![t - idle_secs, t - max_secs])?;
        let session = conn
            .query_row(
                "SELECT username, csrf, last_seen FROM portal_sessions WHERE id_sha256 = ?1",
                params![id_sha256],
                |r| Ok(WebSession { username: r.get(0)?, csrf: r.get(1)?, last_seen: r.get(2)? }),
            )
            .optional()?;
        if session.is_some() {
            conn.execute("UPDATE portal_sessions SET last_seen = ?2 WHERE id_sha256 = ?1", params![id_sha256, t])?;
        }
        Ok(session)
    }

    pub fn remove_portal_session(&self, id_sha256: &str) -> Result<()> {
        self.conn().execute("DELETE FROM portal_sessions WHERE id_sha256 = ?1", params![id_sha256])?;
        Ok(())
    }
}

fn device_row(r: &rusqlite::Row<'_>) -> rusqlite::Result<Device> {
    let mode: String = r.get(6)?;
    Ok(Device {
        id: r.get(0)?,
        name: r.get(1)?,
        hostname: r.get(2)?,
        client_address: r.get(3)?,
        cert_sha256: r.get(4)?,
        enabled: r.get(5)?,
        mode: mode.parse().unwrap_or(DeviceMode::Auto),
        joined_at: r.get(7)?,
        last_seen: r.get(8)?,
    })
}

fn thin_client_row(r: &rusqlite::Row<'_>) -> rusqlite::Result<ThinClientRow> {
    Ok(ThinClientRow {
        mac: r.get(0)?,
        name: r.get(1)?,
        first_seen: r.get(2)?,
        last_seen: r.get(3)?,
        last_report: r.get(4)?,
    })
}

fn placement_row(r: &rusqlite::Row<'_>) -> rusqlite::Result<Placement> {
    Ok(Placement {
        device_id: r.get(0)?,
        username: r.get(1)?,
        uid: r.get(2)?,
        session_type: r.get(3)?,
        started_at: r.get(4)?,
        viewer_attached: r.get(5)?,
    })
}

fn insert_placement(conn: &Connection, p: &Placement) -> Result<()> {
    conn.execute(
        "INSERT OR REPLACE INTO placements (device_id, username, uid, session_type, started_at, viewer_attached)
         VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
        params![p.device_id, p.username, p.uid, p.session_type, p.started_at, p.viewer_attached],
    )?;
    // Every running session, however it was started (through Veil or
    // straight at the host), is that user's latest choice there.
    conn.execute(
        "INSERT OR REPLACE INTO last_types (device_id, username, session_type) VALUES (?1, ?2, ?3)",
        params![p.device_id, p.username, p.session_type],
    )?;
    Ok(())
}

pub fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn join_tokens_are_single_use_and_expire() {
        let db = Db::in_memory();
        db.add_join_token("t1", "aa", 3600, "admin").unwrap();
        assert_eq!(db.consume_join_token("t1", "bb").unwrap(), Err(TokenError::Unknown));
        assert_eq!(db.consume_join_token("nope", "aa").unwrap(), Err(TokenError::Unknown));
        assert_eq!(db.consume_join_token("t1", "aa").unwrap(), Ok(()));
        assert_eq!(db.consume_join_token("t1", "aa").unwrap(), Err(TokenError::Used));
        db.add_join_token("t2", "cc", -1, "admin").unwrap();
        assert_eq!(db.consume_join_token("t2", "cc").unwrap(), Err(TokenError::Expired));
    }

    #[test]
    fn rejoining_with_the_same_certificate_keeps_the_device() {
        let db = Db::in_memory();
        let a = db.join_device("d1", "host1", "host1.lan", "pin1", DeviceMode::Auto).unwrap();
        db.grant(&a, &Grantee::User("alice".into())).unwrap();
        let b = db.join_device("d2", "host1", "host1.example", "pin1", DeviceMode::Direct).unwrap();
        assert_eq!(a, b);
        let d = db.device(&a).unwrap().unwrap();
        assert_eq!(d.client_address, "host1.example");
        assert_eq!(d.mode, DeviceMode::Auto);
        assert_eq!(db.entitlements(&a).unwrap(), vec![Grantee::User("alice".into())]);
        let c = db.join_device("d3", "host3", "host3.lan", "pin3", DeviceMode::Direct).unwrap();
        assert_eq!(c, "d3");
    }

    #[test]
    fn entitlements_match_users_and_groups_once() {
        let db = Db::in_memory();
        db.join_device("d1", "host1", "host1", "p1", DeviceMode::Auto).unwrap();
        db.join_device("d2", "host2", "host2", "p2", DeviceMode::Auto).unwrap();
        db.join_device("d3", "host3", "host3", "p3", DeviceMode::Auto).unwrap();
        db.grant("d1", &Grantee::User("alice".into())).unwrap();
        db.grant("d1", &Grantee::Group("staff".into())).unwrap();
        db.grant("d2", &Grantee::Group("staff".into())).unwrap();
        db.grant("d3", &Grantee::User("bob".into())).unwrap();
        let ids = |v: Vec<Device>| {
            let mut ids: Vec<String> = v.into_iter().map(|d| d.id).collect();
            ids.sort();
            ids
        };
        assert_eq!(ids(db.entitled_devices("alice", &["staff".into()]).unwrap()), vec!["d1", "d2"]);
        assert_eq!(ids(db.entitled_devices("alice", &[]).unwrap()), vec!["d1"]);
        assert_eq!(ids(db.entitled_devices("carol", &["staff".into()]).unwrap()), vec!["d1", "d2"]);
        assert!(db.entitled_devices("carol", &[]).unwrap().is_empty());
        let d2 = db.device("d2").unwrap().unwrap();
        db.update_device("d2", &d2.name, &d2.client_address, d2.mode, false).unwrap();
        assert_eq!(ids(db.entitled_devices("carol", &["staff".into()]).unwrap()), vec!["d1"]);
        // Removing a device takes its entitlements and placements along.
        db.upsert_placement(&Placement {
            device_id: "d1".into(),
            username: "alice".into(),
            uid: 1000,
            session_type: "plasma".into(),
            started_at: 1,
            viewer_attached: false,
        })
        .unwrap();
        assert!(db.remove_device("d1").unwrap());
        assert!(db.placements().unwrap().is_empty());
        assert!(db.entitlements("d1").unwrap().is_empty());
    }

    #[test]
    fn snapshots_replace_a_devices_placements() {
        let db = Db::in_memory();
        db.join_device("d1", "host1", "host1", "p1", DeviceMode::Auto).unwrap();
        let p = |u: &str| Placement {
            device_id: "d1".into(),
            username: u.into(),
            uid: 1,
            session_type: "t".into(),
            started_at: 1,
            viewer_attached: false,
        };
        db.upsert_placement(&p("alice")).unwrap();
        db.replace_placements("d1", &[p("bob")]).unwrap();
        let names: Vec<String> = db.placements().unwrap().into_iter().map(|p| p.username).collect();
        assert_eq!(names, vec!["bob"]);
        db.clear_placements().unwrap();
        assert!(db.placements().unwrap().is_empty());
        db.upsert_placement(&p("bob")).unwrap();
        db.set_viewer("d1", "bob", true).unwrap();
        assert!(db.placements_for_user("bob").unwrap()[0].viewer_attached);
        db.remove_placement("d1", "bob").unwrap();
        assert!(db.placements().unwrap().is_empty());
    }

    #[test]
    fn the_last_type_outlives_the_session() {
        let db = Db::in_memory();
        db.join_device("d1", "host1", "host1", "p1", DeviceMode::Auto).unwrap();
        assert_eq!(db.last_type("d1", "bob").unwrap(), None);
        let p = |t: &str| Placement {
            device_id: "d1".into(),
            username: "bob".into(),
            uid: 1,
            session_type: t.into(),
            started_at: 1,
            viewer_attached: false,
        };
        db.upsert_placement(&p("terminal")).unwrap();
        db.replace_placements("d1", &[p("plasma")]).unwrap();
        db.remove_placement("d1", "bob").unwrap();
        db.clear_placements().unwrap();
        assert_eq!(db.last_type("d1", "bob").unwrap().as_deref(), Some("plasma"));
        assert_eq!(db.last_type("d1", "alice").unwrap(), None);
        assert!(db.remove_device("d1").unwrap());
        assert_eq!(db.last_type("d1", "bob").unwrap(), None);
    }

    #[test]
    fn thin_clients_keep_their_name_across_reports() {
        let db = Db::in_memory();
        db.thin_client_seen("52:54:00:12:34:56", r#"{"hostname":"a"}"#).unwrap();
        assert!(db.rename_thin_client("52:54:00:12:34:56", "Front desk").unwrap());
        db.thin_client_seen("52:54:00:12:34:56", r#"{"hostname":"b"}"#).unwrap();
        let row = db.thin_client("52:54:00:12:34:56").unwrap().unwrap();
        assert_eq!((row.name.as_str(), row.last_report.as_str()), ("Front desk", r#"{"hostname":"b"}"#));
        assert_eq!(db.thin_clients().unwrap().len(), 1);
        assert!(db.remove_thin_client("52:54:00:12:34:56").unwrap());
        assert!(db.thin_client("52:54:00:12:34:56").unwrap().is_none());
        assert!(!db.rename_thin_client("52:54:00:12:34:56", "x").unwrap());
    }

    #[test]
    fn settings_are_replaced() {
        let db = Db::in_memory();
        assert_eq!(db.setting("k").unwrap(), None);
        db.set_setting("k", "1").unwrap();
        db.set_setting("k", "2").unwrap();
        assert_eq!(db.setting("k").unwrap().as_deref(), Some("2"));
    }

    #[test]
    fn portal_sessions_expire_when_idle_or_old() {
        let db = Db::in_memory();
        db.add_portal_session("h", "alice", "csrf").unwrap();
        assert_eq!(db.portal_session("h", 60, 600).unwrap().unwrap().username, "alice");
        // Past the absolute limit however recently it was used.
        db.conn().execute("UPDATE portal_sessions SET created_at = 0", []).unwrap();
        assert!(db.portal_session("h", 60, 600).unwrap().is_none());
        db.add_portal_session("h", "alice", "csrf").unwrap();
        db.conn().execute("UPDATE portal_sessions SET last_seen = 0", []).unwrap();
        assert!(db.portal_session("h", 60, 600).unwrap().is_none());
        db.add_portal_session("h", "alice", "csrf").unwrap();
        db.remove_portal_session("h").unwrap();
        assert!(db.portal_session("h", 60, 600).unwrap().is_none());
    }
}
