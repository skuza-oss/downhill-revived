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

- [x] 2026-11-09: finding reserved.

## Decisions

## Surprises

## Result
