# Session rooms (0.7.4)

Session rooms keep the WebSocket room alive across races. The creator's hello
feature selects the new state machine; old rooms retain their previous behavior.
Grid membership, finish records, timers, running rule copies and last results
are persisted with the Durable Object. Its one alarm serves countdown, finish
deadline, intermission and ping/listing refresh. Relay sockets contain only
active grid entrants and rebuild after hibernation.

The client advertises session, parses room/results/list fields, and exposes
server epoch ms for comparison with host_service_net_server_ms(). Native imports
use the same host wrappers on Windows, Linux and Mac. The browser worker had
network stubs; it now implements the wire and writes the shared C ABI using
offsets generated from net.h. Four clang targets check those offsets.

The cherry-picked native serializers were retained. Their integer choices are:
next race 0 auto / 1 host; track action 0 same / 1 next / 2 random.
Rotation accepts up to 64 bytes; the host bridge checks the full guest span.
Finding 116's groundwork text was moved here because 116 was already occupied.

Decisions:
- Shared rider picks remain legal, following the existing issue #17 behavior.
- A race closes when all humans have finished, even before their final back.
  Otherwise a finished client stays attached to relay until back or closure.
- Empty or single-track rotations keep the track; absent current track selects
  the first list entry for next. Duplicate ids are accepted; screens send unique ids.
- Cancelling the last grid entrant restores AUTO with a fresh 30 seconds.
- A void also applies the running nextRace policy, without replacing lastRace.
- Session names replace C0 controls and unpaired UTF-16 surrogates with U+FFFD. This bounds JSON escaping while
  keeping snapshots below 4 KB with eight members and previous results.
- No production address, deployment, game data or game-screen changes are needed.

References: Cloudflare's [alarm API](https://developers.cloudflare.com/durable-objects/api/alarms/)
and [Workers best practices](https://developers.cloudflare.com/workers/best-practices/workers-best-practices/).
The implementation follows the existing persisted state and hibernated WebSocket
structure. No external code was copied. The full screen contract is in
docs/online-protocol.md, "Session rooms (0.7.4)".

Verification on this branch:
- Server: 46 tests passed (32 existing, 14 session tests); TypeScript check passed.
- Native: test_hash, test_json, test_loopback and test_session passed.
  Loopback covers three four-player latency/drop scenarios; test_session has
  17 session parsing/serialization checks. Nettool builds with --no-run.
- Browser: check_net.mjs passed (message serializers and shared snapshot writes).
- gen_net_layout.py --check passed all field offsets and sizes for four targets.
- Local Wrangler on port 8788: three native nettool connections across two
  completed races. Finisher and give-up players entered both grids. Late join
  waited during the first race and entered the second. All exited zero.
  Wrangler was stopped afterwards. Nothing was deployed.

Build note: this C-only work reused an existing F:-resident SDL3 build and Rust
renderer archive. build_tests.py accepts DHD_NET_TEST_RUST_LIB to select that
archive without rebuilding Rust; the normal path still calls rustgs.py.
Build output, dependencies and scratch are on F: through local junctions.
No full retail-data game build was needed. Screens remain the other lane's work.

## Three fixes from the release checks (2026-10-30)

Found by the in-game checks of finding 118:

- **Ready across a race.** The Ready that put a member in a race stayed set while they were
  back in the room during it: the room showed READY NEXT they never pressed, and Square then
  took it away. The race's close cleared every racer's Ready, one pressed in the room meanwhile
  too. Now `retire` (back, finish-and-back, time up, left) clears the member's Ready, and the
  close clears only those still riding.
- **A restart's ghosts.** A deploy (or Cloudflare restarting the object) drops every socket
  with no close event, so the members stayed while each game reconnected (client.c rejoins a
  lobby it was in) as a new member: the room filled ("The room is full", seen when Wrangler
  reloaded under a room) and the host stayed a ghost nobody could replace. Now a hello under
  the name of a member with no socket takes that member back (id, host, rider, Ready), and
  members not back within the hello timeout (10 s after the instance started) leave as
  disconnects at the next alarm. Checked in the game: Wrangler reloaded under a room, both
  players came back as themselves, the game still the host.
- **A race with one rider.** An AUTO room's timer (or START NOW with only the host ready)
  starts a race with the members ready then; one still on the rider screen waits for the next.
  When that left a single rider, the game (`online.c` barrier) still made it two slots: it
  waited 30 s for the empty slot's rider (a freeze in the log) and put a dead puppet on the
  grid. Seen in the PC<->Mac run: the Mac was still choosing when the timer fired. A one-slot
  race now runs alone, with no puppets.

Server tests: 51 (two new: "back in the room, the race's Ready is spent...", "after a restart,
a player back under their name...").
