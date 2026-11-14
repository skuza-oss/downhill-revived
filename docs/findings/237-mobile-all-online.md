# 237: phones get every online mode

Status: current
Checked: 2026-11-14

Supersedes: 143 (no online on phones); and for Android and iOS the P2P-only rule of 180, 186 and 224 (224 stays current for the rest)

Branch `feat/mobile-all-online` from `main` 2e7f48c6, started 2026-11-14. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The owner, 2026-11-14: "Let's allow mobile to play all online modes normally." Android and iOS
offer the desktop's ONLINE menu: Quick Race, Public Races, Create A Public Race, Create A Private
Race, Room Region, Create A P2P Race, Join With A Code; the non-P2P rooms over the same SFU
transport (Cloudflare Realtime, TURN over TLS as the fallback, finding 177). This replaces finding
143's "no online on phones" and the P2P-only rule of findings 180, 186 and 224 for Android and iOS.
The browser is out of scope: it keeps P2P rooms only.

Done when: `check.py quick`, `port`, `docs` pass; boot and race state traces equal the base build;
on the Mi 11i (release-signed PGO APK of this branch, official server, private rooms): the menu
shows every row, an SFU private room made on the phone and on the PC each joined by the other, a
public room seen in the PC's list, Quick Race from the phone, a P2P room, Home for 10 s in an SFU
race, and the log's SFU link kind with no address of the other player.

## Progress

- [x] 2026-11-14: the one limit removed: `port/net/net.c`'s `NET_OFFERED` was `NET_OFFER_P2P` under
  `__ANDROID__` / `TARGET_OS_IPHONE`; every native build now offers SFU and P2P rooms, and the
  refusals that followed from it (`connect_to`, `net_join_room`, `net_list_lobbies`) are gone.
- [x] 2026-11-14: docs and players' texts (3e2c6e0f), comments (65a115dd), the AGENTS.md decision
  row with this finding's close.
- [x] 2026-11-14: PC checks and state traces; the phone runs a-f (below).

## Decisions

- The server needs no change: it never looks at a platform (nothing in `online/server/src` reads
  one); a phone is a member like a PC. No deploy.
- The browser keeps its own switch (`port/web/www/net.js`, `NET_OFFER_P2P`), as the owner said.
- No new unit test: no platform branch is left in `net.c` to test (one constant for every native
  build), and no unit test links `net.c`; the browser's switch keeps its test (`check_net.mjs`).
- Home in an online race keeps finding 227's rule, the same for SFU and P2P rooms: the game thread
  waits (`app_away_wait`), the network threads keep the room and the race link, the clock is
  rebased at the return and the race goes on (no pause); the others' games run on and hold this
  player's rider (and the AI riders this phone owns as host) until its snapshots come back. A long
  absence (beyond the room's and WebRTC's timeouts, about 30 s, or iOS's suspension) loses the
  links and the room treats the player as gone, as a lost network. Nothing to fix.

## Surprises

- `check.py port` sat 35 min in `stack_escapes`: one `cc1.exe` of the old EE compiler hung at 0 CPU
  on a temp file in the scratch folder; killed by PID, the step rerun alone passed. Not this lane's code.
- Home during the track's load (before GO) only delays the start: the others wait at "waiting for
  the others" (the PC waited 13.5 s) and both count down together.
- A room just left can stay in the public list for a few seconds; Look Again drops it.

## Result

Every native build offers every room; the server is unchanged (it never reads a platform), so no
deploy. The players' release notes' sentence "On Android and iOS online play is P2P races" lives in
the release folder's `RELEASE-NOTES.md` (written per release, not tracked) and in the tracked
`port/release/ANDROID-NOTES.md`, `IOS-NOTES.md` and `README.txt` (changed here).

Checks (Windows): `check.py quick`, `port` (the stack steps rerun alone after the hung compiler),
`docs` pass; `state_trace_run.py boot` and `race` against a base build of the same code as
2e7f48c6 (lane 236's tip 9782fc09): `all 600 rows match`, `all 5000 rows match`, PASS. This
branch's core id ca62b4188e56b7d1 (not 0.8.2's), so its rooms never mix with players'.

The Mi 11i, a release-signed PGO APK of this branch (versionCode 802), the official server, one
hidden PC game of this build each time:
- a. ONLINE shows Name, Quick Race, Public Races, Create A Public Race, Create A Private Race, Room
  Region, Create A P2P Race, Join With A Code. Create A Private Race on the phone (WRFET2, EU West),
  the PC joined by code, READY by tap, a race: both `results:  1. LANEPC 0:20.03  10. RIDER
  0:00.00`; zero `refused`, `freeze:`, `crash:` on both.
- b. The PC made EEWN2E; the phone typed it on Android's keyboard (`joining EEWN2E`), shop START,
  READY by tap: both `1. LANEPC 0:45.03  10. RIDER 0:00.00`, zero errors.
- c. Create A Public Race on the phone (S6CJT4): the PC's Public Races list took it (`view=1`,
  `joining S6CJT4`). The phone's own list by touch: a real player's room as OTHER VERSION, Look
  Again, Back one step (finding 230). Quick Race with no room: "No public race to join right now",
  Create A Public Race or Back; with the PC's public room ZCRKG7: `quick race: room ZCRKG7 (1/10,
  open, region EU)`. Public rooms closed within a minute.
- d. A P2P room made on the phone (`this game is the room's hub`, the address notice), the PC joined
  (`member 1 connected: direct, inside the local network`): equal results, zero errors.
- e. Home for 10 s in an SFU race (the phone's race clock about 23 s): phone `gs: in the background;
  the game waits`, `back in the foreground after 11.7 s; the game clock goes on from now`, `touch:
  back from the background in a race: online, the race goes on`; the PC ran at 60 fps throughout,
  no `freeze:` or stall, held the phone's riders (`gaps>100ms 2`, `slot 2..5 rides on after a
  hold`); equal results `1. LANEPC 0:20.03  6. RIDER 0:00.00`.
- f. `net: SFU connected via UDP 769 ms after the answer` (the PC: via UDP 685 ms); no other
  player's address, SDP or session id in either log.

Not run here: TURN over TLS from a phone (Android takes no `DHD_NET_SFU_UDP=0`; the path is the same
Rust code the desktop's tests force), a phone's reconnect after a network loss (Wi-Fi left alone),
iOS (no Mac work in this lane: the change is the shared `net.c` constant; the iOS app has no other
online code), 6+ racers.

Checked:
- 0.8.3 gate on 128a743a: `check.py port -j 6`, `check.py docs` and `check.py server` OK; the seven state traces
  (`boot`, `race`, `race2`..`race6`) equal `main-524948ba-ref-v3`.
- Mi 11i, release-signed PGO APK, official server, one hidden PC game of this build each time: ONLINE shows all
  eight rows; a private SFU room made on the phone (the PC joined by code) and one made on the PC (the phone typed
  the code): equal results both ways, `SFU connected via UDP`, zero `refused`, no address of the other player in
  the phone's log; a public room made on the phone was taken by the PC's Quick Race, and the phone's Quick Race
  took the PC's public room; a P2P room made on the phone: equal results; Back in ONLINE goes one step.
- One room made on the phone (CDG) never got its SFU link: five sessions in 63 s failed on UDP and on the TURN
  relay (TURN error 400). The next room connected via UDP in 717 ms, as did every later one; TURN over TLS between
  two PC games on the same server worked the same hour. Not reproduced; TURN over TLS from a phone stays untested.
- The iOS Simulator (iPhone test device, sound off) joined a PC game's SFU room with the test switches and both
  games ended with equal results (`1. PCHOST 0:20.03  2. SIMIOS 0:40.03`).
