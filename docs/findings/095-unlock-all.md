# 095: PC SETTINGS "Unlock All" reads, never writes

Status: current
Checked: 2026-11-04

The option (dhd.ini `unlock_all`, `DHD_UNLOCK_ALL`, `port_unlock_all()` in `port/hle/pc_settings.c`,
off by default) makes the game read every unlock as the retail unlock-all code `func_001F0780`
(called from `func_0012C9E0`) leaves it, without that code's writes. Only readers are patched
(`port/patches_unlock.py`, tracks in `port/patches.py` next to `func_00193F38`). The profile
`D_004225A0` (the whole 0x12240 bytes go to the memory card) is never touched, so switching the
option off brings the save's own state back and a save made while it is on stores no extras. The
writers (career rewards `func_0014E6A0`, `func_00169920`, `func_0014F868`; the shop `func_0013F730`;
`func_00194290`) keep reading the real fields, so real progress made with the option on is still
recorded.

## What the cheat sets, the readers, what is patched

| Profile field | Meaning | Cheat | Readers | Patched |
|---|---|---|---|---|
| +0x12114 (32 bits) | tracks | `0x035FFFFF` (all but 21, 23) | `func_00193F38` (+ `func_00193F68`) | yes (main session) |
| +0x12180 (16 bits) | per-rider career bonus (set when the rider's career ends, `func_0014E6A0`) | `0xFFFF` | `func_00193FC8`: rider select (`001EB1D8.cpp`), rider list in `func_001F9118` | accessor ORs `0xFFFF` |
| +0x12110 (14 bits) | pro riders (rider-select statues) | not set | `func_00193FE8`: `func_001BB670`, `func_0018B3A8` | accessor ORs `0x3FFF` (see below) |
| +0x12188 (18 bits) | sponsor / bike catalogue rows (career rewards `func_0014F868`, names `D_00277EC0`) | `0x3FFFF` | direct: `func_00139BF8` (2), `func_0018B3A8` | each read ORs `0x3FFFF` |
| +0x12190..+0x121A6 (23 bytes) | shop items: extras, cheats, options, 4 bikes, a pro rider | all 1 | direct (below) | each read also answers 1 |
| +0x11C68 (63 x {frame, part, colour}) | bike garage; all six `D_004C8508` groups point at it (`func_001FA590`) | appends each of the 41 presets `D_0026DEA0` it lacks | `func_001F9B98` (entries), `func_001F9BF0` (count): bike menus `func_0013CB00`, `func_00139BF8`, the cheat | both accessors answer from a port-owned list |

The 23 shop flags' readers: the shop rows `func_0013F600` (shown bought: price 0, which only toggles
the row, `func_0013F940`, never buys), `func_0015E238` (pause rows, +9/+0xF), `func_0015F0C8`
(options rows, +0xA/+0x10), `func_00176568` (+0xC/+0xE copied to a race object), `func_00193B20` (on
load, options not owned are switched off: +8/+0xA/+0x10/+0x16), `func_0019B780` and `func_001DDE70`
(+5..+7 to `func_0019BDA0`), `func_001F9118` (extras rows, +0..+4), and the front end's menus in
`001EB1D8.cpp` (`func_001ED900`, its two-player twin, `func_001EE2A0`, the menus at lines
~1850/~2340/~3750, `func_001F0160`: +8, +0xB..+0xF). +0x11..+0x14 (shop bikes) and +0x15 (shop pro
rider) are read only by the shop rows.

A sweep of `build/asm` for these offsets found no asm-only (recompiled) reader; every reader is C.

## Notes

- Garage list: the port's list is the save's bikes first, then each missing preset in table order,
  capped at 63 like `func_001953B8`: what the cheat would have stored. It is rebuilt on each
  accessor call; additions (shop, pro riders, career) still go to the real list through
  `func_001F9C78`. Entered while the option is on, the cheat finds every preset present and adds
  none to the save (its other writes still happen, as in retail).
- Pro riders (+0x12110) are beyond the retail cheat, which leaves the pros locked on the rider
  screen; online's "everything unlocked" (`port/hle/online_menu.c`) sets the same 0x3FFF, so the
  option does too. `func_001BB670` caches the bits in `D_004B25A0.x3C` (not saved) and runs on each
  rider-select entry, so a toggle shows the next time the screen opens.
- Online: it still writes and restores its own words with the memory card held; the readers' OR is
  harmless there. online.c picks riders from its own fixed `regular` list, not the save bits.
- Left out: nothing the cheat gives. A bike picked from the port's list is copied by value to
  `D_0036B6B0` and the race setup in `D_004B25A0`, not to the profile.

Check: `python port/tools/build_wasm.py` compiles and links with the patches. No in-game run. 
