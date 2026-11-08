# 044 — Puppet riders: state-sync online races

Status: current
Supersedes: parts of 016, 039, 040, 041, 042
Checked: 2026-11-06

This is the design online races still use. Later changes on top of it: snapshots are stamped
with race time in ms since GO, not ticks, and each PC races at its own frame rate (wire version 3,
`STATE_FIXED` 104 bytes, finding 114; `docs/online-protocol.md`); finishes and results, leaving,
the grid and hits (finding 047); the in-game ONLINE menus and rooms instead of a boot-time lobby
(findings 065, 092, 118, 119); riders' own looks and bikes (finding 128); shared track pickups
(finding 144); ten players (finding 123). Since 0.7.12 (finding 158) the race frames travel over
Cloudflare Realtime SFU instead of through the room's relay, 30 snapshots a second (every 2 race
ticks), and a puppet's delay adapts to each sender (50-150 ms); the sections below give the first
build's numbers.

2026-10-16. Replaces input lockstep (finding 041) as the way online races work
(`docs/ONLINE-PLAN.md`, decision table). Every PC runs a normal one-player race (full screen,
full-detail level files, `DHD_RENDER=native`) and owns its rider: no input delay. About 20 times
a second it sends a snapshot of the riders it owns; the other players, and on the guests the AI
riders, are **puppets**: AI rider slots whose input and physics are switched off and whose state
is written from the snapshots.

## What is there

| Where | What |
|---|---|
| `port/hle/puppet.c` | The puppets: snapshots, interpolation, the race-step hooks, crashes, hits, the wire format, record/replay for tests. |
| `port/patches.py` | Hooks in `0011A100.c` (race step), `0012FE38.cpp` (State switch), `00155E78.cpp` (crash start), `0012E940.c` (hit). |
| `port/hle/online.c` | The online race: (then a lobby barrier at boot and scripted menus; now the in-game room, findings 092, 118), the load wait, then `state_sync` each field. No lockstep. |
| `port/net/` | `net_send_frame` / `net_receive_frame`: binary STATE and EVENT frames (over the SFU since finding 158; `port/render/net`). |
| `online/server/` | The room; it relayed frame types 5 (STATE) and 6 (EVENT) until finding 158, and now sets up each racer's SFU session (`docs/online-protocol.md`). |
| `port/tools/online_race.py` | Two instances on one PC, host and guest, each on its own scripted line; screenshots and memory dumps at race-clock moments. |

## The rider (`D_002AF2C0`, 10 riders × 0x7BD0)

Offsets from the rider. Slot 0 is the PC's own player; in a one-player race slots 1-9 are AI.

| Offset | Meaning |
|---|---|
| +0xFE0 | Virtual pad (Input object). The human decoder (`func_0017DD70`) or the AI writes it. |
| +0x1174 (+0xFE0 +0x194) | 171 words: this tick's 25 action words, last tick's 25 (words 25-49), hold timers (+0x324 of the pad), a counter (+0x3EC), flags (+0x41D-0x43D); about 24 words are constant config. |
| +0x3FD0 | Crash controller. `func_00155E78(+0x3FD0, kind)` starts a crash, `func_00156580` runs it. |
| +0x407C | Crash kind (what started it). |
| +0x4098 | Crash flags: bit 2 = crashing (0xB riding, 0x17 crashed). |
| +0x40C0 | Rigid body. Its +0x34/+0x38 point to the position and rotation below. |
| +0x4110 | Previous position (+0x50). |
| +0x4120 | Linear momentum, mass × velocity (+0x60). |
| +0x4130 | Angular momentum (+0x70). |
| +0x4140 | Inverse inertia (+0x80). |
| +0x4180 | Velocity, units a second (+0xC0). |
| +0x4190 | Angular velocity (+0xD0). |
| +0x4210 | Mass, 100 (+0x150). |
| +0x4218 | Owner (+0x158). |
| +0x4220 | Body state (+0x160); `func_00135690` integrates by it. |
| +0x4270 | Position (+0x1B0). |
| +0x4280 | Orientation, 3 rows of 4 floats (+0x1C0). |
| +0x43A0, +0x4420 | Front and back wheel contact bodies (+0x2E0, +0x360). |
| +0x4414, +0x4494 | Their contact state (+0x74): 0 none, 1 ground, 2 air. |
| +0x4ECC | Current State (a pointer to one of the State objects inside the rider). |
| +0x4ED0 | State: riding. |
| +0x4EE0 | State: crashed. |
| +0x4EF0 | The thrown rider's body during a crash (`func_001A4F70`). |
| +0x5E90, +0x5FE0, +0x5FF0, +0x6210, +0x621C | Further States (+0x5FE0 is the grid before GO; the AI uses 0x5E90, 0x6210 and 0x621C all the time). |
| +0x6248 | State: punch (attack). |
| +0x6268 | State: stagger (a small hit). |
| +0x6298 | State: push-off at the start. |
| +0x6420 | The bike's own rigid module during a crash (`func_00133980`). |
| +0x78D4 | AI controller (AI riders). |
| +0x792C | Body scene node: position +0x10, rotation rows +0x40. |
| +0x7944, +0x7948 | Front and back wheel matrices: (+0x24, +0x28) = (cos, sin), +0x34 = -sin, +0x38 = cos (`func_00134CC8`). |
| +0x7978 | Pad or AI index. |
| +0x7A5C | Rider type: 1 human, 3 AI. |

Other addresses: race clock `D_0043EEC8` (ticks from GO), race phase `D_0043F348` (1 countdown,
2-3 race), menu `D_0029DED8` (9 = the race screen), camera 0's rider `D_0037EA60[0]` +0x2B4
(EE 0x37ED14).

Not in the snapshot yet: power-up state and lap/progress. Track progress is computed on each PC
from the position (`func_0012E160`), so a puppet's progress follows its position.

## The race step and the hooks

`func_0011A100` steps each rider through five passes:

1. `func_0012C8B0`: input. Shifts the virtual pad (`func_0017DCC8` prev = cur, `func_0017DC78`
   clears cur), then the human decoder or the AI controller (`func_0010BE38(+0x78D4)`).
2. `func_0012D248`: bookkeeping (previous positions).
3. `func_0012D360` → `func_00135690(+0x40C0)`: physics; the integrate (`func_00135A30`) ends
   with the wheel spin and `func_0012DDC0` (body to scene node).
4. `func_0012D380`: the State's update (vtable); it may switch State through `func_0012FE38`.
5. `func_0012D3C8`: effects, contact, track progress, sound.

Then the rider pairs collide (`func_00134688`).

For a puppet:

- **Input:** `hle_puppet_input` replaces pass 1. It shifts the pad itself and writes the remote
  rider's pad words, so the State's update and the animation see the same actions.
- **Physics:** `hle_puppet_physics` replaces pass 3. It writes position, orientation, velocity,
  angular velocity, momentum, wheel contacts and wheel spin, then `func_0012DDC0`.
- **State:** the game's own switches are blocked (`hle_puppet_switch` in `func_0012FE38`).
  Only the snapshot switches State. Letting the State machine run freely and forcing it
  only when it disagreed worked for a replayed human, but on AI puppets it flipped every
  field (0x621C/0x6210 against 0x4ED0, because their pad words alone don't decide the AI's
  States), and after a hundred such switches an Enter crashed the game (`func_00202E38`,
  writing address 5).
- **Crashes:** a puppet never starts a crash by itself (`hle_puppet_crash` in
  `func_00155E78`). When the snapshot's crash bit rises, the puppet starts the same kind of
  crash once. While it is crashing nothing is written: the bike (+0x6420) and the thrown body
  (+0x4EF0) are simulated locally until the respawn. Writing the body during the crash threw
  the rider into the sky.
- **Hits:** `func_0012E940` asks `hle_puppet_hit`. A hit between a puppet and anything is not
  applied locally. If the attacker is local (this PC's rider, or the host's AI), it becomes an
  EVENT for the PC that owns the victim, which applies it to its own rider. The victim's next
  snapshots carry the result: the stagger State, or a crash.

Every pass but 1 and 3 runs as usual: the State's update and animation, progress, effects,
sound, and collisions with other riders.

## Interpolation

Each puppet keeps its last 32 snapshots. It estimates the sender's clock from arrivals, as a
moving average of (snapshot clock − local clock), weight 0.05, reset when off by more than 30.
It shows the sender `DHD_PUPPET_DELAY` behind the newest it holds (then 6 ticks; now milliseconds,
default 100, `port/hle/puppet.c`):

- position, velocity and angular velocity are interpolated between the two snapshots around
  that moment;
- orientation is a blend of the two matrices, re-orthonormalised (Gram-Schmidt);
- if no newer snapshot has come, the position is extrapolated along the velocity, for up to
  12 ticks.

A snapshot more than 60 ticks older than the newest held clears the ring: a new race.

## Wire format (version 1, as first built; version 3 today, finding 114)

STATE (type 5) and EVENT (type 6) are binary frames: byte 0 the type, byte 1 the sender's lobby
slot (stamped by the server), then the payload, at most 254 bytes. The relay forwards them to
the other racers in the room, in order.

STATE payload, 101 bytes plus 5 for each pad word that changed since the last one sent:

| Bytes | Field |
|---|---|
| 1 | Version, 1. |
| 1 | Rider: 0xFF = the sender's own; else the AI slot (from the host). |
| 4 | Sender's race clock. |
| 12 + 36 | Position; orientation, 3 rows of 3 floats. |
| 12 + 12 | Velocity; angular velocity. |
| 16 | Wheel spin, (cos, sin) for the front and back wheel. |
| 2 | Wheel contacts. |
| 2 | State (offset in the rider). |
| 1 + 1 | Crash flags; crash kind. |
| 1 + 5n | n changed pad words: (index, value). Last tick's words (25-49) are not sent. |

EVENT payload (18 bytes): version 1, type 1 (a hit), the victim's PC (lobby slot), the victim
(0xFF = that PC's own rider, else an AI slot), the attacker (0xFF = the sender's own rider, else
an AI slot), hit kind (0-9), and the direction (12 bytes). There is also type 2 (this PC has
loaded the race), used by the start wait.

Who owns what: every PC owns slot 0. The host also owns the AI slots from the number of players
up. Rider slots of the other humans: own = 0, then the others in lobby order from 1. The host's
AI slots keep their numbers on every PC.

## Online race flow (`port/hle/online.c`)

1. (Then) at boot the PC waited in the lobby (the barrier) until the host started, and a menu
   script went to a single race. Now the player creates or joins a room in the game's ONLINE
   menu (findings 065, 092, 118).
2. On the load screen the lobby's track and this player's rider go into the setup block
   `D_004B25A0`, and the room seed into `DHD_RACE_SEED`.
3. At the countdown every PC sends EVENT type 2 and waits up to 60 s for the others. The
   field clock slips by the wait, so the GOs land close together.
4. Each field it receives frames. Every 3 race ticks it sends one STATE for each slot it
   owns, plus any queued hit EVENTs. Once a second it updates the window title and logs the
   counts.

Each PC keeps its own frame rate; nothing waits after the start. There's no state hash.

## Results

**Local proof, one PC** (`DHD_PUPPET_RECORD` / `DHD_PUPPET_PLAY`). Rider 0's snapshots from a
solo race, replayed onto AI slot 1 of a second solo run, one in 3 ticks, 6 ticks behind.
Positions are compared against the original 6 ticks earlier.

- **Line:** the puppet follows rider 0's exact line with the same pose (screenshots side by
  side at g+600, g+1200, g+1800).
- **Error:** 0.010 units mean, 0.30 max (before the State rule changed). With the final rule,
  on a fresh recording with two crashes (`build/port/pp7` → `pp8`), 694 ticks outside
  crashes: 0.013 mean, 0.38 max.
- **Smoothness:** the second difference of the position is as small as the original's
  (0.0137 against 0.0195), and consecutive frames look smooth. No jitter, no teleporting.
- **State:** switches land within 0-2 ticks of the original. Over 800 ticks the State differs
  on 6 ticks, all at switch edges.
- **Crashes:** the original crashed at ticks 503 and 597. The puppet starts the same kinds at
  510 and 603, and the black fade and respawn match.
- The race ran to the end without crashing.

**Live, two instances through the relay** (`online_race.py`, host and guest on this PC).

- **Start:** lobby, start, setup, load wait and the same GO all worked.
- **Run 1:** puppets froze at the grid. Snapshots sent on the race screen before GO carried
  clocks of about 1400-1500; after GO the clock restarts at 0, and the receiver dropped every
  later one as old. Fixed on both ends: snapshots only on the race screen in phase 2-3, and a
  clock that jumps far back clears the ring.
- **Run 2:** the guest crashed at tick 114 from the State flipping on AI puppets (above).
  Fixed by blocking the game's own switches.
- **Run 3** (the fixed build), to race tick 936: the host sent 2817 snapshots (its rider and 8
  AI), the guest 319, and each received what the other sent, minus those in flight. Owner
  against puppet at the same race clock (memory dumps):

| Rider | g+600 | g+900 |
|---|---|---|
| Guest's rider on the host | 12.5 units, 7.4 ticks behind | 6.4 units, 3.8 ticks |
| Host's rider on the guest | 34.8 units, 22.8 ticks | 28.4 units, 18.3 ticks |
| Host's AI on the guest (8 riders) | 20-49 units, 21-29 ticks | 22-44 units, 17-19 ticks |

  The two directions add up to about 25 ticks: twice the 6-tick delay plus the relay and the
  3-tick send interval. The split between them is uneven because the two GOs were a few fields
  apart in wall time. States matched apart from that lag. An AI crash on the host showed on
  the guest (slot 5, kind 1). Neither game crashed.

## Limits and next steps

- **Lap and finish:** the finish (each PC reports its own time, the host publishes the
  results) was not done yet; finding 047 added it. Power-ups and pickups: findings 047, 144.
- **Rider models:** the riders' models per slot came from each PC's own race setup, so a
  puppet could wear another character than its player chose; findings 047 and 128 send each
  player's rider, bike, jersey and colour.
- **Crash detail:** the puppet's crash (bike tumble, thrown body) is simulated locally from the
  same start, so its details can differ from the owner's. The respawn point comes from the
  snapshots again.
- **Hits:** the hit events were coded but not triggered in these runs (the scripted lines
  never punched another rider); finding 047 ran them.
- **Clock lag:** the clock offset follows arrivals, so a puppet's lag is the delay plus the
  one-way latency, plus any GO difference between the PCs.

## Sources

- Glenn Fiedler, "Snapshot Interpolation" (gafferongames.com, 2014): sending snapshots at a
  lower rate and showing them a fixed delay behind, with interpolation and short
  extrapolation.
- Valve, "Source Multiplayer Networking" (developer.valvesoftware.com): entity
  interpolation 100 ms behind, the default this follows.
- Findings 039 (server), 041 (lockstep integration), 042 (client).
