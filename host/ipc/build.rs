// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Compiles every .proto the Rust side uses, once, for ghostd, ghostseat,
// ghostlogin and veild. The C++ side compiles its own copies from the same
// files (libgdp/CMakeLists.txt, host/wraith/CMakeLists.txt).
//
// libgdp/proto/: lobby.proto and session.proto are GDP itself (veild's
// gateway reads SessionHello), wisp.proto is the thin-client channel.
// host/proto/: control.proto, ghostseat.proto, ghostauth.proto and
// ghostlogin.proto never leave the host; broker.proto is the host channel
// to Veil.
fn main() {
    // The protos live outside this package, so without this cargo only
    // reruns the script when something under host/ipc changes and a
    // proto edit leaves stale generated types behind.
    println!("cargo:rerun-if-changed=../../libgdp/proto");
    println!("cargo:rerun-if-changed=../proto");
    prost_build::compile_protos(
        &[
            "../../libgdp/proto/lobby.proto",
            "../../libgdp/proto/session.proto",
            "../../libgdp/proto/wisp.proto",
            "../proto/broker.proto",
            "../proto/control.proto",
            "../proto/ghostseat.proto",
            "../proto/ghostauth.proto",
            "../proto/ghostlogin.proto",
        ],
        &["../../libgdp/proto", "../proto"],
    )
    .expect("failed to compile the GDP and host protos");
}
