// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Layered TOML configuration, shared by ghostd and veild
// (docs/reference/configuration.md): built-in defaults, then the main
// file, then its drop-in directory (`<file>.d/*.toml`) in name order, each
// merged key by key over the last. Command-line flags go on top in each
// daemon's main.rs.
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use serde::de::DeserializeOwned;

/// Reads `path` and then its drop-in directory (`path` with a `.d`
/// extension, e.g. /etc/ghost/ghostd.d), returning the merged config and
/// the files that went into it. A missing `path` is an error only when
/// `required` (it was named with -f); the daemon otherwise runs on
/// defaults.
///
/// `strict` is sshd's StrictModes: every file must be owned by root or by
/// the user running the daemon, and not be writable by group or others.
/// ghostd and veild, each running as its own user, always set it.
pub fn load<T: DeserializeOwned>(path: &Path, required: bool, strict: bool) -> Result<(T, Vec<PathBuf>)> {
    let mut files = Vec::new();
    match std::fs::metadata(path) {
        Ok(_) => files.push(path.to_path_buf()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound && !required => {}
        Err(e) => return Err(e).with_context(|| format!("reading {}", path.display())),
    }

    let dropin_dir = path.with_extension("d");
    match std::fs::read_dir(&dropin_dir) {
        Ok(entries) => {
            if strict {
                check_permissions(&dropin_dir)?;
            }
            let mut dropins = Vec::new();
            for entry in entries {
                let entry_path = entry.with_context(|| format!("listing {}", dropin_dir.display()))?.path();
                if entry_path.extension().is_some_and(|ext| ext == "toml") {
                    dropins.push(entry_path);
                }
            }
            dropins.sort();
            files.extend(dropins);
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
        Err(e) => return Err(e).with_context(|| format!("listing {}", dropin_dir.display())),
    }

    let mut merged = toml::Table::new();
    for file in &files {
        if strict {
            check_permissions(file)?;
        }
        let text = std::fs::read_to_string(file).with_context(|| format!("reading {}", file.display()))?;
        // Each file on its own first: unknown keys and wrong types are
        // reported with this file's name and line, which the merged
        // table below no longer carries.
        toml::from_str::<T>(&text).with_context(|| format!("in {}", file.display()))?;
        let table: toml::Table = toml::from_str(&text).with_context(|| format!("in {}", file.display()))?;
        merge(&mut merged, table);
    }
    let config: T = toml::Value::Table(merged).try_into().context("merging the configuration files")?;
    Ok((config, files))
}

// Later files win key by key; tables merge rather than replace, so a
// drop-in can set one field of [auth.penalties] and keep the rest.
fn merge(base: &mut toml::Table, over: toml::Table) {
    for (key, value) in over {
        match (base.get_mut(&key), value) {
            (Some(toml::Value::Table(base_table)), toml::Value::Table(over_table)) => merge(base_table, over_table),
            (_, value) => {
                base.insert(key, value);
            }
        }
    }
}

fn check_permissions(path: &Path) -> Result<()> {
    let meta = std::fs::metadata(path).with_context(|| format!("reading {}", path.display()))?;
    let euid = nix::unistd::geteuid().as_raw();
    if meta.uid() != 0 && meta.uid() != euid {
        bail!("{} is owned by uid {}, not root; refusing to use it", path.display(), meta.uid());
    }
    if meta.mode() & 0o022 != 0 {
        bail!(
            "{} is writable by group or others (mode {:o}); refusing to use it (chmod go-w)",
            path.display(),
            meta.mode() & 0o7777
        );
    }
    Ok(())
}

/// Turns a shipped config file, every key commented out at its default,
/// into the same file with every key uncommented -- for each daemon's
/// test that the shipped copy and the built-in defaults agree.
pub fn uncomment_defaults(shipped: &str) -> String {
    shipped
        .lines()
        .map(|line| line.strip_prefix('#').filter(|rest| !rest.starts_with([' ', '#']) && !rest.is_empty()).unwrap_or(line))
        .collect::<Vec<_>>()
        .join("\n")
}
