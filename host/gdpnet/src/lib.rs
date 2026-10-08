// SPDX-FileCopyrightText: 2026 Joseph Saiko <https://saiko.dev>
// SPDX-License-Identifier: GPL-3.0-only

// Certificate identities, pins and QUIC sockets, shared by ghostd and veild
// (docs/design/trust.md, gdp-spec.md §2.3).
//
// The peers here check certificates by fingerprint, never against a CA:
// PinnedServer is the rustls client side (ghostd dialing Veil, veild
// dialing a wraith), and AnyClientCert lets veild take the certificate a
// joined host presents and check it against that host's pin once the
// handshake is done. Both still verify the handshake signature, so the
// peer has to hold the key for the certificate it shows.
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};
use std::path::Path;
use std::sync::Arc;

use anyhow::{bail, Context, Result};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{verify_tls12_signature, verify_tls13_signature, CryptoProvider};
use rustls::pki_types::pem::PemObject;
use rustls::pki_types::{CertificateDer, PrivateKeyDer, ServerName, UnixTime};
use rustls::server::danger::{ClientCertVerified, ClientCertVerifier};
use rustls::{CertificateError, DigitallySignedStruct, DistinguishedName, SignatureScheme};
use sha2::{Digest, Sha256};

pub struct HostIdentity {
    pub chain: Vec<CertificateDer<'static>>,
    pub key: PrivateKeyDer<'static>,
    /// Lowercase hex SHA-256 of the leaf certificate's DER -- what spectre
    /// shows at first use and records in its known_hosts.
    pub fingerprint: String,
}

pub fn load_host_identity(cert_path: &Path, key_path: &Path) -> Result<HostIdentity> {
    let chain = CertificateDer::pem_file_iter(cert_path)
        .with_context(|| format!("opening {}", cert_path.display()))?
        .collect::<Result<Vec<_>, _>>()
        .with_context(|| format!("parsing {}", cert_path.display()))?;
    let Some(leaf) = chain.first() else {
        bail!("{} contains no certificate", cert_path.display());
    };
    let fingerprint = sha256_hex(leaf);
    let key = PrivateKeyDer::from_pem_file(key_path).with_context(|| format!("loading {}", key_path.display()))?;
    Ok(HostIdentity { chain, key, fingerprint })
}

impl HostIdentity {
    /// The certificate and key, for a rustls config builder.
    pub fn into_parts(self) -> (Vec<CertificateDer<'static>>, PrivateKeyDer<'static>) {
        (self.chain, self.key)
    }
}

pub fn sha256_hex(data: &[u8]) -> String {
    Sha256::digest(data).iter().map(|b| format!("{b:02x}")).collect()
}

/// Whether `s` is a well-formed sha256_hex() value (libgdp's
/// gdp::is_sha256_hex, the same rule).
pub fn is_sha256_hex(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

/// What bind_dual_stack asks for in each direction (libgdp's
/// kSocketBufferSize).
const SOCKET_BUFFER: usize = 8 << 20;

/// Binds a UDP socket on [::] serving both families, and hands it to
/// quinn: a server endpoint when `server_config` is given, a client-only
/// one otherwise (port 0 picks any). The socket is built by hand rather
/// than through Endpoint::server() because IPV6_V6ONLY has to be cleared
/// explicitly: binding [::] otherwise inherits net.ipv6.bindv6only, and
/// with that set to 1 every IPv4 peer would fail to connect. libgdp's
/// sockets, which wraith listens on, are dual-stack the same way.
pub fn bind_dual_stack(
    port: u16,
    endpoint_config: quinn::EndpointConfig,
    server_config: Option<quinn::ServerConfig>,
) -> Result<quinn::Endpoint> {
    bind(None, port, endpoint_config, server_config)
}

/// `bind_dual_stack`, on one address when `address` is given: an IPv4
/// address serves IPv4 only, an IPv6 one IPv6 only (a host with a
/// management interface keeps the lobby off it this way). None is the
/// dual-stack [::], or 0.0.0.0 on a host without IPv6 (ipv6.disable=1),
/// as libgdp's bind_any falls back.
pub fn bind(
    address: Option<IpAddr>,
    port: u16,
    endpoint_config: quinn::EndpointConfig,
    server_config: Option<quinn::ServerConfig>,
) -> Result<quinn::Endpoint> {
    use socket2::{Domain, Protocol, Socket, Type};
    let (addr, socket) = match address {
        Some(ip) => {
            let domain = if ip.is_ipv4() { Domain::IPV4 } else { Domain::IPV6 };
            let socket = Socket::new(domain, Type::DGRAM, Some(Protocol::UDP)).context("creating a QUIC UDP socket")?;
            (SocketAddr::new(ip, port), socket)
        }
        None => match Socket::new(Domain::IPV6, Type::DGRAM, Some(Protocol::UDP)) {
            Ok(socket) => {
                socket.set_only_v6(false).context("clearing IPV6_V6ONLY on a QUIC socket")?;
                (SocketAddr::new(IpAddr::V6(Ipv6Addr::UNSPECIFIED), port), socket)
            }
            Err(_) => {
                let socket = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP))
                    .context("creating a QUIC UDP socket")?;
                (SocketAddr::new(IpAddr::V4(Ipv4Addr::UNSPECIFIED), port), socket)
            }
        },
    };
    // Room for a keyframe's burst, as libgdp's sockets ask for: video
    // arrives in bursts at up to the path's rate, faster than the
    // endpoint's task may get to it, and the kernel's ~208 KiB default
    // overflows in a couple of milliseconds at 800 Mbit/s. Linux quietly
    // caps the request at net.core.rmem_max / wmem_max.
    for size in [SOCKET_BUFFER, SOCKET_BUFFER / 4] {
        if socket.set_recv_buffer_size(size).is_ok() {
            break;
        }
    }
    for size in [SOCKET_BUFFER, SOCKET_BUFFER / 4] {
        if socket.set_send_buffer_size(size).is_ok() {
            break;
        }
    }
    socket.bind(&addr.into()).with_context(|| format!("binding a QUIC socket to {addr}"))?;

    let runtime = quinn::default_runtime().context("no async runtime for quinn's endpoint")?;
    quinn::Endpoint::new(endpoint_config, server_config, socket.into(), runtime).context("building a QUIC endpoint")
}

/// A peer's address with an IPv4-mapped IPv6 address unwrapped
/// (::ffff:a.b.c.d -> a.b.c.d): the sockets above are dual-stack, and
/// logs, penalties and PAM_RHOST should all see the plain v4 form.
/// to_canonical(), not to_ipv4(), which would also unwrap the deprecated
/// v4-compatible ::a.b.c.d range.
pub fn canonical(addr: SocketAddr) -> SocketAddr {
    SocketAddr::new(addr.ip().to_canonical(), addr.port())
}

fn provider() -> Arc<CryptoProvider> {
    CryptoProvider::get_default().cloned().unwrap_or_else(|| Arc::new(rustls::crypto::ring::default_provider()))
}

/// Accepts exactly one server certificate, by its sha256_hex()
/// fingerprint, whatever name it was dialed by.
#[derive(Debug)]
pub struct PinnedServer {
    pin: String,
    provider: Arc<CryptoProvider>,
}

impl PinnedServer {
    pub fn new(pin: &str) -> Arc<PinnedServer> {
        Arc::new(PinnedServer { pin: pin.to_ascii_lowercase(), provider: provider() })
    }
}

impl ServerCertVerifier for PinnedServer {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        if sha256_hex(end_entity) == self.pin {
            Ok(ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::InvalidCertificate(CertificateError::ApplicationVerificationFailure))
        }
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(message, cert, dss, &self.provider.signature_verification_algorithms)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, cert, dss, &self.provider.signature_verification_algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider.signature_verification_algorithms.supported_schemes()
    }
}

/// Asks every client for a certificate and accepts any, or none: the
/// server checks it against a pin itself once it knows who the client
/// claims to be (veild's host channel). spectre, which has no
/// certificate, answers the request with an empty one, as TLS 1.3 allows.
#[derive(Debug)]
pub struct AnyClientCert {
    provider: Arc<CryptoProvider>,
}

impl AnyClientCert {
    pub fn new() -> Arc<AnyClientCert> {
        Arc::new(AnyClientCert { provider: provider() })
    }
}

impl ClientCertVerifier for AnyClientCert {
    fn offer_client_auth(&self) -> bool {
        true
    }

    fn client_auth_mandatory(&self) -> bool {
        false
    }

    fn root_hint_subjects(&self) -> &[DistinguishedName] {
        &[]
    }

    fn verify_client_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _now: UnixTime,
    ) -> Result<ClientCertVerified, rustls::Error> {
        Ok(ClientCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(message, cert, dss, &self.provider.signature_verification_algorithms)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, cert, dss, &self.provider.signature_verification_algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider.signature_verification_algorithms.supported_schemes()
    }
}

/// The fingerprint of the leaf certificate a QUIC peer presented, if any.
pub fn peer_fingerprint(identity: Option<Box<dyn std::any::Any>>) -> Option<String> {
    let certs = identity?.downcast::<Vec<CertificateDer<'static>>>().ok()?;
    certs.first().map(|leaf| sha256_hex(leaf))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha256_hex_matches_the_fips_vector() {
        assert_eq!(sha256_hex(b"abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert!(is_sha256_hex(&sha256_hex(b"")));
    }

    #[test]
    fn is_sha256_hex_rejects_other_forms() {
        let upper = "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD";
        assert!(!is_sha256_hex(upper));
        assert!(!is_sha256_hex(&upper.to_lowercase()[1..]));
        assert!(!is_sha256_hex(""));
    }
}
