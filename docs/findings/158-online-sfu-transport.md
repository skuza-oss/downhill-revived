# 158: race frames over Cloudflare Realtime SFU, and puppets with less delay

Status: in progress

Branch `feat/online-sfu` from `main` 113f76f, started 2026-11-06. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

Online races keep state sync (finding 044): each PC runs its own race and shows the others as
puppets. What changes is how the race frames travel and how long a puppet waits.

Today every STATE and EVENT frame goes over the player's WebSocket (TCP) to the room's Durable
Object, in one city near the room's creator, which copies it to the other racers
(`online/server/src/room.ts`, `FRAME_STATE`). Two costs follow:

- **Detour:** Durable Objects run in about 30 cities. Rooms for South America, Africa and India
  run on another continent (finding 135), so two neighbours can be 120 ms apart one way.
- **Stalls:** on TCP one lost packet holds every later frame until it is resent.

On top of the network, a puppet is drawn 100 ms behind its newest snapshot
(`port/hle/puppet.c`, `DHD_PUPPET_DELAY`) and snapshots go out every 3 race ticks (20 a second).

The lane has two parts.

**Part 1, the game side (any transport):**
- Snapshots every 2 race ticks (30 a second).
- A puppet delay that adapts to each sender: about one snapshot interval plus that sender's
  measured arrival jitter, with a floor near 50 ms and a ceiling near 150 ms.
  `DHD_PUPPET_DELAY` still forces a fixed delay.
- `net:` lines in dhd.log gain each sender's snapshot age (median, 95th and 99th percentile, in
  ms) and the gaps longer than 3 snapshot intervals, in the existing 30 s lines (finding 135).

**Part 2, the transport:**
- Race frames travel over Cloudflare Realtime SFU DataChannels. Every player connects to the
  nearest Cloudflare location; the SFU forwards across Cloudflare's network.
- Each racer publishes two channels: `state` (unordered, `maxRetransmits: 0`) for STATE frames,
  and `events` (reliable, ordered) for EVENT frames. Each racer subscribes to every other racer's
  two channels.
- The Durable Object stays the lobby and the control plane: it creates the SFU sessions and
  channels through the Realtime API with the app's token (a Worker secret), and passes each
  client only its own SDP answer and channel ids over the existing WebSocket.
- The WebSocket race relay (`FRAME_STATE`/`FRAME_EVENT` forwarding) is deleted when the SFU path
  works: one code path (AGENTS.md). Binary PING/PONG on the WebSocket stays for the ping counter.
- `PROTOCOL` becomes 3; `MIN_VERSION` is raised at the release, by the lead.

**Hard rule: no player ever receives another player's IP address.** No peer-to-peer links, no
TURN mesh. A client talks to Cloudflare only. Client SDP goes to the Realtime API through the
Durable Object and is never forwarded to another player.

Acceptance:
1. `python tools/check.py quick`, `python tools/check.py port` and `python tools/check.py server`
   pass. The server tests cover session and channel setup against a fake Realtime API, and a test
   asserts that no message sent to a client carries another client's SDP or any address field.
2. Deterministic offline runs are unchanged: boot and race state traces equal the base build's
   (`docs/TESTING.md`).
3. With Realtime credentials present (`online/server/.dev.vars`, never committed),
   `port/tools/online_race.py` runs a two-client local race over the SFU: puppets move, and the
   `net:` lines show snapshot ages for both senders.
4. With a client-side test switch that drops a share of outgoing `state` messages (for example
   5%), no puppet freezes: the gap count stays at the dropped share and later snapshots arrive on
   time.
5. This file records the measured snapshot ages before (WebSocket) and after (SFU) on the same
   setup.

Out of scope: online on Android or in the browser (finding 143; the client library must not rule
them out later), cheating checks, the puppet extrapolation rules, room placement.

## Progress

- [x] 2026-11-06: lane plan (this file).
- [ ] Part 1: 30 snapshots a second, adaptive puppet delay, snapshot-age `net:` lines.
- [ ] Spike: a native Rust client connects to the Realtime SFU, publishes an unordered channel,
      a second client receives it. Library chosen and recorded under Decisions.
- [ ] Server: SFU sessions and channels from the Durable Object; protocol 3 messages documented
      in `docs/online-protocol.md`.
- [ ] Client: `dhd_net` SFU transport; `port/net/client.c` sends and receives race frames on it.
- [ ] WebSocket race relay deleted; tests and docs updated.
- [ ] Measurements recorded (acceptance 3-5).

## Decisions

- 2026-11-06 (the user): players' IP addresses must never reach other players, so direct
  peer-to-peer is out.
- 2026-11-06 (the user): the SFU design over keeping the Durable Object relay, own UDP relays, or
  a TURN-forced mesh. A mesh sends nine copies per player and still needs relays; own relays need
  servers to run. Cost on the current plan: SFU and TURN egress is free up to 1,000 GB a month
  (Cloudflare Realtime pricing, read 2026-11-05); a full ten-player race is a few MB per player.
- State sync stays (finding 044). Research on how other fan ports and revival servers do online
  (2026-11-06) found no reason for lockstep or rollback. Most of them send race traffic player to
  player and expose IPs. The closest relay designs are PPSSPP's relay-only ad hoc server and the
  MotorStorm Pacific Rift revival's DME relay.

## Surprises

## Result
