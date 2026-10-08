// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Session-type profiles (docs/reference/session-profiles.md): INI files
// under sessions.d/, one per selectable desktop. ghostd only reads them to
// build the lobby's list (id, name, TryExec on PATH) and to validate the id
// a client picks; wraith's session/session_profile.cpp parses the same
// format and is what launches Exec.
use std::collections::HashMap;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionProfile {
    pub id: String,
    pub name: String,
    pub exec: String,
    pub try_exec: Option<String>,
}

/// Parses one profile file's contents. `None` means "masked, disabled or
/// invalid": an empty file (the documented way `/etc/ghost/sessions.d`
/// masks a shipped default), `Enabled=false` (which needs no other keys,
/// so a two-line override disables a shipped profile), or one missing the
/// required `Name`/`Exec` keys all read the same way -- silently absent
/// from the list rather than a startup error, since a single bad profile
/// shouldn't take ghostd down.
pub fn parse(id: &str, text: &str) -> Option<SessionProfile> {
    if text.trim().is_empty() {
        return None;
    }

    let mut in_session_section = false;
    let mut name = None;
    let mut exec = None;
    let mut try_exec = None;
    let mut have_backend = false;
    let mut enabled = true;

    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        if let Some(section) = line.strip_prefix('[').and_then(|s| s.strip_suffix(']')) {
            in_session_section = section.trim() == "Session";
            continue;
        }
        if !in_session_section {
            continue;
        }
        let Some((key, value)) = line.split_once('=') else { continue };
        let key = key.trim();
        let value = value.trim();
        match key {
            "Name" => name = Some(value.to_string()),
            "Exec" => exec = Some(value.to_string()),
            "TryExec" => try_exec = Some(value.to_string()),
            "Enabled" => enabled = matches!(value, "true" | "1" | "yes"),
            // Required, so a profile wraith would refuse is never listed;
            // its value is wraith's to check.
            "Backend" => have_backend = !value.is_empty(),
            // LogoutExec, Environment: wraith's.
            _ => {}
        }
    }

    if !enabled || !have_backend {
        return None;
    }
    Some(SessionProfile { id: id.to_string(), name: name?, exec: exec?, try_exec })
}

/// Loads every profile visible across `dirs`, in priority order (a
/// directory earlier in `dirs` wins for a given filename stem -- callers
/// pass `/etc/ghost/sessions.d` before the shipped datadir so admin
/// overrides and masks take effect). Profiles are returned in the order
/// their id was first seen.
pub fn load(dirs: &[PathBuf]) -> Vec<SessionProfile> {
    let mut seen: HashMap<String, Option<SessionProfile>> = HashMap::new();
    let mut order = Vec::new();

    for dir in dirs {
        let Ok(entries) = std::fs::read_dir(dir) else { continue };
        for entry in entries.flatten() {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("conf") {
                continue;
            }
            let Some(id) = path.file_stem().and_then(|s| s.to_str()) else { continue };
            if seen.contains_key(id) {
                continue; // an earlier (higher-priority) dir already decided this id
            }
            let text = std::fs::read_to_string(&path).unwrap_or_default();
            let profile = parse(id, &text);
            order.push(id.to_string());
            seen.insert(id.to_string(), profile);
        }
    }

    order.into_iter().filter_map(|id| seen.remove(&id).flatten()).collect()
}

/// True if every program in `try_exec` (a whitespace-separated list), or
/// the profile's `exec`'s first word when `try_exec` is absent, resolves to
/// something runnable: an absolute path that's executable, or a bare name
/// found on `$PATH`. Mirrors freedesktop .desktop's TryExec semantics
/// closely enough for this purpose, extended to several programs for
/// sessions that need more than one (a desktop and the compositor it runs
/// under, say).
pub fn is_available(profile: &SessionProfile) -> bool {
    match profile.try_exec.as_deref() {
        Some(list) => {
            let mut programs = list.split_whitespace().peekable();
            programs.peek().is_some() && programs.all(resolve_on_path_or_absolute)
        }
        None => resolve_on_path_or_absolute(
            profile.exec.split_whitespace().next().unwrap_or(profile.exec.as_str()),
        ),
    }
}

fn resolve_on_path_or_absolute(candidate: &str) -> bool {
    let path = Path::new(candidate);
    if path.is_absolute() {
        return is_executable(path);
    }
    let Some(path_var) = std::env::var_os("PATH") else { return false };
    std::env::split_paths(&path_var).any(|dir| is_executable(&dir.join(candidate)))
}

fn is_executable(path: &Path) -> bool {
    std::fs::metadata(path).map(|m| m.is_file() && m.permissions().mode() & 0o111 != 0).unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn write(dir: &Path, name: &str, contents: &str) {
        fs::write(dir.join(name), contents).unwrap();
    }

    fn tempdir(label: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("ghostd-profiles-test-{label}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn parses_a_well_formed_profile() {
        let profile = parse(
            "terminal",
            "[Session]\nName=Terminal\nBackend=screencast-ext\nExec=foot\nTryExec=foot\n",
        )
        .unwrap();
        assert_eq!(profile.id, "terminal");
        assert_eq!(profile.name, "Terminal");
        assert_eq!(profile.exec, "foot");
        assert_eq!(profile.try_exec.as_deref(), Some("foot"));
    }

    #[test]
    fn empty_file_is_masked() {
        assert!(parse("terminal", "").is_none());
        assert!(parse("terminal", "   \n\n").is_none());
    }

    #[test]
    fn enabled_defaults_true_and_false_disables() {
        assert!(parse("x", "[Session]\nName=X\nBackend=screencast-ext\nExec=x\nEnabled=true\n").is_some());
        assert!(parse("x", "[Session]\nName=X\nExec=x\nEnabled=false\n").is_none());
        // A disabled override needs no Name/Exec.
        assert!(parse("x", "[Session]\nEnabled=false\n").is_none());
    }

    #[test]
    fn missing_required_keys_is_invalid() {
        assert!(parse("x", "[Session]\nName=X\nExec=x\n").is_none()); // no Backend
        assert!(parse("x", "[Session]\nName=X\n").is_none()); // no Exec
        assert!(parse("x", "[Session]\nExec=x\n").is_none()); // no Name
    }

    #[test]
    fn load_honors_override_precedence_and_masking() {
        let etc = tempdir("etc");
        let datadir = tempdir("datadir");

        write(&datadir, "terminal.conf", "[Session]\nName=Terminal\nBackend=screencast-ext\nExec=foot\n");
        write(&datadir, "plasma.conf", "[Session]\nName=KDE Plasma\nBackend=screencast-kwin\nExec=plasma\n");
        write(&datadir, "gnome.conf", "[Session]\nName=GNOME\nBackend=screencast-ext\nExec=gnome\n");
        // etc overrides terminal's name, masks plasma entirely, and disables gnome.
        write(&etc, "terminal.conf", "[Session]\nName=Terminal (custom)\nBackend=screencast-ext\nExec=foot\n");
        write(&etc, "plasma.conf", "");
        write(&etc, "gnome.conf", "[Session]\nEnabled=false\n");

        let profiles = load(&[etc.clone(), datadir.clone()]);
        let by_id: HashMap<_, _> = profiles.into_iter().map(|p| (p.id.clone(), p)).collect();

        assert_eq!(by_id.get("terminal").unwrap().name, "Terminal (custom)");
        assert!(!by_id.contains_key("plasma"));
        assert!(!by_id.contains_key("gnome"));

        let _ = fs::remove_dir_all(&etc);
        let _ = fs::remove_dir_all(&datadir);
    }

    #[test]
    fn is_available_finds_a_real_binary_on_path() {
        // `sh` is guaranteed present in any environment these tests run in.
        let profile = SessionProfile {
            id: "x".into(),
            name: "X".into(),
            exec: "sh".into(),
            try_exec: Some("sh".into()),
        };
        assert!(is_available(&profile));
    }

    #[test]
    fn is_available_needs_every_listed_program() {
        let mut profile = SessionProfile {
            id: "x".into(),
            name: "X".into(),
            exec: "sh".into(),
            try_exec: Some("sh ls".into()),
        };
        assert!(is_available(&profile));
        profile.try_exec = Some("sh definitely-not-a-real-binary-anywhere".into());
        assert!(!is_available(&profile));
        profile.try_exec = Some("  ".into());
        assert!(!is_available(&profile));
    }

    #[test]
    fn is_available_rejects_a_nonexistent_binary() {
        let profile = SessionProfile {
            id: "x".into(),
            name: "X".into(),
            exec: "definitely-not-a-real-binary-anywhere".into(),
            try_exec: None,
        };
        assert!(!is_available(&profile));
    }
}
