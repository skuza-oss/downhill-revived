# 246: Phone SFU fallback and network return

Status: current
Checked: 2026-11-14

Branch `fix/phone-online-links` from local main 07e38743, started 2026-11-14.

## Goal and acceptance

Explain the TURN 400 a phone got in the 0.8.3 release checks, prove the TURN-over-TLS path on
two hidden PC games, and fix what is plainly wrong when the link or the room connection is lost
and comes back. Gates: quick, port, docs, the Android native build. No protocol or server change.

## Progress

- [x] Reserve lane and read area rules.
- [x] The 400 from the phone's logs and the client and server code.
- [x] Fixes with unit tests (Rust `dhd_net`, C `test_session`); relay-only race on the PC.
- [x] Gates: quick, port, docs, Android arm64 native build (all OK).
- [ ] The lead's phone check (below).

## What the 400 was

The phone log of the 0.8.3 gate (Mi 11i on Wi-Fi, game 0.8.2 build, local only) has it in 3 of
6 rooms: two SFU rooms and the P2P room's SFU fallback. In each, UDP had not connected 2 s after
the answer, TURN was asked, and the relay failed with `TURN error 400`. The
association then failed as a whole, so its UDP attempt died with it; the game made a new session
after 1, 2, 4, 8, 16 s and the same happened each time, until the room's limit of 6 sessions in
5 minutes answered `Too many Realtime sessions`. In the other 3 rooms UDP connected in 650-800 ms.

What the code rules out: not a stale or reused credential (the room mints new credentials for
every session and refuses a second one per session; the client sends one Allocate per TLS link);
not a second allocation on one session; not an IPv6 relay (the Allocate asks for UDP, so IPv4,
and only the SFU's IPv4 candidates are kept). The same request bytes allocated against the
official TURN service from the PC in 46 ms during the same release checks. The old line did not
say which request was refused. Finding 158 saw Cloudflare answer 400 to a username it doesn't
know after the 401 challenge, so the likeliest causes are a fresh credential the TURN location
did not know yet, or a refused ChannelBind; the PC cannot tell which. One phone-only difference
the code had: the TLS link took the first address the system returned, IPv6 included, while the
relay and the SFU's candidates are IPv4 (the PC has no IPv6, so it never took that path).

## Changes

- A relay that fails before anything connected no longer fails the association while UDP may
  still connect: UDP keeps its attempt until `CONNECT_TIMEOUT` (20 s after the answer), which
  also ends an association whose room never sent TURN credentials (it used to wait forever).
  With `DHD_NET_SFU_UDP=0` a refused relay still fails at once (`sfu.rs`).
- The TURN refusal line names the request and its phase (`TURN Allocate error 400
  (authenticated=..., allocated=..., credentials held=..., request age=...)`), with no username,
  realm, nonce, reason text or peer address (`turn.rs`).
- The TURN TLS link dials IPv4 first, IPv6 only if no IPv4 address answers, and logs which family
  it used (`relay.rs`).
- A connected association whose ICE path stops answering fails after `ICE_LOST_GRACE` (5 s), as
  the P2P peers do, so a new session comes; str0m alone keeps a disconnected agent alive.
- Race frames from the SFU no longer count as room traffic: a room connection that died while the
  race link ran was not noticed, so its rejoin never started (`client.c`, only PONG counts).
- While the room connection is down, the room page says `Connection lost, reconnecting...`
  unless the room sent its own notice; it clears at the welcome.

## Relay-only race on the PC

Local server (wrangler with the main checkout's keys, nothing copied), two hidden games, both
`DHD_NET_SFU_UDP=0`, the guest's room connection cut 20 s after GO: both connected via TURN/TLS
on 443 over IPv4 (allocation 270 ms after the credentials, connected 3.4-3.7 s after the
answer), races to results 0:45.03 / 0:50.03 equal on both, no `freeze:`. ICE round trip through
the relay: median 10 ms, worst 377 ms (host) and 235 ms (guest). The guest was back in the race
on the same association (no new session). Relay read gaps in race windows up to 554-667 ms; the
3 s gaps are in the lobby, where nothing is sent.

## Decisions

Keep SFU signaling and credentials isolated per member; log no other member's address, SDP,
session ID, credentials, address hash or install ID. No server change: the session limit is
right once a refused relay no longer spends a session every few seconds.

## Surprises

The 400 was in the 0.8.3 gate's phone logs, not the production check's (`REPORT.md` there has
none). `online_race.py` passes no `DHD_*` variable of its caller to the games: test switches go
through `--a` / `--b`.

## Result

`check.py quick`, `port` and `docs` OK; `dhd_net` 42 tests and `test_session` pass; the Android
arm64 `libmain.so` builds. The core module is unchanged (no state trace needed): every change is
in the host's net code. No server deploy.

Left for the phone (Mi 11i, the lead): an SFU room made on the phone over Wi-Fi, then Wi-Fi off
for 10-40 s in the lobby and once mid-race, back on; then the same with the phone moving between
two networks. The log should show `TURN relay's TLS link up over IPv4` if TURN is asked, a TURN
refusal as `TURN <request> error <code> (...)` followed by `UDP keeps trying`, no run of six
`a new session` lines, `ICE disconnected` then a new session within about 5 s of the loss, and
`back in the race` (or `back in room`) after the network returns.

Checked:
- 0.8.4 gate on ad4b92f9: `check.py port`, `check.py server` and `check.py docs` OK; boot and race state
  traces equal the reference build.
- Mi 11i, release-signed PGO APK, a private SFU room with a hidden PC joiner: Wi-Fi off 15 s in the lobby and
  10 s in a race came back; 30 s and 35 s ended in a clean "Connection lost"; zero `freeze:` lines.
