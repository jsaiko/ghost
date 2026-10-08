// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// A headless GDP client for testing Veil without spectre's window: one
// real login -- direct to a host,
// or brokered through Veil -- then the session itself, counting what
// arrives. Not installed; run from the build tree:
//
//   cargo run -p veil --example gdp_probe -- <server> <port> <user> <password> \
//       [--device <name>] [--type <session type>] [--seconds <n>] [--size <WxH>]
//   cargo run -p veil --example gdp_probe -- --direct <host> <port> <token> <cert sha256> \
//       [--seconds <n>] [--size <WxH>]
//
// --type picks the session type SessionOpen asks for (empty: the host's
// default, or the user's running session).
// --direct skips the login and opens the session on a direct-connect wraith
// (`wraith -l <port> -t <token>`). --size asks for that output size in
// SessionHello.displays; the probe prints the size SessionAccept reports.
// --change WxH sends a ResolutionChange halfway through the window and
// prints the DisplaysChanged that answers it.
//
// It trusts whatever certificate the login server presents (it prints the
// fingerprint) but pins the session's, as spectre does. Prompts after the
// first get $PROBE_OTP. The session is opened with h264 and nothing else,
// and the probe reports time to SessionAccept, the datagrams that arrive
// (count, rate, largest), and the close.
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::{bail, Context, Result};
use ipc::framing::{read_frame, write_frame};
use ipc::lobby::{lobby_envelope::Msg, AuthMethod, AuthResponse, DeviceSelect, LobbyEnvelope, LobbyHello, Redirect, SessionOpen};
use ipc::session::{
    control_envelope::Msg as ControlMsg, ControlEnvelope, DisplayDescriptor, ResolutionChange, SessionHello,
};
use quinn::crypto::rustls::QuicClientConfig;

#[derive(Debug)]
struct AnyCert;

impl rustls::client::danger::ServerCertVerifier for AnyCert {
    fn verify_server_cert(
        &self,
        end_entity: &rustls::pki_types::CertificateDer<'_>,
        _: &[rustls::pki_types::CertificateDer<'_>],
        _: &rustls::pki_types::ServerName<'_>,
        _: &[u8],
        _: rustls::pki_types::UnixTime,
    ) -> Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        println!("probe: login server certificate {}", gdpnet::sha256_hex(end_entity));
        Ok(rustls::client::danger::ServerCertVerified::assertion())
    }
    fn verify_tls12_signature(
        &self,
        _: &[u8],
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }
    fn verify_tls13_signature(
        &self,
        _: &[u8],
        _: &rustls::pki_types::CertificateDer<'_>,
        _: &rustls::DigitallySignedStruct,
    ) -> Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        Ok(rustls::client::danger::HandshakeSignatureValid::assertion())
    }
    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        rustls::crypto::ring::default_provider().signature_verification_algorithms.supported_schemes()
    }
}

fn endpoint(verifier: Arc<dyn rustls::client::danger::ServerCertVerifier>) -> Result<quinn::Endpoint> {
    let mut crypto = rustls::ClientConfig::builder().dangerous().with_custom_certificate_verifier(verifier).with_no_client_auth();
    crypto.alpn_protocols = vec![b"gdp/1".to_vec()];
    let mut client = quinn::ClientConfig::new(Arc::new(QuicClientConfig::try_from(crypto)?));
    let mut transport = quinn::TransportConfig::default();
    transport.keep_alive_interval(Some(Duration::from_secs(5)));
    client.transport_config(Arc::new(transport));
    let mut endpoint = gdpnet::bind_dual_stack(0, quinn::EndpointConfig::default(), None)?;
    endpoint.set_default_client_config(client);
    Ok(endpoint)
}

fn parse_size(v: &str) -> Result<(u32, u32)> {
    let (w, h) = v.split_once('x').context("a size is WxH")?;
    Ok((w.parse()?, h.parse()?))
}

async fn resolve(host: &str, port: u16) -> Result<std::net::SocketAddr> {
    tokio::net::lookup_host((host, port)).await?.next().with_context(|| format!("{host} doesn't resolve"))
}

// The login: password (then $PROBE_OTP for any later prompt), a device,
// a new session of the default type. Returns the Redirect.
async fn login(
    server: &str,
    port: u16,
    user: &str,
    password: &str,
    device_name: Option<String>,
    session_type: Option<String>,
) -> Result<Redirect> {
    let otp = std::env::var("PROBE_OTP").unwrap_or_default();
    let lobby_endpoint = endpoint(Arc::new(AnyCert))?;
    let conn = lobby_endpoint.connect(resolve(server, port).await?, server)?.await?;
    let (mut send, mut recv) = conn.open_bi().await?;
    let hello = LobbyHello {
        protocol_version: 1,
        client_id: "gdp_probe".into(),
        auth_method: AuthMethod::Password as i32,
        username: user.to_string(),
    };
    write_frame(&mut send, &LobbyEnvelope { msg: Some(Msg::Hello(hello)) }).await?;
    let mut prompts = 0;
    let redirect = loop {
        let env: LobbyEnvelope = read_frame(&mut recv).await.context("the login server closed the stream")?;
        match env.msg {
            Some(Msg::AuthChallenge(c)) => {
                println!("probe: prompt {:?} (echo {})", c.prompt, c.echo_input);
                let response = if prompts == 0 { password.to_string() } else { otp.clone() };
                prompts += 1;
                write_frame(&mut send, &LobbyEnvelope { msg: Some(Msg::AuthResponse(AuthResponse { response })) }).await?;
            }
            Some(Msg::DeviceList(list)) => {
                for d in &list.devices {
                    println!("probe: device {} {:?} online={} session={}", d.id, d.name, d.online, d.has_session);
                }
                let pick = list
                    .devices
                    .iter()
                    .find(|d| device_name.as_ref().map_or(d.online, |n| &d.name == n))
                    .context("no device to pick")?;
                let select = DeviceSelect { device_id: pick.id.clone() };
                write_frame(&mut send, &LobbyEnvelope { msg: Some(Msg::DeviceSelect(select)) }).await?;
            }
            Some(Msg::SessionList(list)) => {
                println!("probe: {} session types, {} running", list.available_types.len(), list.sessions.len());
                let open = SessionOpen { session_type: session_type.clone().unwrap_or_default() };
                write_frame(&mut send, &LobbyEnvelope { msg: Some(Msg::SessionOpen(open)) }).await?;
            }
            Some(Msg::Redirect(r)) => break r,
            Some(Msg::Error(e)) => bail!("login refused: {} ({})", e.message, e.code),
            other => bail!("unexpected {other:?}"),
        }
    };
    conn.close(0u32.into(), b"");
    Ok(redirect)

}

#[tokio::main]
async fn main() -> Result<()> {
    rustls::crypto::ring::default_provider().install_default().ok();
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    // A flag with no value, so it comes out before the pairs below.
    let take_over = args.iter().any(|a| a == "--take-over");
    args.retain(|a| a != "--take-over");
    // Scrolls the pointer's window up this many notches (--scroll N), after
    // putting the pointer in the middle of the output.
    let mut scroll_notches = 0u32;
    if let Some(i) = args.iter().position(|a| a == "--scroll") {
        scroll_notches = args.get(i + 1).context("--scroll needs a count")?.parse()?;
        args.drain(i..=i + 1);
    }
    // Opens a third stream after SessionAccept to see it refused
    // (gdp-spec.md §2.2).
    let extra_stream = args.iter().any(|a| a == "--extra-stream");
    args.retain(|a| a != "--extra-stream");
    let direct = args.first().is_some_and(|a| a == "--direct");
    let positional = if direct { 5 } else { 4 };
    if args.len() < positional {
        bail!(
            "usage: gdp_probe <server> <port> <user> <password> [--device <name>] [--type <id>] [--seconds <n>] [--size <WxH>]\n       \
             gdp_probe --direct <host> <port> <token> <cert sha256> [--seconds <n>] [--size <WxH>] [--change <WxH>] [--take-over] [--extra-stream] [--scroll <notches>]"
        );
    }
    let mut device_name = None;
    let mut session_type = None;
    let mut seconds = 5u64;
    let mut size = None;
    let mut change = None;
    for pair in args[positional..].chunks(2) {
        match (pair[0].as_str(), pair.get(1)) {
            ("--device", Some(v)) if !direct => device_name = Some(v.clone()),
            ("--type", Some(v)) if !direct => session_type = Some(v.clone()),
            ("--seconds", Some(v)) => seconds = v.parse()?,
            ("--size", Some(v)) => size = Some(parse_size(v)?),
            ("--change", Some(v)) => change = Some(parse_size(v)?),
            _ => bail!("unknown argument {}", pair[0]),
        }
    }
    let (host, session_port, token, cert_sha256) = if direct {
        (args[1].clone(), args[2].parse::<u16>()?, args[3].clone(), args[4].replace(':', "").to_lowercase())
    } else {
        let (server, port, user, password) = (&args[0], args[1].parse::<u16>()?, &args[2], &args[3]);
        let r = login(server, port, user, password, device_name, session_type).await?;
        let host = if r.host.is_empty() { server.clone() } else { r.host.clone() };
        println!("probe: redirect to {host}:{} (cert {})", r.port, &r.cert_sha256[..16]);
        (host, r.port as u16, r.token, r.cert_sha256)
    };

    // The session.
    let session_endpoint = endpoint(gdpnet::PinnedServer::new(&cert_sha256))?;
    let started = Instant::now();
    let session = session_endpoint.connect(resolve(&host, session_port).await?, &host)?.await?;
    let (mut control_send, mut control_recv) = session.open_bi().await?;
    let hello = SessionHello {
        token,
        displays: size.map_or_else(Vec::new, |(width, height)| vec![DisplayDescriptor { width, height, ..Default::default() }]),
        codecs: vec!["h264".into()],
        decoders: vec!["software".into()],
        take_over,
        ..Default::default()
    };
    write_frame(&mut control_send, &ControlEnvelope { msg: Some(ControlMsg::Hello(hello)) }).await?;
    let (mut input_send, _input_recv) = session.open_bi().await?;

    let datagrams = Arc::new(AtomicU64::new(0));
    let bytes = Arc::new(AtomicU64::new(0));
    let largest = Arc::new(AtomicU64::new(0));
    let counter = {
        let (session, datagrams, bytes, largest) = (session.clone(), datagrams.clone(), bytes.clone(), largest.clone());
        tokio::spawn(async move {
            while let Ok(d) = session.read_datagram().await {
                datagrams.fetch_add(1, Ordering::Relaxed);
                bytes.fetch_add(d.len() as u64, Ordering::Relaxed);
                largest.fetch_max(d.len() as u64, Ordering::Relaxed);
            }
        })
    };
    // SessionAccept, skipping whatever else comes first (a cursor shape).
    let accept = tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            match read_frame::<ControlEnvelope>(&mut control_recv).await {
                Ok(ControlEnvelope { msg: Some(ControlMsg::Accept(a)) }) => return Ok(a),
                Ok(_) => continue,
                Err(e) => return Err(e),
            }
        }
    })
    .await;
    match accept {
        Ok(Ok(a)) => {
            let output = a.outputs.first().and_then(|o| o.display.as_ref()).map(|d| (d.width, d.height));
            println!("probe: colour {:?}", a.outputs.first().and_then(|o| o.color.as_ref()));
            println!(
                "probe: SessionAccept after {} ms: codec {} encoder {} host {}@{} via_gateway {} output {}",
                started.elapsed().as_millis(),
                a.codec,
                a.encoder,
                a.host_user,
                a.host_name,
                a.via_gateway,
                output.map_or("none".into(), |(w, h)| format!("{w}x{h}"))
            )
        }
        Ok(Err(e)) => bail!("control stream ended before SessionAccept: {e} (close: {:?})", session.close_reason()),
        Err(_) => bail!("no SessionAccept within 10 s"),
    }
    if extra_stream {
        // libgdp grants exactly the two streams §2.2 names, so this normally
        // blocks for want of stream credit and never gets as far as a reset.
        let Ok(opened) = tokio::time::timeout(Duration::from_secs(3), session.open_bi()).await else {
            println!("probe: extra stream: not opened, the host grants no further streams");
            return Ok(());
        };
        let (mut send, mut recv) = opened?;
        send.write_all(b"\x00\x00\x00\x02\x0a\x00").await?;
        let mut buf = [0u8; 16];
        match tokio::time::timeout(Duration::from_secs(3), recv.read(&mut buf)).await {
            Ok(Err(e)) => println!("probe: extra stream refused: {e:?}"),
            Ok(Ok(n)) => println!("probe: extra stream answered with {n:?} bytes"),
            Err(_) => println!("probe: extra stream: no answer within 3 s"),
        }
        println!("probe: connection still open after it: {}", session.close_reason().is_none());
    }
    if scroll_notches > 0 {
        use ipc::session::{input_envelope::Event, InputEnvelope, PointerAxis, PointerMotion};
        tokio::time::sleep(Duration::from_secs(1)).await;
        let motion = PointerMotion { dx: 0.5, dy: 0.5, absolute: true };
        write_frame(&mut input_send, &InputEnvelope { client_time_us: 0, event: Some(Event::PointerMotion(motion)) }).await?;
        tokio::time::sleep(Duration::from_millis(300)).await;
        for _ in 0..scroll_notches {
            let axis = PointerAxis { horizontal: 0.0, vertical: -1.0 };
            write_frame(&mut input_send, &InputEnvelope { client_time_us: 0, event: Some(Event::PointerAxis(axis)) }).await?;
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
        println!("probe: sent {scroll_notches} scroll notches up");
    }
    // Everything wraith sends from here on: report DisplaysChanged.
    let control_task = tokio::spawn(async move {
        while let Ok(env) = read_frame::<ControlEnvelope>(&mut control_recv).await {
            if let Some(ControlMsg::DisplaysChanged(d)) = env.msg {
                let sizes: Vec<String> = d
                    .outputs
                    .iter()
                    .filter_map(|o| o.display.as_ref().map(|d| format!("{}x{}", d.width, d.height)))
                    .collect();
                println!("probe: DisplaysChanged {}", sizes.join(", "));
            }
        }
    });
    let window = Instant::now();
    if let Some((width, height)) = change {
        tokio::time::sleep(Duration::from_millis(seconds * 500)).await;
        let display = DisplayDescriptor { width, height, ..Default::default() };
        let msg = ResolutionChange { stream_id: 0, display: Some(display) };
        write_frame(&mut control_send, &ControlEnvelope { msg: Some(ControlMsg::ResolutionChange(msg)) }).await?;
        println!("probe: sent ResolutionChange {width}x{height}");
        tokio::time::sleep(Duration::from_millis(seconds * 500)).await;
    } else {
        tokio::time::sleep(Duration::from_secs(seconds)).await;
    }
    let secs = window.elapsed().as_secs_f64();
    let (n, b) = (datagrams.load(Ordering::Relaxed), bytes.load(Ordering::Relaxed));
    println!(
        "probe: {n} datagrams, {:.1} kB, {:.1}/s, {:.2} Mbit/s, largest {} bytes (path max datagram {:?})",
        b as f64 / 1000.0,
        n as f64 / secs,
        b as f64 * 8.0 / secs / 1e6,
        largest.load(Ordering::Relaxed),
        session.max_datagram_size()
    );
    // Why the connection ended, if the host ended it (an application code
    // is gdp-spec.md §12's, e.g. 42 for TAKEN_OVER).
    println!("probe: connection close reason before our own close: {:?}", session.close_reason());
    session.close(0u32.into(), b"");
    counter.abort();
    control_task.abort();
    session_endpoint.wait_idle().await;
    println!("probe: closed after {:.1} s", started.elapsed().as_secs_f64());
    Ok(())
}
