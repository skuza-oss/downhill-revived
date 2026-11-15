# 248: Racers on the TURN fallback left out of every race

Status: current
Checked: 2026-11-15

Branch `fix/turn-players-out-248` from `main` 02a8bb3e (0.8.4), started 2026-11-14. The cause is
not proven. The branch hardens the game's two paths to the SFU and adds a report of the failing
step (`sfu_trouble`), so that the next days' logs can name the cause; neither is released or
deployed.

## Goal and acceptance

The 0.8.4 production check saw racers on the TURN fallback get `Realtime setup after 10000 ms:
N/SA:turn`, then `out of the race: no Realtime connection`, race after race (rooms ZPT39U,
8DH5A6, A989NA, WGY4JC). Find how often, since when, whether 0.8.4 clients have it, and why;
fix it only if the cause is proven and the fix small. Out of scope: the subscription timeouts
(finding 182), P2P links.

## Progress

- [x] 2026-11-14: 7 days of the server's Workers Logs, its Analytics Engine joins and race
  starts, and Cloudflare's TURN usage analytics per credential tag (`room-CODE-mN`).
- [x] 2026-11-14: one affected room followed through its server lines; server and client code read.
- [x] 2026-11-14: reproduction on the official server (published 0.8.4 PC build, private rooms,
  hidden desktop): not reproduced (below).
- [x] 2026-11-14: outside causes (Cloudflare's status, changelog, community, TURN docs): none
  found that our code misses (below).
- [x] 2026-11-14: the game's paths fail independently, and every failure names its step and kind.
- [x] 2026-11-14: `sfu_trouble`, the game's report of a failed path, and the room's log line.
- [x] 2026-11-15: commits cd22e607 (game) and b670ffc5 (room); `check.py quick`, `port`,
  `server` and `docs` pass. No state trace: nothing here touches game state.
- [x] 2026-11-15: review fixes: a relay that never connects ends the attempt at the deadline
  (0a381914), the listed port follows 443 on any failure before data (dcb425d2), the room forgets
  a member's report times when it leaves (1a60f862).
- [ ] After a deploy and a release: count the `Realtime trouble:` lines by path, step and kind,
  then fix the cause.

## What the logs say

Workers Logs sample long query windows: a 24 h window returned 153 of the day's lines, 1 h
windows return all of them. Counts below come from 30-minute windows. Race starts are the
`Realtime released` lines; TURN members come from Cloudflare's TURN analytics (a member whose
relay carried under 5 kB in a day is "dead", over 20 kB "worked").

| Day (UTC) | releases | out of the race (all reasons) | out on TURN (members, rooms) | SFU TURN members worked / dead |
|---|---|---|---|---|
| 11-08 | 978 | 92 | 0 | 104 / 15 |
| 11-09 | 692 | 10 | 0 | 39 / 5 |
| 11-10 | 759 | 16 | 0 | 18 / 6 |
| 11-11 | 833 | 15 | 0 | 21 / 5 |
| 11-12 | 656 | 58 | 28 (12, 5) | 12 / 55 |
| 11-13 | 758 | 125 | 77 (30, 14) | 20 / 58 |
| 11-14 | 736 | 149 | 115 (51, 14) | 20 / 100 |

- It starts between 23:00 on 11-11 and 01:00 on 11-12 UTC, hour by hour in the TURN data. No
  server deploy then (one Worker version from 11-11 12:29 to 11-12 08:00) and no release (0.7.16
  came out about 20 h before; 11-11 had 885 joins on 0.7.16 and TURN worked). It got worse on
  11-14 (0.8.3: phones in SFU rooms), but it was there before.
- The same TURN locations flipped: before 11-12 / from 11-12, worked/dead, Oran 181/27 → 18/213,
  Lima 54/12 → 8/52, Santiago 29/6 → 3/19, São Paulo 13/2 → 2/21, Miami 23/1 → 2/9.
- Per room, not per player: of the SFU rooms with TURN members from 11-12, 97 have every TURN
  member dead, 38 every one working, 4 mixed. Of 124 connect-deadline lines naming a `turn`
  racer, one also names a connected racer; the rest are rooms of 1-5 racers where everyone is
  on TURN and nobody connects. In 3 of the 4 rooms of the report the members came from one ISP.
- 0.8.4 has it: WGY4JC's two racers joined on 0.8.4 (Peru, Brazil), and three rooms of 0.8.4
  players in Morocco (CABXBB, PVR639, AQT7GB, 22:09-22:13 UTC) show the same lines.
- Not slow but dead: a dead member's relay carries 50-460 bytes a minute for minutes (a working
  relay in the lobby about 3.7 kB a minute, in a race over 1 MB), and it never connects in the
  lobby either, where no deadline runs. ZPT39U's and A989NA's members (0.8.3) never reached the
  TURN service at all. A longer `SFU_CONNECT_MS` would not help.
- `Realtime subscription outcome unknown` (389 in 7 days, 6 to 99 a day) is a separate matter:
  it was already 65 a day on 11-10 and 11-11, and the deadline that takes these racers out
  checks the member's own connection (`sfu_connected`), before any subscription.

## Reproduction

Published 0.8.4 PC build, official server, private rooms from `online_race.py --hidden`: three
rooms with the guest relay-only (`DHD_NET_SFU_UDP=0`) and one with both games relay-only. All
four raced to the end with equal results (0:40.03 both): `TURN relay's TLS link up over IPv4`,
allocation 38-47 ms after the credentials, `SFU connected via TURN/TLS` 2.7-2.9 s after the
answer. Cloudflare put this PC's relays on the Oran TURN location, the one with the most dead
relays since 11-12, and they carried 3.7 MB each. So the server, the TURN service there and the
0.8.4 relay code work; what fails is something about the players' own path that the in-game
UDP switch does not imitate.

## Leads (not proven)

- The in-game switch drops UDP inside the game; a real block can make `send_to` fail. Any UDP
  send error except `WouldBlock` fails the whole association (`sfu.rs` `drain`), TURN attempt
  included, and the next session after a pause does the same (6 sessions in 5 minutes, then
  `Too many Realtime sessions`). That fits "TURN allocates, then nothing", but this code is older
  than 11-12, so it would need a change outside our code on that date to start it.
- Something changed outside our code around 00:00-01:00 UTC on 11-12, in several countries at
  once: the SFU's answer, the TURN service, or players' networks. The rooms all failing together
  point at the network or the device that the members share, not at the room.

## Outside causes (searched 2026-11-14)

- [Cloudflare's status page](https://www.cloudflarestatus.com/history) shows no Realtime TURN
  incident around 11-12; the [Realtime changelog](https://developers.cloudflare.com/changelog/product/realtime/)
  has no TURN change then (its last entry before is the WebSocket adapter going GA, 09-30).
- One [community report](https://community.cloudflare.com/t/turn-over-udp-gets-no-response-from-turn-cloudflare-com-in-the-philippines/980005)
  from 11-12: TURN over UDP to Cloudflare's TURN address got no answer on 3478, 443 and 53 from
  one machine in the Philippines. The same date, but UDP, and our relay is TLS; no reply from
  Cloudflare.
- [Cloudflare's TURN credentials](https://developers.cloudflare.com/realtime/turn/generate-credentials/):
  the API returns `turn:` on UDP 3478 and 53, TCP 3478 and 80, and `turns:` on 5349 and 443;
  the docs ask browsers to drop port 53 (they block it), and the caller sets the credentials'
  TTL. Our room asks for one hour (`TURN_TTL_S`), and the game renews its session (new
  credentials) before they end; it uses only `turns:` (TLS), 443 first. Nothing in the docs that our code does not do.
- Other projects' threads (WebRTC apps on Cloudflare's TURN) report restrictive networks that
  reset TLS on 5349 but pass 443, and replies that list no `turns:` on 443. Our game took
  `turns:...:443` only when the reply listed it, else the first `turns:` entry. It now always
  tries 443 first (on the listed host), and the listed port after any failure on 443 before the
  relay carried data, or after 5 s without it (review fix, below).

## Changes

- Two paths, independent failures (`sfu.rs`). A UDP send that fails outright (not `WouldBlock`)
  used to fail the whole association, the TURN attempt included, and the next session did the
  same. Now it fails the UDP path only: the relay is asked at once and the association lives on
  it; only when UDP was the path in use, or TURN is off, or TURN failed too, is the association
  failed. Direct input is ignored once UDP has failed.
- Every failure names its path, step and kind (`trouble.rs`): steps `send`, `receive`,
  `connect`, `credentials`, `tls_connect`, `tls_handshake`, `allocate`, `channel_bind`,
  `refresh`, `first_data`, `link`; kinds from the I/O error (`timeout`, `refused`, `reset`,
  `unreachable`, `denied`, `dns`...), the TLS library (`tls`), or the TURN error code
  (`turn_400`... `turn_508`). The log reads `net: SFU: TURN failed at allocate (turn_400)`, and
  with both paths gone `no path to the SFU: UDP failed at send (denied); TURN failed at
  allocate (turn_400)`. No address, credential or id in any of them; a TLS failure adds the TLS
  library's own words. A relay still up but not connected 20 s after the answer (`CONNECT_TIMEOUT`) ends the
  attempt, named by its stage with `timeout` (`tls_connect`, `allocate`, `channel_bind`,
  `first_data`, or `connect` once the SFU answered through it): `no path to the SFU: UDP failed
  at send (denied); TURN failed at first_data (timeout)`, and the send thread's retry with its
  growing pause (1 s, doubling to a minute) goes on. The first version kept such a relay waiting
  for ever, its refreshes keeping the association alive and no new session tried (review fix).
- 443 first, the listed port next (`RELAY_TRY`, review fix). Before the relay carried anything,
  a server that fails at any step (TCP or TLS, allocate, channel bind, the link) or carries
  nothing within 5 s of its credentials gives way to the listed port on a new allocation; the
  listed port has the rest of the 20 s. The first version fell back only on a failed TCP or TLS
  connect, so a network that passes TLS on 443 and drops what follows lost 5349, which 0.8.4
  used directly when it was the only one listed. Worst cases from the room's answer: UDP silent,
  TURN asked at 2 s, credentials at about 2.5 s, the listed port from about 7.5 s; a UDP send
  that fails outright asks for TURN at once, the listed port from about 5.5 s. A link is given up
  20 s after the answer at the latest (15 s after an offer never answered, `ANSWER_TIMEOUT`).
  The room's race deadline is 10 s from the race's start message (`SFU_CONNECT_MS`): a racer
  whose 443 is dead and whose session starts with the race makes it on the listed port only just
  (about 8.5-9.5 s with the offer and answer), one connected in the lobby is not affected.
- `sfu_trouble` (docs/online-protocol.md): once per path and association the game sends
  `{path, step, kind, version}` to a room whose welcome lists `sfu_trouble`; the room logs
  `Realtime trouble: turn/allocate/turn_400 0.8.5 EU (room CODE, member 2)`, 12 a member in
  5 minutes at most, and drops a report with any other word. A member's report times go when it
  leaves (`leave()`, the one place a member is removed) and when the room ends (review fix: they
  stayed for the room's life, and member ids only go up). Today's server answers an unknown
  `sfu_` type with an error and keeps the connection (an unknown type outside `sfu_` closes it);
  today's game ignores a message type it does not know. A game sends the report only to a server
  that lists the feature, so neither side needs the other first.
- Tests: the net crate's fake TURN server (`turn.rs` `FakeServer`) and an in-memory relay link
  run a UDP send error with a healthy relay (connects via TURN), both paths failing (one line,
  both steps named, two reports), a silent relay (the attempt ends at the deadline, both paths
  reported, the retry set after its pause), and 443 whose allocation is never answered or is
  refused (the listed port connects); the server's `trouble.test.ts` checks the word lists, the
  line, the rate limit, members joining, reporting and leaving (the map keeps the host's entry
  alone), and that an unknown `sfu_` type keeps the connection.

## Decisions

- No guess at the cause: the change makes the failing step visible and removes the one path in
  our code (a UDP send error ending the TURN attempt) that could take a relay racer out without
  the relay failing. The lead decides the deploy and the release.
- A relay that never connects now costs a session and a retry, as a failed attempt did before
  this lane (the room's limit is 6 sessions in 5 minutes): waiting on it for ever kept a player
  out of every race with nothing tried.
- The report carries fixed words and the release only, never the error's own text: an OS or
  library message can hold an address.

## Surprises

The Workers Logs query API samples long windows without saying so (24 h: 153 of the lines; the
calculations view reports `sampleInterval`). Count with windows of an hour or less.

## Result

Open: waiting for the deploy (the log line and the feature) and a release (the report, the
hardening, 443 first).

Checked:
- 0.8.5 gate on cffb5e7b: `check.py port`, `check.py server` and `check.py docs` OK; boot and race state
  traces equal the reference build.
- This PC, two hidden games, private rooms on the official server after the server's `sfu_trouble`
  deploy: an SFU room (UDP on both), the same with the guest relay-only (`DHD_NET_SFU_UDP=0`: TURN over
  TLS on port 443, connected 2.8 s after the answer, allocation 52 ms after the credentials) and a P2P
  room; `1. Host 1:10.03  2. Guest 1:12.03` on both games each time; no `failed at` line, so no report
  was sent; zero `refused`, `freeze:` and `crash:` lines, no other player's address in the SFU logs.
- PC <-> Mac (the Mac's own build): a private SFU room on the official server, `1. PCHOST 0:20.03
  2. MACGUEST 0:40.03` on both. A real UDP block (a failing send) and a dead 443 were run only by the net
  crate's tests, not on a real network.
