# 133: Ambient sound too loud on Mt. Liddellroch (issue #19)

Status: current
Checked: 2026-11-04

Branch `fix/ambient-sound` from `next/0.7.7`. Public issue #19 ("Sound effects too loud"): the
ambient/background sound on every Liddellroch circuit and career race is wrong; the player's video
(not watched here) is a Liddellroch career race.

## Which tracks and sounds

- The game's track names (`SCUS_971.77` near 0x183148, in track index order) put **Mt.
  Liddellroch** at 11 (FR), 15 (MX) and 16 (TD): the levels `LVL/AUBERST`, `AUBERMX` and
  `AUBERSTD` (online_menu.c's AUBER ST / AUBER MX / AUBER STD; `AUBERST2`, `AUBERMX2`, `AUBTDH2`
  at 2 players). All of them use one sound bank, `SND/AUBE_S.BNK` (159 sounds; LSN 1226404 on the
  USA disc).
- "Kineticlops" is a rider (index 15, an unlockable). Nothing in the game's code or data ties a
  sound to him; the report most likely means the ambience heard in a Kineticlops career race on
  Liddellroch.
- The Liddellroch ambience is a forest bed: `E_FOREST_LEAF_L` (a launcher: it starts
  `E_BIRDFLAP_C_F_`, `E_BIRDS_LGT_L` and `E_WIND_LGT_L`, and `E_WIND_LGT_L` starts
  `E_RANDOM_THUNDE`) plus `E_TREE_WHOOSH`, which the game plays each time the rider passes a tree
  (37 plays in 90 s of the measured race).

## Cause

`E_TREE_WHOOSH` is a one-shot whoosh, then a **looping** wind tone (offset 40768, 1656 blocks,
loop flags), then grain type 24 (END) 45 ticks later. In the retail driver (`MOD/989SND.IRX`, read
locally; grain table at `.data` 0x1D7B8) the END handler (0x5A60) returns -1; the program
dispatcher (0x6A94) then reports the sound done and the 240-Hz update loop (0x1021C) stops it with
0xFE04(h, 0, 0): the handler's voices are keyed off into their release (0x19B18) and it leaves its
parent/child chain without stopping its children. The port's type 24 only marked the program
done, so the looping tone went on forever: one more endless wind loop per tree passed. A minute
into a Liddellroch race 35 copies were playing at once (PCSX2: at most 5, each released after
45 ticks).

`E_TREE_WHOOSH` has no instance limit (record byte +5 is 0) in the banks of Mt. Liddellroch
(`AUBE_S`), Mt. McSchley BC (`BC_S`), Mt. Zorkovaska (`GLAC_S`, `GLACTD_S`), Mt. Konawaki (`JUNGLE_S`)
and Mt. Quintanaroo (`PERU_S`; its `E_PILLAR_WHOOSH` too), flags 0x0001 (loop): on all of them the
copies piled up without end. Liddellroch is where it was reported (the forest track passes many
trees). Mt. Midoule (`ALPS_S`) and Mt. Imasaki (`FUJI_S`) give it limit 1 with flags 0x19, which the
port did not implement either (below), so there the copies piled up more slowly; Salt City's has no
loop flag. 56 looping sounds in the disc's banks end with an END grain: the tree whooshes, every
track's flywheels (`B_FLYWHEEL_UP/DOWN`), City's sirens, cop cars, phones and trains, and the rider
bank's `K_*` whooshes.

Not the cause, checked against the reference: the volume law (bank × tone × register volume, pan
table, quadratic group curve), the child-sound volumes (0x6658 -> 0x4A34: a child's original
volume is its grain's, its application volume the parent's application × original / 127, as the
port has), volume updates (0x21 -> 0xDBB8 -> 0x6BF4, propagated to children only from a top-level
sound), pitch and envelopes. Sounds that play the same way on both sides have identical register
values in both dumps (e.g. `E_WIND_LGT_L` 2232, 2234, 1228, 842; `B_GRAS_LAND_BG` 6736;
`B_ROCK_LAND_BG` 6080, 2826).

## Second gap: instance limits and solo sounds

The 989snd sound record has an instance limit at +5 and flags at +6 (OpenGOAL's `SFX2`, ISC:
1 loop, 2 solo, 8 instance limit, 0x10 steal the quietest, 0x20 steal the oldest). The port read
neither. Retail's start (0x4A34) does, before it allocates a handler:

- solo (flag 2, 0x10038): every live handler of the same sound is stopped with its children
  (0xFE04(h, 1, 0)); `E_RANDOM_THUNDE`, the bird flaps and the church bells are solo;
- flag 8 (0xF488 -> 0xF34C): count the live handlers of the sound; at the limit, the weakest
  (quietest application volume with 0x10, oldest with 0x20, else the first found) is stopped with
  its children for the new play if (0x10 and it is quieter than the new play) or 0x20; otherwise
  the play is refused and its reply is 0. Flag 8 with a limit of 0 refuses every play.

The port now does the same (`handler_admit`). A stopped handler no longer counts as an instance
(retail frees it at once); its voices still release. Iteration is in the port's slot order, so
which of two equally quiet copies is stolen can differ from retail's list order. Rider sounds
(`B_*`, limit 2-3, flags 0x18/0x19) and most other tracks' ambient loops (limit 1, flags 9) are
affected; Liddellroch's ambient sounds have no limit.

## Measurements

Reference: retail PCSX2 v2.8.2 (the comparison adapter of finding 024 with its NAV and race hooks,
rebuilt with an SPU2 capture hook replacing `SPU2/spu2.cpp` through MSBuild metadata, as finding
022 round 3 did; GPL code stays in the ignored build). It writes the raw SPU2 mix before PCSX2's DC
filter (`spu2-mix.wav`), every 800 samples each sounding voice's start address, pitch, ENVX, ADSR,
current volumes and VMIX bits (`voices.csv`), and each core's MVOL, EVOL, input/external volumes,
effect enable, ATTR and MMIX (`core.csv`). The port side uses `DHD_SND_WAV` and the new
`DHD_SND_VOICE_TRACE` (same columns). Retail voices are named by their SPU address minus the
bank's upload address (245056 words for the level bank in both races), matched against the bank's
tone offsets.

Both sides run the same menu script (race_compare.py's NAV with the track chooser moved right N
times: 15 for Mt. Liddellroch FR, 0 for Mt. Midoule FR), every track unlocked (port:
`DHD_UNLOCK_ALL=1`; PCSX2: a pnach writing the retail unlock code's track mask 0x035FFFFF to the
profile, 0x004346B4), race seed 81257, Cross held from 2 s after GO (race-clock pad), stopped 90 s
after GO. Windows are aligned on each side's own countdown/GO fields. Positions on the hill and the
random grain choices differ between the sides, so these are levels and spectra over the same
stretch, not sample comparisons. The trace build's PCM is byte-identical to the next/0.7.7
reference exe's over the same run.

RMS of the whole mix, dBFS:

| Mt. Liddellroch FR | PCSX2 | before | END fix | + limits |
|---|---:|---:|---:|---:|
| 20 s before the countdown (flyover) | -28.37 | -28.54 | -28.54 | -28.54 |
| countdown | -24.07 | -24.02 | -24.02 | -24.02 |
| race 0-10 s | -23.85 | -23.05 | -23.55 | -23.55 |
| race 10-30 s | -24.52 | -23.78 | -25.87 | -25.97 |
| race 30-60 s | -25.18 | -21.74 | -25.47 | -25.36 |
| race 0-60 s | -24.71 | -22.54 | -25.21 | -25.18 |

Spectrum, race 0-60 s, share of energy in 0-200 / 200-1000 / 1000-4000 / 4000+ Hz: PCSX2
36.4 / 34.4 / 24.2 / 5.1 %, before 24.4 / 51.0 / 21.2 / 3.4 % (the wind loops' 200-1000 Hz
band), END fix 36.2 / 32.5 / 25.9 / 5.4 %, with limits 35.9 / 32.7 / 26.0 / 5.4 %.

Voice power by family over race 0-60 s (sum of each sounding voice's gain², gain = |volume| ×
ENVX / 32767, relative to PCSX2; rows = voices sounding at the 800-sample marks):

| Mt. Liddellroch FR | before | END fix | + limits | PCSX2 rows |
|---|---|---|---|---:|
| ambient `E_*` | +13.3 dB, 56152 rows | +3.4 dB, 20288 rows | +3.4 dB, 20521 rows | 19121 |
| riders `B_*` | -1.0 dB, 20643 rows | -1.0 dB, 20605 rows | -1.1 dB, 19271 rows | 21240 |

`E_TREE_WHOOSH`'s wind loop: at most 35 copies at once before, 3 after, PCSX2 5 (1868/91295/964
sounding rows for PCSX2/before/after; PCSX2's count includes two other sounds that use the same
sample). Part of the remaining `E_*` difference is attribution: `E_FOLIAGE` shares its samples with
`B_COL_*` sounds, so retail rows for those samples count as `B_*`.

The other track, Mt. Midoule FR (Italy, `ALPS_S.BNK`), whose ambience was close already:

| Mt. Midoule FR | PCSX2 | before | END fix | + limits |
|---|---:|---:|---:|---:|
| countdown | -24.16 | -24.12 | -24.12 | -24.12 |
| race 0-60 s | -25.03 | -24.77 | -24.89 | -25.02 |
| ambient `E_*` power vs PCSX2 (PCSX2 11279 rows) | | +3.7 dB, 23609 rows | +3.3 dB, 22168 | +2.8 dB, 21264 |

With the limits the whole mix matches PCSX2 to 0.01 dB over the first minute. Most of the remaining
`E_*` difference is attribution: retail voices are named by sample, and Italy's `E_WIND_HVY_L` (one
instance, four tones: two samples at two pans) shares its first sample with `E_TREE_WHOOSH`, so in
the retail dump half of its voices count under the whoosh; `E_FOLIAGE` again shares its samples
with `B_COL_*`.

## Reverb (seen, not changed)

During the race retail runs core 1's effect (preset 3, EVOL 1966, effects enabled), core 0's
effect off (EVOL 0, an 8-word work area), and core 0's whole output into core 1 at ExtVol 32767
with MMIX 0xF0F (external input dry and wet), so core-0 voices reach core 1's reverb. The port's
mix is flat: its wet path takes only the voices of each core with the reverb tone flag, so in the
race its wet output is -52 to -66 dBFS. At EVOL 1966 (-24 dB) the missing tail adds little energy
and does not change the levels above; it is left for a later round (finding 022 already lists the
flattened two-core routing as open).

## Checks

- `test_sound` (test_runtime.py's guest program): new `test_end_grain_keys_off` (a looping tone
  then END is released and silent, and its handle is gone; a program end alone keeps it; fails on
  the old code) and `test_instance_limits` (refusal, no limit without flag 8, quieter/louder
  stealing, oldest stealing, solo).
- State traces (state_trace_run.py boot and race, defaults) against a next/0.7.7 build (dhd64.exe
  sha256 f439e85e...):
  END fix: boot 600 rows identical; race 5000 rows identical in every column but `sound` (the
  rendered PCM's hash) and `combined`, which differ in 292 rows from field 3430, the race.
  With the limits as well: boot identical; race: `sound` and `combined` differ from field 3231,
  `ram` from 3234 (1767 rows) and `hw` in 20 rows; `cpu`, `scratch`, `vu0` and the GS columns match
  in all 5000 rows. RAM dumps at fields 3300 and 4999 (both builds) differ only in the 989snd EE
  library's command queue and reply buffers (0x004C8xxx-0x004CCxxx, sound names visible in them),
  the sound objects' handle counters (`D_004B2A78`, 0x008A3D2C) and, at field 3300 only, the
  smoothed frame time `D_0029E3F8` (0.17463 against 0.17464; it reads the EE timer, and the game
  does a little less sound work), equal again at 4999: a refused play replies 0 as on the PS2, and
  the game keeps that. If an unchanged RAM trace is required, the limits commit (`d4db1d5`) can be
  left out; the END fix alone keeps every column but the PCM.

## Checks still to run (lead)

Not run by the lane any more (the new process): the state traces of the final branch, and a
listen. What to run and look at:

1. State traces (port/tools/state_trace_run.py boot and race, defaults) against next/0.7.7: boot
   must match in every row; race must match in every column except `sound` (from about field 3430)
   and `combined`, plus, with the limits commit, `ram` from about field 3234 and `hw` in about 20
   rows. `cpu`, `scratch`, `vu0`, `gs_*` and `code` must match in all 5000 rows. If `ram` must
   match too, drop `d4db1d5` (the limits) and keep the END fix.
2. Listen (or record): a free ride on **Mt. Liddellroch FR**: main menu, the race route to the
   track chooser (race_compare.py's NAV), Right 15 times with every track unlocked (PC SETTINGS >
   Unlock All, or `DHD_UNLOCK_ALL=1`), Cross; hold Cross after GO. Before the fix, from about 20 s
   into the race a wind roar builds up with every tree passed and never fades; after it, each
   tree gives a short whoosh that dies away in about half a second, and the birds, the wind bed and
   the distant thunder stay under the music and the bike. A Liddellroch career race (the
   player's video) uses the same bank. Mt. Konawaki, Mt. Zorkovaska, Mt. Quintanaroo and BC had
   the same build-up.
3. The WAV comparison, if wanted (about 2 minutes per run; one game at a time):

       python port/tools/audio_race_compare.py build-pcsx2 --src <prepare_pcsx2_compare.py's tree>
       python port/tools/audio_race_compare.py retail aube-ps2 --track 15
       python port/tools/audio_race_compare.py port aube-old --track 15 --exe <next/0.7.7 dhd64.exe>
       python port/tools/audio_race_compare.py port aube-new --track 15 --exe build/host64-exe/dhd64.exe
       python port/tools/audio_race_compare.py compare aube-ps2 aube-old aube-new --bank AUBE_S.BNK

   and the same with `--track 0` / `--bank ALPS_S.BNK` for Mt. Midoule. Expect the tables above:
   race 0-60 s within about 0.5 dB of PCSX2 after (about 2 dB louder before on Liddellroch), and
   the `E_` voice power +3 dB or less (+13 dB before). The old exe needs no voice trace for the
   levels; for the family table use a build with `DHD_SND_VOICE_TRACE` (this branch's first
   commit), whose PCM equals next/0.7.7's byte for byte.

## Tools

`port/tools/audio_race_compare.py` (above): builds the PCSX2 hook, runs either side on the same
route and compares. It reads the level bank from the disc image itself; nothing is extracted to
disk except the runs' own WAV/CSV under build/audio. The PCSX2 hook replaces one upstream
translation unit through MSBuild metadata; the GPL emulator and its build stay outside the
repository. Research used: OpenGOAL's 989snd (`sfxblock.h` flags, `CheckInstanceLimit`,
`STARTCHILDSOUND`, ISC, read only), PCSX2 v2.8.2 `SPU2/defs.h`, `spu2sys.cpp` (MMIX gates) and
`spu2.cpp` (the capture point), and the retail IRX's code at the addresses above. No game data,
capture or disassembly is committed.
