# 248: Racers on the TURN fallback left out of every race

Status: in progress

Branch `fix/turn-players-out-248` from `main` 02a8bb3e (0.8.4), started 2026-11-14. Diagnosis
only so far: the cause is not proven, nothing in the code is changed.

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
- [ ] A player's `dhd.log` from an affected room (0.8.4 logs name the refused TURN request and the
  TLS family, finding 246), or a diagnostic that reports the failing phase to the room.

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

## Decisions

No code change: the cause is not proven, and the brief says not to guess. The next step is
evidence from a failing client: a player's 0.8.4 `dhd.log`, or a small client report (a
`sfu_trouble` message with the failing phase and error kind only: UDP send error, TURN TLS,
TURN refusal and its request, ICE, DTLS; no address, SDP, credential or id) that the room logs.
That one needs a server deploy and a release; the lead decides.

## Surprises

The Workers Logs query API samples long windows without saying so (24 h: 153 of the lines; the
calculations view reports `sampleInterval`). Count with windows of an hour or less.

## Result

Open.
