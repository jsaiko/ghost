// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// GDP's stream framing (gdp-spec.md §3.1), the Rust side of what
// libgdp/include/gdp/framing.hpp implements in C++:
//
//   +----------------+------------------------+
//   | length: u32 LE | protobuf message bytes |
//   +----------------+------------------------+
//
// length excludes the 4-byte prefix itself; no message may exceed 1 MiB.
//
// Generic over AsyncRead/AsyncWrite, so the same code serves QUIC streams
// and the local Unix sockets; the `_sync` pair is for ghostauth, which
// has no event loop (PAM's conversation callbacks are synchronous).
use std::io::{Read, Write};

use anyhow::{bail, Result};
use prost::Message;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

use crate::lobby::LobbyErrorCode;

pub const MAX_FRAME_LEN: u32 = 0x100000;

// Why a read failed, kept distinct so the lobby can close the QUIC
// connection with the matching gdp-spec.md §12 error code (close_code()).
#[derive(Debug, thiserror::Error)]
pub enum FrameError {
    #[error("frame too large ({len} bytes, max {max})")]
    TooLarge { len: u32, max: u32 },
    #[error("malformed frame: {0}")]
    Malformed(#[from] prost::DecodeError),
    #[error("read error: {0}")]
    Io(#[from] std::io::Error),
}

impl FrameError {
    // None for an I/O error, which isn't a protocol violation by the peer.
    pub fn close_code(&self) -> Option<u32> {
        match self {
            FrameError::TooLarge { .. } => Some(LobbyErrorCode::LobbyErrorFrameTooLarge as u32),
            FrameError::Malformed(_) => Some(LobbyErrorCode::LobbyErrorMalformedFrame as u32),
            FrameError::Io(_) => None,
        }
    }
}

pub async fn read_frame<T: Message + Default>(
    recv: &mut (impl AsyncRead + Unpin),
) -> std::result::Result<T, FrameError> {
    read_frame_max(recv, MAX_FRAME_LEN).await
}

/// read_frame() with a tighter cap than the protocol's 1 MiB, for a peer
/// that hasn't authenticated yet and has no business sending that much
/// (ghostd's lobby before PAM answers). The body buffer is wiped once
/// decoded, since an AuthResponse carries a password; the decoded message
/// is the caller's to wipe.
pub async fn read_frame_max<T: Message + Default>(
    recv: &mut (impl AsyncRead + Unpin),
    max: u32,
) -> std::result::Result<T, FrameError> {
    let max = max.min(MAX_FRAME_LEN);
    let mut len_buf = [0u8; 4];
    recv.read_exact(&mut len_buf).await?;
    let len = u32::from_le_bytes(len_buf);
    if len > max {
        return Err(FrameError::TooLarge { len, max });
    }
    let mut body = zeroize::Zeroizing::new(vec![0u8; len as usize]);
    recv.read_exact(&mut body).await?;
    Ok(T::decode(&body[..])?)
}

/// read_frame_max() without the decode: the frame's body as raw bytes,
/// for a reader that has to try more than one message type on it (veild
/// telling a lobby connection from a gateway session by its first frame).
pub async fn read_raw_frame_max(
    recv: &mut (impl AsyncRead + Unpin),
    max: u32,
) -> std::result::Result<Vec<u8>, FrameError> {
    let max = max.min(MAX_FRAME_LEN);
    let mut len_buf = [0u8; 4];
    recv.read_exact(&mut len_buf).await?;
    let len = u32::from_le_bytes(len_buf);
    if len > max {
        return Err(FrameError::TooLarge { len, max });
    }
    let mut body = vec![0u8; len as usize];
    recv.read_exact(&mut body).await?;
    Ok(body)
}

/// One complete frame -- prefix and body -- as bytes, for a caller that
/// has to hand the socket the whole thing in a single call (ghostd's
/// SCM_RIGHTS reply, where the fd rides on the first byte sent).
pub fn frame_bytes<T: Message>(msg: &T) -> Result<Vec<u8>> {
    let body = msg.encode_to_vec();
    if body.len() > MAX_FRAME_LEN as usize {
        bail!("outgoing frame too large ({} bytes, max {MAX_FRAME_LEN})", body.len());
    }
    let mut frame = Vec::with_capacity(4 + body.len());
    frame.extend_from_slice(&(body.len() as u32).to_le_bytes());
    frame.extend_from_slice(&body);
    Ok(frame)
}

/// frame_bytes() into a buffer that is wiped when dropped, for a message
/// that carries a password. Encoded in place at its exact size, so no
/// copy is left behind by a reallocation; the message itself is the
/// caller's to wipe.
pub fn secret_frame_bytes<T: Message>(msg: &T) -> Result<zeroize::Zeroizing<Vec<u8>>> {
    let len = msg.encoded_len();
    if len > MAX_FRAME_LEN as usize {
        bail!("outgoing frame too large ({len} bytes, max {MAX_FRAME_LEN})");
    }
    let mut frame = zeroize::Zeroizing::new(Vec::with_capacity(4 + len));
    frame.extend_from_slice(&(len as u32).to_le_bytes());
    msg.encode(&mut *frame)?;
    Ok(frame)
}

pub async fn write_frame<T: Message>(send: &mut (impl AsyncWrite + Unpin), msg: &T) -> Result<()> {
    send.write_all(&frame_bytes(msg)?).await?;
    Ok(())
}

/// read_frame_max() on a blocking reader, wiping its body buffer the same
/// way, since ghostauth's frames carry passwords.
pub fn read_frame_sync<T: Message + Default>(recv: &mut impl Read, max: u32) -> std::result::Result<T, FrameError> {
    let max = max.min(MAX_FRAME_LEN);
    let mut len_buf = [0u8; 4];
    recv.read_exact(&mut len_buf)?;
    let len = u32::from_le_bytes(len_buf);
    if len > max {
        return Err(FrameError::TooLarge { len, max });
    }
    let mut body = zeroize::Zeroizing::new(vec![0u8; len as usize]);
    recv.read_exact(&mut body)?;
    Ok(T::decode(&body[..])?)
}

/// write_frame() on a blocking writer.
pub fn write_frame_sync<T: Message>(send: &mut impl Write, msg: &T) -> Result<()> {
    send.write_all(&frame_bytes(msg)?)?;
    Ok(())
}
