# 158: race frames over Cloudflare Realtime SFU, and puppets with less delay

Status: current
Checked: 2026-11-06

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
- [x] Spike: a native Rust client connects to the Realtime SFU, publishes an unordered channel,
      a second client receives it. Library chosen and recorded under Decisions.
- [x] Server: SFU sessions and channels from the Durable Object; protocol 3 messages documented
      in `docs/online-protocol.md`.
- [x] Client: `dhd_net` SFU transport; `port/net/client.c` sends and receives race frames on it.
- [x] WebSocket race relay deleted; tests and docs updated.
- [x] Measurements recorded (acceptance 3-5); local routing and load caveats below.
- [x] Round 3: bare transport probe, client loop delays found and fixed, fair race comparison
      against the relay on the official server (section "Round 3" below).
- [x] Round 4: main merged (the abuse guard, finding 161) with caps for protocol 3; TURN over TLS
      on 443 as the fallback when UDP does not connect, measured through the real SFU and TURN;
      Mac and Linux checks (section "Round 4" below).

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

- Round 3: the SFU socket gets its own reader thread, which blocks in `recv_from` (100 ms
  timeout to notice a dropped association) and wakes the send thread for every datagram. The
  send thread stays the only owner of `Rtc` (str0m is sans-IO and single-owner); only raw
  datagrams cross threads, through a bounded inbox (1,024). Windows' UDP reset after an ICMP
  unreachable is ignored; ICE consent decides whether the association is gone.
- Round 3: the protocol-2 relay baseline for the fair comparison runs against the official
  server `wss://dhd.skuza.app` in the harness's private rooms (unlisted unless DHD_NET_PUBLIC).
  `wrangler dev --remote` cannot host this server: its Durable Objects are SQLite-backed, and
  Cloudflare's docs say SQLite-backed Durable Objects do not support remote development
  ([environments](https://developers.cloudflare.com/durable-objects/reference/environments/)).
  The official server's code is unchanged since v0.7.11 and has no STATE rate limit, so it relays
  the baseline's 30 Hz Part 1 snapshots as they are. Nothing was deployed.
- 2026-11-06 (the user): players' IP addresses must never reach other players, so direct
  peer-to-peer is out.
- 2026-11-06 (the user): the SFU design over keeping the Durable Object relay, own UDP relays, or
  a TURN-forced mesh. A mesh sends nine copies per player and still needs relays; own relays need
  servers to run. Cost on the current plan: SFU and TURN egress is free up to 1,000 GB a month
  (Cloudflare Realtime pricing, read 2026-11-05); a full ten-player race is a few MB per player.
- Round 4: TURN is a fallback, not a second path kept beside the first: UDP is tried first, the
  relay only after DIRECT_TIMEOUT (2 s after the answer; the reasoning is beside the constant and
  in "Round 4"). Credentials are minted per racer per race on request, never handed out to every
  racer in advance: most players never need them, and a credential nobody got cannot leak.
- Round 4: an own TURN client, not `turn-client-proto` (ystreet, MIT OR Apache-2.0, maintained,
  sans-IO). Its `stun-types` 2.0 pulls the older RustCrypto generation (digest 0.10, hmac 0.12,
  sha1 0.10, md-5 0.10, rand 0.9) beside the 0.11 one str0m already links, plus byteorder and
  smallvec: two copies of the hash stack for a few hundred lines of protocol. `turn.rs` needs only
  what this fallback uses (Allocate, Refresh, ChannelBind, ChannelData; no TCP relaying, no Send
  indications) and takes no new crate: HMAC-SHA1 and random ids from ring (already linked for
  rustls), MD5 in 40 lines (only for the long-term key, RFC 8489 9.2.2) with RFC 1321's vectors,
  MESSAGE-INTEGRITY checked against RFC 5769 2.4's sample. Sources:
  [RFC 8656](https://www.rfc-editor.org/rfc/rfc8656), [RFC 8489](https://www.rfc-editor.org/rfc/rfc8489),
  [RFC 5769](https://www.rfc-editor.org/rfc/rfc5769),
  [turn-client-proto](https://crates.io/crates/turn-client-proto).
- Round 4: the TURN credential TTL is an hour and the room revokes them when the racer's session
  ends; a revoke that fails leaves at most an unused credential for the rest of the hour.
- Round 4: only `turns:` urls on `turn.cloudflare.com` are used, 443 first: a network that blocks
  UDP usually lets TLS on 443 through, and TURN over UDP or plain TCP would not help there.
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
- Initial native transport uses IPv4 UDP to SFU candidates (round 4 adds the TURN-over-TLS
  fallback for networks that block UDP).
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
- Round 3: most of round 2's SFU delay came from the PC, not Cloudflare. Three causes, with
  numbers in "Round 3" below: (1) the send thread read the UDP socket only when it woke (a
  queued message or its 10 ms tick, which Windows' default timer stretches to ~15.6 ms);
  (2) frames the game queued were handed to the SFU after the poll that writes them, so each
  waited for the next wake; (3) frames queued during a poll had the same wait (~3% of a
  host's nine-rider snapshots, up to 12.5 ms). str0m itself adds nothing measurable: a write
  reaches the socket in 0.01 ms (p99 0.02-0.6 ms) and received data comes out in the same
  `handle_input`.
- Round 3: this PC ran Cloudflare WARP, and all SFU traffic went through its tunnel (route
  interface `CloudflareWARP`). The tunnel set the floor at ~28 ms RTT and made most of the
  tail: an ICMP ping to the SFU edge alone gave p99 65-79 ms, max 123 ms, and lost packets.
  The owner turned WARP off on 2026-11-06 (it stays off). The traffic now goes over Ethernet and
  the same ping is p50 8 / p99 11 / max 37 ms. Round 2's numbers, and the first round 3
  probes below, include the tunnel. Round 2's age growth from one window to the next did not
  reproduce in a 120 s probe through the tunnel (p50 32 in every window). After the fixes and
  without WARP, the race windows have 0-1 gaps over 100 ms instead of 6-17. The first window
  is lower on both transports because it starts at GO.
- Round 3, tried and ruled out: str0m's SCTP send buffer (128 kB `max_buffered_amount`, never
  near full at these rates), SCTP bundling or pacing (write to datagram 0.01 ms), and a stall
  in our loop when it is polled continuously (longest round 5.9 ms with one rider). Loop
  outliers that remain (poll gaps up to 33 ms, a few input waits over 5 ms per 30 s) come
  with the PC at 55-62% CPU from other lanes' builds and games: the thread is not scheduled.

- Round 4: vitest loads the server's gitignored `.dev.vars`, so with real credentials there the
  tests called the real Realtime API (with invalid offers) and one new test minted a real TURN
  credential (unused, expired within the hour). The test bindings now blank all four secrets.
- Round 4: the first relayed run failed with "TURN server did not answer": the end of each poll
  drained the TURN client's output into a link that was not connected yet, so the first Allocate
  was dropped. Output now stays in the client until the TLS link is up.
- Round 4: Cloudflare's TURN refuses a ChannelBind to some Cloudflare addresses (401 for 1.1.1.1
  and the API host's address) but binds to the SFU's own candidate address (7-13 ms) and to
  outside addresses. Its answer to a made-up username after the 401 challenge is a 400.
- Release review round 1 (before 0.7.12): puppets lost their snapshot history at every snapshot
  (`feed()` in `port/hle/puppet.c` had lost the braces of its new-race reset), so they didn't
  interpolate (3331300); a peer could fill the early-event queue and its overflow closed the
  receiving player's connection, now capped per sender and dropped (`sfu.rs`, 56dce3c); a ban,
  limit or kill-switch message lost the race against its close and showed as "connection closed"
  (`port/render/net/src/lib.rs`, dfab5c4).
- Release review round 2 (before 0.7.12): a ChannelBind was renewed 300 s after its success, but
  the TURN permission it installs lasts 5 minutes (RFC 8656 9; the binding 10) and ChannelData
  renews neither, so a relayed race over 5 minutes lost datagrams at each renewal. It is renewed
  after 240 s now (`turn.rs` CHANNEL_REFRESH, unit-tested).
- Release review round 2: `sfu_connected` walked the live member list across its API calls, and a
  leave or kick (not queued with SFU work) spliced it, so the loop skipped the next racer: two
  remaining racers never subscribed to each other and never got `sfu_ready`. The room now picks
  the next missing subscription afresh after every call until none is missing, and the release
  checks every pair first (server test with a leave during the held call).
- Release review round 2: one racer's failed setup (an API error, or a game that never finished)
  held every racer's traffic forever, since the release waits for all and the failed one could
  not try again. Setup now has a deadline (25 s from the start, from the measured 0.2-3.1 s plus
  ten racers' calls); a racer that fails or misses it is out of the race as one who leaves, and
  the others are released without it (`docs/online-protocol.md`, server tests).
- Release review round 2: teardown lost its only handles when a close or revoke failed: a
  departed member was removed first, so its session id and TURN username went with it, and the
  race-end loop stopped at the first failure. Teardown now goes on past failures, and what did not
  close is stored apart from the members (and the room) and retried by the alarm with backoff
  until it closes or its time is up. Cloudflare's SFU docs give a connected DataChannel-only
  session no end ("close resources when finished"; retain cleanup state while retrying), so the
  room gives up after an hour, as the TURN TTL does.

## Result

Merged into `main` on 2026-11-06 (four rounds; the round sections below hold the details).
State sync is unchanged (finding 044); the race frames' transport and the puppets' delay changed:

- Race frames travel over Cloudflare Realtime SFU DataChannels: STATE on an unordered channel
  with no retransmissions, EVENT on a reliable one. The room creates each racer's SFU session and
  channels and sends each client only its own SDP and channel ids; no player receives another's
  address, SDP or TURN credentials (`online/server/src/sfu.ts`, `port/render/net/src/sfu.rs`).
  The WebSocket race relay is gone; binary PING/PONG on the WebSocket stays for the ping counter.
  `PROTOCOL` is 3; 0.7.12 ships it, and `MIN_VERSION` is 0.7.12 (`online/server/wrangler.jsonc`,
  3e65337): 0.7.11 can't race with it.
- When UDP to the SFU does not connect within 2 s, the client falls back to Cloudflare TURN over
  TLS on 443 with short-lived credentials the room mints for that racer only (`turn.rs`,
  `relay.rs`; test switch `DHD_NET_SFU_UDP=0`).
- 30 snapshots a second, a puppet delay that adapts to each sender (50-150 ms), snapshots that
  survive loss (changed pad words repeated, a full copy about once a second), and snapshot age,
  gaps and the SFU loop's timings in dhd.log's `net:` lines.
- Measured on one PC (WARP off), snapshot age in a race, median: the 0.7.11 relay through the
  official server 71-90 ms, the SFU 19-45 ms, the SFU forced through TURN/TLS 33-54 ms.
- Checked on the merged tree: `python tools/check.py server` and `python tools/check.py port`.
  Mac: the Rust network and renderer tests and the release library; Linux: the library
  cross-builds only.
- PC and Mac (2026-11-06, release check, `port/tools/cross_race.py`, server at the release
  review's fixes): each side hosting, and the Mac forced onto TURN/TLS (`DHD_NET_SFU_UDP=0`).
  Every two-player race finished with the same results on both, no freeze, no gap over 100 ms.
  Snapshot age p50/p95/p99 26-47/43-69/45-71 ms (UDP) and 31-35/43-54/46-60 ms (Mac on TURN);
  puppet delay medians 49-73 ms; snapshots 30 a second per rider throughout.
- Not yet: the deploy's four Worker secrets (`online/server/README.md`), Linux tests, a network
  that really blocks UDP, and a race longer than 5 minutes on TURN (the ChannelBind renewal).

Earlier validation notes, kept from round 1:

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
Signaling is client-offer/session-create-with-SDP/SFU-answer. Round 2 race measurements
and the WebSocket baseline are recorded below. The lead also runs boot/race state traces against the shared reference build,
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
No TURN key credentials are configured locally; no secrets were created or changed. str0m 0.25 is sans IO: its
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
Cargo feature unification cannot subtract it. A scoped AWS-LC gate exception is an option,
but accepting absolute source paths unchanged would leave machine details in release objects.
A clean version needs path normalization like the existing ring exception, including COFF
checksums and source/debug strings; no such build was verified here. Keep the minimal adapter
patch and the unchanged archive gate for now. PATCH.md
now gives the exact reapplication steps and upstream revision. No newer clean published option
was found. The reviewed upstream manifests are linked there.

Round 2 snapshot change: payload version 5 keeps the 104-byte fixed snapshot and sends
changed pad words six times, with a full 834-byte refresh every 30 snapshots. This handles
five consecutive lost changed updates; a longer burst recovers at the next received full
refresh. The receiver keeps a mirror, rejects stale timestamps before applying updates,
and validates every word before mutation. Race start clears mirrors and sender repetition
history. Guest-path unit tests cover five lost repeats, all repeats lost, periodic repair,
zeroing a released action, and malformed updates. Empty updates are 104 bytes; one repeated
word makes 109 bytes. Live human-rider distribution is recorded below. The adaptive lag diagnostic
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

## Round 2 measurements (race windows only)

Method: two native clients from online_race.py, local_server.py owning wrangler dev and
workerd, native renderer at 1x, minimised, screenshot capture disabled, scripted pedal/punch,
forced finish at 70 seconds, eight host-owned AI riders. Ages are the existing GO-clock
estimator sampled by net ticks, not menu ping or a direct network RTT measurement. Per-sender
net lines aggregate each 30-second wall-clock window. The first measured window includes
only the samples after GO. Only the first two age-bearing windows are tabulated: both precede
the finish. Later windows extending into finish/results wait are excluded. Bin 1001 is
histogram overflow, not an exact millisecond value. Other lanes were building on the PC:
these are load-contended observations, not a controlled performance benchmark.

WebSocket baseline is the prepared Part 1 source/executable under the ignored baseline folder,
with the same GO-clock metric, protocol 2 and the DO race relay. Both riders finished at
70.03 s with matching results. Its local DO does not traverse the Internet; the SFU races do.
This setup therefore tests connectivity and loss recovery, not geographic detour savings.

| Transport/run | Receiver, sender | Consecutive race-window age p50/p95/p99 ms; gaps over 100 ms |
|---|---|---|
| SFU, full snapshots | host, guest | 81/140/183; 6 ? 93/208/411; 10 |
| SFU, full snapshots | guest, host | 82/143/211; 6 ? 86/528/1001; 14 |
| WebSocket | host, guest | 25/63/84; 0 ? 30/51/82; 0 |
| WebSocket | guest, host | 37/87/127; 0 ? 30/55/187; 1 |

Raw logs: build/check/sfu-round2 and build/check/ws-round2 (local only). The SFU race completed
and agreed on results. Snapshot ages on this local setup were higher through the Internet SFU
than the local WebSocket relay; no latency improvement is claimed from this experiment.
The first 5% loss run delivered normal snapshot streams for roughly 66 race seconds, but a
server edit triggered wrangler hot reload, closing both sockets. Discard it as an acceptance
run. Its partial raw logs are build/check/sfu-loss-round2. A clean final-build repeat follows.

Race-boundary review also found that a queued old STATE could outlive ended and close the
WebSocket after its SFU association was discarded. Start/ended/welcome now purge queued
race frames, and race frames with no active SFU association are discarded.

Race-scoped setup requires an allocation barrier: SETUP/LOADED can be queued immediately
at start, before another client has subscribed. Each client now acknowledges sfu_ready only
after every race member's channels exist and open; the DO releases sfu_ready after all racer
acknowledgements. Pending events/states flush only after release, preventing early reliable
events being published before a subscriber exists. This is transport setup, not tick agreement.

### Final compact snapshots and clean loss repeat

Both final-build races passed through the real SFU using the same two-client harness/settings
as above. Each rider finished at 70.03 seconds and both clients received identical results.
The clean loss repeat set DHD_NET_DROP_STATE=5 on both clients, with no server edits/reloads
or builds/tests of this lane during either race. Other lanes could still load this shared PC.
The loss run also delivered the reliable hit events. Puppet traces continued throughout;
there was no sustained transport freeze or disconnect. Short age spikes remain: this is not
a claim of zero stutter. Raw local-only evidence: build/check/sfu-final-round2 and
build/check/sfu-final-loss-round2 (stderr.log, puppet.txt and state.bin for A/B).

| Transport/run | Receiver, sender | First two pre-finish windows: p50/p95/p99 ms; gaps over 100 ms |
|---|---|---|
| SFU, compact | host, guest | 66/79/117; 1 ? 93/163/310; 16 |
| SFU, compact | guest, host | 59/75/107; 2 ? 99/187/558; 17 |
| SFU, compact, 5% drop | host, guest | 71/122/157; 2 ? 85/182/429; 17 |
| SFU, compact, 5% drop | guest, host | 70/121/155; 1 ? 85/228/483; 14 |

Thus 5% injected snapshot loss did not cause a persistent backlog; later snapshots continued
arriving. Gap counts count intervals longer than 100 ms, not individual dropped packets:
an isolated 30 Hz loss gives about a 67 ms interval and does not count as such a gap.
Near race second 60 the loss run logged 1,803 guest snapshots sent and 1,712 received by
the host (about 95%, with samples at slightly different ticks). The host logged 16,236
owned-rider snapshots sent; the guest had received 15,398. These counters are approximate
cross-clock observations, not a packet capture. The deterministic native channel test separately
checks exactly 95/100 deliveries under 5% injection and delivery of a reliable event.

Snapshot-size method: DHD_NET_RECORD=state.bin records the human rider's emitted payload as
u16 length then payload bytes, before injection. Parse records with version 5 and timestamps
5,000 through 65,000 ms inclusive. This excludes menus, countdown and finish/results. Each
client supplied 1,800 snapshots. These are human-rider measurements, not an AI-size survey.

| Run | Rider | Payload mean / median / p95 / maximum bytes | At most 200 bytes |
|---|---|---|---|
| Normal | host human | 166.0 / 144 / 149 / 834 | 96.7% |
| Normal | guest human | 170.4 / 144 / 159 / 834 | 96.7% |
| 5% drop | host human | 166.9 / 144 / 144 / 834 | 96.7% |
| 5% drop | guest human | 170.8 / 144 / 159 / 834 | 96.7% |

Before: every payload was 834 bytes. After: median 144 bytes, mean about 166-171 bytes
including the periodic full refreshes. Add ten bytes for frame type/slot and race marker;
SCTP/DTLS/IP overhead is excluded from both sizes. The full refresh remains 834 bytes.
No host-with-nine-AI upload bandwidth figure was measured; do not extrapolate the human
payload profile as a measured AI upload rate.

### Round 2 validation and remaining work

- check.py quick -j 6: pass; compact-pad recovery guest test included.
- check.py port -j 6: pass. The later native readiness/queue fixes were additionally rebuilt
  through build.py --no-core -j 6 and tested through rustgs.py (five dhd_net and eight renderer
  unit tests). The unchanged native archive gate passes. The final live races use that build.
- check.py server -j 6: pass, 97 tests plus TypeScript typecheck, including race-only allocation,
  per-race setup cap and all-subscribers-ready barrier.
- Boot: all 600 state-trace rows match the prepared Part 1 reference. Race: all 5,000 rows
  match. Both use the default deterministic harness recipe and mask. The quick rebuild's
  module SHA-256 equals the traced module (prefix b36f7414b5b12f57); later changes are native
  transport/server only. This is the prepared reference inside this lane's ignored build
  folder, not an independently located shared main reference build.
- All owned harness/client/server/build/test processes have ended. No push, deployment,
  secret change, tag or version change was performed.

Remaining: UDP-blocked players still need a TURN/TLS adapter and separate TURN credentials,
followed by a real blocked-UDP test. str0m can support such an adapter; this round does not
implement it or claim that str0m makes it impossible. Lead decision: reserve that follow-up
before treating restrictive-network support as complete. Also retain the lead's merged
shared-main-reference trace and cross-machine races in both directions, native Mac/Linux
verification, and a full-room/AI upload measurement. The local WebSocket comparison alone
cannot establish a latency improvement for geographically distant rooms.

## Round 3: the client loop, the probe and a fair comparison

### Bare transport probe

`port/render/net/src/probe.rs` (tests only, `#[ignore]`) opens two Realtime SFU sessions
through the real API, publishes `state` (unordered, maxRetransmits 0) and `events` from each,
subscribes each to the other, and sends timestamped snapshots at 30 Hz. Both clients live in one
process, so one clock gives the one-way delay. `probe_sfu_one_way_delay` runs the game's own
stack (`ws_start`, its send thread, `sfu::Sfu`) behind a local stand-in for the room's Durable
Object, which makes the Realtime API calls itself. `probe_sfu_direct` busy-polls two `Sfu` values
on one thread: the floor without our loop. Credentials come from REALTIME_APP_ID /
REALTIME_APP_TOKEN or `online/server/.dev.vars` at run time and are never printed. Run:
`python port/tools/rustgs.py test -- -p dhd_net -p dhd_render --lib -- --include-ignored --nocapture probe_sfu`
(DHD_PROBE_SECONDS, DHD_PROBE_RIDERS, DHD_PROBE_RIDERS_B, DHD_PROBE_BYTES; see the file).

One-way delay, ms, both directions (client 0 / client 1 where they differ); "gaps" are rider-0
arrival intervals over 100 ms. 30 s runs unless noted; 150-byte snapshots, 170 for 9+1.

| Run | Riders per tick | p50 | p95 | p99 | max | gaps |
|---|---|---|---|---|---|---|
| WARP on, before (code of 857fe29) | 1 | 44.3 | 87.8 | 112 | 154 | 3 / 4 |
| WARP on, before, busy-polled floor | 1 | 30.6 | 34-63 | 65-91 | 90-130 | 0 |
| WARP on, before | 10 | 53-54 | 110 | 133-154 | 299 | 11 / 9 |
| WARP on, before, busy-polled floor | 10 | 33-35 | 76-89 | 95 | 131-134 | 1 / 0 |
| WARP on, + reader thread (df0772c) | 1 | 37.7-38.0 | 66 | 97-99 | 122 | 2 / 3 |
| WARP on, + reader thread | 10 | 35.7-36.0 | 93 | 152 | 216 | 5 / 5 |
| WARP on, + flush before poll (4362170) | 1 | 30.6-30.7 | 77-79 | 116 | 158 | 2 |
| WARP on, + flush before poll | 10 | 33.1-33.5 | 59.5 | 93 | 113 | 0 |
| WARP on, + re-poll (c38fa54), 60 s | 9 + 1 | 32.8 / 33.8 | 71 / 78 | 93 / 95 | 166 | 7 / 9 |
| WARP off, final, 60 s | 1 | 8.6 / 9.1 | 9.6 / 10.8 | 11.5 / 12.2 | 21 / 26 | 0 |
| WARP off, final, busy-polled floor, 60 s | 1 | 8.7 / 8.3 | 9.6 / 9.4 | 15.9 / 17.4 | 34 / 33 | 0 |
| WARP off, final, 60 s | 9 + 1 | 8.6 / 11.4 | 10.5 / 12.8 | 12.6 / 14.5 | 25 / 33 | 0 |

The loop's own wait, from its new 30 s `net: SFU` line (dhd.log): before the fixes about half of
all writes waited over 5 ms (up to 12.5-15 ms) and datagrams waited up to a tick. With all three
fixes, input wait max 1-3.5 ms and send wait max 0.1-9.4 ms (0-8 over 5 ms per 30 s, of
1,700-2,300 datagrams). The final game stack through the SFU is within ~1 ms of the busy-polled
floor and of the ICMP ping to the edge. Edge ping, 900 samples, the same window as a probe:
WARP on p50 28 / p95 32 / p99 79 / one lost; WARP off p50 8 / p95 9 / p99 11 / max 37 / none lost.

### Fair race comparison (WARP off, same PC, same harness)

Same method as round 2: two native clients from `online_race.py`, native renderer at 1x,
minimised, no screenshots, scripted pedal/punch, forced finish at 70 s, eight host-owned AI
riders, the GO-clock age estimator, the first two 30 s windows after GO (both before the finish).
SFU: this branch at c38fa54, control plane on a local `wrangler dev` (it only sets up the
channels), race frames through the nearest Cloudflare location. Relay: the prepared Part 1 +
age-metric build (protocol 2) against the official server in a private room; the room's
Durable Object ran in a city in another country, game ping 43-49 ms. The nearest Cloudflare
location is ~8 ms RTT from this PC. Runs were interleaved (SFU, relay, relay, SFU). The PC was
not idle: other lanes ran builds and two games throughout. CPU is the whole PC's, sampled every
2 s for 200 s per run.

| Run (PC CPU avg / p95) | Receiver, sender | Window 1 p50/p95/p99; gaps | Window 2 p50/p95/p99; gaps |
|---|---|---|---|
| SFU 1 (62% / 82%) | host, guest | 45/66/70; 0 | 41/61/67; 0 |
| SFU 1 | guest, host | 19/41/45; 0 | 37/62/67; 0 |
| Relay 1 (56% / 73%) | host, guest | 71/87/89; 0 | 89/101/102; 0 |
| Relay 1 | guest, host | 72/87/92; 0 | 90/104/105; 0 |
| Relay 2 (54% / 75%) | host, guest | 85/102/104; 0 | 83/104/106; 0 |
| Relay 2 | guest, host | 77/99/109; 0 | 78/99/104; 0 |
| SFU 2 (55% / 74%) | host, guest | 30/45/49; 0 | 43/60/64; 0 |
| SFU 2 | guest, host | 28/53/56; 0 | 37/62/66; 0 |

All races finished at 70.03 s for both riders with matching results. The third window (it
reaches the finish and results wait) shows bin 1001 on both transports and is excluded, as in
round 2. Raw logs (local only): build/check/sfu-round3-2, -3 and ws-round3-1, -2. An earlier
SFU run (sfu-round3-1) still went through WARP and is left out. The estimator adds ~30 ms to
the one-way transit on both transports (time to the next snapshot at 30 Hz, network-tick
sampling, clock offset).

Result: through the SFU the snapshot age is about 40-45 ms lower at p50 and at p95 than the
relay through the official server (SFU p50 19-45, p95 41-66; relay p50 71-90, p95 87-104),
with no gaps over 100 ms on either. On this PC the relay pays the Durable Object detour
(finding 135): ~45 ms RTT against ~8 ms to the SFU's edge. Where a room's Durable Object runs in
the players' own city, the relay's detour is small and the two should come out close; that case
was not measured here. The SFU keeps its other advantage there: one lost packet does not hold
later snapshots back (no TCP head-of-line blocking), and loss repeats (round 2) showed that.

### Round 3 validation and recommendation

- Unit tests (5 dhd_net, 8 renderer) pass with every commit. `check.py quick -j 6`, `check.py
  server -j 6` (npm test, typecheck) and `check.py port -j 6` (game build, net tests, GS and
  wasm2c tests) pass on c38fa54 plus the probe and this file.
- No state trace was run: round 3 changes only the Rust network library (the core module is
  reused unchanged, "module C as before"), and deterministic offline runs never start the
  network. The lead's merged-branch traces still apply.
- All harness, wrangler/workerd, game and probe processes this round started have ended.
  Nothing was pushed, deployed, tagged or changed in secrets or version.

Recommendation: keep the SFU. With the client loop fixed it adds ~1 ms over the network path
in the bare probe, and in the race it beats the relay through the official server by ~40-45 ms
at p50 and p95 on this PC, where the room's Durable Object sits in another country. The relay
plus Part 1 is the fallback only if the remaining SFU work (TURN/TLS for UDP-blocked players,
native Mac/Linux checks, the cross-machine races) turns out too costly. Unmeasured: a room
whose Durable Object runs in the players' own city, and two PCs on different networks.

## Round 4: main merged, TURN over TLS on 443, Mac and Linux

### Main merged (the abuse guard, finding 161)

`git merge main` (77bf688): conflicts only in the findings index, the health route of
`docs/online-protocol.md` and `room.test.ts` (protocol 3 plus the guard's `online` field), and
`port/net/test_session.c` (both new tests kept). The guard's checks stay first in
`Room.webSocketMessage`, before any `sfu_*` message is read, so protocol 3 clients get the kill
switch, the limits per address, bans and the caps like everyone else. Close codes do not collide:
the SFU lane adds none (its errors are `bad_value`), the guard owns 4010-4013. Fix-up e1b0051:
protocol 3 sends race frames over the SFU, so a member's WebSocket binary traffic is only the
race's two PINGs a second; the binary cap went from 2000 to 100 a 5 s window (ten times the game),
text stays at 100 and covers the race's few Realtime messages. `tools/loadtest.mjs` measured the
deleted relay; it now sends the game's PINGs and checks every PONG and that no cap is hit.

### TURN over TLS: the design

- **When:** UDP stays the first path. `DIRECT_TIMEOUT` is 2 s after the SFU's answer
  (`port/render/net/src/sfu.rs`). Direct setup is an ICE check plus the DTLS handshake, about four
  round trips to the nearest Cloudflare location: measured 198-241 ms here (8 ms RTT), under a
  second at 150 ms RTT, with room for one lost packet's retransmission. A shorter timeout would put
  slow-but-working UDP on the relay; a longer one only delays the race start for blocked players.
- **Credentials:** the client sends `sfu_turn`; the room (`online/server/src/sfu.ts` `Turn`) mints
  them with `generate-ice-servers` only for a racer with an SFU session in the running race, once
  per member per race, TTL one hour (longer than any race with its finish wait, so a running
  relay's Refresh never meets an expired credential), `customIdentifier` `room-<CODE>-m<id>`
  (accepted by that endpoint, checked live: 201). It sends the `turns:` urls, username and
  credential to that member only, and revokes them when the member's SFU session is cleared (race
  end, retire, leaving). A failed mint sends nothing and leaves the session usable (UDP may still
  connect). The client accepts only `turns:` servers on `turn.cloudflare.com`, port 443 first.
- **Client:** `turn.rs`, a sans-IO TURN client (RFC 8656 subset): Allocate with the long-term
  credential (401 challenge, MD5 key, HMAC-SHA1 MESSAGE-INTEGRITY), 438 stale-nonce retry, Refresh a
  minute before the lifetime ends, ChannelBind per SFU candidate (which also installs the
  permission) refreshed every 5 minutes, ChannelData padded to 4 bytes on the stream, integrity
  checked on every success answer, a 10 s transaction timeout, Refresh with lifetime 0 at the end.
  `relay.rs`: the TLS stream (rustls on ring, the bundled roots, SNI `turn.cloudflare.com`) with its
  own reader thread that wakes the send thread, as the UDP reader does. The allocation's address
  goes to str0m as a relayed candidate; str0m's transmits from it go to the relay, relayed datagrams
  come back as input with the relayed address as destination. str0m's ICE keeps the association
  alive through its Disconnected state, so the candidate added after the timeout is checked and
  nominated like any other.
- **Test switch:** `DHD_NET_SFU_UDP=0` drops every UDP datagram to and from the SFU, as a network
  that blocks UDP does; the timeout and fallback then run as they would there.
- **Hard rule:** a TURN allocation carries only that player's own traffic to the SFU; the SFU sees
  the TURN service's address. Credentials go to their owner only (server test). No player gets
  another's address, SDP or credentials.

### Round 4 measurements (WARP off, route interface Ethernet for the SFU and TURN)

Bare probe (`probe_sfu_one_way_delay`, both clients in one process, one clock), one-way ms.
Runs were interleaved; the PC ran other lanes' work.

| Run | Riders A+B, bytes | Client 0 p50/p95/p99/max; gaps >100 ms | Client 1 p50/p95/p99/max; gaps | Setup |
|---|---|---|---|---|
| UDP, 30 s | 1+1, 150 | 10.6/26.1/29.6/37.7; 0 | 10.5/26.7/29.8/42.9; 0 | connected 216-241 ms after the answer |
| TURN/TLS, 30 s | 1+1, 150 | 9.1/23.4/40.0/139.5; 1 | 8.7/19.3/26.0/28.7; 0 | allocation 38 ms after the credentials; connected 2395-2399 ms after the answer |
| UDP 1, 60 s | 10+1, 170 | 8.8/17.1/25.1/32.1; 1 | 10.2/17.8/26.3/45.0; 1 | |
| TURN/TLS 1, 60 s | 10+1, 170 | 8.3/14.6/22.3/35.4; 0 | 11.9/17.3/24.6/71.6; 0 | |
| UDP 2, 60 s | 10+1, 170 | 9.7/16.4/25.4/32.9; 0 | 9.9/15.9/25.1/32.1; 0 | |
| TURN/TLS 2, 60 s | 10+1, 170 | 9.7/13.1/26.0/110.1; 2 | 11.2/13.8/30.5/185.4; 3 | |

Every TURN run delivered 1800/1800 (900/900) rider-0 snapshots; the UDP runs 1797-1800. The 30 s
`net: SFU` line says `via TURN/TLS` with 0 direct datagrams and ~1,150 relayed each way. Result:
from this PC the relay costs nothing measurable at p50/p95 (Cloudflare's TURN and SFU are the same
edge); the tail is what TCP adds: one lost segment holds later ones (max 110-185 ms, 1-3 gaps
over 100 ms a minute against 0-1 on UDP). Setup through the relay adds ~2.1-2.4 s (the 2 s
timeout, the credential mint through the room, TLS ~25-45 ms, Allocate ~20 ms, ChannelBind 7-13
ms, ICE and DTLS through the relay). `probe_turn_allocate` times the bare steps.

**Two-game race through TURN/TLS** (`online_race.py` through `local_server.py`, both games with
`DHD_NET_SFU_UDP=0`, minimised, 120 s after GO, eight host AI riders; raw logs local only in
`build/check/sfu-turn-round4`). Both games fell back as designed: no UDP connection 2 s after the
answer, the credentials through the local room, the TURN allocation 41-42 ms later, connected via
TURN/TLS 2420 and 3125 ms after the answer; both loaded, the countdown started +0.0/+0.2 ms from
the agreed moment and GO was 0.4 ms apart. Every `net: SFU` line says `via TURN/TLS`, 0 direct
datagrams. Snapshot age (the GO-clock estimator, as in rounds 2 and 3), the 30 s windows after GO:

| Receiver, sender | Window 1 | Window 2 | Window 3 | Window 4 |
|---|---|---|---|---|
| host, guest | 37/53/56; 0 | 52/65/66; 0 | 54/65/68; 0 | 54/65/97; 3 |
| guest, host | 33/52/57; 0 | 53/849/1001; 13 | 47/66/155; 4 | 44/66/73; 2 |

(p50/p95/p99 ms; gaps over 100 ms.) Apart from one window this matches round 3's UDP race
(p50 19-45, p95 41-66).

The guest's window 2 (race seconds ~30-60) is not the TURN setup: setup ended before GO and
window 1 is clean. In it, the guest's snapshots from the host came in bursts (per second 117, 334,
63, 406, ... against a steady 270) with puppet lag up to 249 ms, so the stream stalled and then
delivered what had queued. Our code is not where they waited: the send threads never paused more
than 12.7 ms (poll gap), frames waited at most 10.2 ms to be written, and nothing was dropped.
To check the receive side as well, the `net: SFU` line now reports the relay's longest read gap and
how long read bytes waited for the send thread (52c39be). In ten more forced-TURN probe runs (60 s,
host load) bytes waited 0.1-3 ms after the read. Snapshot stalls of about one second happened in
two of them (1034 and 1039 ms; 0-3 gaps over 100 ms in the others). So the time is lost before
the bytes reach our socket read: TCP retransmission on the TLS stream, which holds every later
byte back (head-of-line blocking; the UDP runs on the same PC and path had no such stalls). That is
the known cost of a TCP fallback, not a bug here. The relay is only for players whose UDP is
blocked, and for them it replaces no online play at all. Two parallel TLS links carrying duplicate
snapshots would hide most of these stalls; not done, a lead decision if players on the relay
report it.

Not measured: a real UDP-blocking network, and players on different networks through the relay.

### Cost

Cloudflare's pricing (read 2026-11-06): SFU and TURN egress share one allowance, 1,000 GB a month
free, then $0.05 per GB; "traffic between Realtime TURN and Realtime SFU ... does not incur any
charges" (TURN FAQ); TURN bills what its edge sends to the client. So a relayed player's downstream
is billed once, as TURN egress instead of SFU egress, plus the framing (4-byte ChannelData header,
padding, TLS records: roughly 10-15% on 150-200-byte snapshots). The fallback runs only for players
whose UDP fails. Minting and revoking credentials cost nothing. Nothing here changes the plan's
cost picture. Sources: [pricing](https://developers.cloudflare.com/realtime/pricing/),
[TURN FAQ](https://developers.cloudflare.com/realtime/turn/faq/),
[credentials](https://developers.cloudflare.com/realtime/turn/generate-credentials/).

### Mac and Linux

- **macOS arm64** (the Mac build host, macOS on M1, the same sources copied to a folder of its
  own and removed afterwards): `rustgs.py test --target aarch64-apple-darwin -- -p dhd_net -p
  dhd_render --lib`: 13 dhd_net and 8 renderer tests pass. Among them the UDP/DTLS/SCTP loopback
  test (`native_channels_deliver_and_bind_sender_with_state_loss`: the reader thread, its wakeups
  and the 5% loss count on macOS sockets), the TURN codec tests and
  `probe_turn_tls_no_credentials` (TLS to `turn.cloudflare.com:443` ready in 74 ms, the 401
  challenge and refusal in 191 ms). `rustgs.py build --target aarch64-apple-darwin` builds the
  release `libdhd_render.a` with its gates (the vendored str0m crypto patch compiles there: no
  AWS-LC, no OpenSSL). The live SFU/TURN probes need the Realtime and TURN keys, which stay on
  this PC, so they did not run on the Mac. The game itself was not linked there (the Mac's disk
  had about 6 GB free); the lead's PC<->Mac race builds it.
- **Linux x64:** `rustgs.py build --target x86_64-unknown-linux-gnu` cross-builds the release
  library here with its gates. Linux tests did not run: no Linux machine is reachable from here,
  WSL's Ubuntu 22.04 has no cargo, and its networking does not start (mirrored mode fails, falls
  back to none). Nothing in the new code is Windows-specific: std sockets and threads, rustls,
  ring; the one platform case in the UDP reader (Windows' reset after an ICMP unreachable) is a
  harmless extra match elsewhere.

### Round 4 validation, what is left, lead decisions

Commits on `feat/online-sfu` this round: 77bf688 (merge of main), e1b0051 (guard caps for protocol 3,
the load test), 47cb3ff (server: TURN credentials), 184f3e0 (client: TURN over TLS), c4bab01 (a
failed mint keeps the session; hermetic server tests), 1d762be (the credential-free TLS probe),
52c39be (the relay's stream timing), and this file with the protocol doc and the server README.

- After the merge: `check.py server` and `check.py quick` passed. On the final code (52c39be):
  `check.py server` (129 tests, typecheck), `check.py port -j 6` (game build, net tests, GS and
  wasm2c tests), `check.py quick -j 6` and `check.py docs` pass.
- dhd_net 13 and renderer 8 unit tests on Windows and macOS; the live probes on Windows.
- No state trace: round 4 changes only the Rust network library, the server and docs (the core
  module is unchanged), and deterministic offline runs never start the network.
- Every harness, wrangler/workerd, game, probe and Mac job this round started has ended; my Mac
  folders are removed. Nothing was pushed, deployed, tagged, or changed in secrets or version.

Left: the lead's PC<->Mac race (the TURN path can be forced there with `DHD_NET_SFU_UDP=0` on
either side); a test on a network that really blocks UDP; Linux unit tests on a Linux machine; a
full game link on the Mac.

For the lead: the deploy needs the two new secrets `TURN_KEY_ID` and `TURN_KEY_API_TOKEN`
(online/server/README.md) besides the Realtime pair. `DIRECT_TIMEOUT` (2 s) is a judgment call: a
shorter one puts slow UDP on the relay, a longer one delays blocked players' race start.
Duplicate TLS links to hide TCP stalls on the relay: only if relay players report stutter.
