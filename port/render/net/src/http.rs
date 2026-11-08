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

/// A URL's scheme, lower-cased (schemes are case-insensitive), and what follows its ':'.
fn split_scheme(url: &str) -> Option<(String, &str)> {
    let (scheme, rest) = url.split_once(':')?;
    let mut chars = scheme.chars();
    let valid = chars.next()?.is_ascii_alphabetic() && chars.all(|c| c.is_ascii_alphanumeric() || "+-.".contains(c));
    valid.then(|| (scheme.to_ascii_lowercase(), rest))
}

/// A path with its "." and ".." segments resolved (RFC 3986 5.2.4); it starts with '/'.
fn remove_dot_segments(path: &str) -> String {
    let segments: Vec<&str> = path.split('/').skip(1).collect();
    let mut out: Vec<&str> = Vec::new();
    for (i, segment) in segments.iter().enumerate() {
        let last = i + 1 == segments.len();
        match *segment {
            "." => {}
            ".." => {
                out.pop();
            }
            s => out.push(s),
        }
        if last && (*segment == "." || *segment == "..") {
            out.push("");  // "a/b/.." names the folder: a/
        }
    }
    format!("/{}", out.join("/"))
}

/// Where a redirect from `from` to `location` goes (RFC 9110 10.2.2): an absolute http(s) address
/// (its scheme in any case), a path on the same host, or a path relative to `from`'s folder. A
/// secure address is never left for a plain one; another host without a scheme ("//host/...") is
/// refused.
fn redirect_target(from: &str, location: &str) -> Result<String, String> {
    let unusable = || format!("unusable redirect to {location}");
    let (from_scheme, from_rest) = split_scheme(from).filter(|(_, r)| r.starts_with("//")).ok_or("bad URL")?;
    let secure = from_scheme == "https" || from_scheme == "wss";
    let location = location.trim();
    let location = location.split('#').next().unwrap_or_default();  // a fragment is never sent
    let (target_scheme, target) = match split_scheme(location) {
        Some((scheme, rest)) => {
            if !(scheme == "https" || scheme == "http") || !rest.starts_with("//") {
                return Err(unusable());
            }
            let target = format!("{scheme}:{rest}");
            (scheme, target)
        }
        None if location.starts_with("//") => return Err(unusable()),
        None => {
            let host_end = from_rest[2..].find(['/', '?', '#']).map_or(from_rest.len(), |i| i + 2);
            let from_path = from_rest[host_end..].split(['?', '#']).next().unwrap_or_default();
            let (path, query) = location.split_at(location.find('?').unwrap_or(location.len()));
            let joined = if path.starts_with('/') {
                path.to_string()
            } else if path.is_empty() {
                // only a query: the same path
                if from_path.is_empty() { "/".to_string() } else { from_path.to_string() }
            } else {
                // relative to from's folder (up to its last '/')
                let folder = from_path.rfind('/').map_or("/", |i| &from_path[..=i]);
                format!("{folder}{path}")
            };
            let target = format!("{from_scheme}:{}{}{query}", &from_rest[..host_end], remove_dot_segments(&joined));
            (from_scheme.clone(), target)
        }
    };
    if secure && target_scheme != "https" && target_scheme != "wss" {
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
pub(crate) fn dechunk(mut body: &[u8]) -> Result<Vec<u8>, String> {
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
    }

    /// RFC 9110 lets Location be relative to the request's address, and schemes are case-insensitive.
    #[test]
    fn relative_and_case_insensitive_redirects() {
        let from = "https://github.com/skuza-oss/downhill-revived/releases/latest/download/latest.json?x=1";
        let base = "https://github.com/skuza-oss/downhill-revived/releases";
        assert_eq!(redirect_target(from, "latest2.json").unwrap(), format!("{base}/latest/download/latest2.json"));
        assert_eq!(redirect_target(from, "./a/b.json?y=2#frag").unwrap(), format!("{base}/latest/download/a/b.json?y=2"));
        assert_eq!(redirect_target(from, "../../download/v1.0.0/latest.json").unwrap(), format!("{base}/download/v1.0.0/latest.json"));
        assert_eq!(redirect_target(from, "..").unwrap(), format!("{base}/latest/"));
        assert_eq!(redirect_target(from, "/a/./b/../c").unwrap(), "https://github.com/a/c");
        assert_eq!(redirect_target(from, "/../../x").unwrap(), "https://github.com/x");
        assert_eq!(redirect_target(from, "?page=2").unwrap(), format!("{base}/latest/download/latest.json?page=2"));
        assert_eq!(redirect_target("http://localhost:8787", "b").unwrap(), "http://localhost:8787/b");
        assert_eq!(redirect_target("http://localhost:8787?q", "b").unwrap(), "http://localhost:8787/b");
        assert_eq!(redirect_target("wss://dhd.test/v1/a", "b").unwrap(), "wss://dhd.test/v1/b");
        // the scheme in any case: lower-cased; never from https to http, whatever its case
        assert_eq!(redirect_target(from, "HTTPS://Example.org/x").unwrap(), "https://Example.org/x");
        assert_eq!(redirect_target("HTTP://localhost:8787/a/b", "c").unwrap(), "http://localhost:8787/a/c");
        assert!(redirect_target(from, "HTTP://example.org/latest.json").is_err());
        assert!(redirect_target("HTTPS://github.com/a", "Http://github.com/b").is_err());
        // other schemes, and a scheme without "//", are refused
        assert!(redirect_target(from, "ftp://example.org/x").is_err());
        assert!(redirect_target(from, "javascript:alert(1)").is_err());
        assert!(redirect_target(from, "https:latest.json").is_err());
    }
}
