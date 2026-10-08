// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// main.rs's shipped sessions.d fallback (GHOST_DATADIR) and session.rs's
// ghostseat exec path (GHOST_LIBEXECDIR) need the install prefix at
// compile time -- there's no installed-location lookup at runtime. The
// top-level CMakeLists.txt sets GHOST_PREFIX to -DCMAKE_INSTALL_PREFIX
// when it invokes cargo; plain `cargo build` (e.g. `cargo test` during
// development) falls back to /usr/local, matching CMakeLists.txt's own
// default. Proto compilation is in ipc/build.rs.
fn main() {
    let prefix = std::env::var("GHOST_PREFIX").unwrap_or_else(|_| "/usr/local".to_string());
    println!("cargo:rustc-env=GHOST_DATADIR={prefix}/share/ghost");
    println!("cargo:rustc-env=GHOST_LIBEXECDIR={prefix}/lib/ghost");
    println!("cargo:rerun-if-env-changed=GHOST_PREFIX");
}
