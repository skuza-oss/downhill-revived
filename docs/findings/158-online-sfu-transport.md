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
- [x] Spike: a native Rust client connects to the Realtime SFU, publishes an unordered channel,
      a second client receives it. Library chosen and recorded under Decisions.
- [x] Server: SFU sessions and channels from the Durable Object; protocol 3 messages documented
      in `docs/online-protocol.md`.
- [x] Client: `dhd_net` SFU transport; `port/net/client.c` sends and receives race frames on it.
- [x] WebSocket race relay deleted; tests and docs updated.
- [x] Measurements recorded (acceptance 3-5); local routing and load caveats below.
- [x] Round 3: bare transport probe, client loop delays found and fixed, fair race comparison
      against the relay on the official server (section "Round 3" below).

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
