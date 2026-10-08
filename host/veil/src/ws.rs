// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// The browser client's fallback transport (gdp-spec.md §15): a GDP
// session over one WebSocket, for browsers or networks where
// WebTransport fails. Every binary message is one byte of channel and
// then that channel's bytes:
//
//   0  the control stream, both directions
//   1  the input stream, both directions
//   2  one datagram, video and audio toward the browser
//
// A WebSocket is reliable and ordered, so nothing on it is ever lost: under
// congestion it queues instead, and the latency shows. To keep that
// bounded, datagrams toward the browser that find the send queue full are
// dropped, as on a QUIC leg. A close carries the GDP close code as 4000 +
// the code (4000-4999 is the range WebSocket leaves to applications).
use std::sync::Mutex;

use axum::extract::ws::{CloseFrame, Message, WebSocket};
use bytes::Bytes;
use futures_util::{SinkExt, StreamExt};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::{mpsc, watch};

use crate::gateway::{BoxRead, BoxWrite};

const CONTROL: u8 = 0;
const INPUT: u8 = 1;
const DATAGRAM: u8 = 2;
/// Datagrams waiting to go out before new ones are dropped: about a
/// frame's burst of full-size slices.
const DATAGRAM_QUEUE: usize = 64;
/// Bytes buffered per stream between the WebSocket and the relay.
const STREAM_BUFFER: usize = 256 * 1024;
const CLOSE_BASE: u16 = 4000;

enum Out {
    Data(u8, Bytes),
    Close(u32),
}

pub struct WsLeg {
    datagrams: tokio::sync::Mutex<mpsc::Receiver<Bytes>>,
    streams: tokio::sync::Mutex<mpsc::Receiver<(BoxWrite, BoxRead)>>,
    out: mpsc::Sender<Out>,
    out_datagrams: mpsc::Sender<Bytes>,
    closed: Mutex<watch::Receiver<Option<u32>>>,
}

impl WsLeg {
    pub fn new(socket: WebSocket) -> WsLeg {
        let (sink, stream) = socket.split();
        let (out_tx, out_rx) = mpsc::channel::<Out>(256);
        let (dgram_out_tx, dgram_out_rx) = mpsc::channel::<Bytes>(DATAGRAM_QUEUE);
        let (dgram_in_tx, dgram_in_rx) = mpsc::channel::<Bytes>(DATAGRAM_QUEUE);
        let (streams_tx, streams_rx) = mpsc::channel(2);
        let (closed_tx, closed_rx) = watch::channel(None);
        tokio::spawn(write_loop(sink, out_rx, dgram_out_rx, closed_tx.clone()));
        tokio::spawn(read_loop(stream, out_tx.clone(), dgram_in_tx, streams_tx, closed_tx));
        WsLeg {
            datagrams: tokio::sync::Mutex::new(dgram_in_rx),
            streams: tokio::sync::Mutex::new(streams_rx),
            out: out_tx,
            out_datagrams: dgram_out_tx,
            closed: Mutex::new(closed_rx),
        }
    }

    pub async fn read_datagram(&self) -> Option<Bytes> {
        self.datagrams.lock().await.recv().await
    }

    /// Queues a datagram for the browser, or drops it when the queue is
    /// full.
    pub fn send_datagram(&self, d: Bytes) {
        let _ = self.out_datagrams.try_send(d);
    }

    /// The next stream the browser started: the control stream first,
    /// then the input stream.
    pub async fn accept_stream(&self) -> Option<(BoxWrite, BoxRead)> {
        self.streams.lock().await.recv().await
    }

    /// The GDP close code, once the WebSocket is closed.
    pub async fn closed(&self) -> u32 {
        let mut rx = self.closed.lock().unwrap_or_else(|p| p.into_inner()).clone();
        loop {
            if let Some(code) = *rx.borrow() {
                return code;
            }
            if rx.changed().await.is_err() {
                return 0;
            }
        }
    }

    pub fn close(&self, code: u32) {
        let _ = self.out.try_send(Out::Close(code));
    }
}

async fn write_loop(
    mut sink: futures_util::stream::SplitSink<WebSocket, Message>,
    mut out: mpsc::Receiver<Out>,
    mut datagrams: mpsc::Receiver<Bytes>,
    closed: watch::Sender<Option<u32>>,
) {
    loop {
        // Stream bytes before datagrams: they can't be dropped.
        let message = tokio::select! {
            biased;
            o = out.recv() => match o {
                Some(Out::Data(channel, data)) => frame(channel, &data),
                Some(Out::Close(code)) => {
                    let close = CloseFrame { code: CLOSE_BASE + code.min(999) as u16, reason: "".into() };
                    let _ = sink.send(Message::Close(Some(close))).await;
                    let _ = closed.send(Some(code));
                    return;
                }
                None => return,
            },
            d = datagrams.recv() => match d {
                Some(d) => frame(DATAGRAM, &d),
                None => return,
            },
        };
        if sink.send(message).await.is_err() {
            let _ = closed.send(Some(0));
            return;
        }
    }
}

fn frame(channel: u8, data: &[u8]) -> Message {
    let mut v = Vec::with_capacity(data.len() + 1);
    v.push(channel);
    v.extend_from_slice(data);
    Message::Binary(v.into())
}

async fn read_loop(
    mut stream: futures_util::stream::SplitStream<WebSocket>,
    out: mpsc::Sender<Out>,
    datagrams: mpsc::Sender<Bytes>,
    streams: mpsc::Sender<(BoxWrite, BoxRead)>,
    closed: watch::Sender<Option<u32>>,
) {
    // The relay side of each stream channel, created on its first bytes.
    let mut writers: [Option<tokio::io::WriteHalf<tokio::io::DuplexStream>>; 2] = [None, None];
    let code = loop {
        let Some(Ok(message)) = stream.next().await else { break 0 };
        let data = match message {
            Message::Binary(data) => data,
            Message::Close(frame) => {
                break frame.map(|f| f.code.checked_sub(CLOSE_BASE).map(u32::from).unwrap_or(0)).unwrap_or(0);
            }
            _ => continue,
        };
        let Some((&channel, payload)) = data.split_first() else { continue };
        match channel {
            CONTROL | INPUT => {
                let slot = &mut writers[channel as usize];
                if slot.is_none() {
                    // A new stream: one end for the relay, the other
                    // pumped to and from the WebSocket here.
                    let (relay_end, our_end) = tokio::io::duplex(STREAM_BUFFER);
                    let (relay_read, relay_write) = tokio::io::split(relay_end);
                    let (our_read, our_write) = tokio::io::split(our_end);
                    tokio::spawn(pump(our_read, channel, out.clone()));
                    *slot = Some(our_write);
                    if streams.send((Box::new(relay_write), Box::new(relay_read))).await.is_err() {
                        break 0;
                    }
                }
                let writer = slot.as_mut().expect("just set");
                if writer.write_all(payload).await.is_err() {
                    break 0;
                }
            }
            DATAGRAM => {
                let _ = datagrams.try_send(Bytes::copy_from_slice(payload));
            }
            _ => {}
        }
    };
    let _ = closed.send(Some(code));
}

// The relay's bytes for one stream, out to the browser on its channel.
async fn pump(mut from: tokio::io::ReadHalf<tokio::io::DuplexStream>, channel: u8, out: mpsc::Sender<Out>) {
    let mut buf = vec![0u8; 64 * 1024];
    while let Ok(n) = from.read(&mut buf).await {
        if n == 0 || out.send(Out::Data(channel, Bytes::copy_from_slice(&buf[..n]))).await.is_err() {
            return;
        }
    }
}
