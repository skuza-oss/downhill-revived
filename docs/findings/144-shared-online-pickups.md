# 144: Shared online track pickups

Status: current
Checked: 2026-11-04

## Retail lifecycle

The pickup pool is `D_004348F0`: 150 twenty-byte entries at +0x18,
with the used count at +0xC00. `func_001961D8` inserts in level-load order;
`func_001964A8` installs the type and spawn position. `func_00196970` gives
these entries the touched-method vtable `D_002873D0`.

`func_00196590` checks active, race mode, crash and rider type, gives the
collector its reward, stores its rider pointer at +0xC and the configured
duration at +0x10, then clears the scene node's active byte (+4).
`func_00196888`, called by `func_00196138`, first waits for that rider's
scene position to move more than ten units away (squared distance > 100).
It clears +0xC, and on subsequent steps subtracts `func_00238F60()` from
+0x10. At expiry it restores the active byte and reloads the duration.
Thus +0x10 is the pickup respawn countdown, not a rider effect duration;
+0xC holds off that countdown while the collector remains nearby.
The constructor's durations are 60 ticks except type 3 (1800).
`func_00196820` resets the taken state; `func_00196558` disables a slot.
Neither retail function is changed.

## Port implementation

The touched-method patch first checks ownership. An online puppet cannot
collect locally; only this PC's human and, on the host, its AI can.
The existing puppet code skips local input/physics for remote riders, but
the explicit touched-method check also protects other collision paths.
After successful retail collection, EVENT version 2, type 9 broadcasts:
version, type, pool index, global collector slot, pickup type, track (one
byte each), then the exact twelve spawn xyz bytes. All integers/float
words use the existing little-endian client format. Heap pointers never
travel. The pool index follows the same level-file order on each PC;
track, type and spawn position must also match before applying an event.

The receiver checks length, index, rider and sender ownership, track,
type, position and active state. It installs the collector's local rider
pointer (normally a puppet), the original configured duration and inactive
byte. No reward, sound, statistics or rider effect function is invoked.
The retail update then handles the distance hold and respawn, using the
puppet's snapshot position. Network transit/interpolation can shift the
remote timer by its delivery delay; no lockstep or frame-rate assumption
is introduced. Already-taken pickups ignore events, so simultaneous local
collectors retain their rewards. Both menu rooms and scripted Create/Join
use this same event path. Offline hooks return without changing game RAM.

`online/server/src/room.ts` relays every FRAME_EVENT to other race sockets,
with the authenticated sender slot; no server modification is required.
Wire/identity tests are in `port/hle/test_online_pickup.c` (standalone C11).

Research: consulted Glenn Fiedler's state-synchronization discussion
<https://www.gafferongames.com/post/state_synchronization/> as background;
this change uses the repository's existing reliable event relay. The
lifecycle and offsets above come from the matching retail sources and
vtable. No external code was copied.

## Validation

- `build_wasm.py -j 6`: all 3815 game units and 38 module units compiled;
  strict link passed, offset scan clean. Core id `31b5ecad0513ff50`.
- `build_host64.py build/wasm/dhd_core.wasm --jobs 6 --num-outputs 6`:
  PASS, 141 imports, 40 host objects (Windows x64).
- Standalone wire/identity test compiled with Clang, `-Wall -Wextra -Werror`,
  and passed. Patch anchor application was checked separately.
- Built the unmodified `48013f6` core and Windows host first. Ran
  `state_trace_run.py boot` (600 fields) and `race` (5000 fields) on that
  executable and the changed executable, at the harness's deterministic
  60 fps / `DHD_FRAME_CLOCK=1` settings. `state_diff.py`: all 600 and all
  5000 rows match, respectively. No mask or code-pointer list changes.
- Local relay race: two `hidden_game.Game` instances through
  `local_server.running()`, scripted Create/Join, `DHD_TEST_INPUT=1`,
  60 fps, track 0, computer riders enabled, both humans pedalling.
  Logs: `build/pickup-room/host/game.log` and
  `build/pickup-room/guest/game.log`. Ten AI collection events were sent;
  the guest applied all ten, in the same order. For example:

  ```text
  online: pickup 53 taken by slot 4 (sent)
  online: pickup 53 taken by slot 4 (event from player 0)
  ```

  Pickup 40 was taken successively by slots 5, 3 and 9, with matching
  received/hide logs each time: it respawned and could be collected again
  on both PCs. No freeze lines. The harness stopped both games and its
  server before completion.

Untested: a human physically collecting on either PC (neither scripted
line hit a pickup), simultaneous collection, menu-room rematches,
host migration during pickup flight, other tracks, mixed frame rates
and cross-platform peers. Exact wall-clock respawn alignment was not
measured; the unchanged retail timer and its collector-distance hold
run on each PC, with network/interpolation delay as described above.
Server tests were not run because no server code changed.
