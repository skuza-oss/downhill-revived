# 186: P2P as a race transport of the server's rooms (hub star, SFU fallback)

Status: in progress

Branch `feat/p2p-transport` from local `main` 3518ce84, started 2026-11-09. This file is the lane's
plan while it runs and its finding when it merges.

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
- [ ] `check.py port`, `check.py server`; boot and race traces against 0.7.13.
- [ ] Game runs (local server): 2 games + bots, the hub killed mid-race; 7+ racers with one on the
  SFU (real Realtime); a normal SFU room; a 0.7.13 release game in a normal room.
- [ ] One PC/Mac P2P race on the LAN.

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
  no link between two members), then the earliest joiner. Replaced at once when it leaves, when
  its room connection drops, or when every link that worked died; not within a minute for having
  no link at all (direct links work for others) or for having no SFU session to bridge with.
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

## Result
