//! The TURN relay's stream: TLS over TCP to Cloudflare's TURN service on port 443, for players
//! whose network blocks UDP (finding 158, round 4). A thread connects and then reads; the SFU's
//! owner thread writes. One rustls connection sits behind a lock that only the TLS bookkeeping
//! holds: the reader waits for bytes on its own handle of the socket without it.
use crate::sfu::Wake;
use crate::trouble::{Kind, Step};
use crate::stream::{tls_config, TIMEOUT};
use rustls::pki_types::ServerName;
use rustls::ClientConnection;
use std::collections::VecDeque;
use std::io::{self, ErrorKind, Read, Write};
use std::net::{SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// How often the reader looks whether its link was dropped.
const READER_CHECK: Duration = Duration::from_millis(100);
/// A write that blocks this long (a stalled TCP connection) fails the link.
const WRITE_TIMEOUT: Duration = Duration::from_secs(2);
/// At most this many bytes read and not yet taken; more fails the link (the owner stalled).
const INBOX_MAX: usize = 1 << 20;

/// When the stream's bytes came, for the 30 s line: the longest wait between two reads that
/// brought data (a TCP stall shows here, before our code sees anything), and when the oldest
/// byte not yet taken arrived.
#[derive(Default)]
struct Timing {
    last_read: Option<Instant>,
    read_gap_max: Duration,
    oldest_unread: Option<Instant>,
}

struct Shared {
    tls: Mutex<Option<(ClientConnection, TcpStream)>>,
    inbox: Mutex<VecDeque<u8>>,
    timing: Mutex<Timing>,
    local: Mutex<Option<SocketAddr>>,
    alive: AtomicBool,
    failed: AtomicBool,
    /// The step that failed and the kind of error (finding 248); a failed write is `Link`.
    failure: Mutex<Option<(Step, Kind)>>,
    /// Tests: the stream is this buffer (writes land here), no TCP or TLS.
    #[cfg(test)]
    memory: Option<Mutex<Vec<u8>>>,
}

pub struct Link {
    shared: Arc<Shared>,
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> { m.lock().unwrap_or_else(|e| e.into_inner()) }

impl Link {
    /// Connects to host:port in a thread; `ready()` says when writes can go.
    pub fn open(host: &str, port: u16, wake: Wake) -> Self {
        let shared = Arc::new(Shared {
            tls: Mutex::new(None), inbox: Mutex::new(VecDeque::new()), timing: Mutex::default(), local: Mutex::new(None),
            alive: AtomicBool::new(true), failed: AtomicBool::new(false), failure: Mutex::new(None),
            #[cfg(test)]
            memory: None,
        });
        let (s, host) = (shared.clone(), host.to_owned());
        let spawned = std::thread::Builder::new().name("dhd-turn".into()).spawn(move || {
            let failure = match run(&s, &host, port, &wake) {
                Ok(()) => (Step::Link, Kind::Closed),
                Err((step, kind, detail)) => {
                    // Only the TLS library's own words (a certificate or protocol problem), never
                    // an address; the step and kind go in the relay's failure line (sfu.rs).
                    if kind == Kind::Tls && s.alive.load(Ordering::Acquire) { eprintln!("net: SFU: TURN relay's TLS: {detail}"); }
                    (step, kind)
                }
            };
            lock(&s.failure).get_or_insert(failure);
            s.failed.store(true, Ordering::Release);
            wake();
        });
        if spawned.is_err() {
            lock(&shared.failure).get_or_insert((Step::TlsConnect, Kind::Other));
            shared.failed.store(true, Ordering::Release);
        }
        Self { shared }
    }
    /// Tests: a link that is up at once, its stream an in-memory buffer (`written`, `deliver`).
    #[cfg(test)]
    pub fn memory() -> Self {
        Self { shared: Arc::new(Shared {
            tls: Mutex::new(None), inbox: Mutex::new(VecDeque::new()), timing: Mutex::default(), local: Mutex::new(None),
            alive: AtomicBool::new(true), failed: AtomicBool::new(false), failure: Mutex::new(None), memory: Some(Mutex::new(vec![])),
        }) }
    }
    /// Tests: what the client wrote since the last call.
    #[cfg(test)]
    pub fn written(&self) -> Vec<u8> { self.shared.memory.as_ref().map_or(vec![], |m| std::mem::take(&mut *lock(m))) }
    /// Tests: bytes "read from the stream".
    #[cfg(test)]
    pub fn deliver(&self, bytes: &[u8]) { lock(&self.shared.inbox).extend(bytes.iter().copied()); }
    pub fn ready(&self) -> bool {
        #[cfg(test)]
        if self.shared.memory.is_some() { return !self.failed(); }
        lock(&self.shared.tls).is_some()
    }
    pub fn failed(&self) -> bool { self.shared.failed.load(Ordering::Acquire) }
    /// Which step failed and how, once `failed()`.
    pub fn failure(&self) -> Option<(Step, Kind)> { *lock(&self.shared.failure) }
    /// The local end of the TCP connection (the relayed candidate's base).
    pub fn local(&self) -> Option<SocketAddr> { *lock(&self.shared.local) }
    /// What was read since the last call, and when its oldest byte arrived.
    pub fn take(&self) -> (Vec<u8>, Option<Instant>) {
        let bytes: Vec<u8> = lock(&self.shared.inbox).drain(..).collect();
        (bytes, lock(&self.shared.timing).oldest_unread.take())
    }
    /// The longest wait between two reads with data since the last call.
    pub fn take_read_gap(&self) -> Duration { std::mem::take(&mut lock(&self.shared.timing).read_gap_max) }
    /// Writes bytes to the stream (after `ready()`), blocking at most WRITE_TIMEOUT.
    pub fn write(&self, bytes: &[u8]) {
        if bytes.is_empty() { return; }
        #[cfg(test)]
        if let Some(m) = &self.shared.memory { lock(m).extend_from_slice(bytes); return; }
        let mut guard = lock(&self.shared.tls);
        let Some((conn, tcp)) = guard.as_mut() else { return };
        let ok = conn.writer().write_all(bytes).is_ok() && flush_tls(conn, tcp);
        if !ok {
            lock(&self.shared.failure).get_or_insert((Step::Link, Kind::Reset));
            self.shared.failed.store(true, Ordering::Release);
        }
    }
}

impl Drop for Link {
    fn drop(&mut self) {
        self.shared.alive.store(false, Ordering::Release);
        if let Some((_, tcp)) = lock(&self.shared.tls).as_ref() { let _ = tcp.shutdown(std::net::Shutdown::Both); }
    }
}

/// The TURN server over IPv4 first: the relay is an IPv4 allocation and the SFU's candidates are
/// IPv4 only (sfu.rs keeps IPv4 candidates), as the SFU's own address lookup, and the relayed
/// candidate's base (this socket's address) should be IPv4 too; IPv6 only when no IPv4 address
/// answers. A phone on a dual-stack network could otherwise take the IPv6 address first (246).
fn dial_ipv4_first(host: &str, port: u16) -> io::Result<TcpStream> {
    // A failed lookup counts as NotFound (the failure's `dns`), whatever the resolver said.
    let mut addrs: Vec<SocketAddr> = (host, port).to_socket_addrs().map_err(|e| io::Error::new(ErrorKind::NotFound, e))?.collect();
    addrs.sort_by_key(|a| !a.is_ipv4());
    let mut last = io::Error::new(ErrorKind::NotFound, "no address");
    for addr in addrs {
        match TcpStream::connect_timeout(&addr, TIMEOUT) {
            Ok(s) => return Ok(s),
            Err(e) => last = e,
        }
    }
    Err(last)
}

fn flush_tls(conn: &mut ClientConnection, tcp: &mut TcpStream) -> bool {
    while conn.wants_write() {
        if conn.write_tls(tcp).is_err() { return false; }
    }
    true
}

/// A failed link: its step, the kind of error, and (for the TLS library's errors) its words.
type Failed = (Step, Kind, String);

/// The kind of a failed read or write on the TLS stream: rustls hands its own errors back as
/// InvalidData.
fn stream_kind(e: &io::Error) -> Kind {
    if e.kind() == ErrorKind::InvalidData { Kind::Tls } else { Kind::of_io(e.kind()) }
}

fn run(s: &Shared, host: &str, port: u16, wake: &Wake) -> Result<(), Failed> {
    let io = |step: Step| move |e: io::Error| (step, Kind::of_io(e.kind()), String::new());
    let mut tcp = dial_ipv4_first(host, port).map_err(io(Step::TlsConnect))?;
    let _ = tcp.set_nodelay(true);
    tcp.set_read_timeout(Some(crate::stream::TIMEOUT)).map_err(io(Step::TlsConnect))?;
    tcp.set_write_timeout(Some(WRITE_TIMEOUT)).map_err(io(Step::TlsConnect))?;
    let name = ServerName::try_from(host.to_owned()).map_err(|_| (Step::TlsConnect, Kind::Dns, String::new()))?;
    let config = tls_config().map_err(|e| (Step::TlsHandshake, Kind::Tls, e.to_string()))?;
    let mut conn = ClientConnection::new(config, name).map_err(|e| (Step::TlsHandshake, Kind::Tls, e.to_string()))?;
    while conn.is_handshaking() {
        conn.complete_io(&mut tcp).map_err(|e| (Step::TlsHandshake, stream_kind(&e), e.to_string()))?;
    }
    let family = if tcp.peer_addr().is_ok_and(|a| a.is_ipv4()) { "IPv4" } else { "IPv6" };
    eprintln!("net: SFU: TURN relay's TLS link up over {family} (the relay asked for: UDP over IPv4)");
    *lock(&s.local) = tcp.local_addr().ok();
    let mut reader = tcp.try_clone().map_err(io(Step::Link))?;
    reader.set_read_timeout(Some(READER_CHECK)).map_err(io(Step::Link))?;
    *lock(&s.tls) = Some((conn, tcp));
    wake();
    let mut buffer = vec![0u8; 16384];
    while s.alive.load(Ordering::Acquire) {
        let n = match reader.read(&mut buffer) {
            Ok(0) => return Err((Step::Link, Kind::Closed, String::new())),
            Ok(n) => {
                let now = Instant::now();
                let mut t = lock(&s.timing);
                if let Some(last) = t.last_read { t.read_gap_max = t.read_gap_max.max(now - last); }
                t.last_read = Some(now);
                n
            }
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted) => continue,
            Err(e) => return Err((Step::Link, Kind::of_io(e.kind()), String::new())),
        };
        let mut plain = vec![];
        {
            let mut guard = lock(&s.tls);
            let Some((conn, tcp)) = guard.as_mut() else { return Ok(()) };
            let mut input = &buffer[..n];
            while !input.is_empty() {
                conn.read_tls(&mut input).map_err(|e| (Step::Link, stream_kind(&e), e.to_string()))?;
                let state = conn.process_new_packets().map_err(|e| (Step::Link, Kind::Tls, e.to_string()))?;
                if state.plaintext_bytes_to_read() > 0 {
                    let mut chunk = vec![0u8; state.plaintext_bytes_to_read()];
                    conn.reader().read_exact(&mut chunk).map_err(|e| (Step::Link, stream_kind(&e), e.to_string()))?;
                    plain.extend_from_slice(&chunk);
                }
                if state.peer_has_closed() { return Err((Step::Link, Kind::Closed, String::new())); }
            }
            if !flush_tls(conn, tcp) { return Err((Step::Link, Kind::Reset, String::new())); }
        }
        if !plain.is_empty() {
            let mut inbox = lock(&s.inbox);
            if inbox.len() + plain.len() > INBOX_MAX { return Err((Step::Link, Kind::Other, String::new())); }
            inbox.extend(plain);
            lock(&s.timing).oldest_unread.get_or_insert_with(Instant::now);
            drop(inbox);
            wake();
        }
    }
    Ok(())
}
