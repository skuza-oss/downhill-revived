# 180: P2P rooms, chosen per room (host star)

Status: current

Branches: `feat/p2p-prototype` from `main` db3702d (the prototype, 2026-11-08), then
`feat/p2p-clean` from its tip b592126 (the cleanup, the fixes and the tests below, 2026-11-08),
then `fix/p2p-review` from local main 2634978f (the merge review's fixes, 2026-11-09).

## Goal and acceptance

A second kind of online room beside the SFU one. The room's creator picks SFU or P2P; the room
keeps it; SFU stays the default. In a P2P room the guests connect to the host's game, which
forwards their race frames and runs the room's rules; Cloudflare TURN (over TLS on 443) is the
automatic fallback. Up to 10 humans, within the tracks' limits; any platform can host while in
the foreground. Host loss ends the room; a guest who drops becomes the host's AI rider. The race's
synchronization is the SFU room's (findings 044, 169, 173): only the transport and the room's
coordinator differ.

Owner's decisions of 2026-11-08: P2P rooms are private (joined by code, never listed; the game
says "Players in this room can see your network address."), reach players as a **test
prerelease** first, and run on the official Cloudflare account with TURN.

Acceptance: `check.py port` and `check.py server` pass; boot and race state traces equal the base
build's (main db3702d, release 0.7.13); a PC-host LAN run that selects a direct path; a 6+ racer
P2P room; the PC/Mac matrix (SFU and P2P, both hosts, forced TURN); a test package. Out of scope:
native iOS (the design allows it: the coordinator and the transport are plain Rust), host
migration, rejoin into a P2P room, automatic SFU fallback.

## Progress

- [x] 2026-11-08 (prototype, Sol): str0m peers with the shared TURN/TLS client, the host's
  coordinator, the browser bridge, the isolated P2P Worker, the PC/Mac benchmark tools (c97f715,
  04bf605, b592126).
- [x] Server: private rooms, the SFU rooms' guard (caps, moderation log, bans), readable
  `p2p.ts`, routes shared by both Workers (d9dbd2c).
- [x] Per-room choice, private rooms, 7-character codes, P2P-only on Android and the browser,
  the address notice (c3b5d44).
- [x] The PC-host LAN cause found and fixed (answer-only host candidate); coordinator moved into
  `dhd_session`; send thread and peer cleanup (fec0ca1, b02d367).
- [x] Test tools: `p2p_crowd.py`, `local_server.tunnelled`, cross_bench `--server local` and
  `--env`, nettool P2P bots (5284051, 18282b7).
- [x] Checks: `check.py port`, `check.py server`, boot 600 and race 5000 field traces equal
  0.7.13's; native 2- and 10-game room tests; PC-host and Mac-host LAN runs direct; 7-racer room;
  the 24-trial LAN matrix (0c86d38).
- [x] Test package: Windows and macOS from 3b3920f (not published); the packaged Windows dhd.exe
  hosted a P2P room with two bots (one relayed, one direct), all raced, no freeze.
- [x] 2026-11-09: merged with main 7fabfc9 (findings 181, 182, 184) on `merge/p2p-main` (456b974c):
  `check.py port`, `check.py server`, boot and race traces equal to 0.7.13's, the native 2- and
  10-game room tests.
- [x] 2026-11-09: the internet matrix, 24 trials, Mac on a phone hotspot (below).
- [x] 2026-11-09: test prerelease 0.7.14-p2p-test1 on `prerelease/0.7.14-p2p-test1` (never main):
  Windows and macOS packages, not published; the packaged Windows game hosted a P2P room.
- [x] 2026-11-09, `fix/p2p-review` (the merge review): a peer whose path stops answering fails
  instead of freezing its rider for good; TURN credentials renewed for long rooms; the official
  `wrangler.jsonc` back to what `tools/deploy.mjs` can upload (P2P rooms from
  `wrangler.selfhost.jsonc` for local and self-hosted servers); P2P codes masked in dhd.log;
  signaling errors without the text that failed; a P2P room's storage deleted when it ends, and
  every guest's credentials revoked with the host; the prerelease version commits from
  `prerelease/0.7.14-p2p-test1` (update checks, installer, test tools; not its version commit).
  Checks below ("The merge review's fixes").
- [ ] Lead: deploy the P2P Worker right before publishing, the billing alert, publish the
  prerelease.

## What was simplified (feat/p2p-clean)

- **One choice, made by the creator.** The prototype had a global `net_backend` dhd.ini setting
  and an ONLINE row toggling it, so a guest had to set the same backend before joining a code;
  the browser forced it by environment variables. Now HOME has CREATE A P2P RACE and the code's
  length (7 for P2P, 6 for SFU) routes a join: no setting, no probe request.
- **Platforms.** The prototype turned all online play on for Android, SFU included.
  `host_service_net_offered` now returns NET_OFFER_* bits: the desktop offers both, Android and
  the browser P2P only (the AGENTS.md exception covers P2P rooms only). The menu builds its rows
  from them (row kinds instead of row numbers).
- **Server.** The P2P routes moved out of `index.ts` into `p2p.ts` (`p2pRoute`), which both the
  main Worker (local and self-hosted servers) and the official P2P Worker (`p2p-entry.ts`, which
  no longer imports the SFU Worker) use. The lobby index, `/v1/p2p/lobbies` and the host's
  per-second `p2p_listing` storage writes are gone; `p2p_capacity` is sent when MAX PLAYERS
  changes. Members get the SFU rooms' guard (`tally` caps sized for signaling, the moderation join
  log, install-id bans) and the same close codes (`iso_mismatch`, a new `host_left` 4014).
- **Rust.** The coordinator is the `dhd_session` crate (no `#[path]` include of a file compiled
  twice); its timings are named constants; `Action` reads Game/Frame/Peer/Server. `p2p.rs` is a
  `P2pRoom` with one method per step and shares `WsConn::deliver` / `write_now` with the SFU send
  thread (the delivery and socket-write code had been copied). `peer.rs` takes its STUN Binding
  from `turn.rs` (the second hand-written STUN parser went), has named limits, logs each peer's
  path once, and tests the answer-only socket and the newest-state queue.
- **Kept**: the bench instrumentation (`DHD_NET_BENCH`, `cross_bench.py`, `analyze_netbench.py`;
  removed in finding 185, the per-peer counters stay as `DHD_NET_PEER_STATS`); the browser bridge (`p2p.js`, `check_p2p.mjs`).

## Decisions

- The owner authorizes a second room kind, peer addressing and P2P on Android and in the browser
  (AGENTS.md). Gameplay synchronization stays shared with the SFU.
- **7-character P2P codes** instead of a lookup: the code says which service to join, so a join
  costs no extra round trip, and a room that shares addresses gets a 31^7 code space.
- **Private only, enforced twice**: the server keeps no listing and ignores a `public` request;
  the coordinator never sets `public`; the menu locks ROOM to P2P, PRIVATE. The notice shows on
  the CREATE A P2P RACE row (footer, 45 characters: "Players in it can see your network
  address.") and for 15 s in the room's status line when a P2P room opens (the owner's sentence).
- **The official server runs P2P as its own Worker** (`wrangler.p2p.jsonc`, route
  `dhd.skuza.app/v1/p2p/*`), same account, D1 and TURN key: a P2P deploy during the test
  prerelease never restarts an SFU room. The main Worker still serves the same routes for local
  and self-hosted servers. The P2P Worker's v2 migration deletes its unused lobby index.
- **The answer-only host candidate** (Surprises): each peer offers two host candidates on one
  address; the second sends nothing until the other game reached it. Holding back only the guest
  would have moved the failure to Mac-host.
- **Peers bind to the internet-facing address** (a route lookup to the STUN server), not the room
  connection's: with a local server or an ssh tunnel that is 127.0.0.1.
- A guest the host can't reach in 20 s is removed with `reason: "unreachable"` (the server says
  "Your game could not connect to the host's."), its rider in a running race the host's AI.
- The test prerelease is **0.7.14-p2p-test1** (owner), its version commit on its own branch.
  The launcher's and the game's update checks, `build_launcher.py` and `package_release.py` read
  a prerelease's "-suffix" there (a prerelease comes before its release, so a tester's launcher
  offers 0.7.14 when it is out); the Windows installer's file version takes the numbers only.
  Those commits suit main as well.
- **Merge with 184 (rollouts):** P2P rooms get no restart-for-new-code hook. An SFU room restarts
  between races and its games rejoin; a P2P game never rejoins (its host carries the race), so a
  restart would end the room. P2P rooms take the deferred code updates instead (the main Worker's
  `code_update_strategy`, now also in `wrangler.p2p.jsonc`: a room keeps its code until it ends,
  24 h at most), and the P2P Worker is deployed with `wrangler deploy`, not `tools/deploy.mjs`.
- **Merge with 182 (call gate):** a P2P room's TURN calls go through its own `CallGate` (5 at a
  time): ten members joining at once would otherwise pass the Durable Object's six connections.
- **A lost path ends the peer (review, 2026-11-09).** str0m reports ICE `Disconnected` once the
  path in use stopped answering (about 22 s with its default timing: 15 s after the other side's
  last check, its RECENT_BINDING_REQUEST, and its own retransmissions) and never closes the
  connection itself, so `is_alive()` stayed true and both games kept a frozen rider. A connected
  peer whose ICE stays disconnected for 5 s (ICE_LOST_GRACE: room for a pair still being checked,
  a late relay candidate) now fails like any other: the host's coordinator gives the rider to its
  AI, a guest whose host is gone leaves; the browser already did this on `failed`. str0m's timing
  is unchanged (a shorter check interval would also change how connections come up).
- **TURN credentials renewed (review).** An allocation has no time limit but each Refresh needs a
  valid credential, and Cloudflare documents updating credentials on a live allocation (Realtime
  TURN FAQ; `setConfiguration` in browsers). The room mints each member fresh credentials 45 min
  after the last (TURN_RENEW_MS, 3/4 of the 1 h TTL; a failed renewal retries a minute later,
  silently) and sends `p2p_turn` again; the native TURN client re-keys and signs an immediate
  Refresh of the same allocation with them (`turn::Client::renew`, once no request is in flight),
  the browser calls `setConfiguration`. New peers use the newest credentials, so a guest joining
  a room older than an hour gets a relay too. Both the current and the previous credentials are
  revoked when the member leaves. The SFU path renews between races instead (finding 177), as its
  sessions are replaced anyway.
- **The official main Worker's config has no P2P class (review).** Cloudflare refuses `wrangler
  versions upload` for a version with a pending Durable Object migration ("must be applied with
  `wrangler deploy`", reproduced on a temporary Worker), and `tools/deploy.mjs` always uploads
  versions, so the merge's `v2-p2p` migration in `wrangler.jsonc` would have stopped the next
  official deploy. `wrangler.jsonc` is again the official main Worker's (v1 only, no P2P_ROOM);
  `tools/selfhost-config.mjs` writes `wrangler.selfhost.jsonc` (gitignored) from it with the P2P
  binding and `v2-p2p`, for local test servers (`local_server.py`, `npm run dev`, `dev:guard`) and
  self-hosted servers (`wrangler deploy --config wrangler.selfhost.jsonc`); the tests add the
  binding in vitest.config.ts. A main Worker without the binding answers `/v1/p2p/*` with "This
  server has no P2P rooms". A generated file rather than a named environment: wrangler's
  environments inherit neither bindings nor vars, so one would repeat the whole file.
- **P2P codes masked in dhd.log (review).** Anyone holding an open P2P room's code can join and
  see the host's address, and players post dhd.log: `net_log_code()` (port/net/net.h, for the core
  and the host) and p2p.rs's `log_code` show a 7-character code as `AB*****`; SFU codes as they
  are. `DHD_NET_LOG_CODE=1` (docs/TESTING.md) keeps them whole for the tools that read the host's
  code from its log (cross_race, cross_bench, p2p_crowd, online_race set it).
- **Signaling errors say which step failed, never the text** (review): str0m's parse errors quote
  the SDP or candidate, with the other game's addresses.
- **A P2P room forgets itself (review).** When the host leaves (or goes silent), or the last
  connection is gone (a code claimed by a game that never said hello), the room deletes its alarm
  and storage; the object keeps `closed` in memory while it lives, so the code stays refused, and
  once evicted it is unknown. The guests' credentials are revoked on that path at once, not when
  (or if) their sockets' close arrives.
- **The P2P Worker's migrations stay as they are (review).** Its v1 creates `LobbyIndex`, which
  `p2p-entry.ts` doesn't export, and v2 deletes it; a fresh deploy (a temporary workers.dev Worker
  from `wrangler.p2p.jsonc`, 2026-11-09) applied both in one upload and served rooms, then
  redeployed: Cloudflare checks the end state. No migration added, no applied tag changed.
- **Merge with 181 (Room Region):** the region row is an SFU row (a P2P room's server only relays
  signaling, so where it runs doesn't matter), placed after Create A Private Race and before Create
  A P2P Race.

## Surprises

- **PC-host never connected directly on the LAN: the Mac refused flows the game opened.**
  Temporary per-socket counters (DHD_NET_P2P_TRACE, removed) on both games, PC hosting: the PC's
  checks went out to the Mac's LAN candidate (no send errors, no firewall drop on the PC side),
  but on the Mac every `send_to` toward the PC (LAN, srflx, relay addresses) failed with
  EHOSTUNREACH, and the PC's checks never reached the Mac's socket (0 LAN datagrams received).
  Mac hosting, the same builds: the PC learns the Mac's candidate from the offer and sends first;
  the Mac's LAN sends then all succeeded; only its own first sends to public addresses failed.
  A small ad-hoc signed C program sending the same way over ssh, blocking or not, never failed,
  and the Mac's application firewall is off: something filters the game app's own UDP flows
  (an AdGuard network extension and a Proton VPN extension are installed and active there).
  Holding the Mac guest's direct sends until the PC's first ICE packet arrived gave a direct LAN
  path with the PC hosting (both ends `udp/lan`, ICE RTT 5.5 ms). Not Windows Firewall,
  candidate gathering, interface choice or mDNS (native peers use no mDNS). The ICE role mattered
  only because the answerer learns the other's candidates first and so sends first.
- **The fix, proven** (cross_bench, LAN, final code, 30 s windows):

  | PC/Mac, P2P | Path at both ends | EVENT RTT median / p95 (PC, Mac) | Echoes |
  |---|---|---|---|
  | PC hosts, before (prototype, 2 trials) | TURN/TLS | 30.5-30.8 ms | all |
  | PC hosts, now | direct LAN | 15.8 / 19.2, 15.5 / 19.2 ms | 147/147, 145/145 |
  | Mac hosts, now | direct LAN | 16.3 / 17.2, 16.7 / 20.0 ms | 146/146, 147/147 |

  The Mac still logs its one refused send (`net: P2P: this computer refused a direct send`).
- With a local test server both games bound to 127.0.0.1 (the room connection's address) and
  went through TURN; fixed (Decisions). The prototype only ever met the official server.
- nettool session bots re-sent LOADED every 0.5 s and their rider once: a host still in its
  menus keeps only its newest 128 frames (NET_RELAY_QUEUE), so the rider was pushed out and the
  host waited the full 30 s for it. Real games send their rider and wait for everyone's before
  any LOADED, so this is the bots'; they now repeat both every 2 s. With 6+ humans the sixth bot
  mirrored an AI rider that doesn't exist (10 riders in all) and sent no snapshots; fixed.
- The `freeze:` lines a scripted run can show at the load screen are the rider exchange's wait
  (port/hle/online.c `exchange_rides`, up to 30 s) for a game still in its menus; the SFU room
  has the same wait.
- **The Mac never left the home LAN**, although the plan said it was on a phone hotspot: for the
  whole session it had the PC's public address and the home router as its gateway (checked three
  times). So the internet matrix was not run; nothing in this finding measures the internet.
- **A relay outlived its expired credentials** (2026-11-09, `cloudflare_turn_*` tests, real TURN):
  a relay-only pair whose first credentials lasted 60 s and were never renewed kept carrying
  events for 11 more minutes (its Refresh at about 9 minutes went through); revoking them then
  answered 404. Cloudflare's FAQ says an expired credential's allocation is "disconnected after a
  short delay"; when is not pinned down, so the renewal stays and no test relies on the cut. The
  renewed pair's Refresh with the new credentials was taken at once, and it carried events 150 s
  past the first credentials' end (`cloudflare_turn_takes_renewed_credentials_on_the_live_allocation`).
- Cloudflare's 1,000 GB monthly free Realtime allowance is **shared between SFU and TURN**
  (developers.cloudflare.com/realtime/sfu/platform/pricing/); TURN bills the relay's egress to
  its client at $0.05/GB. A relayed guest receives about 30 kB/s in a 7-racer room (below), about
  110 MB an hour.

## 6+ racer P2P room (2026-11-08, `p2p_crowd.py`)

PC host (the game, scripted), 5 nettool P2P bots on the PC (one with DHD_NET_P2P_UDP=0, relay
only) and the Mac's game: 7 racers, one race, this branch's server through an ssh tunnel, LAN.

| | Run b (relay bot, Mac) | Run c (same, after the bot fix) |
|---|---|---|
| Room open / host countdown after open | 1.0 s / 55.4 s | 1.0 s / 63.4 s |
| Raced | all 7 | all 7 |
| Paths at the host | 5 direct LAN, 1 relay | 5 direct LAN, 1 relay |
| Host upload, all peers | 196 kB/s | 185 kB/s (31-32 kB/s per guest) |
| Host CPU (one core = 100 %) | 79 % | 74 % |
| Snapshot age at the host, p50 / p95 / p99 | Mac 38/45/46, bots 19-27/36-44/43-51 ms | Mac 36/44/46, bots 19-26/37-46/44-60 ms |
| `freeze:` lines, host / Mac | 2 / 2 (rider-exchange waits) | 0 / 2 (the Mac waited 8 s for the host's rider) |

The setup time is mostly the scripted host's own run from the room through its menus to the load
screen (about 20 s) and the rider exchange; a menu-driven room runs the same steps. The age is the
receiving game's clock against the sender's stamp, both on the server's clock (client.c's 30 s
lines). Bots mirror the host's AI at 30 Hz; the Mac sends its own rider at 60 Hz. The host's CPU
includes the game itself at 60 fps (no P2P-free baseline measured; PC free mode, no timing runs).
An earlier run with a local server only (all games on 127.0.0.1, 6 racers): 139 kB/s upload, 78 %.
Evidence: build/check/p2p-crowd-7b, build/check/p2p-crowd-7c (local, not tracked).

## PC/Mac matrix on the LAN, final code (2026-11-08)

`cross_bench.py --matrix --rounds 3 --seconds 30 --server local`: 24 trials, each case 3 times
(automatic paths in alternating order), PC on Ethernet, Mac on Wi-Fi, same LAN, VPN off, two
humans plus eight host AI, 60 fps. Reliable EVENT probes, application round trip on the sender's
clock; per case the median of the 6 endpoint medians and of their p95s, and the worst p95.

| Case | Path (both ends) | RTT median | p95 median / worst | EVENT echoed |
|---|---|---|---|---|
| SFU, PC hosts | UDP to the SFU | 22.1 ms | 30.3 / 291.4 ms | 877/877 |
| SFU, Mac hosts | UDP to the SFU | 22.3 ms | 30.3 / 34.0 ms | 878/878 |
| P2P, PC hosts | direct LAN | 16.3 ms | 19.3 / 20.2 ms | 877/877 |
| P2P, Mac hosts | direct LAN | 16.1 ms | 19.8 / 20.0 ms | 879/879 |
| SFU, forced TURN, PC hosts | TURN/TLS | 23.4 ms | 35.5 / 46.6 ms | 880/880 |
| SFU, forced TURN, Mac hosts | TURN/TLS | 22.7 ms | 36.6 / 64.7 ms | 880/880 |
| P2P, forced TURN, PC hosts | TURN/TLS | 29.3 ms | 47.4 / 50.8 ms | 879/879 |
| P2P, forced TURN, Mac hosts | TURN/TLS | 29.3 ms | 37.9 / 40.6 ms | 880/880 |

Every race ran at a median 60 fps. STATE probes: 1-4 echoes missing in some trials of both
kinds (the coalescing probe queue the prototype's run described, not loss). Each trial has one
`freeze:` line on the Mac, SFU and P2P alike: the load screen's rider exchange (the Mac reaches
`race setup` first and waits about 5 s for the PC's rider), not the race. Direct P2P is about
6 ms faster than the SFU here with either host; a relayed P2P pair is about 6-7 ms slower than
the SFU's own relayed path (two relay legs through Cloudflare against one). This is one LAN, not
the internet. Evidence: build/check/netbench-lan-final (local, not tracked).

## The internet matrix (2026-11-09)

The merged build (456b974c) on both machines: the PC on its home connection (Ethernet), the Mac on
a phone's hotspot (cellular, Proton VPN off; checked first: a different public address, the
hotspot's gateway). The rooms on two temporary workers.dev Workers deployed from the merged
server, one for the SFU rooms (with its Realtime and TURN secrets) and one for P2P rooms (the
official P2P Worker's code), both deleted afterwards. `cross_bench.py --matrix --rounds 3
--seconds 30 --server SFU --p2p-server P2P`: 24 trials, each case 3 times. EVENT probes; per case
the median of the 6 endpoint medians and of their p95s, the worst p95 and the worst single echo.

| Case | Path (PC / Mac) | RTT median | p95 median / worst | Max | EVENT echoed |
|---|---|---|---|---|---|
| SFU, PC hosts | UDP / UDP | 52.9 ms | 74.0 / 104.9 ms | 1037 ms | 875/875 |
| SFU, Mac hosts | UDP / UDP | 52.3 ms | 74.7 / 183.1 ms | 987 ms | 878/878 |
| P2P, PC hosts | UDP to the Mac's relay / TURN | 67.6 ms | 130.7 / 181.6 ms | 284 ms | 878/878 |
| P2P, Mac hosts | UDP to the Mac's relay / TURN | 62.3 ms | 78.2 / 103.9 ms | 463 ms | 879/879 |
| SFU, forced TURN, PC hosts | TURN / TURN | 54.7 ms | 70.9 / 77.0 ms | 279 ms | 876/876 |
| SFU, forced TURN, Mac hosts | TURN / TURN | 58.7 ms | 111.7 / 183.9 ms | 376 ms | 870/870 |
| P2P, forced TURN, PC hosts | TURN / TURN | 62.7 ms | 88.5 / 124.4 ms | 624 ms | 878/878 |
| P2P, forced TURN, Mac hosts | TURN / TURN | 63.9 ms | 90.5 / 149.4 ms | 315 ms | 880/880 |

Snapshot age on arrival, the games' own 30 s lines while racing (median of the windows' p50 and
p95, worst p95):

| Case | p50 | p95 / worst | Gaps over 100 ms (all windows) |
|---|---|---|---|
| SFU, PC / Mac hosts | 48 / 49.5 ms | 76-80 / 209 ms | 55 / 114 |
| P2P, PC / Mac hosts | 46.5 / 54 ms | 69-90 / 142 ms | 128 / 14 |
| SFU forced TURN, PC / Mac hosts | 47.5 / 50 ms | 69 / 247 ms | 21 / 29 |
| P2P forced TURN, PC / Mac hosts | 52 / 56 ms | 69-74 / 151 ms | 15 / 34 |

- **No direct P2P path through the hotspot.** In every P2P trial the Mac's end was its TURN relay
  (the phone's carrier NAT lets no direct path through); the PC reached that relay over UDP. So an
  automatic P2P pair here is one relay leg, and P2P's RTT is about 10-15 ms above the SFU's (whose
  two UDP legs go to a Cloudflare location near each player).
- Every race ran at a median 60 fps; every EVENT probe was echoed; STATE echoes missing 3-8 per
  case (the coalescing probe queue, as on the LAN).
- 7 `freeze:` lines in 48 game runs, all on the Mac and all the load screen's rider wait
  (`waiting for 1 player's riders`, about 5 s), SFU and P2P alike; none in a race.
- Evidence: build/check/netbench-internet (local, not tracked).

## Billing alert (owner)

Cloudflare dashboard > Billing > Billable usage > Create budget alert: an account-level spend
threshold (for example $5 a month), which covers Realtime (SFU and TURN together). Optionally a
usage notification for Realtime under Notifications. TURN in P2P rooms is used only by guests
whose direct path fails.

## Platform status

Windows and macOS (M1) native: built, LAN races both ways, a 7-racer room. Android: compiles as
before (P2P only); not run on a phone. Browser: the coordinator is the same crate; `check_net.mjs`
passes; `check_p2p.mjs` (real Chromium WebRTC) and the browser build were not rerun after the
crate move. Linux, iOS: not run. TURN credentials are renewed every 45 minutes (Decisions); a
failed relay no longer ends a peer that runs on a direct path (it did in the prototype), and a
peer with no working path at all ends after the ICE grace.

## The merge review's fixes (2026-11-09, `fix/p2p-review`)

| Check | Result |
|---|---|
| `a_connected_peer_whose_path_stops_answering_fails` (peer.rs) | fails without the fix (no error in 30 s), passes with it (the peer fails about 20 s after the other went silent) |
| TURN unit test (`renewed_credentials_sign_a_refresh_of_the_same_allocation`) and the real-TURN renewal test | pass |
| `npm test` (P2P: renewal, revoke both on leave, host-left revokes and storage, a silent host's room forgotten) and `npm run typecheck` | pass |
| `wrangler deploy --dry-run`: `wrangler.jsonc`, `wrangler.p2p.jsonc`, `wrangler.selfhost.jsonc` | pass |
| Temporary workers.dev Workers (deleted): the merge's config through `versions upload` | refused (the bug) |
| The same, this branch's `wrangler.jsonc` through `tools/deploy.mjs --emergency --no-checks` | uploaded and deployed |
| That Worker redeployed from `wrangler.selfhost.jsonc` (a self-hosted server adding P2P) | `v2-p2p` applied, P2P route served |
| A fresh P2P Worker from `wrangler.p2p.jsonc` | deployed (v1 and v2 at once), served, redeployed |
| `check.py port`, `check.py server` (on the branch's last commit) | OK |
| Boot (600 fields) and race (5000) state traces against 0.7.13's | every row equal |
| Native P2P room tests through `local_server.py` (now `wrangler.selfhost.jsonc`) | 2 and 10 games joined, raced, got the results; dhd.log showed the code as `A5*****` |
| `p2p_crowd.py --bots 6 --turn-bots 1`, local server, PC only | 7 racers, all raced; paths 5 direct LAN, 1 relay; countdown 43 s after the room opened; host 73 % of a core, 177 kB/s up; snapshot age at the host p50 20, p95 36-37 ms; no `freeze:` line |

Not done: no PC/Mac or internet run (not needed for these fixes); the browser's renewal
(`setConfiguration`) is not exercised by a test (check_p2p.mjs was not rerun).

## Result

Merged into `main` 2026-11-09 (with the review's fixes). A room's creator can make a private P2P
room (ONLINE > Create A P2P Race, a 7-character code); the guests connect to the host's game,
with Cloudflare TURN as the fallback, and race with the SFU room's synchronization. SFU stays the
default and is unchanged. Over the internet (Mac on a phone hotspot) P2P measured 62-68 ms
against the SFU's 53 ms, every P2P pair relayed by the carrier's NAT; on the LAN P2P is direct and
faster (about 16 ms against 22). The official server serves P2P rooms from its own Worker
(`wrangler.p2p.jsonc`); the main Worker's `wrangler.jsonc` has no P2P class. P2P reaches players
as a test prerelease first (owner, 2026-11-08).
