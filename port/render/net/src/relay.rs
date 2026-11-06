//! The TURN relay's stream: TLS over TCP to Cloudflare's TURN service on port 443, for players
//! whose network blocks UDP (finding 158, round 4). A thread connects and then reads; the SFU's
//! owner thread writes. One rustls connection sits behind a lock that only the TLS bookkeeping
//! holds: the reader waits for bytes on its own handle of the socket without it.
use crate::sfu::Wake;
use crate::stream::{dial, tls_config};
use rustls::pki_types::ServerName;
use rustls::ClientConnection;
use std::collections::VecDeque;
use std::io::{ErrorKind, Read, Write};
use std::net::{SocketAddr, TcpStream};
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
            alive: AtomicBool::new(true), failed: AtomicBool::new(false),
        });
        let (s, host) = (shared.clone(), host.to_owned());
        let spawned = std::thread::Builder::new().name("dhd-turn".into()).spawn(move || {
            if let Err(e) = run(&s, &host, port, &wake) {
                if s.alive.load(Ordering::Acquire) { eprintln!("net: TURN link failed ({e})"); }
            }
            s.failed.store(true, Ordering::Release);
            wake();
        });
        if spawned.is_err() { shared.failed.store(true, Ordering::Release); }
        Self { shared }
    }
    pub fn ready(&self) -> bool { lock(&self.shared.tls).is_some() }
    pub fn failed(&self) -> bool { self.shared.failed.load(Ordering::Acquire) }
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
        let mut guard = lock(&self.shared.tls);
        let Some((conn, tcp)) = guard.as_mut() else { return };
        let ok = conn.writer().write_all(bytes).is_ok() && flush_tls(conn, tcp);
        if !ok { self.shared.failed.store(true, Ordering::Release); }
    }
}

impl Drop for Link {
    fn drop(&mut self) {
        self.shared.alive.store(false, Ordering::Release);
        if let Some((_, tcp)) = lock(&self.shared.tls).as_ref() { let _ = tcp.shutdown(std::net::Shutdown::Both); }
    }
}

fn flush_tls(conn: &mut ClientConnection, tcp: &mut TcpStream) -> bool {
    while conn.wants_write() {
        if conn.write_tls(tcp).is_err() { return false; }
    }
    true
}

fn run(s: &Shared, host: &str, port: u16, wake: &Wake) -> Result<(), String> {
    let mut tcp = dial(host, port).map_err(|e| format!("can't connect to {host}: {e}"))?;
    let _ = tcp.set_nodelay(true);
    tcp.set_read_timeout(Some(crate::stream::TIMEOUT)).map_err(|e| e.to_string())?;
    tcp.set_write_timeout(Some(WRITE_TIMEOUT)).map_err(|e| e.to_string())?;
    let name = ServerName::try_from(host.to_owned()).map_err(|_| "bad host name".to_string())?;
    let mut conn = ClientConnection::new(tls_config()?, name).map_err(|e| e.to_string())?;
    while conn.is_handshaking() {
        conn.complete_io(&mut tcp).map_err(|e| format!("TLS handshake: {e}"))?;
    }
    *lock(&s.local) = tcp.local_addr().ok();
    let mut reader = tcp.try_clone().map_err(|e| e.to_string())?;
    reader.set_read_timeout(Some(READER_CHECK)).map_err(|e| e.to_string())?;
    *lock(&s.tls) = Some((conn, tcp));
    wake();
    let mut buffer = vec![0u8; 16384];
    while s.alive.load(Ordering::Acquire) {
        let n = match reader.read(&mut buffer) {
            Ok(0) => return Err("closed by the server".into()),
            Ok(n) => {
                let now = Instant::now();
                let mut t = lock(&s.timing);
                if let Some(last) = t.last_read { t.read_gap_max = t.read_gap_max.max(now - last); }
                t.last_read = Some(now);
                n
            }
            Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted) => continue,
            Err(e) => return Err(e.to_string()),
        };
        let mut plain = vec![];
        {
            let mut guard = lock(&s.tls);
            let Some((conn, tcp)) = guard.as_mut() else { return Ok(()) };
            let mut input = &buffer[..n];
            while !input.is_empty() {
                conn.read_tls(&mut input).map_err(|e| e.to_string())?;
                let state = conn.process_new_packets().map_err(|e| format!("TLS: {e}"))?;
                if state.plaintext_bytes_to_read() > 0 {
                    let mut chunk = vec![0u8; state.plaintext_bytes_to_read()];
                    conn.reader().read_exact(&mut chunk).map_err(|e| e.to_string())?;
                    plain.extend_from_slice(&chunk);
                }
                if state.peer_has_closed() { return Err("closed by the server".into()); }
            }
            if !flush_tls(conn, tcp) { return Err("write failed".into()); }
        }
        if !plain.is_empty() {
            let mut inbox = lock(&s.inbox);
            if inbox.len() + plain.len() > INBOX_MAX { return Err("inbox full".into()); }
            inbox.extend(plain);
            lock(&s.timing).oldest_unread.get_or_insert_with(Instant::now);
            drop(inbox);
            wake();
        }
    }
    Ok(())
}
