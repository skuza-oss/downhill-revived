//! The byte stream under both clients: TCP, wrapped in rustls for the secure schemes, and the
//! WebSocket handshake over it.

use std::io::{self, Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::sync::{Arc, OnceLock};
use std::time::Duration;

use rustls::pki_types::ServerName;
use rustls::{ClientConfig, ClientConnection, RootCertStore, StreamOwned};
use tungstenite::client::IntoClientRequest;
use tungstenite::handshake::HandshakeError;
use tungstenite::http::header::{HeaderValue, USER_AGENT};
use tungstenite::http::Uri;
use tungstenite::WebSocket;

/// Name and connect timeouts, and each read and write while connecting (WinHTTP's were 5 s).
pub const TIMEOUT: Duration = Duration::from_secs(5);
pub const AGENT: &str = "dhd-port";

pub enum Stream {
    Plain(TcpStream),
    Tls(Box<StreamOwned<ClientConnection, TcpStream>>),
}

impl Read for Stream {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            Stream::Plain(s) => s.read(buf),
            Stream::Tls(s) => s.read(buf),
        }
    }
}

impl Write for Stream {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            Stream::Plain(s) => s.write(buf),
            Stream::Tls(s) => s.write(buf),
        }
    }
    fn flush(&mut self) -> io::Result<()> {
        match self {
            Stream::Plain(s) => s.flush(),
            Stream::Tls(s) => s.flush(),
        }
    }
}

/// The one TLS configuration: ring's provider, its safe protocol versions, Mozilla's roots.
pub(crate) fn tls_config() -> Result<Arc<ClientConfig>, String> {
    static CONFIG: OnceLock<Result<Arc<ClientConfig>, String>> = OnceLock::new();
    CONFIG
        .get_or_init(|| {
            let roots = RootCertStore { roots: webpki_roots::TLS_SERVER_ROOTS.to_vec() };
            ClientConfig::builder_with_provider(Arc::new(rustls::crypto::ring::default_provider()))
                .with_safe_default_protocol_versions()
                .map(|b| Arc::new(b.with_root_certificates(roots).with_no_client_auth()))
                .map_err(|e| format!("TLS setup failed ({e})"))
        })
        .clone()
}

/// The TCP connection to host:port, each of its addresses in turn.
pub(crate) fn dial(host: &str, port: u16) -> io::Result<TcpStream> {
    let mut last = io::Error::new(io::ErrorKind::NotFound, "no address");
    for addr in (host, port).to_socket_addrs()? {
        match TcpStream::connect_timeout(&addr, TIMEOUT) {
            Ok(s) => return Ok(s),
            Err(e) => last = e,
        }
    }
    Err(last)
}

/// Opens the stream to uri's host: secure when `secure`. Also returns a second handle of the socket.
pub fn open(uri: &Uri, secure: bool) -> Result<(Stream, TcpStream), String> {
    let host = uri.host().ok_or("no host in the URL")?.trim_start_matches('[').trim_end_matches(']');
    let port = uri.port_u16().unwrap_or(if secure { 443 } else { 80 });
    let tcp = dial(host, port).map_err(|e| format!("can't connect to {host} ({e})"))?;
    let _ = tcp.set_nodelay(true);
    let _ = tcp.set_read_timeout(Some(TIMEOUT));
    let _ = tcp.set_write_timeout(Some(TIMEOUT));
    let handle = tcp.try_clone().map_err(|e| format!("socket error ({e})"))?;
    if !secure {
        return Ok((Stream::Plain(tcp), handle));
    }
    let name = ServerName::try_from(host.to_owned()).map_err(|_| format!("bad host name {host}"))?;
    let conn = ClientConnection::new(tls_config()?, name).map_err(|e| format!("TLS error ({e})"))?;
    Ok((Stream::Tls(Box::new(StreamOwned::new(conn, tcp))), handle))
}

/// The WebSocket at url (ws:// or wss://), its socket's handle and a third for peeking. On failure,
/// why (the close reason ws.h passes for code -1).
pub fn connect_ws(url: &str) -> Result<(WebSocket<Stream>, TcpStream, TcpStream), String> {
    let mut request = url.into_client_request().map_err(|_| format!("bad URL {url}"))?;
    request.headers_mut().insert(USER_AGENT, HeaderValue::from_static(AGENT));
    let secure = match request.uri().scheme_str() {
        Some("wss") => true,
        Some("ws") => false,
        _ => return Err(format!("bad URL {url}")),
    };
    let (stream, tcp) = open(request.uri(), secure)?;
    let peek = tcp.try_clone().map_err(|e| format!("socket error ({e})"))?;
    match tungstenite::client(request, stream) {
        Ok((ws, _)) => {
            // from here the receive thread peeks with a short wait; writes keep the 5 s limit
            let _ = tcp.set_read_timeout(Some(crate::PEEK));
            Ok((ws, tcp, peek))
        }
        Err(HandshakeError::Failure(tungstenite::Error::Http(r))) => Err(format!("HTTP {} from {url}", r.status().as_u16())),
        Err(HandshakeError::Failure(e)) => Err(format!("can't connect to {url} ({e})")),
        Err(HandshakeError::Interrupted(_)) => Err(format!("can't connect to {url} (timed out)")),
    }
}
