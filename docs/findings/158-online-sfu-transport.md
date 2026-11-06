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

- Part 1 committed as `0a16e3c`; quick and C loopback/session tests passed.
- Protocol 3 server fake API tests and typecheck pass, including private SDP/allocation messages.
- Native str0m transport compiles on Windows. Native loopback delivered 95/100 STATE messages with 5% injection, the reliable EVENT,
  and corrected a forged sender slot from its channel identity. All three dhd_net unit tests
  also pass with the patched crypto adapter. The patched native archive passes the unchanged path/linkage gate. The final serial port tier passes. Credentials arrived during final validation; live harness checks are now running.

- [x] 2026-11-06: lane plan (this file).
- [x] Part 1: 30 snapshots a second, adaptive puppet delay, snapshot-age `net:` lines.
- [ ] Spike: a native Rust client connects to the Realtime SFU, publishes an unordered channel,
      a second client receives it. Library chosen and recorded under Decisions.
- [x] Server: SFU sessions and channels from the Durable Object; protocol 3 messages documented
      in `docs/online-protocol.md`.
- [x] Client: `dhd_net` SFU transport; `port/net/client.c` sends and receives race frames on it.
- [x] WebSocket race relay deleted; tests and docs updated.
- [ ] Measurements recorded (acceptance 3-5).

## Decisions

- DataChannels prefix race frames with the existing room startAt value as an opaque race marker.
  No new clock sample, tick agreement or lobby field is introduced. Separate WebSocket control
  and SFU streams can reorder at race boundaries: early reliable events buffer until start,
  old-race frames are discarded, and all message callbacks run on the send thread. This keeps
  the association across races without replaying an old finish/pickup into the next race.

- Native library choice: str0m 0.25 with default features disabled and `rust-crypto`.
  The sans-IO loop fits the existing dedicated transport thread, supports negotiated channels
  and MaxRetransmits(0), and its RustCrypto/dimpl backend avoids system OpenSSL on all three
  desktop targets and Android. MIT/Apache-2.0 licences are GPL-compatible. webrtc-rs remains
  the second candidate but adds an async runtime and shared connection tasks we do not need.
  Sources: [str0m](https://docs.rs/str0m/0.25.0/str0m/),
  [Cloudflare channels](https://developers.cloudflare.com/realtime/sfu/features/datachannels/),
  [connection recipes](https://developers.cloudflare.com/realtime/sfu/get-started/connection-patterns/),
  [limits](https://developers.cloudflare.com/realtime/sfu/platform/limits/).
  The live API requires the initial offer in sessions/new. The client supplies its own SDP;
  the DO sends it to that endpoint and returns only that session's SFU answer. A native negotiated
  bootstrap channel reserves stream 65534 so application allocations cannot collide with stream 0.
  No client SDP goes to another client. The empty-session/establish recipe in the docs failed
  against the live service; see also Cloudflare docs issue 28488 (sessionDescription omitted
  from the published session-create schema).

- Part 1 uses an RFC 3550-style EWMA of absolute arrival-spacing variation (gain 1/16).
  Delay is the estimated interval plus twice jitter, clamped to 50-150 ms; the environment
  override remains fixed. The final age metric uses the existing per-sender EVENT_GO stamp
  (server clock) plus snapshot race time, sampled on network ticks into bounded millisecond
  histograms. Thus p50/p95/p99 include transit and time waiting for a newer snapshot. It is an
  estimate subject to each peer's server-clock offset error and simulation clock drift; values
  below zero clamp to zero, and bin 1001 means over one second. No new wire fields are needed.
  Gaps count arrival intervals over 100 ms. The follow-up metric commit also reads the v3 batch
  header, so it can be applied to Part 1 for the WebSocket baseline before comparing ages.

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

- The live spike completed: two clients published/subscribed through the SFU, puppets moved,
  both finished at 70 s and agreed on results. The first run overlapped a baseline build; its
  timing numbers are a spike only. Comparison/loss runs wait until builds finish.
- The old one-second puppet lag diagnostic still used the fixed-delay sentinel (-1 s), producing
  bogus negative lag while actual interpolation was correct. It now shares the adaptive delay
  calculation with rendering. The new snapshot-age net lines were already correct.

- Do not overlap two host builds in this worktree: one validation attempt collided while copying
  a generated header. Machine-wide build slots do not serialize a single worktree's outputs.
  Finish the current host build, then run the port tier serially.

- The native test caught DTLS receive-queue overflow when several datagrams were fed before
  polling str0m. The transport now drains outputs after every datagram and after each timeout.
- Initial native transport uses IPv4 UDP to SFU candidates; no TCP/TURN path is implemented.
  Readying waits for all current members' negotiated channels to open. Reliable events queue
  during setup; snapshots retain only the newest per rider under congestion. Queue overflow
  fails the connection for reliable messages instead of silently losing gameplay events.

- Upstream str0m's RustCrypto adapter enables dimpl's rcgen feature, which forces AWS-LC and
  silently selects its DTLS provider. The unchanged archive gate rejected absolute source paths
  in AWS-LC's Windows objects. A minimal vendored adapter patch disables that feature, uses
  rcgen/ring (already linked for HTTPS) for certificates, and explicitly selects RustCrypto DTLS.
  The final graph contains neither AWS-LC nor OpenSSL. See vendor/str0m-rust-crypto/PATCH.md.
  Upstream source revision is 5f2183ce7c67117cd1960dfeb11bed11fc188b66, crypto/rust-crypto;
  the repository MIT licence and original source are retained.
- A snapshot used to be capped at 255 bytes and the initial pad could be silently truncated.
  Protocol 3 now sends one full 834-byte rider snapshot per STATE (payload version 4), at 30 Hz.
  This costs more bandwidth, especially for the host's AI, but restores controls after packet loss.
  Slot identity is bound to the channel allocation's member ID, mapped by each race's start.

- Credentials were initially absent, then arrived during final validation. Live harness checks
  proceeded without printing or copying credential values.
- Initial snapshots encoded pad deltas; protocol 3 sends independent snapshots and rejects
  stale packets before changing the mirror.

## Result

Validation completed:
- `python tools/check.py quick -j 6`: pass (core build and runtime tests).
- `python tools/check.py server -j 6`: pass, 94 tests and TypeScript typecheck.
- `python tools/check.py port -j 6`: pass after rerunning serially; native game build, C network
  tests, renderer checks and wasm2c chunk tests all pass. The native archive gate is unchanged.
- All three dhd_net unit tests pass through rustgs's environment and version gates. The native
  UDP/DTLS/SCTP test delivers 95 of 100 STATE messages with 5% outgoing loss, one reliable EVENT,
  and corrects a forged sender slot from the receiving channel's identity. This is a local
  stand-in SFU endpoint, not a live Cloudflare measurement.
- The C metric test covers GO timestamps, both snapshot layouts, stale-packet rejection for
  the estimator, per-sender long gaps and histogram overflow.
- Windows was built and tested here; native Mac/Linux builds and cross-machine races remain
  with the lead's merged verification. Online remains disabled on Android and in the browser.


Live acceptance 3-5 is being checked with the supplied credentials. The first harness attempt
stayed in the lobby because storing native fetch as an instance method supplied an invalid
this binding. The default fetch adapter now calls global fetch through an arrow function.
Signaling was also made client-offer/session-create-with-SDP/SFU-answer, as requested. No real SFU latency numbers, packet-loss race or
before/after comparison have been measured. Do not release this lane until the live two-client
spike and race pass. The lead also runs boot/race state traces against the shared reference build,
and the cross-machine race in both directions after merging.

Restart steps: provide the documented Realtime env/secrets in the server's gitignored `.dev.vars`,
run the native channel test and server suite, then use `local_server.py` with `online_race.py`.
Repeat with `DHD_NET_DROP_STATE=5` in the clients' environments; collect both 30 s `net:` windows.
For the WebSocket baseline, use Part 1 and apply only the `port/net/client.c` and
`port/net/client.h` changes from the GO-clock age metric commit; keep that baseline's protocol 2
and relay server. The metric reads both snapshot layouts, so compare the same estimator on
both transports. To repeat the native channel test with the normal CLI wrapper:
`python port/tools/rustgs.py test --target x86_64-pc-windows-msvc -- -p dhd_net -p dhd_render --lib native_channels`.


## Round 2: transport constraints and crypto review

Cloudflare [TURN service](https://developers.cloudflare.com/realtime/turn/) supports TLS/TCP
on 443 and is free with the SFU. The [credential API](https://developers.cloudflare.com/realtime/turn/generate-credentials/)
requires a separate TURN key ID/API token; SFU app credentials alone are insufficient.
No secrets were created or changed. str0m 0.25 is sans IO: its
[upstream README](https://github.com/algesten/str0m#nic-enumeration-and-turn-and-stun)
explicitly leaves TURN allocation and transport to the caller. It accepts relayed candidates,
but has no iceServers setting, TURN credential client, TLS stream framing, allocation refresh,
CreatePermission or ChannelBind loop. Merely retaining TCP candidates from the SFU answer
cannot provide a TCP socket or TURN transport. UDP-blocked access remains unimplemented.
The smallest option is one TURN/TLS adapter beside the UDP socket in Sfu: rustls on 443,
a maintained TURN client (licence/API review first), bounded stream queues, authenticated
Allocate/Refresh/permission maintenance, candidate registration and demultiplexing returned
UDP datagrams into str0m. The DO must mint expiring credentials for that race member only,
revoke on departure/end, and never broadcast them or peer SDP. This is feasible with str0m,
but is a separate transport implementation and needs blocked-UDP integration testing before
claiming support. No TCP fallback has been advertised to players.

Crypto review: latest upstream str0m/adapter/dimpl still have the forced AWS-LC dependency.
Cargo feature unification cannot subtract it. Keep the minimal adapter patch and the unchanged
archive gate; accepting embedded source paths would undermine the repository rule. PATCH.md
now gives the exact reapplication steps and upstream revision. No newer clean published option
was found. The reviewed upstream manifests are linked there.

Round 2 snapshot change: payload version 5 keeps the 104-byte fixed snapshot and sends
changed pad words six times, with a full 834-byte refresh every 30 snapshots. This handles
five consecutive lost changed updates; a longer burst recovers at the next received full
refresh. The receiver keeps a mirror, rejects stale timestamps before applying updates,
and validates every word before mutation. Race start clears mirrors and sender repetition
history. Guest-path unit tests cover five lost repeats, all repeats lost, periodic repair,
zeroing a released action, and malformed updates. Empty updates are 104 bytes; one repeated
word makes 109 bytes. Live distribution is still to be measured. The adaptive lag diagnostic
now uses the same calculated delay as rendering.

Round 2 abuse bounds: SFU offers are accepted only for active race slots, one attempt/session
per member per race (failed attempts consume the cap). The cap is persisted before external
I/O; queued mutations recheck membership and race identity. Clients now start WebRTC on start,
so lobby readiness does not allocate SFU resources. Race end destroys the local association
and closes server allocations; departure removes publication/subscriptions. Remote snapshots
are counted per member and rider in one-second windows; more than 120/s for any rider (4x the
normal 30/s) silences that member's snapshots and events for the rest of the race. A new race
resets the limiter. No close/error codes from the guard lane are used. Server tests cover
lobby/end rejection and repeated setup attempts; the native limiter has a flood test.
Sources for server lifecycle review:
[Workers best practices](https://developers.cloudflare.com/workers/best-practices/workers-best-practices/).
