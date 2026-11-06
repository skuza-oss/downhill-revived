//! dhd_net: the port's WebSocket and HTTPS client, the C interface of `port/net/ws.h` (`ws_start`,
//! `ws_send`, `ws_stop`, `http_get`) on every native platform. tungstenite does the WebSocket over
//! a TCP stream, wrapped in rustls (ring) for `wss://` and `https://`; the trusted roots are
//! webpki-roots' (Mozilla's), bundled, so the client behaves the same on Windows, Linux, macOS and
//! Android and reads no platform certificate store. No proxy support.
//!
//! Threads, as ws.h says: a receive thread connects, reads messages and reconnects when the handler
//! asks; a send thread drains the queue and ticks the handler every 10 ms. One TLS stream cannot be
//! read and written at once, so it sits behind `Conn::live` (the send lock): the receive thread
//! waits for bytes by peeking at its own handle of the socket, unlocked (50 ms at most, so a
//! dropped connection is noticed), then reads what arrived without blocking under the lock; the
//! send thread writes, blocking, under the same lock. It also owns the room's SFU UDP socket,
//! str0m state machine, negotiated channels and bounded event/state queues; the SFU's reader
//! thread wakes it for each datagram, as ws_send does for each message. Race frames never
//! enter the TCP stream. Signaling remains on the room WebSocket.
#![forbid(unsafe_op_in_unsafe_fn)]

mod http;
mod relay;
mod sfu;
mod stream;
mod turn;

use std::collections::VecDeque;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::io::ErrorKind;
use std::net::{Shutdown, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use tungstenite::{Bytes, Message, Utf8Bytes, WebSocket};

use stream::Stream;

/// ws.h's `WsHandler`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct WsHandler {
    pub ctx: *mut c_void,
    pub on_open: Option<unsafe extern "C" fn(ctx: *mut c_void, ws: *mut WsConn)>,
    pub on_message: Option<unsafe extern "C" fn(ctx: *mut c_void, binary: c_int, data: *const u8, n: usize)>,
    pub on_close: Option<unsafe extern "C" fn(ctx: *mut c_void, code: c_int, reason: *const c_char, attempt: c_int) -> *const c_char>,
    pub on_tick: Option<unsafe extern "C" fn(ctx: *mut c_void) -> c_int>,
}

// The handler's context is the caller's; ws.h promises the callbacks come from the client's threads.
unsafe impl Send for WsHandler {}
unsafe impl Sync for WsHandler {}

/// How long the receive thread waits for bytes before it looks whether the connection was dropped.
const PEEK: Duration = Duration::from_millis(50);
/// The send thread's tick.
const TICK: Duration = Duration::from_millis(10);

/// The open connection: the WebSocket and the socket's handle for shutting it down.
struct Live {
    ws: WebSocket<Stream>,
    tcp: TcpStream,
}

#[derive(Default)]
struct Queue {
    out: VecDeque<(bool, Vec<u8>)>,
    connected: bool,
    incoming: VecDeque<(bool, Vec<u8>)>,
    /// set by the SFU socket reader: a datagram waits for the send thread
    sfu_input: bool,
    /// Received messages queued for the send thread, and those it has handed to the game: the
    /// receive thread waits for the two to match before it reports the close, so a refusal's
    /// text reaches the game before its close does.
    received: u64,
    handled: u64,
}

/// ws.h's `WsConn` (opaque to C): shared by the two threads and the caller through an `Arc`.
pub struct WsConn {
    h: WsHandler,
    queue: Mutex<Queue>,
    wake: Condvar,
    live: Mutex<Option<Live>>,
    stopping: AtomicBool,
    /// set while the receive thread runs; cleared (and `ended` notified) when it ends
    running: Mutex<bool>,
    ended: Condvar,
    threads: Mutex<Option<(JoinHandle<()>, JoinHandle<()>)>>,
}

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

impl WsConn {
    fn ptr(self: &Arc<Self>) -> *mut WsConn {
        Arc::as_ptr(self) as *mut WsConn
    }

    /// Drops the connection from outside the receive thread: its next look finds none.
    fn force_close(&self) {
        let mut live = lock(&self.live);
        {
            let mut q = lock(&self.queue);
            q.connected = false;
            q.out.clear();
            q.incoming.clear();
            q.handled = q.received; // nothing more of this connection will be handed over
        }
        self.wake.notify_all();
        if let Some(l) = live.take() {
            let _ = l.tcp.shutdown(Shutdown::Both);
        }
    }

    /// Waits until the send thread has handed the game every message received so far (a server's
    /// refusal is a text then a close), at most `limit`, or until ws_stop.
    fn wait_handled(&self, limit: Duration) {
        let q = lock(&self.queue);
        let _ = self.wake.wait_timeout_while(q, limit, |q| q.handled < q.received && !self.stopping.load(Ordering::Acquire));
    }

    /// Waits up to `ms`, or until ws_stop.
    fn backoff(&self, ms: u64) {
        let q = lock(&self.queue);
        let _ = self.wake.wait_timeout_while(q, Duration::from_millis(ms), |_| !self.stopping.load(Ordering::Acquire));
    }
}

fn cstring(text: &str) -> CString {
    CString::new(text.replace('\0', " ")).unwrap_or_default()
}

/// The receive thread: connect, read until the connection ends, ask the handler where next.
fn io_main(c: Arc<WsConn>, mut url: String) {
    let mut attempt: c_int = 0;
    while !c.stopping.load(Ordering::Acquire) {
        let (code, reason) = match stream::connect_ws(&url) {
            Ok((ws, tcp, peek)) => {
                attempt = 0;
                {
                    let mut live = lock(&c.live);
                    let mut q = lock(&c.queue);
                    q.out.clear();
                    q.incoming.clear();
                    q.connected = true;
                    *live = Some(Live { ws, tcp });
                }
                if let Some(f) = c.h.on_open {
                    unsafe { f(c.h.ctx, c.ptr()) };
                }
                let end = receive(&c, &peek);
                c.wait_handled(Duration::from_secs(1));
                c.force_close();
                end
            }
            Err(why) => (-1, why),
        };
        let reason = cstring(&reason);
        let next = match c.h.on_close {
            Some(f) => unsafe { f(c.h.ctx, code, reason.as_ptr(), attempt) },
            None => std::ptr::null(),
        };
        attempt += 1;
        if next.is_null() || c.stopping.load(Ordering::Acquire) {
            break;
        }
        url = unsafe { CStr::from_ptr(next) }.to_string_lossy().into_owned();
        c.backoff(if attempt < 4 { 500 << attempt } else { 8000 });
    }
    *lock(&c.running) = false;
    c.ended.notify_all();
}

/// Reads messages until the connection ends; returns its close code and reason.
fn receive(c: &WsConn, peek: &TcpStream) -> (c_int, String) {
    let mut byte = [0u8; 1];
    loop {
        if let Err(e) = peek.peek(&mut byte) {
            if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut | ErrorKind::Interrupted) {
                if lock(&c.queue).connected {
                    continue;
                }
                return (1006, "connection dropped".into());
            }
            // anything else: the read below reports it
        }
        let mut got = Vec::new();
        let mut end = None;
        {
            let mut guard = lock(&c.live);
            let Some(live) = guard.as_mut() else {
                return (1006, "connection dropped".into());
            };
            let _ = live.tcp.set_nonblocking(true);
            loop {
                match live.ws.read() {
                    Ok(Message::Text(t)) => got.push((false, Bytes::from(t))),
                    Ok(Message::Binary(b)) => got.push((true, b)),
                    Ok(Message::Close(frame)) => {
                        end = Some(match frame {
                            Some(f) => (u16::from(f.code) as c_int, f.reason.as_str().to_owned()),
                            None => (1005, String::new()),
                        });
                        break;
                    }
                    Ok(_) => {} // ping, pong (tungstenite answers pings itself)
                    Err(tungstenite::Error::Io(e)) if e.kind() == ErrorKind::WouldBlock => break,
                    Err(tungstenite::Error::ConnectionClosed) => {
                        end = Some((1000, String::new())); // the close this side started (ws_stop)
                        break;
                    }
                    Err(e) => {
                        end = Some((1006, format!("connection lost ({e})")));
                        break;
                    }
                }
            }
            let _ = live.tcp.set_nonblocking(false);
            if end.is_some() {
                let _ = live.ws.flush(); // the reply to a close
            }
        }
        for (binary, data) in got {
            if binary && matches!(data.first(), Some(5 | 6)) { continue; }
            let mut q = lock(&c.queue);
            if !q.connected { return (1006, "connection closed".into()); }
            if q.incoming.len() >= 256 { return (1006, "control queue full".into()); }
            q.incoming.push_back((binary, data.to_vec()));
            q.received += 1;
            drop(q);
            c.wake.notify_all();
        }
        if let Some(end) = end {
            return end;
        }
    }
}

/// The send thread: drain the queue in order, tick the handler.
fn send_main(c: Arc<WsConn>) {
    let mut sfu: Option<sfu::Sfu> = None;
    // The SFU reader wakes this thread for each datagram, as ws_send does for each message.
    let weak = Arc::downgrade(&c);
    let wake: sfu::Wake = Arc::new(move || {
        if let Some(c) = weak.upgrade() {
            lock(&c.queue).sfu_input = true;
            c.wake.notify_all();
        }
    });
    // Race frames the last flush handed to the SFU after its poll: poll again before waiting.
    let mut unwritten = false;
    while !c.stopping.load(Ordering::Acquire) {
        {
            let q = lock(&c.queue);
            let wait = if unwritten { Duration::ZERO } else { TICK };
            let (mut q, _) = c.wake.wait_timeout_while(q, wait, |q| q.out.is_empty() && q.incoming.is_empty() && !q.sfu_input && !c.stopping.load(Ordering::Acquire)).unwrap_or_else(|e| e.into_inner());
            q.sfu_input = false;
        }
        if !lock(&c.queue).connected { sfu = None; continue; }
        // Lobby and SFU callbacks share this thread. Apply the transport's start marker
        // before the game sees start, then deliver any early reliable race events.
        loop {
            let item = lock(&c.queue).incoming.pop_front();
            let Some((binary, data)) = item else { break };
            if !binary {
                if let Ok(message) = serde_json::from_slice::<serde_json::Value>(&data) {
                    if matches!(message["type"].as_str(), Some("start" | "ended" | "welcome")) {
                        lock(&c.queue).out.retain(|(binary, bytes)| !*binary || !matches!(bytes.first(), Some(5 | 6)));
                    }
                    let result = if message["type"] == "start" {
                        match sfu::Sfu::start(wake.clone()) {
                            Ok((rtc, offer)) => {
                                let mut rtc = rtc;
                                let result = rtc.signal(&message);
                                sfu = Some(rtc);
                                lock(&c.queue).out.push_back((false, offer.to_string().into_bytes()));
                                result
                            }
                            Err(e) => Err(e),
                        }
                    } else if message["type"] == "ended" || message["type"] == "welcome" {
                        sfu = None; Ok(())
                    } else if let Some(rtc) = sfu.as_mut() { rtc.signal(&message) } else { Ok(()) };
                    if result.is_err() {
                        eprintln!("net: SFU signaling failed"); c.force_close(); sfu = None; break;
                    }
                }
            }
            if let Some(f) = c.h.on_message {
                let mut bytes = data.clone(); bytes.push(0);
                unsafe { f(c.h.ctx, binary as c_int, bytes.as_ptr(), data.len()) };
            }
            lock(&c.queue).handled += 1;
            c.wake.notify_all();
        }
        // Race frames the game queued go to the SFU before this poll, which writes them out:
        // they never wait for the next wake (finding 158, round 3).
        flush(&c, &mut sfu);
        let mut incoming = Vec::new();
        if let Some(rtc) = sfu.as_mut() {
            match rtc.poll() {
                Ok((text, frames)) => {
                    for t in text { lock(&c.queue).out.push_back((false, t.to_string().into_bytes())); }
                    incoming = frames;
                }
                Err(_) => { eprintln!("net: SFU connection failed"); c.force_close(); sfu = None; }
            }
        }
        for data in incoming {
            if let Some(f) = c.h.on_message { unsafe { f(c.h.ctx, 1, data.as_ptr(), data.len()) }; }
        }
        flush(&c, &mut sfu);
        // Once: frames a full SCTP buffer refuses wait for the next wake, not a busy loop.
        unwritten = !unwritten && sfu.as_ref().is_some_and(|s| s.unwritten());
        if let Some(f) = c.h.on_tick {
            if unsafe { f(c.h.ctx) } != 0 {
                c.force_close();
            }
        }
    }
}

/// Sends what is queued, in order: race frames to the SFU association, the rest on the WebSocket.
fn flush(c: &WsConn, sfu: &mut Option<sfu::Sfu>) {
    loop {
        let item = lock(&c.queue).out.pop_front();
        let Some((binary, data)) = item else { break };
        if binary && matches!(data.first(), Some(5 | 6)) {
            let failed = sfu.as_mut().is_some_and(|rtc| rtc.send(&data).is_err());
            if failed { c.force_close(); *sfu = None; }
            continue;
        }
        let mut live = lock(&c.live);
        if let Some(l) = live.as_mut() {
            let msg = if binary {
                Message::Binary(Bytes::from(data))
            } else {
                Message::Text(Utf8Bytes::from(String::from_utf8_lossy(&data).into_owned()))
            };
            if l.ws.send(msg).is_err() {
                // a failed or timed-out write leaves the stream unusable: drop it
                lock(&c.queue).connected = false;
                if let Some(l) = live.take() {
                    let _ = l.tcp.shutdown(Shutdown::Both);
                }
            }
        }
    }
}

/// ws.h: starts the client's threads on url. NULL only when url is not text or a thread cannot start.
#[no_mangle]
pub unsafe extern "C" fn ws_start(url: *const c_char, h: *const WsHandler) -> *mut WsConn {
    if url.is_null() || h.is_null() {
        return std::ptr::null_mut();
    }
    let url = unsafe { CStr::from_ptr(url) }.to_string_lossy().into_owned();
    let c = Arc::new(WsConn {
        h: unsafe { *h },
        queue: Mutex::new(Queue::default()),
        wake: Condvar::new(),
        live: Mutex::new(None),
        stopping: AtomicBool::new(false),
        running: Mutex::new(true),
        ended: Condvar::new(),
        threads: Mutex::new(None),
    });
    let (a, b) = (c.clone(), c.clone());
    let io = thread::Builder::new().name("dhd-ws-io".into()).spawn(move || io_main(a, url));
    let send = thread::Builder::new().name("dhd-ws-send".into()).spawn(move || send_main(b));
    match (io, send) {
        (Ok(io), Ok(send)) => {
            *lock(&c.threads) = Some((io, send));
            Arc::into_raw(c) as *mut WsConn
        }
        _ => std::process::abort(), // as std's thread::spawn would
    }
}

/// ws.h: queues a message; dropped when not connected.
#[no_mangle]
pub unsafe extern "C" fn ws_send(ws: *mut WsConn, binary: c_int, data: *const c_void, n: usize) {
    let Some(c) = (unsafe { ws.as_ref() }) else { return };
    let bytes = if n == 0 { Vec::new() } else { unsafe { std::slice::from_raw_parts(data as *const u8, n) }.to_vec() };
    let mut q = lock(&c.queue);
    if !q.connected {
        return;
    }
    if q.out.len() >= 256 {
        // State may be superseded; reliable control/events must fail visibly, never vanish.
        let state = binary != 0 && bytes.first() == Some(&5);
        drop(q);
        if !state { c.force_close(); }
        return;
    }
    q.out.push_back((binary != 0, bytes));
    drop(q);
    c.wake.notify_all();
}

/// ws.h: sends what is queued, closes, stops the threads and frees ws.
#[no_mangle]
pub unsafe extern "C" fn ws_stop(ws: *mut WsConn) {
    if ws.is_null() {
        return;
    }
    let c = unsafe { Arc::from_raw(ws as *const WsConn) };
    for _ in 0..50 {
        // let the queue drain (a leave message)
        if lock(&c.queue).out.is_empty() {
            break;
        }
        thread::sleep(TICK);
    }
    c.stopping.store(true, Ordering::Release);
    c.wake.notify_all();
    let Some((io, send)) = lock(&c.threads).take() else { return };
    let _ = send.join();
    if let Some(l) = lock(&c.live).as_mut() {
        let _ = l.ws.close(None);
        let _ = l.ws.flush();
    }
    let running = lock(&c.running);
    let (running, _) = c.ended.wait_timeout_while(running, Duration::from_secs(2), |r| *r).unwrap_or_else(|e| e.into_inner());
    let late = *running;
    drop(running);
    if late {
        c.force_close();
    }
    let _ = io.join();
}

/// ws.h: a blocking HTTP(S) GET (ws:// and wss:// read as http:// and https://). Returns the body's
/// length (at most cap - 1, NUL-terminated in out) after a 200, else -1.
#[no_mangle]
pub unsafe extern "C" fn http_get(url: *const c_char, out: *mut c_char, cap: usize) -> c_int {
    if url.is_null() || out.is_null() || cap == 0 {
        return -1;
    }
    let url = unsafe { CStr::from_ptr(url) }.to_string_lossy();
    match http::get(&url) {
        Ok(body) => {
            let n = body.len().min(cap - 1).min(c_int::MAX as usize);
            let out = unsafe { std::slice::from_raw_parts_mut(out as *mut u8, cap) };
            out[..n].copy_from_slice(&body[..n]);
            out[n] = 0;
            n as c_int
        }
        Err(_) => -1,
    }
}
