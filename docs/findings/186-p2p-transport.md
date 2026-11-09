# 186: P2P as a race transport of the server's rooms (hub star, SFU fallback)

Status: in progress
Supersedes: parts of 180 (its coordinator in the host's game, its own server and Worker, TURN between games)

Branch `feat/p2p-transport` from local `main` 3518ce84, started 2026-11-09; main 10520d2e (finding
187) merged in on 2026-11-09. This file is the lane's plan while it runs and its finding when it
merges.

## Goal and acceptance

The owner's decisions (2026-11-09). Today there are two room implementations: SFU rooms run their
rules in the server's Room (`online/server/src/room.ts`) and race over Cloudflare Realtime SFU; P2P
rooms (finding 180) run their rules in the host's game (`port/render/session`) behind a server of
their own (`src/p2p.ts`, `p2p-entry.ts`, the `dhd-revived-p2p` Worker). Make it one room
implementation with two race transports:

1. **One server room.** A P2P room is a server Room made with the P2P transport; the room runs every
   rule as for SFU rooms (ready, countdown, release, results, AI takeover, rejoin, region, rollout
   restarts between races). The coordinator, `src/p2p.ts`, `p2p-entry.ts`, the P2P-only client parts
   and the self-host config that exists only for the P2P class go.
2. **Star with hub migration.** Race frames go over WebRTC data channels in a star: one member (the
   hub, at first the creator) forwards the others' frames. The room picks the hub and relays the
   peer connections' signaling; when the hub leaves or its links fail, it picks the next one, tells
   everyone, the members reconnect, and a running race goes on.
3. **Per-member SFU fallback.** A member who can't reach the hub races over the room's SFU in the
   same room (mixed rooms). No P2P TURN credentials or renewals any more. SFU rooms unchanged.
4. **Shared code.** One client path for room logic on every platform (native `port/net`,
   `port/render/net`; the browser's `net.js`, `p2p.js`, `net_layout.js`); race synchronization
   (snapshots, events, puppets) exactly as now; the browser works with these rooms, still not offered
   to players.
5. **Privacy.** P2P rooms stay private and code-only (7 characters) with the in-game notice; a
   fallback member is exposed no more than P2P already implies; SFU rooms expose nothing; no address
   in dhd.log (codes masked).
6. **Compatibility.** 0.7.13 in SFU rooms works with the new server; protocol bumped only if
   unavoidable; test-p2p-1 keeps its own Worker until test-p2p-2.
7. **Fix:** a P2P guest got no results when the finish window ended the race (finding 185,
   Surprises); results must arrive in every case, with a test.

Done means: `python tools/check.py port` and `python tools/check.py server` OK; boot and race state
traces equal 0.7.13's; unit/server tests for hub choice and migration, SFU fallback, mixed-room
frame delivery, results at the finish window, a hub leaving mid-race; local runs (2 games + bots with
the hub leaving mid-race; 7+ racers with one forced onto the SFU; a normal SFU room; a 0.7.13
release game in a normal room on the new server); one PC/Mac P2P race on the LAN.

Out of scope: native iOS; offering online in the browser or on Android beyond what finding 180
allowed (P2P rooms).

## Design (plan)

```
                 room WebSocket (rules, signaling, routes)         Cloudflare Realtime SFU
   +-----------------------------------------------------+        +--------------------+
   |                 server Room (room.ts)               |        |  (fallback only)   |
   +-----------------------------------------------------+        +--------------------+
      |  p2p_signal / p2p_link / p2p routes  |                       ^  ^        ^
      v                                      v                       |  |        |
   [member A] ==peer== [ HUB ] ==peer== [member B]          hub's    |  |  F's own publication,
                         ||                               publication|  |  subscribed by the hub
                         ||  (bridge: star <-> SFU)                  |  |  and by other F's
                         ++------------------------------------------+  |
                                               [member F: no link to the hub] --+
```

- **Room.** `RoomState.transport = "p2p"`, `hub` (member id) and per member `link`: pending (none),
  `star` (linked to the hub) or `sfu` (fallback). Codes of 7 characters, never listed, `public`
  ignored. Creation: `/v1/rooms/new?transport=p2p` (no `/v1/p2p/` path: that is test-p2p-1's
  Worker's route). The room relays `p2p_signal` only between the hub and a member, takes link
  reports (`p2p_link`), and sends the routes (`p2p`: hub, each member's link, the running race's
  marker and slots) on every change.
- **Star.** The hub's game offers a peer connection to every member; each member answers. Race
  frames on a peer are the SFU's own format (the race marker, then the frame); the hub stamps each
  with its sender's slot and forwards it to the other racers. The hub is not necessarily a racer.
- **Fallback, bridged by the hub.** A member whose link doesn't come up within the room's
  `P2P_LINK_MS`, or whose link fails, is `sfu`. It races over the SFU like an SFU room's racer, but
  its subscriptions are only: to the hub's publication (which carries every star frame, each with
  its own sender's slot) and to the other fallback racers; the hub subscribes to every fallback
  racer and forwards their frames into the star. Star members never touch the SFU. Chosen over
  "every member also publishes and subscribes on the SFU": that would double every star member's
  upload whenever one member falls back and make each of them depend on its own SFU session.
- **Slots.** On the SFU of a P2P room a frame carries its sender's slot (each game stamps its own);
  a receiver keeps it from the hub's publication and requires a non-hub's to be its own slot (else
  dropped). SFU rooms keep stamping on receipt as before.
- **Hub choice.** First the creator. The next one when the hub leaves, when it has no linked member
  while a member that linked before is present, or when fallback racers exist and the hub has no
  working SFU session (it can't bridge): among present members, one that has been on a star (its
  network allows direct links), then one with a working SFU session, then the lowest measured room
  ping, then the earliest joiner.
- **Client.** One send thread in `dhd_net` for both kinds of room: the SFU association as today,
  and in a P2P room the peers (`peer.rs`) and a shared router (`port/render/star`, the browser's
  wasm too) that stamps, forwards and bridges frames and reports links. Results and every room
  message come from the server, so they reach every game however the race ended.

## Progress

- [x] 2026-11-09: finding reserved (3ab956cd).
- [x] Server: P2P rooms are Rooms with `transport: "p2p"`; hub, links, routes, signaling relay,
  hub choice, bridged SFU group, fallback deadlines; `src/p2p.ts`, `p2p-entry.ts`,
  `wrangler.p2p.jsonc`, `tools/selfhost-config.mjs` and their test deleted (569f5023). A star
  racer's failed SFU session takes nobody out of the race; routes before the start (7808a41a). A
  hub that is gone is replaced at once (9373bf4a). `test/p2p-rooms.test.ts`: 15 tests.
- [x] Client: `dhd_star` (`port/render/star`) replaces the `dhd_session` coordinator; `dhd_net`
  runs the star beside the SFU association in one send thread; the SFU association's P2P mode;
  no TURN between games (`peer.rs` direct only, `turn.rs` without the renewal); hello feature
  `p2p`, the kind of room from the welcome; rejoin as any room (fc5786a3). Tests: `dhd_star` 10,
  `dhd_net` 32 (one new), the local-server room tests 2, 10 games and the hub leaving (pass).
- [x] Browser: `net.js` and `p2p.js` on server rooms and the star as wasm; `check_net.mjs`,
  `check_p2p.mjs` (real Chrome) pass (693730e7).
- [x] Docs and tools: the protocol's P2P section, STATE, TESTING, `p2p_crowd.py --sfu-bots`,
  `cross_race.py`, `online_race.py --kill-host-at` (6fe476de and the next).
- [x] 2026-11-09: `check.py port`, `check.py server` (218 tests) OK on d30c8e32; boot (600) and race
  (5000) state traces equal 0.7.13's (Result).
- [x] Game runs on local servers: the hub killed mid-race; 7 racers with one on the SFU (real
  Realtime); a normal SFU room; 0.7.13's release games in a normal room (Result).
- [x] One PC/Mac P2P race on the LAN (Result).

## Decisions

- **The hub bridges; star members never use the SFU.** The alternative, every member also
  publishing and subscribing on the SFU when someone falls back, doubles each star member's upload
  and makes every one of them depend on its own SFU session at once. Here only the fallback
  racers and the hub take part in the race's SFU side, every ordered pair of them subscribed (that
  is exactly a fallback racer to the hub and to the other fallback racers, the hub to them): the
  room's existing subscription pass runs unchanged on that group (`bridged`, `sfuGroup`).
- **Slots on a P2P room's SFU are the sender's.** Each game stamps its own slot; a receiver keeps
  the slot of frames from the hub's publication (it carries every star racer's) and drops another
  publisher's frame unless it carries that publisher's own slot (`dhd_star` `sfu_frame`): so no
  "relay" flag on subscriptions, and frames from an earlier hub are dropped once the routes change.
  SFU rooms keep stamping on receipt, unchanged.
- **Every member still makes its SFU session on joining**, as in an SFU room: a member can fall
  back or become the hub at any time, and an idle session costs about nothing (finding 177).
- **Link decisions belong to the room**: a link is up when either end says so, down when either
  end says so or after `P2P_LINK_MS` (10 s), and decided once per try; a fallback member tries the
  hub again between races. The clients only report; the server's routes say who is where, so the
  hub and the members never disagree about a frame's way.
- **A peer message is the SFU's frame format** (race marker, then the frame) behind a kind byte;
  keepalives each second, a link silent 5 s is down (the race's frames would stop for 27 s before
  ICE says so). Room commands no longer travel over peers.
- **Hub choice**: present members first; one that has been on a star, then a working SFU session,
  then the lowest room ping (the server's own measure of a member's connection; the games measure
  no link between two members), then the earliest joiner; a member whose own working link died in
  the last minute comes last, and is never picked while the hub is still there. Replaced at once
  when it leaves or its room connection drops; after 12 s with fallback racers waiting and no
  working SFU session (by a member with one); and, not within a minute of the last change, when
  no member links to it although direct links work for some (two or more members, all on the
  SFU, their lost links settled 5 s). One member's link going down never moves it (review).
- **Results come from the room** (item 7): `ended` and the room's `lastRace` go from the server to
  every member's socket however the race ended, so the bug of finding 185 (the host's game
  stopped before it sent them) is gone by construction; server test "results reach every member
  when the finish window ends the race".
- **No protocol bump.** SFU rooms see no new message; a P2P room needs the hello's `p2p` feature
  and a 7-character code; the welcome's `transport` and the `p2p`, `p2p_signal`, `p2p_link`
  messages exist only in P2P rooms. A game takes the kind of room from the welcome, so during a
  canary a P2P creation that lands on a previous version's object (which ignores the transport)
  gives an SFU room with a 7-character code, which the game then races as an SFU room.
- **No `/v1/p2p/` path** on the new server: `dhd.skuza.app/v1/p2p/*` is test-p2p-1's own Worker's
  route while it exists; new rooms are `/v1/rooms/new?transport=p2p`, so the two never meet.
- **Android**: P2P rooms stay the only rooms offered there (finding 180's exception); a P2P room's
  SFU fallback on a phone is part of that room.
- **The browser has no SFU client yet** (it stays not offered): a page that can't reach the hub is
  out of the race at the fallback deadline, and a page hub with fallback racers gives the hub to a
  game with a working SFU session. Its star is the native games' code.

## Surprises

- In the first room tests every racer left its race a few seconds after the start: the local server
  has no Realtime, each game's SFU session failed, and the room retired racers whose session failed,
  as an SFU room does. In a P2P room only a fallback racer needs the SFU (7808a41a).
- A hub that dies would have sent every member to the SFU (each link silent, reported down)
  before the room's 20 s rejoin wait ended; without a working SFU path they would then have left
  the race at the fallback deadline. The room now replaces a hub when its room connection drops or
  when every link that worked died (9373bf4a).
- `rustgs.py test` always passed the renderer's `native` feature, so `-p dhd_net` alone failed;
  `-- -p dhd_net -p dhd_star` now runs those crates' tests through the wrapper.

## The lead's review (2026-11-09)

An independent read-only review of the branch found nothing that blocks live SFU rooms and eight
things to fix, each fixed in its own commit with a server or client test that fails without it:

| # | Found | Fix (commit) |
|---|---|---|
| 1 | The "every link to it died" rule moved the hub at once and without limit: `hubLinked` stayed true for the room's life, so one member's blip (or a false report) with the others on the SFU moved it, and every star racer reconnected mid-race | The instant rule went; the hub moves for its links only after HUB_RETRY_MS since the last change, with two or more members all on the SFU and their lost links 5 s old, and never to a member whose own link died in the last minute (`lostLinkAt`; a forced change takes it last) (8be70ed8) |
| 2 | A hub whose SFU session was being renewed (a new offer clears it) looked "unbridged" and was moved mid-race | 12 s of grace (`unbridgedSince`, UNBRIDGED_GRACE_MS) before a hub without a working session is replaced (7de2a51c) |
| 3 | One fallback racer not ready held back the bridged group's release; a hub that can't bridge got its fallback racers retired at 25 s before the 60 s rule could move it; racers released through an earlier hub were skipped by the deadlines and could race on with no route | The hub and the ready fallback racers are released each when ready; the hub moves (to a member with a working session) rather than its racers being retired while one exists; a hub change clears the releases, so a racer that falls back again has its deadlines again (41e21af5) |
| 4 | A member back from a drop kept its old link deadline and went to the SFU without trying the hub | Its link time starts when it is back, and doesn't run while it is away (35ed08bb) |
| 5 | No hub epoch: an old hub's late offer or candidates reached the new hub, and the native hold and the page's pending applied them | The routes carry the hub's `epoch`, every `p2p_signal` too; the room, the native send thread and the page drop another epoch's (ccdc28f6) |
| 6 | The server's logs and the moderation log carried whole P2P codes | `logCode` shows them by their first two characters, as dhd.log does; `moderate.mjs joins --room` matches that (f9b20a6f) |
| 7 | 400 text frames of 16 kB per window let one member send about 1.3 MB/s of signaling to the hub | 64 kB of signaling per member per window; the excess dropped, the first answered with `too_fast` (29bac0f8) |
| 8 | `wrangler.jsonc`'s comment named the deleted P2P Worker files | Rewritten (41f3486e) |

The reviewer also checked and found fine: the SFU path is unchanged for SFU rooms (their release,
stamping on receipt, deadlines and sessions); privacy (no address, hash or install id in any room
message, signaling only between the hub and a member, codes masked in dhd.log); results always
from `closeRace` to every member's socket; the clients' slot stamping (the hub stamps by
connection, an SFU frame from a non-hub must carry its own slot).

After the fixes: `p2p-rooms.test.ts` 24 tests (server 227), `dhd_net` 33 and `dhd_star` 10, the
local-server room tests (2 games, 10 games, the hub leaving mid-race) and `check_p2p.mjs` pass;
`check.py port` and `check.py server` OK (on 41f3486e; the core is unchanged by the fixes: `build_wasm` took the cached module).

## Merged with finding 187 (2026-11-09)

Main moved to 10520d2e (finding 187, live as server version b1e58d0e: a race link that delivers
nothing is reported and repaired, dead stream ids never reused, the start moment also over the
room). Merged into this branch with a merge commit (05e4053b, conflicts kept both sides), then:

| Commit | What |
|---|---|
| c7334257 | 187's server rules for a P2P room's SFU side: a dead session (`poisoned`) and `sfu_silent` repaired with a new session for a fallback racer and for the bridging hub (racing or not); a star racer's dead session replaced at once (its race runs on the star) and its `sfu_silent` ignored; the hub never taken out of its race for the SFU (the renewal deadline neither); a hub with a dead session can't bridge (`bridges`); fallback deadlines spare a racer being renewed; 187's log lines mask P2P codes |
| 6e326ff7 | The SFU association's silence counts only its part of a P2P race (the publishers the star names: a star racer's never reports); the page takes and names the start moment over the room (`race_go`) |
| 713132d3 | A bridging hub still silent after its one renewal is only logged (with one fallback racer, that racer's upload may be the cause, and moving the hub reconnects every star racer); a hub with no working session still moves (checkHub) |
| 56128744 | No race-frame silence rule on a star link (below) |
| 65481c57 | `rustgs.py`'s cache key had no star sources (nor `dhd_session`'s before): a star-only change linked a stale library (`test_rustgs_key.py`) |
| 349edca9 | The merge put 187's `heard = true` in the SFU room's branch of `receive_frame` only: in a P2P room the hub and every fallback racer reported silence 8 s after the release whatever came, and fallback racers then left the race |

Decisions:

- **A star racer's silence is the keepalives'.** Asked what silence means for a star member: the
  keepalives travel on the reliable channel the race's events use, and a star's two channels are
  negotiated once and never closed or reused, so 187's dead stream ids can't happen on a star; a
  link whose keepalives stop for 5 s is down (link failure, the SFU fallback) as before. A
  race-frame silence rule (no race frame from the hub 3 + 8 s after the start) was tried
  (6e326ff7) and dropped (56128744): it took a racer off the star while the hub's game was still on
  its way to the load screen, where the rides are first sent.
- **The bridging hub reports and is renewed once** (the brief's same repair), never retired or
  moved for silence; a fallback racer is renewed once, then out of the race, as in an SFU room.
- **`race_go`** needs nothing P2P-specific: the room's namer is the lowest slot still racing,
  whoever the hub is; server test "the start moment comes over the room in a P2P room too, from
  the next racer when the hub leaves before it". The page now sends and takes it too.
- SFU rooms and 0.7.13 games: 187's code paths unchanged (the helpers return what they did for an
  SFU room; `silent-link.test.ts` passes as merged).

Checks on the merged branch (2026-11-09, this PC): `check.py port` and `check.py server` (238
tests) OK on 349edca9; boot (600) and race (5000) state traces equal 0.7.13's (`state_diff.py` PASS);
`dhd_net` 35 and `dhd_star` 10 tests; the local-server room tests (2 games, 10 games, the hub
leaving mid-race) and `check_p2p.mjs` pass. Runs on local servers with real Realtime:

| Run | Result |
|---|---|
| 187's forced-silent guest in an SFU room (`online_race.py --bots 2 --b DHD_NET_SFU_SILENT=1`), final build | as in 187: "no race frame from any of the 3 other racers 8 s after the release", a new session released 3.2 s after its start message, "the start moment came from the room", countdown 1.5 s, 618 messages received by race clock 301, results on both games |
| P2P, nettool hub + star bot + fallback bot with `DHD_NET_SFU_SILENT=1` | before 349edca9: hub and fallback both "silent", the fallback out of the race; after: only the fallback reports, a new session released 2.1 s later, it races and finishes (120 frames received) |
| P2P, the hub's game, two star bots, a fallback game (`DHD_NET_P2P_UDP=0`, `DHD_NET_SFU_SILENT=1`) | the fallback game reports, a new session (released 2.1 s after), the start moment from the room, countdown 1.5 s, 634 messages by race clock 301, results on both games; the hub's game reported silence once (its fallback game sent nothing until its load screen, about 30 s after the start in this scripted flow): one renewal, logged the second time |

Risk (187's, SFU rooms included): a racer whose others reach the load screen more than about 11 s
after the start message (the scripted test flow here: 20-35 s with three games on one PC) reads
as a silent link: one renewal, and a fallback or SFU racer silent again leaves the race. The menu
flow reaches the load screen sooner; production logs will tell.

## Result

One room implementation, two race transports. A P2P room is a server Room created with
`?transport=p2p`: the room runs every rule (ready, countdown, release, results, AI takeover, rejoin,
region, rollout restarts) as for an SFU room; its race frames travel in a star of WebRTC data
channels around the hub the room picks and replaces; a member who can't reach the hub races over
the room's SFU, bridged by the hub; results always come from the room. The design: above (Design,
Decisions); the wire: `docs/online-protocol.md`, "P2P rooms".

**Deleted** (`git diff --stat main`, 55 files, +2,392 -2,537; code alone, inline Rust tests included,
+1,340 -2,106; tests +669 -254): the host's coordinator `port/render/session` (`dhd_session`, 875
lines) and the old send thread of `p2p.rs`; the P2P server `online/server/src/p2p.ts` (426),
`p2p-entry.ts`, `wrangler.p2p.jsonc`, `tools/selfhost-config.mjs` and the generated
`wrangler.selfhost.jsonc` (local servers and `npm run dev` use `wrangler.jsonc`), `test/p2p.test.ts`;
TURN between games (`peer.rs`'s relay, `p2p_turn`, the 45-minute renewal, `turn::Client::renew`,
their tests); the `host_left` close (4014) and the P2P rooms' "no rejoin" special case in `net.c`.
New: `port/render/star` (`dhd_star`, the router, 420 lines with its wasm glue), the Room's P2P part
(about 330 lines of `room.ts`), the send thread's star (`p2p.rs`, `lib.rs`).

**Protocol** (no version bump; nothing new in SFU rooms): `/v1/rooms/new?transport=p2p` (7-character
code), the hello feature `p2p` (a P2P room refuses a game without it), the welcome's
`"transport":"p2p"`, and in P2P rooms only `p2p` (routes), `p2p_signal`, `p2p_link`; text frames up
to 16 KB for `p2p_signal` and 400 text frames per caps window there. Peer messages: kind 1 (race
marker + frame) or 2 (keepalive). Gone: `/v1/p2p/*`, `p2p_welcome`, `p2p_members`, `p2p_turn`,
`p2p_capacity`, `p2p_kick`, room messages over peers.

**Checks** (2026-11-09, this PC; the Mac an M1 Air on the same LAN):

| Check | Result |
|---|---|
| `python tools/check.py port` (d30c8e32) | OK |
| `python tools/check.py server` | OK, 218 tests (15 in `p2p-rooms.test.ts`: hub choice, migration when the hub leaves, drops or loses every link, no second change within a minute, a hub that can't bridge, links up/down/timeout, bridged subscriptions only, a racer falling back mid-race, a star racer's failed SFU session, the fallback deadline, results at the finish window, rejoin) |
| `rustgs.py test -- -p dhd_star -p dhd_net` | 10 + 32 pass (`a_mixed_room_delivers_every_racer_to_every_racer_once`, `no_game_speaks_for_another_rider`, the SFU association's P2P mode...) |
| `local_server.py -- rustgs.py test -- -p dhd_net p2p_room -- --ignored` | 2 games, 10 games, and `p2p_room_hub_leaves_mid_race`: all joined, raced, heard every racer, got the results |
| `check_net.mjs`; `local_server.py -- node port/web/check_p2p.mjs` (real Chrome) | pass: two pages in a P2P server room, linked directly, frames both ways, results from the room |
| Boot (600 fields) and race (5000) state traces, this build against 0.7.13's release `dhd.exe` (`--env DHD_LAUNCHER=1`) | every row equal (`state_diff.py` PASS both) |
| A. P2P room, 2 games + 1 bot, the hub's game killed 20 s into the race (`online_race.py --a DHD_NET_CREATE=p2p --bots 1 --kill-host-at 20`, no Realtime) | the room: "member 1 is the hub now (the hub's connection dropped)"; the guest linked to the new hub (direct, LAN) and raced on; the bot finished at 1:00; the finish window ended the race and the guest and the bot got the results (`online: result 1. Bot0 1:00.00` ... 10 lines) |
| B. 7 racers, one bot on the SFU (`p2p_crowd.py --bots 6 --sfu-bots 1`, real Realtime) | all 7 raced and finished, no `freeze:`; the room made 2 subscriptions (hub and the bot, each way) during the countdown and released 17 ms after the start; 5 bots direct on the LAN; the SFU bot heard every racer (snapshot age p50 25-41 ms) and every racer heard it through the hub (p50 27 ms; at the hub 24 ms); host 74.5 % of a core, 139 kB/s up; countdown 42 s after the room opened |
| C. Normal SFU room, 2 games + 1 bot of this build (real Realtime) | both games got the results; released 10 ms after the start |
| D. Normal SFU room, 0.7.13's release games + a bot of this branch (`online_race.py --exe <0.7.13 dhd.exe> --bots 1`) | both got the results; released 10 ms after the start |
| E. PC/Mac P2P race on the LAN (`cross_race.py --backend p2p --host pc --races 2`; the Mac's game built from this branch with the PC's wasm2c core, a local server through the ssh tunnel) | both races ran on both and closed with the results on both (PCHOST 0:20.03, MACGUEST 0:40.03); the link direct inside the LAN at both ends (the Mac's one refused send, as in finding 180, the answer-only candidate took over); no `freeze:` line; snapshot age while racing p50/p95 16/26 ms at the PC, 19/32 ms at the Mac; when the run stopped the PC's game the Mac's became the hub |

**Open.** The browser has no SFU client (a page that can't reach the hub doesn't race; a page hub
gives the hub away when racers fall back); Android, Linux and the internet were not run; a
mid-race fallback's first events can be lost while its subscriptions are made (state resumes);
during a deploy's 10 % canary a P2P creation that lands on an object of the previous version gives
an SFU room with a 7-character code (the game races it as one: it takes the kind from the welcome).

**The lead's steps.**

1. Merge, then deploy the main Worker as usual: `node online/server/tools/deploy.mjs` (no new
   Durable Object class: `wrangler.jsonc` is unchanged, so `versions upload` works). Busy rooms
   take the new code between races (finding 184); 0.7.13 in SFU rooms is unaffected (run D).
2. Leave the `dhd-revived-p2p` Worker deployed while test-p2p-1 is in testers' hands: its route
   `dhd.skuza.app/v1/p2p/*` keeps serving those builds' rooms from the code it was deployed with
   (the files are no longer in the tree; nothing redeploys it). The new client never uses
   `/v1/p2p/`.
3. Package test-p2p-2 from the merged main (P2P rooms need this client: the hello's `p2p`).
4. Once test-p2p-2 has replaced test-p2p-1: `npx wrangler delete --name dhd-revived-p2p` (its
   Worker, route, Durable Objects and secrets; not the shared `dhd-moderation` D1). From then on
   test-p2p-1 can't open P2P rooms. Its TURN credentials expire within the hour by themselves.
5. Nothing to set on the main Worker: a fallback racer uses the Realtime and TURN secrets it has.
   P2P rooms no longer use TURN between games; a fallback racer's traffic is billed as an SFU
   racer's (finding 180's billing alert covers both).
