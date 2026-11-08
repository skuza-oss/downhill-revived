# 180: Selectable host-star P2P races

Status: in progress

Branch `feat/p2p-prototype` from `main`, started 2026-11-08.

## Goal and acceptance

An independently selectable P2P room backend, alongside the existing SFU backend. Windows,
Linux, macOS, Android and desktop/mobile browsers share the existing snapshot gameplay.
Native iOS needs a compatible design, but no app work in this lane. Up to ten humans within
track limits; any foreground platform may host. Cloudflare supplies membership, signaling,
discovery and short-lived TURN credentials. The host coordinates the current room rules,
countdowns, AI, finish deadlines and results. Host loss ends the session; disconnected guests
become AI and may join again between races. No automatic backend switch or host migration.

Acceptance: required docs, quick, port and server check tiers; unchanged offline boot/race
traces against the baseline; real game races over direct and forced TURN/TLS paths; packaged
test builds with instructions. Report which platforms and network paths were actually tested.
No game data, credentials, machine paths or non-official server addresses in tracked files.

## Progress

- [x] 2026-11-08: inspected transport, gameplay, room coordination and browser build boundaries.
- [x] 2026-11-08: examined candidate libraries, including their native TURN implementation.
- [x] Qualify direct and Cloudflare TURN/TLS connectivity with the selected transport.
- [x] Shared session coordinator and native/browser host-star adapters.
- [x] Isolated signaling service and backend selection; deployment dry run passes.
- [x] Required checks, unchanged offline traces, live Windows race and test snapshot ZIP.

## Decisions

- The user explicitly authorizes a second backend, direct peer addressing and online support on
  Android/browser, overriding the older SFU-only and platform restrictions for P2P rooms.
- Preserve local rider authority, snapshot packing, prediction, correction, combat and pickups.
- Reuse str0m and the existing TURN/TLS client rather than introduce another unqualified stack.
  Share transport components; do not duplicate the TURN client or game synchronization.
- Preserve the current ready/countdown/rematch/rotation behavior. Its coordinator runs on the
  host; Cloudflare does not adjudicate P2P races. Use a common clock for existing snapshot stamps.

## Surprises

- Matchbox 0.15's native webrtc 0.21.1 dependency explicitly skips secure and non-UDP TURN URLs.
  The webrtc 0.17 dependency of Matchbox 0.14 also leaves TCP/TLS relay gathering unimplemented.
  Sources: <https://github.com/webrtc-rs/webrtc/blob/v0.21.1/src/peer_connection/transport/turn_relayer.rs>
  and <https://github.com/webrtc-rs/webrtc/blob/v0.17.0/ice/src/agent/agent_gather.rs>.
- libdatachannel advertises TLS through libnice, but the inspected libnice TCP relay path only
  wraps legacy compatibility modes in pseudo-TLS. It was not accepted as verified Cloudflare TLS
  support. Sources: <https://github.com/paullouisageneau/libdatachannel/blob/master/DOC.md> and
  <https://github.com/libnice/libnice/blob/master/agent/agent.c>.
- Browser online code is disabled and still speaks protocol 2. Peer connection setup must run
  in the page context, bridged to the game worker. WebRTC specifies RTCPeerConnection on Window:
  <https://www.w3.org/TR/webrtc/#rtcpeerconnection-interface>.

## Validation so far

- Rust workspace unit tests pass. Native two-client room tests pass over direct UDP and
  forced Cloudflare TURN/TLS (direct UDP disabled).
- Ten native transport clients start, each receives the other nine riders, then gets results.
- `port/web/check_p2p.mjs` passes with real Chromium WebRTC and the shared WASM coordinator.
- Android Rust cross-check and the server check tier pass.
- `wrangler.p2p.jsonc` deploys only the P2P path to an independent Worker and object namespace;
  its dry run passes; the isolated Worker is deployed at the official P2P path. Cloudflare documents that routes can precede a Custom Domain origin:
  <https://developers.cloudflare.com/workers/configuration/routing/routes/>.
- The packaged Windows game completed a live two-instance race with the same results on both
  games (test-triggered finishes at 12 and 16 seconds). A four-minute direct race stayed connected.
- A forced-TURN/TLS game race exchanged snapshots and hits; stopping its guest handed that rider
  to the host AI. Native live room tests also passed with direct UDP disabled.
- `check.py docs`, `check.py port` (includes quick), and `check.py server` pass. Offline boot
  (600 fields) and race (5,000 fields) traces exactly match the pre-change baseline.
- The release packager correctly refuses the dirty checkout, which includes pre-existing local
  work. The friend build is explicitly a development snapshot, assembled under build/ using
  the packager's executable, Rust library, dependency, license, retail-data and path/address scans.

## PC/Mac validation (2026-11-08)

The same core ran on Windows x64 and an Apple M1 Mac. Native macOS build passed with the
existing pinned toolchain. `port/tools/cross_race.py` now accepts `--backend p2p`, `--server`
and `--force-turn`; without those arguments its SFU/local-server behavior remains the default.
The test navigates the actual ONLINE menus, readies, runs scripted riders, finishes, watches,
returns to the room and starts again.

- Mac-host and PC-host automatic-path runs both passed two shared races and repeated starts.
- Mac-host automatic mode used direct UDP, confirmed by gameplay-sized UDP counters.
- PC-host automatic mode used TURN/TLS, confirmed by TCP gameplay traffic and only ICE probes
  on UDP. This is a successful automatic fallback, not evidence of direct connectivity in that role.
- PC-host forced-TURN passed a shared race and return to the room; UDP counters were zero.
- Mac-host forced-TURN also passed a shared race and return to the room; UDP counters were zero.
- Both automatic-path runs completed two shared races; both forced-TURN runs completed one.
  No freeze or peer-failure lines appeared. Temporary diagnostic logging was removed from
  the Mac snapshot before the forced-TURN checks; no gameplay or transport fix was needed.
- The initial apparent start stall was a partial-log reading during a slow load. The complete
  log shows that race finished; additional tracing and repeat runs required no gameplay fix.

## Result

Windows friend-test snapshot verified and packaged. The existing SFU Worker was not redeployed.
macOS now has a native build and PC/Mac gameplay validation. Android has a Rust compile check
and browsers a real WebRTC/WASM session check. Linux, physical Android, Safari/iPad/iPhone,
full browser gameplay and native/browser interoperability still require device testing; do not describe them as validated. Native iOS remains deferred.
TURN credentials currently last one hour: recreate long-running prototype rooms rather than
adding credential recovery or host migration to this first experiment.
