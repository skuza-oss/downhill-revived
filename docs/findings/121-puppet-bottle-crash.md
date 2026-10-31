# 121: Online crash when a remote rider throws a bottle

Two players sent the dhd.log of a 0.7.3 crash in an online race (2026-10-30). One PC hosted
(one puppet: the guest, 60 fps). The other was a guest in another room (nine puppets: the host
and eight computer riders, 100 fps). They had different CPUs and GPUs and raced on different
tracks. Both logs end the same way:

```
host64 memory: write of uncommitted guest address 0x000000010 at rip ... (image base ...)
  at rva 0x8e5a22
  ...
```

The two stacks are identical, so the cause is a fixed path in the game code, not a driver.

## Reading the stack

The rvas are offsets into that build's dhd.exe. Its lld-link map (`dhd64.map`, from
`build_host64.py`) gives them names. `port/tools/crash_stack.py --map MAP dhd.log` prints
them. `package_release.py` now keeps the map beside the Windows zip
(`DownhillRevived-<version>-win.map`). 0.7.3's map still existed in its build tree: dhd.log's
first-line hash, 7b5170238c98b723, is its `release-work/dhd64.exe`.

```
w2c_core_ps2_store128              a 128-bit store (sq) to guest 0x10
w2c_core_run_fast_program          the asm runtime (port/runtime/ps2asm.c)
w2c_core_ps2a_1197e61788e9bc2b     an inline-asm block: COPY_VECTOR's lq/sq
w2c_core_func_00153DD0             the bottle move's update
w2c_core_func_00152410             the attack state's (+0x5E90) update: its current move's
w2c_core_func_0012D380             the race step's state pass (pass 4)
w2c_core_func_0011A100             the race step
w2c_core_func_0016C0C8 / rc_func_001B40A0 / ...   the race frame loop
```

## The cause

The attack state (+0x5E90) holds five moves: punch, kick, check, stick and bottle (+0x5F40).

- Its test (`func_00152318`) asks each move's test and makes the first that passes the current
  move (`x8`).
- The state machine then switches to it. The Enter (`func_001523C0`) runs the move's start.

The bottle's start, `func_00153C70`:

- asks the move's test again, through `func_001525E8`, for the throw's animation (`x14`);
- uses up a bottle and sets `x2C = -1` (a bottle in the hand);
- takes a bottle object only when that animation is not 0 (`x28 = func_001591D8(model)`).

The bottle's update, `func_00153DD0`, moves `x28` to the hand whenever the bottle model is
loaded and `x2C == -1`. The first store is `x28->pos`, at `x28 + 0x10`. The bottle's exit, the
end of its flight and its hit all free the object and leave `x28 = 0`.

The bottle test (`func_00153AA8`) only passes while the throw button shows on the rider's pad.
A rider run on this PC is tested and switched in the same step, so the start always finds the
throw and takes a bottle.

A puppet is different (finding 044). Its own switches are blocked (`hle_puppet_switch`), but
its state pass still runs the tests, so the bottle can become the attack state's move while the
pad shown from a snapshot has the press. The switch into the attack state comes later, when a
snapshot says so (`hle_puppet_physics`: snapshots come about 20 a second and are shown 100 ms
behind). By then the press is usually gone, and the start's test fails:

- `x14 = 0`, so there is no animation and no bottle object, and `x28` stays 0;
- `x2C = -1` all the same;
- the next update writes the hand's position to guest 0x10.

That is the fault in both logs. In the guest's log, a rider was hit by a bottle earlier in the
same race (`crash kind 9`: `func_0012E940` turns a bottle hit, kind 4, into crash 9), so the
host's computer riders were throwing bottles. Finding 047 already noted that no test run ever
picked up a power-up. Puppets have taken the attack kind and bottles from snapshots since then
(2026-10-16), so 0.7.3 did not bring this in. More players racing online made it show.

## The fix

`port/patches.py`, unit `00153A30.c`: the update moves the bottle in hand only when there is
one (`&& obj->x28`). A rider run on this PC always has one there, so nothing changes for it,
offline or online. A puppet whose start missed the throw shows no bottle for that throw. Its
owner's PC still throws it, and any hit arrives as that PC's event, as before.

Possible later work, not done: let a puppet's bottle start find the throw, so that remote
throws show their bottle every time.

## Checked

The crash on one PC, offline, by a puppet replay (`DHD_PUPPET_PLAY`, finding 044):
- The standard race (race_compare's screen-keyed route, seed 81257) was recorded with
  `DHD_PUPPET_RECORD`, one computer rider a run. Riders 2-7 carried 3 bottles from about 30 s
  but threw none.
- One recording was edited so the order that leaves a puppet without a bottle happens:
  - at clock 3033 (riding, bottles held) one snapshot shows the throw press (the bottle test's
    action words, rider +0x11A4 and +0x126C), so the puppet's own test makes the bottle the
    attack state's move while its switch stays blocked;
  - from 3063 to 3152 the snapshots say the attack state (+0x5E90) with no press shown.
- Replayed onto slot 1 (one snapshot in 3 ticks, 100 ms behind), 0.7.3's release exe
  (7b5170238c98b723) stopped with `write of uncommitted guest address 0x000000010`. Its 16-frame
  stack is the players' rva for rva.
- 0.7.4's exe played the whole recording, past clock 13983, with no fault.
