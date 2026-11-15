# 252: Why the TURN fallback fails (channel bind timeout, allocate 400)

Status: current

Branch `fix/turn-fallback-252` from local `main` cae6f05c (0.8.5 plus lane 247), started
2026-11-15. Follows finding 248, whose `sfu_trouble` reports named the failing steps.

## Goal and acceptance

The first hours of 0.8.5 and server 071ef967 logged 349 `Realtime trouble:` lines from 30 members
in 13 rooms (SA 297, ASIA 37, NA 12): every troubled member had `udp/connect/no_answer`, then
either `turn/channel_bind/timeout` (125, a few `turn/allocate/turn_508` and `turn/allocate/timeout`
beside it) or `turn/allocate/turn_400` (45). Find the cause of each and fix what is ours. Explain
why the lines say `0.8.4` and `0.8.5` in the same rooms. Done: `check.py quick`, `port`, `server`
and `docs`, and an SFU race and a relay-only race on the official server with equal results.

## Progress

- [x] 2026-11-15: our TURN client read against RFC 8656; Cloudflare's TURN service probed from
  this PC with our own credentials (scratch script, nothing tracked).
- [x] 2026-11-15: `channel_bind/timeout` and `udp/connect/no_answer` reproduced on this PC with
  the real SFU and TURN service, cause proven, fixed in the game (c3c8bfce) and in the room
  (49b7f341).
- [x] 2026-11-15: the `0.8.4` word explained: a stale cache entry of the Rust library (a13abddb).
- [x] 2026-11-15: `turn_400` not reproduced; the report now tells the two Allocates apart
  (eb04b53f).
- [x] 2026-11-15: `check.py quick`, `port`, `server` and `docs` OK; two races on the official
  server (below). No state trace: nothing here touches game state.

## What the SFU answers now

On 2026-11-15 every answer of Cloudflare's SFU to this PC wrote its candidate like this (address
and ufrag changed here):

```
a=candidate:513273236 1 udp 2130648831 203.0.113.5 1473 typ host ufrag XZSl... generation 0
a=candidate:513273236 2 udp 2130648831 203.0.113.5 1473 typ host ufrag XZSl... generation 0
```

str0m 0.25 (the games' WebRTC library) drops a candidate with the `ufrag ... generation 0` tail
without an error. The game was left with no remote candidate at all, so:

- no ICE check ever went out over UDP: `udp/connect/no_answer`, though UDP was fine;
- the TURN relay allocated, but str0m had nothing to send through it, so no ChannelBind was ever
  asked for, and the relay was given up at the stage it had reached: `turn/channel_bind/timeout`.

Evidence: the bare transport probe (`probe.rs`, `probe_sfu_one_way_delay`, the game's own stack
against the real SFU and TURN service) failed on this PC exactly as the players did: `ICE Checking`,
no datagram sent at all (counted in the transmit path), `TURN failed at channel_bind (timeout)`
on 443 and then on 5349. With the candidate written plainly (`... typ host`) the same probe
connected over UDP 291-312 ms after the answer, and with `DHD_NET_SFU_UDP=0` through TURN/TLS
2358-2409 ms after the answer, 150 of 150 snapshots both ways. In a unit test str0m sent 0 ICE
checks with the tail and 3 without it.

This fits finding 248's data: the trouble starts in rooms, not players (a room's sessions are made
by its room, and every answer of the affected SFU had the new form); every member of such a room
fails, whatever its network; the relay "carries" 50-460 bytes a minute. Cloudflare's TURN usage
for these members today is whole multiples of 228 bytes in and 204 out, that is Allocate exchanges
only: the TURN service never saw a ChannelBind (one is 240 bytes). Why the change reached some
places on 2026-11-12 and this PC only now is Cloudflare's to say; it reaches everyone sooner or
later, so every SFU race is at risk until the fix is out.

### The fix

- The room (`online/server/src/sfu.ts`, `plainCandidates`) writes each candidate of the SFU's
  answer plainly before it sends `sfu_answer`: the fields up to the type, then only `raddr`,
  `rport` and `tcptype`. This alone fixes every released game: the lead deploys it.
- The game (`port/render/net/src/sfu.rs`, `answer_candidates`) does the same itself, keeping only
  UDP over IPv4, component 1, as before; an answer left with no candidate is logged
  (`the answer has no UDP IPv4 candidate this game can use`), and ICE's progress before the
  association is up is logged (`ICE Checking ... ms after the answer`).
- Tests: the test SFU (`LocalSfu`) can now be ICE-lite, as Cloudflare's is (a server that sends
  its own checks let the client find it without any candidate, so the old tests could not see
  this), and its answer can be rewritten as Cloudflare writes it. Both new association tests,
  over UDP and through the test TURN server, fail without the fix and pass with it.

## The `0.8.4` lines

The released 0.8.5 `dhd.exe` (Windows) and the Mac's `dhd` carry `#define DHD_VERSION "0.8.4"`
in their Rust library; the Android, iOS and Linux binaries carry `0.8.5` (checked in the
packages, read-only). `trouble.rs` reads the version with `include_str!` from
`port/include/version.h`, outside `port/render`, and the object cache key of the Rust library
(`port/tools/rustgs.py`) covered only files under `port/render` and four WGSL files. The lane 248
library built on the tree before the version commit was taken from the cache for the release. The
same gap covered `port/hle/state_bundle.h`, which `bundle.rs` includes: a change to the bundle
layout alone would not have rebuilt the library either.

Fix: those files are in the key (`INCLUDED`), and the dep-info check after every real build
(`inputs_gate`) now refuses a build that read a file of the tree the key does not cover. So the
lines' version word is the platform: `0.8.4` = Windows or Mac, `0.8.5` = Android, iOS or Linux.
Of the 125 `channel_bind/timeout` lines 102 came from PCs and 23 from the others; all 45
`turn_400` lines came from the others (phones, or Linux), none from a PC.

## `turn_400`

Not reproduced. What the probes of Cloudflare's TURN service (443 over TLS, this PC, our own
credentials) proved:

| Request | Answer |
|---|---|
| Allocate right after the credentials were made | success |
| Allocate with credentials revoked 1 s before | success (revoking takes effect later, if at all) |
| Allocate after a 60 s TTL ran out (70 s) | success |
| Allocate with a wrong password | **400** |
| Allocate with an unknown username | **400** (80 ms: looked up) |
| A second Allocate on the same TLS link | 437 |
| Allocate on 5349 and on another 443 link while one is up | success both |
| Authenticated Allocate with another link's nonce | 438 (our client retries) |
| ChannelBind to the SFU's address, any port | success |
| ChannelBind of a peer already bound to another channel | 400 |
| ChannelBind to private, loopback, Cloudflare's own and IPv6 peers | 401 |

So a 400 on the Allocate means the TURN server could not check our message integrity: the
username is unknown where the player connects, or the key (username, realm, password) differs.
Ruled out: our MD5 on another platform (its constants are computed with `sin`, but none lies
within 0.015 of a rounding edge), the nonce (plain ASCII letters and digits), a second
allocation, old or revoked credentials. Not testable here: TURN over IPv6 (this PC has no IPv6
route) and the phones' own networks. Finding 246 saw the same 400 on a phone on the lead's Wi-Fi
(0.8.2, its TLS link took the first address the phone's resolver gave, maybe IPv6) while the PC on
the same network allocated.

One room followed through its log (it ran in Singapore, made from Jakarta, three members on
0.8.5, so phones or Linux): every attempt of every member ended `turn/allocate/turn_400`, about
5 s after its credentials (443 first, then the listed port), for four minutes. Cloudflare's TURN
usage has no line for these members: no allocation ever succeeded.

The report could not say which Allocate got the 400: the first, sent without credentials (whose
answer should be 401 with the realm and nonce), or the authenticated one. It now can: the first
is its own step, `challenge` (game `trouble.rs` and `turn.rs`, room `TROUBLE_STEPS`,
`docs/online-protocol.md`). `turn/challenge/turn_400` would mean the TURN server refuses our
Allocate itself where these players connect; `turn/allocate/turn_400` that it does not take their
credentials there. A room before this change drops a report with the new word (it logs nothing),
so the room goes first.

## Decisions

- Fix the candidates on both sides: the room's rewrite reaches every released game at once; the
  game's own keeps it safe against a server that does not rewrite (a local test server, an old
  deploy) and against the next change of the answer's form.
- No change to the TURN client for `turn_400`: nothing proven, and the rewrite may change what
  phones report (with the candidates back, a phone's UDP connects and its TURN is never asked).

## Surprises

- The UDP failure was never the players' networks: with no candidate, nothing was sent at all.
- The old tests could not see it: the test SFU was a full ICE agent whose checks let the client
  learn its address without any candidate.

## Result

Proven and fixed: the SFU's new candidate form left every game without a candidate
(`udp/connect/no_answer` plus `turn/channel_bind/timeout`); the stale `0.8.4` word. Not proven:
why phones get `turn_400`.

Checked:
- `check.py quick`, `port`, `server` and `docs` OK on eb04b53f; `dhd_net` 50 tests.
- This PC, two hidden games of this branch, rooms by code on the official server (not yet
  rewriting the answer, so the game's own fix carried them): an SFU room, both `connected via UDP`
  590-618 ms after the answer, and the same with the guest relay-only (`DHD_NET_SFU_UDP=0`): TURN
  over TLS on 443, allocation 43 ms after the credentials, `connected via TURN/TLS` 2921 ms after
  the answer, relayed datagrams both ways. `1. Host 0:20.03  2. Guest 0:40.03` on both games each
  time; no `failed at`, `freeze:` or `crash:` line.
- The room's rewrite only by its unit test (`sfu.test.ts`): a released game against the rewritten
  answer was not run (no local server with the keys in this worktree). After the deploy the
  `Realtime trouble:` lines with `channel_bind/timeout` and the `out of the race: no Realtime
  connection` lines should stop for every version; that is the check.

Left: deploy the room (the rewrite and the `challenge` word), then release the game. If phones
still report, `turn/challenge/turn_400` or `turn/allocate/turn_400` says which Allocate fails.
