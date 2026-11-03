//! http_get's client: one HTTP/1.1 GET with `Connection: close` on the WebSocket's stream (TCP,
//! rustls for https), its response parsed by httparse; Content-Length or chunked bodies. Redirects
//! (301, 302, 303, 307, 308) are followed, at most MAX_REDIRECTS, never from https to http: a
//! GitHub release asset's address answers with one (the Android update check, finding 143).

use std::io::{Read, Write};
use std::time::Duration;

use tungstenite::http::Uri;

use crate::stream::{self, AGENT};

/// The longest response read (the lobby list is a few kB).
const LIMIT: usize = 4 << 20;
/// Redirects followed for one GET (GitHub's release downloads take one).
const MAX_REDIRECTS: usize = 5;

/// One answer: the body of a 200, or where a redirect points.
enum Answer {
    Body(Vec<u8>),
    Redirect(String),
}

/// The body of a 200 response to GET url, after the redirects.
pub fn get(url: &str) -> Result<Vec<u8>, String> {
    let mut url = url.to_string();
    for _ in 0..=MAX_REDIRECTS {
        match get_once(&url)? {
            Answer::Body(body) => return Ok(body),
            Answer::Redirect(location) => url = redirect_target(&url, &location)?,
        }
    }
    Err("too many redirects".into())
}

/// Where a redirect from `from` to `location` (absolute, or a path on the same host) goes. A secure
/// address is never left for a plain one.
fn redirect_target(from: &str, location: &str) -> Result<String, String> {
    let secure = from.starts_with("https://") || from.starts_with("wss://");
    let target = if location.starts_with("https://") || location.starts_with("http://") {
        location.to_string()
    } else if location.starts_with('/') && !location.starts_with("//") {
        let scheme_end = from.find("://").ok_or("bad URL")? + 3;
        let host_end = from[scheme_end..].find('/').map_or(from.len(), |i| scheme_end + i);
        format!("{}{location}", &from[..host_end])
    } else {
        return Err(format!("unusable redirect to {location}"));
    };
    if secure && !target.starts_with("https://") {
        return Err(format!("redirect from https to {target} refused"));
    }
    Ok(target)
}

fn get_once(url: &str) -> Result<Answer, String> {
    let (secure, rest) = if let Some(r) = url.strip_prefix("https://").or_else(|| url.strip_prefix("wss://")) {
        (true, r)
    } else if let Some(r) = url.strip_prefix("http://").or_else(|| url.strip_prefix("ws://")) {
        (false, r)
    } else {
        return Err(format!("bad URL {url}"));
    };
    let uri: Uri = format!("{}://{rest}", if secure { "https" } else { "http" })
        .parse()
        .map_err(|_| format!("bad URL {url}"))?;
    let (mut s, tcp) = stream::open(&uri, secure)?;
    let _ = tcp.set_read_timeout(Some(Duration::from_secs(10)));
    let authority = uri.authority().map(|a| a.as_str()).unwrap_or_default();
    let path = uri.path_and_query().map(|p| p.as_str()).unwrap_or("/");
    let request = format!(
        "GET {path} HTTP/1.1\r\nHost: {authority}\r\nUser-Agent: {AGENT}\r\nAccept: */*\r\nConnection: close\r\n\r\n"
    );
    s.write_all(request.as_bytes()).and_then(|_| s.flush()).map_err(|e| e.to_string())?;
    let mut data = Vec::new();
    let mut buf = [0u8; 16384];
    loop {
        match s.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => data.extend_from_slice(&buf[..n]),
            // a peer that closes without TLS's close_notify, or a read error after the body: the
            // length checks below decide
            Err(_) => break,
        }
        if data.len() > LIMIT {
            return Err("response too long".into());
        }
    }
    let mut headers = [httparse::EMPTY_HEADER; 64];
    let mut response = httparse::Response::new(&mut headers);
    let start = match response.parse(&data) {
        Ok(httparse::Status::Complete(n)) => n,
        _ => return Err("no HTTP response".into()),
    };
    let header = |name: &str| {
        response.headers.iter().find(|h| h.name.eq_ignore_ascii_case(name)).map(|h| String::from_utf8_lossy(h.value).trim().to_string())
    };
    if let Some(code @ (301 | 302 | 303 | 307 | 308)) = response.code {
        return header("location").map(Answer::Redirect).ok_or_else(|| format!("HTTP {code} without a Location"));
    }
    if response.code != Some(200) {
        return Err(format!("HTTP {}", response.code.unwrap_or(0)));
    }
    let body = &data[start..];
    if header("transfer-encoding").is_some_and(|v| v.to_ascii_lowercase().contains("chunked")) {
        return dechunk(body).map(Answer::Body);
    }
    match header("content-length").map(|v| v.parse::<usize>()) {
        Some(Ok(n)) if n <= body.len() => Ok(Answer::Body(body[..n].to_vec())),
        Some(_) => Err("short body".into()),
        None => Ok(Answer::Body(body.to_vec())),
    }
}

/// A chunked body's data (RFC 9112 section 7.1).
fn dechunk(mut body: &[u8]) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    loop {
        let line = body.windows(2).position(|w| w == b"\r\n").ok_or("short chunk")?;
        let size = std::str::from_utf8(&body[..line]).ok().and_then(|l| usize::from_str_radix(l.split(';').next()?.trim(), 16).ok());
        let size = size.ok_or("bad chunk size")?;
        body = &body[line + 2..];
        if size == 0 {
            return Ok(out);
        }
        if body.len() < size + 2 {
            return Err("short chunk".into());
        }
        out.extend_from_slice(&body[..size]);
        body = &body[size + 2..];
    }
}

#[cfg(test)]
mod tests {
    use super::redirect_target;

    #[test]
    fn redirects() {
        let from = "https://github.com/skuza-oss/downhill-revived/releases/latest/download/latest.json";
        assert_eq!(
            redirect_target(from, "https://release-assets.githubusercontent.com/x?y=1").unwrap(),
            "https://release-assets.githubusercontent.com/x?y=1"
        );
        assert_eq!(
            redirect_target(from, "/skuza-oss/downhill-revived/releases/download/v1.0.0/latest.json").unwrap(),
            "https://github.com/skuza-oss/downhill-revived/releases/download/v1.0.0/latest.json"
        );
        assert_eq!(redirect_target("http://localhost:8787/a", "/b").unwrap(), "http://localhost:8787/b");
        assert!(redirect_target(from, "http://example.org/latest.json").is_err());
        assert!(redirect_target(from, "//example.org/latest.json").is_err());
        assert!(redirect_target(from, "latest.json").is_err());
    }
}
