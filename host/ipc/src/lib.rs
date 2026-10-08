// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Shared between ghostd, ghostseat, ghostauth, ghostlogin and veild: the
// protobuf framing, the generated types for every .proto any of them
// needs (build.rs), and the local socket paths.
pub mod framing;

pub mod lobby {
    include!(concat!(env!("OUT_DIR"), "/gdp.lobby.rs"));
}

pub mod session {
    include!(concat!(env!("OUT_DIR"), "/gdp.session.rs"));
}

pub mod wisp {
    include!(concat!(env!("OUT_DIR"), "/gdp.wisp.rs"));
}

pub mod broker {
    include!(concat!(env!("OUT_DIR"), "/ghost.broker.rs"));
}

pub mod control {
    include!(concat!(env!("OUT_DIR"), "/ghost.control.rs"));
}

pub mod ghostseat {
    include!(concat!(env!("OUT_DIR"), "/ghost.ghostseat.rs"));
}

pub mod ghostlogin {
    include!(concat!(env!("OUT_DIR"), "/ghost.ghostlogin.rs"));
}

pub mod ghostauth {
    include!(concat!(env!("OUT_DIR"), "/ghost.ghostauth.rs"));
}

pub mod paths {
    use std::path::PathBuf;

    /// Every ghost socket lives here (docs/reference/files-and-paths.md).
    /// Owned by the `ghost` user, mode 0711: each session user can reach
    /// their own control socket and list nothing.
    pub const RUN_DIR: &str = "/run/ghost";

    /// The group that gates the daemons' sockets: ghostd's own, and veild
    /// on a broker.
    pub const GROUP: &str = "ghost";

    /// ghostd or veild -> a fresh ghostauth instance per connection
    /// (host/proto/ghostauth.proto); systemd's ghostauth.socket.
    pub const AUTH_SOCKET: &str = "/run/ghost/auth.sock";

    /// ghostd -> a fresh ghostseat instance per session
    /// (host/proto/ghostseat.proto's Open); systemd's ghostseat.socket.
    pub const SEAT_SOCKET: &str = "/run/ghost/seat.sock";

    /// ghostseat -> ghostd, one connection per event
    /// (host/proto/ghostseat.proto's Event); ghostd's own.
    pub const EVENTS_SOCKET: &str = "/run/ghost/events.sock";

    /// ghostlogin -> ghostd (host/proto/ghostlogin.proto); ghostd's own,
    /// and ghostlogin runs as root.
    pub const GHOSTLOGIN_SOCKET: &str = "/run/ghost/ghostlogin.sock";

    /// ghostseat <-> wraith control socket (host/proto/control.proto),
    /// owned by the session's uid.
    pub fn control_socket(uid: u32) -> PathBuf {
        PathBuf::from(RUN_DIR).join(format!("{uid}.sock"))
    }

    /// ghostd -> the open session's ghostseat (STATUS, CLOSE), root:ghost
    /// 0660. The path comes from the uid alone, so ghostd finds every
    /// open session after its own restart without persisting anything.
    pub fn seat_socket(uid: u32) -> PathBuf {
        PathBuf::from(RUN_DIR).join(format!("{uid}-seat.sock"))
    }

    /// The uid a `seat_socket` file name belongs to, for ghostd's scan of
    /// RUN_DIR at startup and for Veil's snapshot.
    pub fn seat_socket_uid(file_name: &str) -> Option<u32> {
        file_name.strip_suffix("-seat.sock")?.parse().ok()
    }
}

/// Seconds since the Unix epoch, the unit every timestamp on the wire uses.
pub fn unix_now() -> i64 {
    use std::time::{SystemTime, UNIX_EPOCH};
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs() as i64
}
