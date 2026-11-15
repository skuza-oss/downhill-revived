//! Why one path to the SFU failed (finding 248): the path, the step it reached and the kind of
//! error, as fixed words. They go in dhd.log and, to a room that lists "sfu_trouble" in its
//! welcome, in one `sfu_trouble` report per failed link attempt. Nothing else: no address, SDP,
//! session id, TURN credential, address hash or install id, and no free text from the network.
use std::io::ErrorKind;

/// Which way to the SFU.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Path {
    /// Direct UDP to the SFU.
    Udp,
    /// The TURN relay over TLS.
    Turn,
}

/// How far that path got.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    /// UDP: a datagram to the SFU could not be sent.
    Send,
    /// UDP: the socket could not be read.
    Receive,
    /// Both: datagrams went out (and, through the relay, some came back), but ICE and DTLS never
    /// finished by CONNECT_TIMEOUT.
    Connect,
    /// TURN: the room sent no credentials.
    Credentials,
    /// TURN: the TCP connection to the TURN service (its name lookup included).
    TlsConnect,
    /// TURN: the TLS handshake.
    TlsHandshake,
    /// TURN: the first Allocate, sent without credentials: its 401 brings the realm and nonce
    /// (finding 252: a 400 here and one to the authenticated Allocate mean different things).
    Challenge,
    /// TURN: the authenticated Allocate request.
    Allocate,
    /// TURN: the ChannelBind request, which also installs the permission (the client sends no
    /// separate CreatePermission).
    ChannelBind,
    /// TURN: an allocation's Refresh.
    Refresh,
    /// TURN: allocated and bound, but nothing came back from the SFU through the relay.
    FirstData,
    /// TURN: the TLS stream failed after it was up.
    Link,
}

/// The kind of error, without its text.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    Timeout,
    Refused,
    Reset,
    Unreachable,
    Denied,
    Dns,
    Tls,
    Closed,
    /// A TURN answer failed its message integrity.
    Integrity,
    /// Nothing answered (a path that never connected).
    NoAnswer,
    /// A TURN error response, by its code (RFC 8656 / 8489).
    Turn400,
    Turn401,
    Turn403,
    Turn437,
    Turn438,
    Turn440,
    Turn441,
    Turn442,
    Turn486,
    Turn508,
    TurnOther,
    Other,
}

impl Path {
    pub fn word(self) -> &'static str {
        match self { Path::Udp => "udp", Path::Turn => "turn" }
    }
}

impl Step {
    pub fn word(self) -> &'static str {
        match self {
            Step::Send => "send", Step::Receive => "receive", Step::Connect => "connect", Step::Credentials => "credentials",
            Step::TlsConnect => "tls_connect", Step::TlsHandshake => "tls_handshake", Step::Challenge => "challenge", Step::Allocate => "allocate",
            Step::ChannelBind => "channel_bind", Step::Refresh => "refresh", Step::FirstData => "first_data", Step::Link => "link",
        }
    }
}

impl Kind {
    pub fn word(self) -> &'static str {
        match self {
            Kind::Timeout => "timeout", Kind::Refused => "refused", Kind::Reset => "reset", Kind::Unreachable => "unreachable",
            Kind::Denied => "denied", Kind::Dns => "dns", Kind::Tls => "tls", Kind::Closed => "closed", Kind::Integrity => "integrity",
            Kind::NoAnswer => "no_answer", Kind::Turn400 => "turn_400", Kind::Turn401 => "turn_401", Kind::Turn403 => "turn_403",
            Kind::Turn437 => "turn_437", Kind::Turn438 => "turn_438", Kind::Turn440 => "turn_440", Kind::Turn441 => "turn_441",
            Kind::Turn442 => "turn_442", Kind::Turn486 => "turn_486", Kind::Turn508 => "turn_508", Kind::TurnOther => "turn_other",
            Kind::Other => "other",
        }
    }
    /// A socket error's kind.
    pub fn of_io(e: ErrorKind) -> Kind {
        match e {
            ErrorKind::TimedOut | ErrorKind::WouldBlock => Kind::Timeout,
            ErrorKind::ConnectionRefused => Kind::Refused,
            ErrorKind::ConnectionReset | ErrorKind::ConnectionAborted | ErrorKind::BrokenPipe | ErrorKind::UnexpectedEof => Kind::Reset,
            ErrorKind::NetworkUnreachable | ErrorKind::HostUnreachable | ErrorKind::NetworkDown | ErrorKind::AddrNotAvailable => Kind::Unreachable,
            ErrorKind::PermissionDenied => Kind::Denied,
            ErrorKind::NotFound => Kind::Dns,
            _ => Kind::Other,
        }
    }
    /// A TURN error response's code.
    pub fn of_turn(code: u16) -> Kind {
        match code {
            400 => Kind::Turn400, 401 => Kind::Turn401, 403 => Kind::Turn403, 437 => Kind::Turn437, 438 => Kind::Turn438,
            440 => Kind::Turn440, 441 => Kind::Turn441, 442 => Kind::Turn442, 486 => Kind::Turn486, 508 => Kind::Turn508,
            _ => Kind::TurnOther,
        }
    }
}

/// One path's failure.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Failure {
    pub path: Path,
    pub step: Step,
    pub kind: Kind,
}

impl Failure {
    pub fn new(path: Path, step: Step, kind: Kind) -> Self { Self { path, step, kind } }
    /// For dhd.log: "UDP failed at send (denied)".
    pub fn line(&self) -> String {
        format!("{} failed at {} ({})", if self.path == Path::Udp { "UDP" } else { "TURN" }, self.step.word(), self.kind.word())
    }
    /// The room's report (docs/online-protocol.md, `sfu_trouble`).
    pub fn report(&self) -> serde_json::Value {
        serde_json::json!({"type": "sfu_trouble", "path": self.path.word(), "step": self.step.word(), "kind": self.kind.word(), "version": version()})
    }
}

/// This release, from port/include/version.h (the one place it is written, finding 054).
pub fn version() -> &'static str {
    const HEADER: &str = include_str!("../../../include/version.h");
    HEADER.split("#define DHD_VERSION \"").nth(1).and_then(|rest| rest.split('"').next()).unwrap_or("unknown")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_report_holds_fixed_words_and_the_version_only() {
        let f = Failure::new(Path::Turn, Step::ChannelBind, Kind::of_turn(400));
        assert_eq!(f.line(), "TURN failed at channel_bind (turn_400)");
        let r = f.report();
        assert_eq!(r["type"], "sfu_trouble");
        assert_eq!((r["path"].as_str(), r["step"].as_str(), r["kind"].as_str()), (Some("turn"), Some("channel_bind"), Some("turn_400")));
        assert_eq!(r.as_object().unwrap().len(), 5, "type, path, step, kind, version: nothing else");
        let v = version();
        assert!(v.split('.').count() == 3 && v.split('.').all(|n| n.parse::<u32>().is_ok()), "{v}");
        assert_eq!(Kind::of_io(ErrorKind::PermissionDenied), Kind::Denied);
        assert_eq!(Kind::of_io(ErrorKind::NetworkUnreachable), Kind::Unreachable);
        assert_eq!(Kind::of_turn(599), Kind::TurnOther);
    }
}
