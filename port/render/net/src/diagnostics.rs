//! Opt-in native transport counters. Addresses are classified, never printed.
use std::net::SocketAddr;
use str0m::stats::PeerStats;

pub fn enabled() -> bool {
    std::env::var_os("DHD_NET_BENCH").is_some()
}

pub fn peer(backend: &str, stats: &PeerStats, relay: Option<SocketAddr>) {
    if !enabled() {
        return;
    }
    if let Some(pair) = &stats.selected_candidate_pair {
        let path = if relay == Some(pair.local.addr) {
            "turn_tls"
        } else {
            "udp"
        };
        let scope = match pair.remote.addr.ip() {
            std::net::IpAddr::V4(ip) if ip.is_private() || ip.is_loopback() => "lan",
            std::net::IpAddr::V6(ip) if ip.is_unique_local() || ip.is_loopback() => "lan",
            _ => "public",
        };
        let epoch = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis();
        eprintln!("netbench: epoch_ms={epoch} backend={backend} path={path} remote_scope={scope} rx={} tx={} ice_rtt_ms={:.3}",
            stats.peer_bytes_rx, stats.peer_bytes_tx,
            pair.current_round_trip_time.map_or(-1.0, |d| d.as_secs_f64() * 1000.0));
    }
}
