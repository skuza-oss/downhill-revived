# 090: Game speed round 1 (x64 to 144 fps)

Status: current
Supersedes: parts of 019, 048, 066
Checked: 2026-11-04

The changes finding 089's profile ranked. Each task has a section here.

## VU1R: VU1 speed round

Branch `rr/VU1R` from main (8cc733200). This is OP3's list for x64 and the browser. The VU1 output
stays bit-exact. The core module did not change: `dhd_core.wasm` is byte-identical to 0.4.0's.

### What changed

1. **Entry 12E0 for the native kernels** (`vu0_interp.c` run()). The dispatch now accepts
   0x12E0 as well as 0x1040 and 0x1CB0. At 12E0, VI13 0x11 maps to kernel kind 0.
   `vu1_seq_diff` now builds for x64 (`port/tools/vu1_seq_diff.py`, `port/runtime/vu1_seq_diff.c`).
   It uses build_host64's toolchain and flags, plus stubs for the host functions. The old tool
   was i686-only and could not read x64 captures.
2. **Kernels for loops 0798 and 3140** (`vu1_native_extra.h`):
   - `native_geometry_0798`: VI13 0x0E0, 9 pairs per vertex, one DIV per vertex.
   - `native_geometry_3140`: VI13 0x616, 8 pairs per vertex.
   - Both kernels check their code words against the copies kept in `vu1_native_clip.py`
     before they run (`native_code_compare`).
   - Both decline a bad vertex count or a pending Q, and the interpreter then runs that program.
   - `native_finish()` is the shared end state of the kernels that have no clipping path.
   - `PS2_VU1_NATIVE_KINDS` is now 14, and the per-kind caches are indexed by ctz(kind).
3. **Lazy flags.** `vu1_aot.py` runs a liveness pass, `lazy_flags()`, a backward dataflow over
   the micro pairs:
   - Each MAC writer's update is tracked at distance 0, 1, 2, 3 and 4 or more pairs. A later
     FMAC writer kills the update at distance 4 or more, because of the 4-cycle flag latency.
   - Flag readers keep the update live: FMAND, FMEQ, FMOR, FSAND, FSEQ, FSOR, and the
     clip/status reads used by branches.
   - The control-flow graph is conservative. Every pair falls through. Branch delay slots also
     go to the branch target. JR and unknown code count as reading everything.
   - The end of a run is not a reader. The flags are folded at the next drain, so the state at
     the end of a run is still exact.
   - An update is made lazy only when no reader can see it while it is still queued. The
     generated code then calls `commit_upper_lazy`. Instead of queueing a flag entry, this ORs
     the sign and zero lanes into accumulators and remembers the last result and its dest mask.
   - `lazy_fold()` rebuilds MAC, STATUS Z/S and sticky 0x40/0x80 at the next `drain_flags`.
     A queued (non-lazy) update clears the remembered result, so the newest update always wins.
   - Packages that contain sticky readers or FSSET get no lazy blocks. None exist in DHD.
     OPMULA and OPMSUB are left out.
   - Static counts: 1795 of the 1959 MAC writers in the 718 blocks are lazy. In the native
     clipping routine, 76 of its 94 FMACs are lazy (`vu1_native_clip.py`; its single MAC read
     keeps the rest queued).
   - Lazy blocks only run when VU1 micro memory holds the whole package (`aot_package`, checked
     once per code version). Otherwise the dispatcher falls back to that package's non-lazy
     blocks, or to the interpreter. Finding 030's rule holds: no update that a reader can see
     is removed. The updates are only moved later, and they still reach the same state.
4. **VIF1 UNPACK with WL=1** (`port/hle/vif.c`, `stream_wl1` and `wl1_fields`; built into the
   x64 and browser VIF1 only, never into `vif0.c`):
   - With WL=1 the write cycle is always 0. Mask row 0 applies to every vector, and the
     destination advances by CL.
   - Mask row 0 is split once per command into take, fixed (row or col) and keep lane masks.
     Old memory is read only when some lane keeps it.
   - The decoders use SSE2: S-8, V2-16, V4-8 and V4-32 (signed and unsigned, where the form
     has a USN bit), and V4-5. In the browser, simde maps them to wasm SIMD.
   - Short commands and commands cut across packets use the same vector buffering as
     `stream_form`.
   - The forms this covers are OP3's V2-16 masked, S-8 masked, S-8 and V4-5 at CL3 WL1, plus
     V4-8 and V4-32. Other modes and WL values take the existing paths unchanged.

### Exactness proof

- **`vu1_seq_diff`, x64.** It replays the captures three ways and compares registers, the
  16 KB data memory and the GIF bytes after every run. The referee is the interpreter (AOT off,
  kernels off). Mode one is blocks only; mode two is blocks plus kernels.
  - Captures: menu, race1 and race2, about 75,000 runs. Result: MATCH, both modes, every run.
  - Runs that took the new paths:
    - kernel 4096 (loop 0798): 1596 + 1757 + 1736;
    - kernel 8192 (loop 3140): 0 + 570 + 1653;
    - 12E0 entry runs: 16 and 32 in the race captures.
  - Each capture's micro memory equals package A or package B exactly, so the lazy blocks ran.
  - Negative controls, each of which fails the compare:
    - a corrupted store in the 0798 kernel;
    - making every MAC writer lazy, which ignores the analysis: 5766 runs differ, at the
      clipping routine's FMAND output.
  - Result: 0 differing trials out of 200,000.
  - Four deliberate corruptions each fail: the V4-5 alpha multiplier, write-protected lanes
    ignored, the wrong column register, and V4-8 ignoring USN.
  - `test_vif.c` itself stays i686-only.

### Builds and the state trace

- `build_wasm.py -j 2`: 3815 of 3815 units, offset scan clean. `dhd_core.wasm` is unchanged
  (sha 3096ae7a, same as 0.4.0).
- `package_release.py` (x64): `dhd.exe` sha256 50a9d4a09b61ff94.
- Both web builds (threaded and `--no-threads`): `check_web_layout.py build/web` OK.
- State trace of a race start, 5000 fields, against main's exe:
  - Main's exe is the 0.4.0 release build 6f4aa0e1. Main differs from it only by the ini
    template string in host.c.
  - Result: `trace_columns` PASS. All 13 columns are identical (menu, phase, clock, ram,
    scratch, hw, cpu, vu0, gs_stream, gs_regs, gs_vram, sound, code). 7 words were masked.

### Timing

OP3's method: a hidden desktop, scale 2, `DHD_FPS=144`, a race at full pedal, fields 7400-14600, one
game at a time. The PC was shared: CPU load was about 20% before the first run, 55% between the runs
and 7% after. Treat the numbers as rough.

| | vu busy ms/field | vu thread CPU ms/field | fields/s | game busy ms | p50 / p99 ms |
|---|---|---|---|---|---|
| before (0.4.0) | 4.093 | 4.123 | 169.7 | 5.088 | 5.656 / 11.053 |
| after | 3.279 (-20%) | 3.415 | 179.7 | 5.015 | 5.240 / 10.124 |

The EE's mean wait on VU1 (`ee_wait_vu`) went from 0.591 to 0.290 ms per field.

**Harness replay**, blocks plus kernels, total ms per capture:

| capture | main | after items 1-2 | final |
|---|---|---|---|
| menu | 41.7 | 41.3 | 35.8 |
| race1 | 37.0 | 35.1 | 28.7 |
| race2 | 58.6 | 54.3 | 48.7 |

- Blocks only, race2: 145 to 117 ms.
- A repeat of race2 under a different PC load (63%, then 5%) gave 59.5, 60.6 and 47.5 ms.

**Per microprogram**, race2, µs per run (main / after 1-2 / final):

| program | main | after 1-2 | final |
|---|---|---|---|
| 0040@12E0/020C | 5.08 | 1.20 | 1.11 |
| 0040@12E0/0260 | 2.97 | 0.79 | 0.72 |
| 0040@12E0/0616 | 1.86 | 0.43 | 0.35 |
| 0040@1040/0616 | 1.89 | 0.41 | 0.36 |
| 0020@1040/0616 | 1.91 | 0.43 | 0.38 |
| 30A0@1040/0616 | 2.08 | 0.44 | 0.39 |
| 06F0@1040/00E0 | 1.72 | 0.48 | 0.42 |
| 06F0@1CB0/00E0 | 0.84 | 0.34 | 0.30 |

- The 0040 setup went from 2.20 to 1.49 µs.
- The 0020/0010 skin tails at 0590 went from 3.8-3.9 to 2.9-3.0 µs.

### Left

- Lazy blocks fall back when micro memory does not hold a whole package. The captures always
  held one, but partial uploads are not sped up.
- The conservative control-flow graph keeps some updates queued: the last FMACs before an
  E-bit whose delay slot falls into the next program's `JR vi13` (for example 0570-0590).
  Knowing the real JR targets would make those lazy too.
- Kernels with kind below 64 (including 020C and 0260) still treat a decline as fatal. This is
  not new behaviour, but it is now reachable from 12E0. No declines were seen.
- `test_vif.c` and `test_runtime.py` are still i686-only. The VIF fuzz used a scratch x64
  driver.
- Browser timing was not measured; the browser shares the same code.
- OP3's GPU path (c) was not attempted.

Sources: findings 030 (flag observability), 046 and 089, and OP3 above. The lazy-flag idea follows
PCSX2's microVU flag pass (`mVUsetFlags`, which only writes flag instances that can be read). No
PCSX2 code was used.

## OP3: VU1 per microprogram

Research only, branch `rr/OP3` (from `render/rust` 84cb6af61). No game code changes are committed.

### Summary

- **One family of overlay-B mesh programs is about 91% of VU1 time in a race.** They all reach the
  `JR vi13` dispatch at 1040, 1CB0 or 12E0, which jumps to one per-vertex loop, kicks one triangle
  strip of 30-42 vertices, and clips through the shared routine at 1948/1CC0. B0078 alone is 38%.
- **(a) Flags.** The AOT path queues 112,000 MAC updates a field. 98.7% are never read. No
  program reads the MAC, status or clip flags that an earlier program left. VU0, the EE and VIF1
  never read VU1's flags. The only readers outside the program itself are the end-of-run
  registers that the state trace snapshots.
  - Flag work is 22% of the vu thread's work: 41% of AOT-block time and 40% of
    `native_clip_run`'s.
- **Cheaper than (a), found on the way: programs that miss their native kernels.**
  - B0040's MSCNT at **12E0** is the same `JR vi13` as 1040, but the kernel dispatch only looks at
    1040/1CB0, so the 0x20C and 0x260 loops run block by block. Allowing 12E0 cut those runs from
    11.3 to 3.0 us and from 6.2 to 1.9 us: -3.7% of VU1, measured. About 1 h plus the
    `vu1_seq_diff` check.
  - Loop **0798** (B06F0, vi13 0x0E0) and loop **3140** (vi13 0x616, B0040/0020/30A0) have no
    kernel at all. They are 7% and 4% of race VU1, and 06F0 is 16-21% in the menus. They are
    siblings of the 07F0 kernel's loop, which runs 3.5x faster than its blocks. 3-5 h each.
- **(b) VIF1.** UNPACK is 0.67 ms a field (12% of the vu thread). Command handling adds 0.3 ms.
  - Four forms are 72% of the unpack time: V2-16 masked, S-8 masked, S-8 and V4-5, all CL 3 WL 1
    and mode 0.
  - Half of all commands write 1-2 vectors, so the per-command setup matters as much as the
    per-vector loop.
  - Specialising these forms could save about 0.25 ms a field. 4-6 h.
- **(c) The GPU candidate is the B mesh family's per-vertex loop, led by 0078's 0170 loop.**
  - It is vertex-parallel apart from a 3-vertex strip window, and nothing reads its output back.
  - Batches are small: about 1,400 kicks of about 36 vertices a field.
  - The blocker is the exact lens-flare Z probe. The CPU still needs exact output for Z-writing
    batches near the probe, and for whole fields at the bottom edge.
  - 100-200 h. It pays off for the browser (VU1 is 38% of its graphics worker), but not for x64
    before (a), the kernels and the fold fix are in.

### Method

**Build.** A local copy of `vu0_interp.c`, `vif.c`, `graphics_queue.c` and `vu1_native_clip.py`,
plus two local headers (`op3_vif.h`, `op3_vu.h`), built with `build_host64.py` and release flags.
The wasm core is unchanged (CL7's `dhd_core.wasm`, sha256 13c7217c...). `DHD_OP3=file` and
`DHD_OP3_FIELDS=a-b,...` turn the counters on for windows of virtual fields:
- **Per run** (one `ps2_vu1_run`), keyed by overlay, MSCAL program, entry address, MSCNT or not,
  and VI13 when the entry is 1040, 1CB0 or 12E0 (the dispatch register). The counters:
  - rdtsc time, and the XGKICK callback's time;
  - pairs run in AOT blocks, in the interpreter or in native kernels, and the native kernels'
    time;
  - for each XGKICK, the GIF packet parsed in VU1 memory: tags, PRE primitives and their types,
    PACKED/REGLIST XYZ registers (vertices), A+D writes and IMAGE qwords;
  - kicks per run.
- **Flag liveness.** Every queued flag update gets an id. When it lands, it becomes the current
  MAC, or enters the 4-deep clip history. Each FM*/FS*/FC* read marks what it reads (FCAND by
  mask group, FSAND by bit group: ZSUO from the MAC, D/I from Q, sticky). Reads of state from an
  earlier program are counted apart. At the program's end the unmarked current MAC and clip
  entries count as "live at end". So **dead = queued - read - live at end**. Native kernels and
  `native_clip_run` commit flags directly; their state is tagged "native", and reads of it are
  counted but not attributed.
- **vu thread totals.** `execute()` per packet, `vif_feed`, UNPACK per form, MPG, PATH2/PATH3
  GIF, and calls of `host_vu1_file` (VU0 reading the VU1 register file) and `host_vu1_snapshot`.

- A hidden desktop, scale 2, `DHD_FPS=144`, `DHD_FRAME_CLOCK=1`, sound off, the full-pedal race,
  seed 81257.
- Windows: two menu stretches (fields 3000-4000 and 5000-6400) and the race (7400-14400, 66 s of
  wall time at about 107 fields/s). `DHD_BENCH=7400-14600` with `DHD_TIMELINE`.
- Three runs:
  - `stats2`, `stats3`: the baseline (stats3 adds VI13 at 12E0);
  - `k12e0`: the same exe with `DHD_OP3_12E0=1`, which lets the native kernels run from entry
    12E0.
  - So the absolute ms are about 1.5x finding 089's quiet numbers: VU1 4.55 ms a field here
    against 2.99 ms in PR1's sampled run.
  - Shares, counts and per-call ratios are what to use. Where this section gives "quiet" ms, it
    scales by PR1's numbers.
  - The counters add 2 rdtsc and a small table lookup per VU1 run, per XGKICK and per UNPACK
    chunk. That is about 0.1 ms a field of UNPACK time, so short commands look worse than they
    are.

**Work kinds inside the code.** The rdtsc counters cannot split flag work from arithmetic.
- That split comes from finding 089's own vu-thread samples (`s8-prof` + `s2-prof`, 27,859
  samples, PR1's exe and PDB; VU code unchanged since).
  - the innermost frame gives the kind (`queue_flags`/`retire_flags`/`fmac_flags4`/`native_result`
    = flags; `operand4` = clamp; `pair_stalls` = stalls; ...);
  - the first `aot_block_N`, native-kernel or `native_clip_run` frame gives the code unit.
- `keyflags.py` splits each block's samples between programs by how often each program ran that
  block start (the per-program block counts the local build dumps).

### Where the vu thread's time goes (race)

Measured ms a field (loaded PC). The quiet equivalents from finding 089 are VU work 3.86, VU1 2.99
and VIF1 0.66.

| part | ms/field | share of wall |
|---|---|---|
| VU1 programs (AOT blocks, native kernels, dispatch) | 4.17 | 44% |
| XGKICK: copying the packet into the GS ring | 0.38 | 4% |
| VIF1 UNPACK | 0.67 | 7% |
| VIF1 commands, MPG, run_micro bookkeeping | 0.30 | 3% |
| PATH2 DIRECT + PATH3 GIF | 0.06 | 1% |
| waiting for packets (`vu_idle`) | 3.73 | 40% |
| field (wall) | 9.38 | |

Menus: 3.6-4.2 ms busy a field (VU1 2.8-3.2), with 0.47-0.62 ms of UNPACK.

**Samples, by unit** (share of the vu thread's work; finding 089's data):
- native kernels 31%;
- AOT blocks 22%;
- VIF1 17%;
- `native_clip_run` 14%;
- run loop and dispatch 6%;
- `retire_flags` (out of line) 3%;
- memmove 2%;
- other 5%.

**By kind:**
- flags 22%;
- VIF 17%;
- upper decode and operation 13%;
- operand clamp 12%;
- lower 5%;
- dispatch 5%;
- register commits 4%;
- stall model 3%;
- Q/P 2%;
- kick 1%.

Inside the AOT blocks: flags 41%, lower 12%, clamp 10%, commits 9%, upper 8%, stalls 8%, Q 4%.

### Per microprogram

**Race, fields 7400-14400** (stats3). Each MSCAL program includes its MSCNT continuations.
- "in native kernels" is the share of the program's time spent in a native loop. That includes
  `native_clip_run`.
- "MAC updates" counts only the AOT and interpreter path.

| program | runs/field | share of VU1 | us/run | in native kernels | XGKICKs/field | vertices/kick | max/run | AOT pairs/field | MAC updates/field | dead | clip updates/field | flag-skip saving ms/field |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| B 0078 | 593.4 | 38.0% | 2.91 | 73% | 559.4 | 35.7 | 92 | 26104 | 13297 | 99.4% | 511 | 0.054 |
| B 0040 | 168.1 | 13.4% | 3.64 | 11% | 131.5 | 35.8 | 44 | 55200 | 38994 | 99.8% | 481 | 0.159 |
| B 1638 | 136.7 | 10.7% | 3.57 | 75% | 124.2 | 33.4 | 63 | 5750 | 3262 | 99.2% | 110 | 0.013 |
| B 06F0 | 191.0 | 7.1% | 1.68 | 0% | 117.0 | 19.9 | 44 | 29874 | 21557 | 99.1% | 0 | 0.088 |
| B 2DC8 | 43.2 | 6.2% | 6.51 | 79% | 53.8 | 29.7 | 89 | 2188 | 1165 | 99.7% | 39 | 0.005 |
| B 0020 (skinning) | 77.1 | 4.8% | 2.86 | 48% | 69.0 | 40.1 | 44 | 10958 | 7024 | 99.7% | 22 | 0.029 |
| B 07F0 | 202.3 | 4.2% | 0.94 | 43% | 151.7 | 31.8 | 44 | 8801 | 5061 | 99.0% | 0 | 0.021 |
| A 29C0 (particles) | 4.0 | 3.4% | 38.82 | 0% | 4.0 | 333.3 | 386 | 24276 | 10932 | 91.6% | 0 | 0.041 |
| A 2E48 (particle quads) | 9.3 | 2.6% | 12.73 | 79% | 57.8 | 72.9 | 512 | 979 | 278 | 93.3% | 9 | 0.001 |
| B 17E0 | 42.0 | 2.2% | 2.35 | 66% | 36.5 | 26.3 | 100 | 1906 | 990 | 98.9% | 31 | 0.004 |
| B 0010 (skinning) | 28.6 | 1.7% | 2.74 | 52% | 25.4 | 39.6 | 44 | 3077 | 1847 | 99.8% | 0 | 0.008 |
| B 1430 | 31.3 | 1.5% | 2.18 | 63% | 25.5 | 41.3 | 44 | 1326 | 561 | 99.0% | 25 | 0.002 |
| A 02D0 | 4.4 | 1.0% | 9.95 | 0% | 4.6 | 36.0 | 292 | 2577 | 1669 | 99.7% | 177 | 0.007 |
| B 30A0 | 20.0 | 0.9% | 2.15 | 0% | 11.3 | 42.0 | 42 | 4364 | 3115 | 99.4% | 0 | 0.013 |
| A 0E10 (sprites) | 8.6 | 0.9% | 4.91 | 74% | 23.8 | 64.2 | 410 | 458 | 111 | 100.0% | 0 | 0.000 |
| B 12F0 | 20.4 | 0.4% | 0.91 | 0% | 0.0 | - | 0 | 1227 | 1063 | 98.1% | 0 | 0.004 |
| A 26D8 | 0.9 | 0.2% | 11.37 | 0% | 16.7 | 0.2 | 36 | 676 | 139 | 98.2% | 0 | 0.001 |
| all | 1611.9 | 100% (4.55 ms) | | 53% | 1431 | 35.7 | | 181338 | 112088 | 98.7% | 1452 | 0.45 |

**Menus.** VU1 is 2.8-3.2 ms a field.
- Fields 3000-4000: B1638 31%, B06F0 21%, B17E0 20%, B07F0 7%, A29C0 6%, B0078 5%, B0020 4.5%.
- Fields 5000-6400: B0078 35%, B1638 25%, B06F0 16%, B17E0 14%, B07F0 5%, A29C0 3%.
- The same programs as in the race, in a different mix. B06F0, which has no kernel, is
  16-21%.

**Per entry, the parts worth naming** (race, share of VU1):
- **B0078's continuations** at 1040 and 1CB0 (dispatch 0x11 to the 0170 loop) are 28.7% and 7.4%.
  - 77% and 73% of their time is in the native kernel and its clipping.
  - The rest is the shared setup at 1AE0 and the GIF tail at 0FF8.
  - 4.9-5.1 clipping-routine runs a call. 38.5 and 42.0 vertices a call.
- **The MSCAL setups that only `B L1AE0`** (0078, 06F0, 07F0, 1638, 17E0, 12F0, 1050, ...) take
  0.7-0.9 us each and together about 5%.
  - 60 pairs and 52 MAC updates a call, 98% dead.
  - 46% of their samples are flag work.
- **B0040's setup** (MSCAL 0040, 11E8: a lighting/matrix loop at 1280) is 3.6%. 546 pairs and 426
  MAC updates a call, no kick.
- **B0040's MSCNT at 12E0** (`JR vi13`):
  - vi13 0x20C (loop 1110): 2.7%, 11.3 us a call;
  - vi13 0x260 (loop 13A0): 2.4%, 6.2 us;
  - vi13 0x616 (loop 3140): 0.6%.
  - The kernels for 0x20C (`native_geometry_20c`) and 0x260 (`native_geometry_260`) exist, but
    `run()` selects them only when the entry is 1040 or 1CB0.
- **B06F0 through 1040 and 1CB0** (vi13 0x0E0, loop 0798-07D8) is 5.8%, plus 1.3% for its setup.
  - 9 pairs a vertex: the matrix transform, DIV, MULq projection, FTOI4 and stores.
  - No flag reads, no clipping. 7.4 MAC updates a vertex, 99.5% dead.
- **Loop 3140 (vi13 0x616)**, through 0040, 0020 and 30A0 at 1040 and 12E0, is 3.8%.
  - 8 pairs a vertex, no perspective divide in the loop. No flags read, no clipping.
- **Skinning tails** (0020 and 0010 at 0590, after `native_skin_468`) are 1.8%. 480 MAC updates a
  call, 99.8% dead.
- **A29C0** is the particle generator, 2 calls of about 39-65 us and 333 sprites each.
  - It runs a recurrence (`MADD vf13, vf13, vf23` with ACC = vf22, `RNEXT` random numbers).
  - It culls with FMAND on the MAC sign and zero bits, and writes a variable number of sprites
    (SQI).
  - Of all VU1 programs it reads the most MAC flags: 909 observed updates a field.

### Flags: who reads them

Over the race (per field):
- **Reads.** 5,313 MAC reads, almost all inside `native_clip_run` (FMAND at 1970) and overlay A.
  457 status reads, all of D/I (FSAND 0x020 after DIV). 9,292 clip reads.
- **No reads of the status ZSUO bits** (from FMAC results) and **no sticky-bit reads** at all.
- **No program ever reads a flag left by an earlier program**: 0 reads of entry-state MAC or
  clip, in all windows.
- **VU0 never reads the VU1 register file** (0 `host_vu1_file` calls). The EE has no access to VU1
  registers, and no game function addresses VU1 memory (0x11008000/0x1100C000 appear nowhere in
  the asm).
- **VIF1 reads only TOP/ITOP/TPC**, never a flag.
- So the end-of-run MAC, status and clip registers matter only to `state_trace`'s VU1 snapshot.
  They must stay exact for that check, but no instruction reads them.
- **MAC updates.** 112,088 queued in the AOT path. 923 observed, all in overlay A: 29C0, 2E48,
  1010, 26D8 and 1240. 560 are live at the end. **98.7% are dead.**
- **Clip updates.** 1,452 a field (CLIP + FCSET) in the AOT path. All are read or overwritten.
  Clipping is real work, not dead work.

Finding 030's conservative pass found no removable complete flag update. That still holds: every
FMAC sets sticky status bits, and the end state is exact. The saving is in how the updates are
made, not in deleting them:
- Today each FMAC computes the MAC (`fmac_flags4`: 2 movemasks and 2 table lookups) and queues a
  16-slot ring entry. That is 6 stores plus the status derivation. `retire_flags` later walks
  the ring.
- Where no MAC/status reader is reachable before the program ends (every overlay-B program
  outside the clip routine), the generated block could do what the native kernels already do:
  - OR the sign and zero masks into two sticky accumulators;
  - keep the last result and its dest mask;
  - build MAC and status once, at the end (the drain) or at a reader.
- With no reader in reach, the 4-cycle flag latency doesn't matter. The final registers, and
  so the oracle's and the state trace's end state, stay identical.

**(a) Estimate.**

| where | today (quiet ms/field) | removable |
|---|---|---|
| AOT blocks: `queue_flags` + `fmac_flags4` inline (41% of 0.83 ms), plus out-of-line `retire_flags` (0.12) | 0.46 | about 0.35-0.40 |
| `native_clip_run`: `fmac_flags4` per FMAC and the clip commits (40% of 0.55 ms) | 0.22 | about 0.12 (MAC is needed only from 1950's SUBA, read at 1970) |
| native kernels: `native_result` sticky accumulation | 0.15 | about 0 (already the cheap form) |
| total | 0.83 | **about 0.45-0.5 ms (12-14% of the vu thread's 3.86 ms)** |

- By program, the AOT part goes mostly to B0040 (0.16), B06F0 (0.09), B0078 (0.05: the 1AE0 setup
  and the tail), A29C0 (0.04), B0020/0010 (0.04) and B07F0 (0.02). See the table's last column.
- Once the missing kernels below are in, 0040's and 06F0's share moves into kernels, which
  already use the cheap flag form.
  - a reachability pass over each overlay in `vu1_aot.py` (readers FM*/FS*-ZSUO, and the 1970
    FMAND for the clip code);
  - a "lazy" `commit_upper` variant and a drain that builds the end flags;
  - the same change in `vu1_native_clip.py`;
  - checks: the PCSX2 VU1 oracle (`test_vu1_pcsx2.py --aot`) and `vu1_seq_diff.py` on three race
    captures.
- **Overlay A keeps the queue** in 29C0's loop, 2E48, 1010, 26D8 and 1240.

### Programs that miss a native kernel (cheaper than (a))

- **12E0 entry (measured).**
  - `DHD_OP3_12E0=1` adds `addr == 0x12e0` to the native-kind selection in `run()`. VI13 0x11
    keeps kind 0 there.
  - B0040@12E0/20C went from 11.28 to 2.96 us a call, and /260 from 6.20 to 1.86 us. The
    kernels took 47% and 42% of those runs.
  - That is -0.167 ms a field on this run (-3.7% of VU1; about -0.11 ms quiet). No "native
    geometry failed" in the log.
  - Exactness rests on the kernels being entry-independent: they start at the loop's first pair
    from the live pipeline state, as at 1040. Before shipping it needs `vu1_seq_diff.py` over
    captures that include 0040's runs. About 1 h.
- **Loop 0798 (B06F0, vi13 0x0E0).**
  - 7.1% of race VU1, 16-21% in the menus.
  - The unlit, unclipped sibling of 07F0's 08B0 loop. That loop's kernel (`native_geometry_800`)
    runs 3.5x faster than its blocks (finding 046).
  - Estimated -4% of race VU1 and -10-14% in menus. 3-5 h.
- **Loop 3140 (vi13 0x616).** 3.8%, similar shape. About -2.5%, 3-4 h.
- **Skinning tail 0590.** 1.8%. Mostly setup and the GIF tail. Low priority.

### (b) VIF1 UNPACK

The race sends 11,000 UNPACKs and 186,000 vectors a field. All are mode 0 and WL <= CL, so all go
through `unpack_stream`; none takes the filling path.

| form (CL/WL) | commands/field | vectors/field | vectors/command | ms/field | share | ns/vector |
|---|---|---|---|---|---|---|
| V2-16 masked (3/1) | 1226 | 43669 | 35.6 | 0.170 | 25.5% | 3.9 |
| S-8 masked (3/1) | 4076 | 9074 | 2.2 | 0.117 | 17.5% | 12.9 |
| S-8 (3/1) | 1223 | 43645 | 35.7 | 0.106 | 16.0% | 2.4 |
| V4-5 (3/1) | 947 | 33162 | 35.0 | 0.090 | 13.5% | 2.7 |
| V3-32 USN (4/4) | 311 | 20586 | 66.1 | 0.047 | 7.1% | 2.3 |
| V3-8 (4/4) | 110 | 11314 | 102.9 | 0.037 | 5.6% | 3.3 |
| V4-32 (4/4) | 1123 | 8881 | 7.9 | 0.033 | 4.9% | 3.7 |
| V4-8 USN (3/1) | 276 | 10483 | 38.0 | 0.031 | 4.6% | 2.9 |
| V4-32 (3/1), one vector | 1223 | 1223 | 1.0 | 0.021 | 3.2% | 17.5 |
| S-32 (4/4), one vector | 323 | 323 | 1.0 | 0.006 | 0.9% | 18.4 |
| 10 more forms | 53 | 3100 | | 0.007 | 1.0% | |

The ns/vector figures include the counters' two rdtsc and a key lookup per transfer chunk, roughly
10 ns each (about 0.1 ms a field), so the 1-2-vector rows are inflated by up to half.

Worth specialising, in order:
1. **The masked forms with WL = 1** (V2-16 masked, S-8 masked).
   - With WL 1 every vector uses mask row 0, so the mask becomes one constant per-lane select
     between data, row, column and the old value.
   - That is a 4-lane blend instead of the per-field `switch`. About 1 ns a vector.
2. **Short commands** (S-8 masked at 2.2 vectors, V4-32 and S-32 with one vector: 5,600 commands a
   field).
   - Skip `stream_form`'s 16-entry mask table and the row/column copies when the count is small or
     the command is unmasked.
   - Decode the mask once per STMASK rather than per UNPACK.
3. **S-8, V4-5 and V4-8 at stride 3** (CL 3 WL 1). Widen 4-8 vectors per step with SSE (byte to
   lane broadcast, the 5:5:5:1 shifts).
4. V3-32 and V3-8 at CL 4 WL 4 are already near memory speed. Leave them.

**Estimate.** About 0.25 ms a field off the vu thread: 40% of UNPACK, plus some of the 0.3 ms of
command handling. 4-6 h, with `test_vif.c`'s 200,000-stream fuzz as the check. x64 and browser.

### XGKICK pattern and batches

- **The mesh family kicks once per batch,** at the GIF tail 0FF8. The packet is one tag with
  PRE = triangle strip and NLOOP = vertices, and the run toggles the TOPS double buffer. 1.0-1.3
  kicks a run.
  - About 2-5% of runs (more in 2DC8: 1.3-1.95 kicks a run) have extra kicks from the clipping
    routine. It emits each clipped polygon as a triangle fan. Fans are 4% of the mesh family's
    primitives.
- **Batch sizes.** 30-42 vertices a kick on average. 44 is the usual maximum; a few batches reach
  52-100. The kernels decline over 64, and for 07F0 over 341.
- **Overlay A.**
  - 2E48: 6.2 kicks a run of about 73 sprites, 454 a run.
  - 0E10: 2.8 kicks, 178 sprites a run.
  - 29C0: 1 kick of 333 sprites.
  - 26D8: 18 kicks of register-only packets.
- **Per field:** 1,431 kicks, 51,100 vertices and 155,000 qwords in the race. The copy into the GS
  ring is 0.38 ms (finding 089: 0.9% of samples are the kick itself).

### (c) A GPU path for one program

**Candidate: the B mesh family's per-vertex loop, with 0078/0170 first.**
- 0078's 0170 loop is 36% of race VU1 by itself. With 1638's 1708, 2DC8's 2F00, 17E0, 1430 and
  the three loops without a kernel (0798, 3140, 1110/13A0 via 12E0), the family is 91%.
- Each loop is the same pipeline:
  - 3 input qwords a vertex (from the UNPACKs above);
  - a 4x4 transform, DIV/Q, the perspective multiply;
  - a CLIP judgement against w times the guard band;
  - FTOI4 to XYZ2 with the ADC (no-kick) bit, plus ST/Q and colour;
  - 3 output qwords.

**Dependencies.**
- **Vertex-parallel, except for a sliding window.** Within a batch, no vertex reads data written
  while an earlier vertex was processed.
  - The software pipeline carries the previous vertices' positions and clip codes (`v[10]`,
    `v[28]`, `v[29]` and the 24-bit clip shift register).
  - The backface test (a cross product with the previous two positions) and the clip test
    (FCAND 0x02FBEF: the current judgement and the two before it) are functions of 3
    consecutive vertices.
  - So: a pass per vertex, then a pass per strip triangle.
- **Clipping is the sequential part.** When the window test fails, `native_clip_run` (1948/1CC0)
  clips the triangle and writes and kicks a fan.
  - That happens 4.9-21 times a batch (0078: 4.9, 1638: 11, 2DC8: 19-21). It is 14% of the vu
    thread's samples.
  - On a GPU this is a second compute pass with an append buffer. To keep the native renderer's
    blend order, its output needs a prefix sum per batch, or the fans drawn after their strip
    with a per-batch indirect draw.
- **Flags:** only the clip judgements (vertex-local plus the window) feed the output. MAC and
  status have no reader, as shown above.
- **Nothing the CPU reads.** No EE, VU0 or VIF reads of VU1 output, registers or flags.
  Everything goes to the GS.
  - The GIF tag and A+D register writes (TEX0 and the like, 0.3-4 a run) come from the
    per-batch header, not from the transform. They can stay on the CPU and feed the GS state
    machine as now.

**Precision.** The native renderer could run the transform in plain IEEE floats on the GPU.
Positions would differ from the VU's toward-zero, clamped results by about an ulp, below a
pixel. But two consumers need exact VU1 output:
- the exact renderer (`DHD_RENDER=exact`), which must keep the CPU path;
- the **lens-flare Z probe**, the one GS result the game reads (native_core.inc). It needs exact
  Z for every Z-writing primitive inside the watch rectangle, a replay log for camera cuts, and
  whole exact frames while the probe is near the bottom edge.
  - So a GPU path still needs, per batch, a conservative screen bound on the CPU: a plain-float
    transform of the batch's bounding box, cheap next to exact VU1.
  - It also needs the exact CPU VU1 for every batch that may touch the rectangle, and for whole
    fields in the band.

**Feasibility and gain, roughly.**
- **Work.** WGSL kernels for about 8 loop variants and the clipping routine, a batch upload from
  VU memory (the unpacked data, 3 qwords a vertex), and indirect draws. Then GS-state plumbing
  around VU1-produced primitives (they no longer pass `gs_write`/`native_record`), the Z-probe
  bound and fallback, and the exact-mode split. 100-200 h, high risk.
- **x64.** It would take about 2.5 of the vu thread's 3.9 ms, and part of the GS thread's
  `gs_write`/`vertex_kick`/`native_record` for these primitives.
  - The vu thread is not the x64 limit until finding 089's item 1 (the alias fold) lands.
  - (a), the missing kernels and (b) together take about 0.9-1.0 ms off it for about 25-35 h.
- **Browser.** VU1 is 7.2 ms of the graphics worker's 16.3 ms (finding 089). There a GPU path is
  the one item big enough for 120 fps. For 60 fps the cheap items here, plus 089's worker
  split, are enough.
- **Bad candidates.** A29C0 (a recurrence with random numbers, MAC-flag culling and variable
  output) and overlay A's particle programs: small shares, sequential or already native.

### Raw data

- `runs/{stats1,stats2,stats3,k12e0}/`: game.log, op3.txt (the counters), summary_w*.txt
  (`op3stats.py`), table_w*.md (`final_table.py`), keyflags_w2.txt and timeline.csv. stats1 has
  only the first menu window (its race window ended at the bench exit).
- `pr1_vu_samples.{txt,json}`: finding 089's vu-thread samples by kind, unit and block.
- `tools/`:
  - `run.sh`: the hidden-desktop run;
  - `vuprof.py`, `keyflags.py`, `op3stats.py`, `final_table.py`;
  - `patch_vif.py`, `patch2.py`, `patch3.py`: how the local build was patched.
- `op3-instrumentation.diff`, `op3_vif.h`, `op3_vu.h`: the local counters.

Sources: findings 029 and 030 (the earlier flag-liveness pass, the AOT design), 046 and 051 (the
native kernels, `DHD_VU1_PROFILE`, `DHD_VIF_STATS`), 089 (the profile and its samples);
port/gs/native_core.inc (the Z probe); PCSX2 microVU's `mVUsetFlags` (flag liveness that skips
unread flag instances, the model for (a); no code taken).

## OP2: VU0's code-signature compare and the per-field getenvs

Finding 089's item 6, and the facts behind the cheap part of its item 2. Branch `rr/OP2`.

### The VU0 signature compare

`ps2_vu0_run` (port/runtime/vu0_interp.c) runs five vcallms entries through native routines (finding
066: 0000 cull, 02E0 bounds, 05E8 basis, 07B8 length, 0808 direction) only where VU0 micro memory
holds the exact words they were written from. It checked that with a `memcmp` of 192, 528, 376, 80
and 40 bytes on every call of those entries. In the core module that `memcmp` is wasi-libc's byte
loop (`w2c_core_memcmp`: one `i32_load8_u` per byte, each with the guest-memory alias fold on x64),
so a call cost several hundred byte loads: 4.5% of the x64 EE thread and 1.7% of the browser game
worker in finding 089. The two shorter compares were inlined by clang; the three long ones were
calls.

Now each routine's answer is kept with the code version it was computed for, as VU1's
`native_code_matches` and `aot_find` already do with `ps2_vu1_code_version`:
- `uint64_t ps2_vu0_code_version` (vu0_interp.c, declared in ps2asm.h).
- `vu0_native_code(cpu, which, at, words, bytes)`: for the game's micro memory
  (`GADDR(cpu->vu0_code) == 0x11000000`) the `memcmp` runs once per version and routine; any other
  code buffer (the replay and oracle tests: vu0_native_diff.c, test_vu0_native.py, test_ps2asm.c's
  static buffer) is compared on every call, as before.
- `DHD_VU0_CODE_CHECK=1` also compares on every call and prints
  `vu0: micro memory at XXXX changed without VIF0 MPG` if the kept answer was wrong (then uses the
  fresh one). Off, it costs one static test.

**Every path that writes VU0 micro memory, and how it invalidates:**
1. **VIF0 MPG** (port/hle/vif.c `vif_feed`, compiled into the core as vif0.c's
   `module_vif0_feed`, reached from dma.c's channel-0 transfers). It already compared each word
   before writing it; a changed word now also bumps `ps2_vu0_code_version` (`if (!unit)`, next to
   VIF1's `ps2_vu1_code_version++`). This is how the game loads its VU0 package (0x26CF80).
2. **The signature words themselves** (`ps2_vu_aot_load`, which fills `vu0_*_words` from the
   image's .vutext at image load): it bumps the version too, so an answer computed before the
   words were there (all zero; a zero first word never matches) is not kept.
3. **EE stores to 0x11000000-0x11000FFF.** The hardware allows them, and nothing in the core could
   see them, but the game does not make them: every `lui 0x1100` in the game's code builds a VIF
   code (FLUSH 0x11000000 in DMA packets: 00215B68, 00216BB8, 00218B80, 002249E0, 00224B88, 00226588,
   002286F8, 0022E560, 0022E9A0, 002370F0, 00237918, 00242F10) except 0021CB98, which stores
   to VU0 *data* memory (0x11004000); the one data word 0x11000000 (00283604) sits in a table of
   hardware address ranges (next to 0x1000C000, 0x1000E000, 0x12000000). The inline asm (ps2_asm) and the VU0 macro mode never address
   micro memory. `DHD_VU0_CODE_CHECK=1` is the check for this claim.
4. **Nothing else**: DMA from or to the scratchpad copies RAM and SPR only; VU1, the GS and the
   host never write VU0 memory; the state trace and the recorders only read it; memory.c zeroes it
   once when it commits the page, before the first vcallms.

The generator comment in port/tools/vu1_aot.py ("VU0 code has no version counter") is updated; its
output is unchanged.

### The getenvs read every field

Finding 089 found `common_getenv_nolock` at 0.5% of the x64 EE thread, called through
`w2c_env_host_getenv`: the core's `getenv` is the `host_getenv` import (runtime/wasi_adapter.c),
which on x64 is the CRT's `getenv` (a case-insensitive scan of the whole environment) plus the
bridge's value interning, and in the browser a call out to JavaScript. Getenvs that ran on every
field or every pad read, uncached:
- online.c `choose_view` (every field): DHD_VIEW; `pads_read` online: DHD_NET_SCRIPTED.
- kernel.c `vsync` (every field): DHD_SCREENSHOT.

These now go through `hle_env(&cache, name)` (port/hle/hle.h): the value is read once and again only
after the core changed its environment: every `host_putenv` of the core is followed by
`hle_env_changed()` (online.c's `set_env`, DHD_NAV and DHD_RACE_SEED at an online race's start and
end; online_menu.c's `save`, DHD_NET_NAME; pc_settings.c's rows; widescreen.c's aspect). Of the
cached variables only DHD_NAV changes at run time. The host's own writers (present.c's
DHD_MONITOR_MODE and window size, host.c's settings and DHD_ISO before the core starts) set none of
the cached variables. The returned pointers stay valid: the x64 bridge and the browser's `intern`
keep every value they hand out.

Left as they are: host.c's `host_pad_vibrate` reads DHD_VIBRATION on each motor change (the PC
settings page changes it live; host side, not every field), present.c's capture settings (render
thread), and the reads already behind a static.

### VU0 blocks in the core module (finding 089 item 2, cheap part): not switched on

The facts:
- The core compiles vu0_interp.c with `-DPS2_VU0_NATIVE` only (build_wasm.py): the five native
  routines and their word arrays (vu0_native_words.py), not `-DPS2_VU_AOT`. The `aot0_*` blocks
  and the whole AOT dispatch in `run()` are inside `#ifdef PS2_VU_AOT`, so the core does not even
  contain them. `PS2_VU_AOT` pulls in all of vu1_aot.inc: 718 VU1 blocks (55,000 of its 62,000
  lines) plus the VU1 native kernels (vu1_native_clip.inc, vu1_native_geometry.h,
  vu1_native_extra.h), dead code in the core, where VU1 never runs. The VU0 part is 53 blocks,
  about 6,200 lines.
- Where the blocks are compiled (the host's VU copy, the old i686 build) they are off by default:
  `DHD_VU0_AOT=1` turns them on. Findings 051 and 066 proved them exact on i686 (vu0_aot_diff.c:
  104 captures, 13 entries, 936 and later 3,432 full-state comparisons; the race gates MATCH with
  `DHD_VU0_AOT=1`) but kept them opt-in because the EE was not the limit there (EE busy -0.2 to
  -0.35 ms a field, wall time unchanged).
- `aot0_find` compares each block's words on every dispatch with `memcmp`; in the core that is the
  same byte loop as above, for every block of every interpreted program.
- They were never checked in the wasm core: the core's float operations are wasm's (the exact VU
  rounding comes from the float layer, not MXCSR), and vu0_aot_diff.c has no wasm build.

So it is not only a build switch, and nothing was changed. What switching them on would take:
1. vu1_aot.py writes a VU0-only include (the 53 `aot0_*` blocks, `aot0_find`, the word fills),
   and vu0_interp.c gets a `PS2_VU0_AOT` mode that compiles the VU0 half of the AOT dispatch in
   `run()` without the VU1 kernels; build_wasm.py builds the core with it. 2-3 h.
2. `aot0_find` keeps its answer per pc and `ps2_vu0_code_version`, as `aot_find` does for VU1
   (the version from this section makes that possible). 1 h.
3. Exactness in the core: vu0_aot_diff.c run against the core's own build (a wasm or wasm2c
   harness over the 104 captures and seeded variants), then a state-trace race compare with the
   default on. 3-5 h.
4. The default: on in the core, `DHD_VU0_AOT=0` to turn it off. With a short timing pair.
Estimate 6-10 h in all, as finding 089 says. Expected gain: the blocks remove the interpreter's
decode and dispatch for 71% of VU0 time, not the float work; from the i686 numbers and the share
here, about 0.3-0.8 ms of the x64 EE's 7.2-8.0 ms, and somewhat more in the browser.

### Checks

Builds, after merging `render/rust` 2979bc0b2 (CL6 and OP3 in): `build_wasm.py -j 2` (offset scan
clean), `package_release.py --wasm build/wasm/dhd_core.wasm --jobs 2` (dhd.exe sha256 8b28d259...),
`port/web/build.py --renderer rust --no-threads` and the threaded default, each followed by
`check_web_layout.py build/web`: OK.

**State trace** (`state_trace_run.py race`, 5000 fields, GO at 3214, the same input as the base;
the new build also with `DHD_VU0_CODE_CHECK=1`, which never fired):
- Before the CL6 merge, against the CL7 release exe (8a824262...): all 5000 rows match.
- After it, against render/rust 18c8f153e built the same way (992bbcb3...): 2,743 rows differed
  in `ram` (fields 25-27, 143-1112, 1444-3213; from GO on they match) and 6 in `cpu` (143-148).
  Every other section (scratch, hw, vu0, gs_stream, gs_regs, gs_vram, sound) matched all 5000.
  Dumps (fields 25, 143, 2000) and `DHD_STATE_WATCH` on the words found two host-layout values
  in game memory, the kind state_trace_mask.txt already lists:
  - 0x0093C628-0x0093C637: the t0 and t1 slots of func_00254D68's (sprintf) vararg spill, next to
    the already masked 0x0093C620. func_00208900's sound-bank message passes one argument, so
    t0/t1 still hold what strcat (func_002542C4, copying through func_00254544) left from
    appending the module's `";1"` literal: base 0x70 / 0x088AEE38, this build
    0x0101010101010101 / 0x3263666D7100313B (the word loop's constant, and `";1\0qmfc2"`: the
    word read past the NUL into the next .rodata bytes, an asm template). The literal is the
    tail of the merged string `"...LIBSD.IRX;1"`, followed by `"qmfc2..."` in both builds, at
    0x70104A45 in the base and 0x70104A58 in this build (read from the cores' data segments):
    func_00254544 (strcpy) copies word by word when source and destination are both 8-byte
    aligned and byte by byte otherwise, so the registers it leaves follow the literal's alignment.
  - 0x00760A10: a SIF RPC word that func_0025CF88's HLE call sets to an entry of sif.c's .bss
    `pending` and sif_poll clears: 0x7028DC44 against 0x7028DE34, the .bss moved by 0x1F0.
  A build without the second commit (pc_settings.c/widescreen.c) stores the same values as this
  one: they follow the data layout of commit 1 plus CL6, not a behaviour change. The five words are
  now in state_trace_mask.txt with their reasons. With the mask, rerun: `ram` and every other
  section match all 5000 rows. Left: `cpu` in fields 143-148, the same strcat leftovers still in
  t0/t1 at the end of those fields (a later sound-bank load); the mask covers RAM words only, and
  the registers are rewritten before field 149 with nothing else differing.
- After merging render/rust b739ef49f (OP1 in), against OP1's exe from the same port/ and src/
  (4c3753ec9, e0713288...), with the mask: the same result, every section but `cpu` 143-148
  matches all 5000 rows (the same two hashes as above); `DHD_VU0_CODE_CHECK=1` never fired.

**EE time** (PR1's recipe and launcher: hidden desktop, scale 8, 144 steps a second, full-pedal
race, `DHD_BENCH=7900-10900`, sound off; one run each, base then new; the PC quiet of builds, CPU
21% before and 46% after, other users' load not excluded):

| build | fields/s | p50 / p99 ms | EE (game) busy ms/field | VU | GS |
|---|---|---|---|---|---|
| render/rust 18c8f153e | 113.2 | 8.68 / 16.75 | 8.25 | 5.30 | 4.57 |
| OP2 | 120.1 | 8.04 / 14.15 | 7.73 | 5.14 | 4.38 |

EE busy -0.52 ms a field (-6%), in line with the profile's 4.5% for the memcmp plus 0.5% for the
getenvs. One pair on a shared PC: finding 089 saw single runs move by up to 15%.

## OP1: guest-memory alias fold out of the hot path

Branch `rr/OP1` (from `render/rust` 84cb6af61, render/rust 2979bc0b2 merged). x64 (and the Linux
host, which shares the header) only: the fold lives in the wasm2c memory layer
(`port/host64/memory_access.h`, applied by `port/tools/prepare_wasm2c.py`), not in the core C, so
`dhd_core.wasm` and the browser build are unchanged (the browser's own pass, BS3's `inline_memory`,
is untouched).

### What changed

1. **Fold out of line (0a6dd9d0e).** The full adapter (window test, crossing barrier, mirror
   fold) is unchanged. It is now inlined only for compile-time constant spans (it folds away) and
   otherwise called out of line (`noinline, cold`). Scalar/SIMD accesses (`MEM_ADDR_MEMOP`,
   1..16 bytes) use `host64_access_address`: one bit test of the address's 32-MiB window, mod 64,
   against a set of nine windows: the five alias windows and the window below each upper one,
   where a span may start and cross into an alias. An address the compiler proves at or above
   0x3a000000 (module data reached through a large memarg offset) has no test.
2. **One test per base value (d9817ee8a).** wasm2c writes every access as
   `NAME_default32(..., (u64)(base) + offset, ...)`. `prepare_wasm2c.py` rewrites each site to
   `host64_base_ok(base, offset) ? NAME_hot_default32(...) : NAME_cold_default32(...)`, and adds a
   `_hot` and a `_cold` copy beside each WABT access template. The hot copy addresses
   `base + address` with no test of its own. The cold copy calls the full adapter. The test reads
   the *base's* window (valid for offsets up to 32 MiB - 16), so every access through the same
   base value shares one test. Clang merges the diamonds and keeps plain addressing modes and
   alias analysis on the hot path. For offsets at or above 0x3a000000 the test is constant true.
   All 201,428 access sites of the module have this shape; `split_accesses` refuses any other.

### Why it is safe

- The fold only exists so that clang never sees two different addresses for one location. The
  OS maps every view onto the same pages (finding 082).
- Outside the six RAM views every address maps uniquely. Canonical RAM is one of the views.
  An address that touches no view other than canonical RAM is its own canonical address, so the
  full adapter returns `base + address` for it.
- The hot path is taken only where that holds:
  - The base's window w is outside the slow set, and offset + size <= 32 MiB. So the span lies
    in windows w and w+1, and neither holds a non-canonical view.
  - A span crossing w to w+1 joins two uniquely mapped ranges, so it needs no barrier.
  - Windows are taken mod 64. Every in-bounds address is below 2 GiB (the module's maximum
    memory). Beyond that the full adapter folds nothing.
- So every access gets the same host address as before, and alias analysis on the plain hot
  addresses only ever relates uniquely mapped locations. Alias spans, constant alias addresses,
  crossings near alias windows and bulk operations still go through the full adapter.
- Nothing is skipped on an unproven guess. The test is still at run time; it is only shared per
  base value.
- `selftest_host.c` checks the new adapter, `host64_base_ok` and the cold arm against the full
  adapter at every 4-MiB edge up to 8 GiB, for sizes 1..16 and offsets of each shape.
  `host64_selftest.py --optimized --negative-controls` passes; the prepared self-test module has
  174 split sites.

Not done: compile-time proofs per base register (sp-, $gp-relative). At the wasm2c level the guest
registers are loads from the module's `ps2.gpr` array. Their values can't be proven canonical
without a whole-program invariant. With the per-base test the remaining cost is about one bit test
per base value, so there is little left to gain there.

### Results

| | dhd64.exe | EE busy ms/field (2 runs) | fields/s |
|---|---|---|---|
| base (render/rust 84cb6af61) | 45.7 MB | 9.55, 10.49 | 98.8, 90.8 |
| step 1 (out of line, bit test) | 26.2 MB | 7.67, 7.84 | 115.6, 112.8 |
| step 2 (per-base test) | 24.9 MB | 6.06, 5.78 | 138.1, 141.9 |

These are finding 089's x64 runs (`hidden_run.py` on a hidden desktop, full-pedal race,
`DHD_FPS=144`, scale 2, sound off, `DHD_BENCH=7900-10900`), alternating base, step 1, step 2 and
then in reverse. The PC was shared: CPU load was 29-72% before the runs, so base ran slower than
in finding 089 (8.3 ms). Relative to base in the same rounds:
- step 1: -20%;
- step 2: -40%, about finding 089's no-fold probe (5.0 ms on a quieter PC, also -40%).

VU and GS busy also fell (VU 6.0-6.4 to 5.1, GS 5.1-5.5 to 4.3-4.4), which is probably less
contention for the cores.

### Checks

- `build_wasm.py -j 2`: offset scan clean.
- `package_release.py`: ok.
- Both web builds (threaded and `--no-threads`): ok, and `check_web_layout.py build/web` ok.
- State trace, `state_trace_run.py race` (5000 fields, through the race start): base exe against
  the new exe, `trace_columns.py` PASS, all 13 columns identical. It was run twice, before and
  after merging render/rust 18c8f153e.

## SP3: GS front end in native mode

2026-10-26, branch `rr/SP3`, based on main `d5fd0eceb` (VU1R included).

The existing recorder already merges primitives under unchanged state. This pass keeps that batching
and the C/Rust vertex ABI, and reduces work inside it:

- Cache FST UV normalization per draw state. GS texture dimensions are powers of two (1..1024),
  so multiplying by the combined reciprocal gives the same float as the two old divisions.
- Convert integer Z directly to float before scaling by 2^-16, 2^-24 or 2^-32. The exponent
  shift commutes with integer rounding here; neither order reaches subnormal or overflow values.
- Cache the 256 fog/255 values on first state preparation on the GS worker. Creation can run
  in EE rounding mode, so it must not initialize those values there.
- Accumulate bounds in local integers, then store the box once per primitive. Read SMODE2's
  fixed privileged-register slot directly (the same slot gs_priv_read returns).
- Skip the per-primitive field-trace function when its two enabling environment variables
  are absent; skip command segment hashing when interpolation is off.
- Form texture keys by iterating the set bits in the page mask, preserving ascending page
  order and therefore every hash and ownership query. This replaces 512 bit tests per lookup.

`gpu_exact.inc`, the exact-mode entry points, exact Z probe/replay and all game-state paths are
unchanged. No GPU shader, Rust ABI, draw ordering or texture invalidation rule changed.
`port/tools/test_native_vertex.py` extracts the production conversion and compares complete 32-byte
vertices against the previous expressions: all UV values, all fog bytes, all GS texture dimensions
and formats, randomized raw STQ bits, and every uint32 float rounding tie and its immediate
neighbours. It passes byte for byte.

Build note: the supplied checkout is on C:. Existing generators and package_release reject resolved
junction outputs outside their literal ROOT/build ancestry. SDL, ELF, asm and vutext are copies of
gate-X64 inputs. No build output or game data is tracked. The wasm core is reused identically for
base/change, because this lane changes only the host GS front end; the browser rebuild compiles that
same C front end for wasm.

### Checks

- x64 `build_host64.py` (8 wasm2c outputs, at most 2 jobs): PASS; final host refresh:
  57 cache hits, 1 changed GS object. `package_release.py --exe ... --jobs 1`: PASS.
- `port/web/build.py build/wasm/dhd_core.wasm --renderer rust`: PASS;
  `check_web_layout.py build/web`: all three modules and shared memory layout pass.
- Native and native-interpolated g+3600 replays: 4/4 golden files identical each,
  including both pictures, per-field VRAM hashes and final VRAM.
- Exact-mode g+3600 replay: 4/4 golden files identical.
- `test_native_transfers.py`: 33 pictures in each blending configuration;
  `gs_golden.py compare native-transfers`: all 198 golden items identical.
- Native vertex-byte arithmetic differential: PASS.

The first replay inputs were later baseline-gates race captures, which do not correspond to the
frozen replay goldens. Both sets failed the golden comparison (native input: 917k pixels; exact
input: 1.8k pixels). The recorder revision `8cd2b251f:port/tools/gs_golden.py` identifies the actual
capture source as `build/port/compare/r48-plain/port-gs` in the main checkout. Using its original
gs-g+3600.gs gives the exact matches above. No referee or golden changed. Creation-time fog
initialization was rejected during review because it can inherit EE rounding; the retained version
initializes on the GS worker. No separate timing probes were run.

### Timing

released immediately afterwards. No lane builds ran while holding it. Both ran on a hidden
desktop using the existing PR1 launcher (power throttling disabled), native mode, scale 2,
borderless 1920x1080, DHD_FPS=144, unpaced frame clock, seed 81257, full-pedal race,
sound/movies/memory-card/settings off. Same core wasm and race route; both reached GO at field
7139. Timing window 7900-17900: 10,000 race fields (80.5 s base, 73.2 s change).

| Metric | Base main d5fd0eceb | SP3 e7d8fba19 | Change |
|---|---:|---:|---:|
| GS busy ms/field (timeline, includes probe waits) | 4.571 | 3.807 | -16.7% |
| GS thread CPU ms/field | 4.704 | 4.231 | -10.1% |
| VU busy ms/field | 4.352 | 4.140 | -4.9% |
| EE busy ms/field | 7.470 | 6.823 | -8.7% |
| Fields/s | 124.201 | 136.523 | +9.9% |
| Field p50 / p99 ms | 7.510 / 18.901 | 6.920 / 15.427 | |

The measured GS busy time is 0.363 ms below the 4.17 ms target. This is one pair on a shared PC, not
a confidence interval: navigation took 43.7 s base versus 71.0 s changed, and the EE and VU numbers
moved too. The CPU-time reduction corroborates lower GS work but does not eliminate
frequency/contention effects. Do not attribute the full fields/s gain to this lane or claim
sustained 240 fps; the EE still limits throughput. No additional timing runs were taken.

Risks and next ideas: golden replay coverage is the captured race plus the synthetic transfer
scenes, not every game scene. This keeps existing batching, texture ownership/invalidation and the
C/Rust ABI intact. Next investigate caching the CLUT contribution to native texture keys with a
dedicated palette-content epoch (rather than the broad drawing-state epoch), and a broader
compatible-state batching change with capture-based picture/readback comparisons. No further timing
candidate was tested in this lane.

## SP2: recompiler runtime

2026-10-26, branch `rr/SP2`, commit `d653e7fcc` on base main `d5fd0eceb` (main merged in afterwards
as `3228a97ab`; the merge touches no file below). Finding 089 put the recompiler runtime at 13.6% of
the EE thread (`ps2_asm`, `lookup`, `ps2_exec`, `run_fast_program`). Five small cuts, all in
`port/runtime/` plus the recompiled-code generator:

- **One-entry program cache** (`ps2asm.c`): `lookup()` first compares the template, constraints and operand-count pointers against the last program used. Hot loops re-execute one call site thousands of times in a row; the 8192-entry table stays the authority on a miss, and the cache preserves its exact key semantics (the table ignores `n` after creation, so the cache does too).
- **Lazy `$zero` clear** (`ps2asm.c`, `ps2asm_fast.h`): the per-instruction `memset` of `gpr[0]` (and the fast templates' copies of it) now stores only when `gpr[0]` is non-zero. Clearing an already-zero register is the identity, and most instructions never write it.
- **`ps2_exec` direct single instruction** (`ps2asm.c`): with no AOT program and exactly one instruction, `ps2_exec` calls `run_ins` directly. `run()` for one instruction is exactly that call, and the AOT branch (`p->aot && asm_fast_enabled()`) still goes through `run_program`.
- **Memory-fence quick reject** (`ps2asm.h`): `ps2_memory_address` returns at once outside `[0x10000000, 0x12001100)`. All five fenced ranges (VU1/VIF1/GS windows) lie inside that interval, so no fenced access can be skipped; the RAM and mirror addresses that make up nearly every access skip the range chain.
- **Inline recompiled entry** (`recomp.h`, `recomp.py`): generated wrappers call `ps2_recomp_enter_inline()`, which is the same `if (!GPR(29))` test `ps2_recomp_enter` performs, inlined; the call remains for the first entry. `ps2_asm` also skips the `run_fast_program` call when `fast_kind` is 0 (it returns 0 at once then).

### Checks

- `build_wasm.py -j 2`: offset scan clean (rebuilt before the builds below).
- `package_release.py --wasm build/wasm/dhd_core.wasm --jobs 2`: ok (0.4.0 zip; an earlier direct `build_host64.py` attempt without the SDL arguments fails to link SDL symbols, which is what `package_release.py` supplies, not a code failure).
- Browser `port/web/build.py build/wasm/dhd_core.wasm --renderer rust` (threaded): ok; `port/web/check_web_layout.py build/web`: OK (core, graphics, renderer; shared memory).

### Timing

Hidden desktop (PR1 launcher, power throttling disabled), native renderer, scale 2, `DHD_FPS=144`,
unpaced frame clock, seed 81257, full-pedal race, sound/movies/memory-card/settings off; bench
window 7900-10900 (3000 race fields). Both reached GO and exited 0.

| Metric | Base main d5fd0eceb | SP2 d653e7fcc | Change |
|---|---:|---:|---:|
| EE busy ms/field (timeline) | 8.074 | 8.144 | +0.9% |
| EE thread CPU ms/field | 8.192 | 8.364 | +2.1% |
| Fields/s | 105.800 | 106.669 | +0.8% |
| Field p50 / p99 ms | 8.828 / 19.859 | 8.972 / 16.835 | |

No measurable EE gain in this pair. The changes are kept because each is provably
behaviour-identical (and the trace proves it end to end), costs nothing when its fast path misses,
and removes per-call work whose absence a quieter or browser run can still compound; they are not
claimed as a measured speedup.

Risks: the one-entry cache and lazy `$zero` clear rely on single-threaded EE execution of `ps2_asm` (true for the wasm core and both hosts today; the statics are not synchronised). The fence quick reject is exact only as long as every fenced range stays inside `[0x10000000, 0x12001100)`; adding a fenced window outside it would silently skip its sync. Next ideas: a guest-PC histogram in `ps2_asm` to find the remaining asm-run guest functions worth recompiling (finding 089's item 5), and specialising the operand bind/write-back loop in `ps2_asm` for the hottest templates instead of trimming single instructions.
## SP6: slowest frames

2026-10-26, `rr/SP6`, based on main `80b851d90` (SP3 included). This lane changes host
synchronization and opt-in diagnostics; the game core and GPU operations are unchanged.

### Finding and fix

The retired-queue wait checked its completion predicate, then loaded the consumption counter again
inside `wait_retired`. If the consumer finished between those reads, the wait took the
already-completed value as its baseline. Nothing had to advance it again until the EE queued the
next field, so progress depended on the nominal 1 ms timeout. GS retirement and the two
ring-capacity waits had the same second-read pattern. Timeout expiry can also be delayed by OS
scheduling; it is not a bound on wall time.

Carry the observation that failed the predicate into `idle_wait` instead. The wait still rechecks
after announcing sleep; packet publication, ordering, ring ownership, spin budgets, events and the
two-waiter timeout remain unchanged. No priority, affinity, timer-resolution, shader, rendering
arithmetic or game-state changes were made.

`test_retired_snapshot.py` extracts the production EE/GS wait functions and deterministically
publishes the final retirement between the predicate and wait. It passes for normal counters and
rollover; the same test against pre-fix `ee7ced87c` fails as intended. This proves the race and its
correction independently of the machine's load. It does not prove that every timeout in a live run
was unnecessary: long packets can legitimately time out too.

### Attribution

`DHD_TIMELINE=<csv>` now records scheduled CPU cycles (millions, not milliseconds) for EE, VU, GS
and native threads beside the existing per-field wall/wait bins. Raw cycles include spinning and are
not a game-function profiler. `DHD_SPIKES=1` adds first-use native pipeline creation, native
picture/texture calls, exact GPU readback polls, native present/acquire and host file-read timings.
GPU calls below 1 ms are omitted except pipelines; file reads and queue timeouts are all logged.
Disabled diagnostics do not read clocks on those paths. `DHD_PACING_LOG=<prefix>` supplies the
existing EE/presenter timing. No runtime VU compiler was added: VU1's AOT/native paths and VU0's
interpreter/signature dispatch remain unchanged.

`python port/tools/profile_spikes.py <run-directory>` reads `game.log` and `timeline.csv`, aligns
GPU timestamps with the benchmark's precise UTC/host clock pair, and writes `causes.csv` (a tag for
every field) and `causes.json`. Tags identify overlapping events, not exclusive proven causes. The
asynchronous native picture commonly overlaps a slow EE field; a long readback can include
descheduling. Low EE cycles despite long non-graphics-wait wall time are tagged
`ee_descheduled_or_uninstrumented`, explicitly an inference, not an ETW context-switch trace.

Counts within each 10,000-field race window:

| Observed event | Base | Change |
|---|---:|---:|
| Queue timeout, consumption unchanged at return | 2,443 | 513 |
| Queue timeout, consumption advanced at return | 158 | 10 |
| Longest unchanged-counter timeout, ms | 106.540 | 2.424 |
| Native draw pipeline creations | 2 | 2 |
| Longest pipeline creation, ms | 0.764 | 0.782 |
| Texture creation/upload calls >=1 ms | 3 | 1 |
| Exact readback polls >=1 ms | 571 | 326 |
| File reads (all <0.405 ms) | 40 | 40 |
| Native present/acquire calls >=1 ms | 189 | 156 |

Among the worst 100 fields, unchanged-counter timeouts overlap 69 base / 49 changed, readback polls
10 / 6, present/acquire 19 / 19, and slow texture creation 1 / 0. The low-cycle/long-wall inference
tags 55 / 5; high EE cycles tag 20 / 90, including synchronization spins. These categories overlap.
There is no evidence here that pipeline compilation or file reads dominate race p99. Sound output
was disabled in both runs; audio-enabled tails and the remaining game-logic/VU lookup costs are not
isolated by this experiment.

### One timing pair

Hidden desktop only: the existing PR1 `hidden_run.py` uses the same CreateDesktop/lpDesktop
mechanism as updtest's launcher, adds log redirection, owned-process cleanup and disables EcoQoS. No
visible desktop or focus was used. Native, scale 2, borderless 1920x1080, FPS 144, unpaced virtual
clock, seed 81257, full pedal, settings/movies/memory card/sound output off. Both reached GO at
field 7139; window 7900-17900. Same core wasm SHA-256
`3564afccdd3293bc2b1d1cf947d5aff33b454727a97cbfbe0bdfc40d797e3ab6`. Both runs enabled the same
attribution and pacing logs.

| Metric | Base + diagnostics (`ee7ced87c`) | Fixed (`ac10f7541` production code) |
|---|---:|---:|
| Field p50, ms | 8.151 | 6.994 |
| Field p99, ms | 73.803 | 17.763 |
| Field max, ms | 159.765 | 42.137 |
| Worst 1% mean, ms | 98.613 | 22.539 |
| Fields/s | 85.490 | 132.193 |
| EE / VU / GS timeline busy, ms/field | 9.509 / 4.518 / 4.580 | 6.657 / 4.441 / 4.262 |
| EE / VU / GS thread CPU, ms/field | 7.343 / 4.396 / 4.476 | 7.224 / 4.215 / 4.139 |

The shared PC was heavily contended, especially in the base run. The baseline p99 is therefore not
comparable to finding 089's or SP5's earlier 10-11 ms reference. These are observations from one
pair, not an isolated speedup estimate: do not attribute the full 76% p99 reduction to this fix. CPU
work moved much less than wall time. The deterministic test establishes the bug; reduced timeout
counts corroborate the intended direction. This does not reach 240 fps or remove all 144 fps
stutter. No additional timing runs were taken.

### Validation and follow-up

- x64 `package_release.py --wasm build/wasm/dhd_core.wasm --jobs 2`: PASS, source `834257fc0`;
  release zip SHA-256 `d24183ec8fd3422f60432bbb6283851214a7219d5d8aeb1e4a227cd8022fcb4f`.
  The measured changed exe and final packaged build have identical .text and .rdata sections;
  their PE link timestamps differ. Latest main was merged before packaging (already current).
- Rust browser build and `check_web_layout.py`: PASS, all three shared-memory modules.
- Deterministic retirement regression: PASS; old-source negative control fails as intended.
- Split GS FIFO stress: PASS with adaptive waits off/on, 12,000 exact callback records and full
  GS state/VRAM; both planted negative controls fail as intended. The separate VIF executable
  in `test_split_graphics.py` cannot link: its existing fixture lacks host_cycles, host_fp_csr,
  host_fp_set_csr, host_mem_protect, host_callback_fault_guard and host_fault_guard_here. That
  fixture and the VU/graphics sources producing those references are unchanged by this lane.
- `state_trace_run.py race`, 4,500 fields per executable, on a hidden desktop: PASS.
  `trace_columns.py` matches every compared section, including RAM, CPU, VU0, GS stream,
  registers/VRAM and sound. No ignored columns; the existing seven-word mask and code-pointer
  list are unchanged. Runs exited normally in 131 / 134 seconds.

No game data or generated output is tracked.

Research used: [Microsoft
WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)
for event/timeout semantics, and [wgpu
PipelineCache](https://docs.rs/wgpu/29.0.0/wgpu/struct.PipelineCache.html) for cache support. Its
persistent pipeline cache is Vulkan-only, so it is not a DX12 fix here; no external code was copied.
Pipeline prewarming and ISO prefetch were not implemented because the measured calls were too small
to explain these tails. Next: a quiet-machine/ETW scheduling capture, audio-enabled attribution, and
separating EE useful work from retirement spinning before changing spin policy or introducing
address-based wakeups. Readback waits still deserve critical-path profiling; preserve exact Z-probe
results and synchronization when investigating.

## SP7: thread handoffs

2026-10-26, branch `rr/SP7`, based on main `5ebc0a1b8` (SP3 and SP6 included). The isolated timing
pair uses identical core wasm, SHA-256
`3564afccdd3293bc2b1d1cf947d5aff33b454727a97cbfbe0bdfc40d797e3ab6`. Main advanced during validation;
SP5b (`e63bc6835`) was merged before the final builds (merge `451954506`). The final core includes
those main changes, SHA-256 `ed08db93d956040e33457b4889e88c09881ba7e81f5b310a05e011cda534c8e7`.

### Changes and attribution

- `DHD_TIMELINE` splits each host thread's queue wait into spin wall time and event-wait
  wall time (`ee/vu/gs_spin`, `ee/vu/gs_park`), and records total EE/VU/GS CPU milliseconds.
  The original queue bins and call-site attribution remain. These new bins are subsets of
  queue waits, not extra deductions from busy. Disabled diagnostics do not read clocks.
- `profile_handoffs.py` summarizes the per-field CSV. **Outside queue** is the old timeline's
  busy residual: it still includes descheduling, GPU/API blocking and instrumentation.
  Total CPU includes spins and host/driver work; neither number alone is pure game computation.
  Windows CPU-time rows are coarse; use their window average, not an individual row.
- Native VIF/VU and GS publication counters and sleep announcements each start on their own
  64-byte boundary. The linked x64 map confirms distinct aligned addresses for all counters.
  This separates writes to `written` and `consumed` and avoids publishing invalidating the
  opposite counter or its sleep flag. The browser already has this padding.
- Retirement and full-ring waits spin for at most 512 pauses instead of 4096. Empty consumers
  keep their existing adaptive 128..4096 policy (or fixed 4096 with `DHD_FIFO_ADAPTIVE=0`).
  Packet copies, ring sizes, FIFO ordering, atomics, events, SP6's predicate observations,
  sleep handshake and the shared-retirement-event 1 ms timeout are preserved.

The existing `busy_ms_per_field game` value already subtracts `graphics_wait` and ring-full waits.
Thus an EE busy value near 5 ms is already outside those queue waits; subtracting spin/park from it
again would be wrong. Thread CPU time, in contrast, includes queue spinning. Useful CPU alone would
need scheduled-cycle attribution inside the waits (or an ETW sample/ context-switch trace); the wall
bins deliberately do not claim that precision.

No `graphics_wait` fence was moved. The audited callers protect mapped VU1/VIF1/GS state, readback
setup/results, or VBLANK/presentation state. VU0 executes synchronously on the EE; its
interpreter/AOT computation belongs to the EE's outside-queue/CPU totals, not another host worker's
handoff. This lane changes no EE/VU arithmetic, GPU operation or renderer path.

Browser audit: `web_queue.c::wait_until` and `reserve` already pass the same `done` observation that
failed the predicate to `wait_consumed`. `graphics.js` waits on `written` with `consumed` as its
expected value after `web_q_run` announces sleep. Only that consumer writes consumed, and it does
not advance while it is returning to JS or asleep. A producer publication in that interval therefore
makes `Atomics.waitAsync` return not-equal instead of waiting on the new written value. The C wait
loop retains `seen` across its announce/recheck/atomic-wait sequence. No SP6-style second-read race
was found; no web source was changed, avoiding SP4's area.

Research: [Microsoft's false-sharing
explanation](https://learn.microsoft.com/en-us/archive/msdn-magazine/2008/october/net-matters-false-sharing)
for independent counters sharing a coherence line and padding as a mitigation. No external code was
copied. This is a targeted layout change, not a claim from hardware cache-miss counters.

### Validation

- Deterministic SP6 retirement test: PASS (EE, GS and counter rollover).
- Split GS FIFO stress: PASS with adaptive waits off/on, 12,000 exact callback records,
  full GS state/VRAM, producer reuse, provenance and rollover; both planted negative controls
  fail as intended. The queue fixture was compiled directly with the installed x86 SDK,
  excluding the separate VIF fixture's already documented missing-host-symbol problem.
- `package_release.py --wasm build/wasm/dhd_core.wasm --jobs 2`: PASS, source `cede74e40`;
  zip SHA-256 `6587324b079e94fb36ab9a31fc7458e6457ca2d386bcf13775aacf68e8fd3d7e`.
- Final integrated `build_wasm.py -j 2`: PASS, strict link and offset scan clean.
- Final integrated x64 packaging (`--wasm`, `--jobs 2`): PASS, source `6f37afadf`; zip SHA-256
  `2519132b2bc111cb89a4070db9235862f0f985c133d8ad4b236894442e03cbe9`.
- Rust browser builds, initial and after main integration: PASS.
  `port/web/check_web_layout.py build/web`: PASS (three shared-memory
  modules). An initial checker invocation used the wrong `port/tools` path; the correct command
  passed without changing the build.

### Final paired timing after main integration

Continuation on 2026-10-26: merged main `70fd3eeb5` (SP1 compiled VU0, UI1, SP9 and SP10) into SP7
as `42c068331`. Rebuilt the core with `build_wasm.py -j 2`: strict link and offset scan PASS,
SHA-256 `acaeb3f7fce0b5f6739a7ad665a2e70694a3d7a6df730cae4bc67f2b153847d0`. `package_release.py
--wasm build/wasm/dhd_core.wasm --jobs 2` PASS; candidate exe SHA-256
`9e5df6e1486b442bd8c4916bc28c01808279c53deab4d7025890956d4e6b7064`. The baseline host build also
passed, using that same core and main's unpadded counters/4096-pause policy, with SP7's identical
timeline instrumentation retained. Only the padding and retired/full-ring spin policy differ between
measured variants. These measurements supersede the earlier timing setup described above.

No own builds overlapped measurements. All four exited 0 and recorded 10,000 fields (7900-17900).
Identical settings: native, scale 2, 1920x1080 borderless, FPS 144, frame clock on, seed 81257, race
sync on, `state_trace_run.RACE_NAV`, pad `2:1200:cross`, movies/memcard/sound off,
`DHD_SETTINGS=none`. Busy, spin and park below are wall milliseconds per field; triples are EE / VU
/ GS. Busy is the outside-queue residual, including descheduling and GPU/API blocking. Frame
quantiles are from timeline `wall_ms`.

| Run | Busy EE / VU / GS | Spin EE / VU / GS | Park EE / VU / GS | p50 ms | p99 ms | Fields/s |
| --- | --- | --- | --- | ---: | ---: | ---: |
| Base 1 | 7.275 / 5.300 / 5.312 | 1.492 / 0.069 / 0.302 | 0.786 / 4.193 / 3.939 | 8.250 | 26.515 | 104.548 |
| Change 1 | 5.863 / 4.450 / 4.429 | 0.568 / 0.031 / 0.304 | 0.642 / 2.598 / 2.337 | 6.420 | 16.891 | 141.226 |
| Base 2 | 6.676 / 4.380 / 4.871 | 1.029 / 0.031 / 0.254 | 0.498 / 3.799 / 3.077 | 7.045 | 21.869 | 121.783 |
| Change 2 | 6.473 / 4.449 / 4.819 | 0.650 / 0.032 / 0.292 | 0.909 / 3.559 / 2.920 | 6.992 | 24.024 | 124.349 |

Retained the 64-byte padding, 512-pause policy, diagnostics and summary tool: throughput was higher
and median field time lower in both pairs, with EE spin lower in both. No slower-throughput result
triggered the conditional spin revert. This combined A/B does not isolate the padding benefit from
the spin-policy benefit. Pair 2's p99 regressed 21.869 -> 24.024 ms; its VU busy and VU/GS CPU also
rose (VU CPU 4.1094 -> 4.3266, GS CPU 4.3438 -> 4.4891 ms/field). Scheduling/other host load varies
substantially between runs, so the large first-pair gain is not an isolated speedup estimate and
there is no consistent tail-latency improvement. No new arithmetic/core changes or fresh state-trace
comparison in this continuation; the preceding queue tests remain the functional evidence. All owned
build/driver/game processes finished; the SP7 lock was removed.

scripts with real source/output ancestry. Inputs are copied from gate-X64, the identical core
wasm from SP6 for the isolated pair, then the final core was rebuilt from the merged source.
The initial package gate was already on current main; the updated main was merged before
the final core/package/browser builds. A snapshot line-ending/stat mismatch in gs_queue.inc
briefly failed the package dirty-source gate; copying the committed working file cleared it.
The gate was not changed or bypassed. No game data or
build output is tracked. Timing/trace driver and raw logs are in `SP7-build`;
executables, browser artifacts and trace TSVs are in `SP7-src/build`.
## SP5b: any frame rate up to 240

The offline cap goes from 144 to 240, and the per-update constants still left in the game code at
high rates are converted the way finding 067's patches are (port/patches_fps.py): every edit is
behind `port_fps_high()`, so 60 keeps the retail arithmetic. Each site was read against the clock
contract (`port_step()` = fields this update, `port_whole_step()` = 0/1 whole fields, `port_blend()`
= per-field relaxation over a fractional step) before anything changed; four of its sites were wrong
and one site it missed is the most important gameplay fix.

### The cap

- `port/hle/fps.c`: `port_fps_max()` = 240, or `DHD_FPS_MAX` when lower; `port_fps()` clamps
  `DHD_FPS` and the `monitor` path (`host_display_hz()`) to it. At 240 an update is 256
  animation subticks of 1024 per field (the comment says to keep the cap well below 1024).
- `port/hle/pc_settings.c`: Frame Rate offers 60, 120, 144, 165, 240, Monitor, built in
  `choices_build` from the rates up to `port_fps_max()` (as Resolution is built from
  `DHD_SCALE_MAX`).
- The browser shows the same `pc_settings.c` page, so its list is kept as it was by
  `DHD_FPS_MAX: '144'` in `port/web/www/settings.js`'s `BROWSER_PC_SETTINGS`: the browser still
  offers 60, 120, 144, Monitor, and Monitor on a 240 Hz display stays 144 there.
- `port/hle/host.c`'s dhd.ini template: "60 (original), 120, 144, 165, 240 or monitor (max 240)".
- Harness: `test_fps_race.py --rate` accepts 60..240; `test_fps.py` and `test_input_edges.py`
  list 165 and 240. (`test_fps.py` does not link on main either: `fps.c` calls `host_getenv`,
  `host_display_hz` and `host_exit`, which `test_fps.c` doesn't provide. Not fixed here.)

Online stays 60 (`pads_online()`), and on a panel below the rate the pictures keep their own grid
(`display_period`, host.c): 240 on a 144 Hz monitor gives about 144 pictures/s.

### Sites fixed

| Site | What runs per update | Fix |
|---|---|---|
| 00134688 rider-pair overlap | both riders moved apart by 0.1 of the overlap per call (00231A48 translates the body): a relaxation, 4x stiffer at 240 | `port_blend(0.1f)` |
| 001E6970 trick button sequencer (not in the survey) | the human rider's trick input: buttons within `x724 > 2` updates form one chord, the trick resolves after `x720 > 3` quiet updates. At 240 the chord window was 12 ms and a trick resolved 17 ms after the last press | `x724`, `x720` advance by `port_whole_step()` |
| 001E6F58 HUD code sequencer | the same machine on the HUD (26 codes of up to 8 chords): `x8 > 2` chord, `x4 > 20` timeout (83 ms at 240) | `x8`, `x4` by whole fields |
| 001C5040 `func_001D1528` jump wind sound | pitch/volume random walk: slope per field (`x40/(x70*60)`) added on every call of the rider's sound update (001C8380); 4x the swing at 240 | slopes `* port_step()` |
| 00228BE8 `func_0022A168` particle rise | after half its life a burst's spawn z rises by `x2764*60` per call (0022B708 calls it once per update; it already advances the life `x2760` by f20) | `* port_step()` |
| 0015E750 OPTIONS volume sliders | a held direction stepped 0..0x81 once per update (0.5 s end to end at 240) | step only on updates that advance a field |
| 00178730 trick banner, score popup | `x12BC` (120 fields), `x12C4` (90/120 fields with its fade in/out) counted per draw | whole fields |
| 00179A60 pickup icon blink, fades | `x132C` 120-field countdown, 32-field blink; `x135D`/`x135E` 75-field fades | whole fields (the countdown clamps at 0) |
| 0017A1B0 HUD panel slide | 20 steps of `delta`, one per call | runs on whole-field updates only |
| 0017B3C0 message popup | `x12F0` 180 fields, `x1308` 16-field blink | whole fields |
| 00176ED8 / 0017C638 | mode 10's 240-field banner cycle `x1324`; the item list's 300-field timer `x1310`, whose `== 60` event (flip of `x1288`, `x1318 = 60`) then fires once per field, not on every update of that field | whole fields; the event gated by `port_whole_step()` |

At high rates `step_fields` is capped at 1, so `port_whole_step()` is 0 or 1 and whole-field
counters pass every value; the `== 60` and `>= 16` tests keep working.

### Survey sites rejected

- **P2 camera bob (00148F60:153)**: not an accumulator. `tmp.x10` is copied from `obj->xA0` on
  every call; the `speed * (1/60)` term reads rider `+0x4564`, the airborne age (body `+0x4A4`),
  which `port_air_tick` already keeps in fields, and the smoothing (`func_00235EA8`) is already
  rate-corrected. Rate-correct as it is.
- **P5 particle size/alpha (00228BE8:2286-2297)**: the proposed edit is a spawn-time bounding
  radius (`x27D4 * x27DA` = growth per field x life in fields), not per-update work. The per-update
  consumers of `x27D0`/`x27D4` are in asm-only 0022B708, and `recomp.py` already multiplies those
  slopes (`$sp+0x1C`, `+0x2C`) by `port_step()`.
- **M1 "menu auto-repeat" (001E6F58)**: not menu navigation (that repeat is 0021F8E0, patched by
  067); it is the HUD code sequencer, fixed above. Its twin 001E6970 (tricks) is the real
  gameplay site.
- **M3 UI cooldown (001A9230)** and **H4 loading screen (001A9058)**: both are the ghost
  recorder's (001A8DE8: 001A9230 is the record path's checkpoint, 001A9058 the playback path
  with a once-a-second position check against the recording, not a loading screen). 001A8DE8
  returns at entry above 60 (067: recording and playback disabled), so neither runs.
- S1-S5 of the survey (subticks, display grid, throughput, online 60, sound grid) need no code:
  noted above.

### Checks

- Every patched function comes from its patched C object (build/wasm/dhd.map: 00134688,
  001C5040 `func_001D1528`, 00228BE8 `func_0022A168`, 001E6970, 001E6F58, 0015E750, 00178730,
  00179A60, 0017A1B0, 0017B3C0, 00176ED8, 0017C638), none from the recompiler.
- State trace at the default rate (60), `state_trace_run.py race` (5000 fields, seed 81257),
  base main 80b851d90 vs rr/SP5b (main merged): **identical in every section** (menu, phase,
  clock, ram, scratch, hw, cpu, vu0, gs_stream, gs_regs, gs_vram, sound, code), no `--ignore`,
  mask unchanged: PASS.
- x64 exe (package_release.py) and the browser (port/web/build.py --renderer rust,
  check_web_layout.py) build. No game run above 60 and no timing: the high-rate effect of these
  edits is by reading, not measured.

### Risks

- The trick sequencer's sample ring stays per update while its counters are in fields: the
  chord look-back (`D_0029D744` = 2 samples) covers 8 ms at 240 instead of 33 ms. Held buttons
  are in every sample, so chords still form; a tap shorter than a field can be lost, as a
  one-field tap already is at 60.
- HUD counters on whole fields move in field steps (as at 60), not smoothly.

## SP1: compiled VU0

Goal: the VU0 microprograms the game calls (VCALLMS/VCALLMSR) as straight C, built ahead of time
like VU1 (vu1_aot.py), to cut EE time per field.

### Change

- `port/tools/vu0_aot.py` (new) reads the VU0 package from .vutext 0x26CF80. That is 426
  instructions and 17 entries: micro address 0, plus the pair after each E-bit's delay slot.
  For each entry it writes one C function into build/wasm/vu0_aot.inc (build output, never
  committed).
  - The interpreter always starts a vcallms at cycle 0 with nothing pending. So the generator
    works out stalls (vf_ready per register), the 4-cycle MAC/status flag queue and flag reads
    at build time.
  - Branches on VI values known at build time (BAL/JR, IADDIU constants) are resolved. Branches
    on run-time VI values become if/else forks.
  - Registers load lazily and are written back once at the end.
  - The MAC flags are Z and S per lane, as in vu0_native.h. Sticky status bits are a vector OR.
    The flag registers are written once at the end, which is where the lazy flags come in.
  - Arithmetic uses the interpreter's per-lane clamp (arithmetic4). Operations whose operands are
    both constants get an opaque barrier (PS2_OPAQUE_M128), so clang cannot fold them in
    round-to-nearest. VU0 runs toward zero, and LOI constants make the difference here.
  - Anything it cannot prove declines (Reject): DIV/Q, CLIP, FC*, FSSET/FSEQ/FSOR, R, dynamic
    memory addresses, dynamic JR, or more than 4000 pairs on a path. In this package, none of
    the 17 entries decline.
- In `port/runtime/vu0_interp.c` (`-DPS2_VU0_COMPILED`), `ps2_vu0_run` tries the existing native
  routines first (0000, 02E0, 05E8, 07B8, 0808), then a compiled program, then the interpreter.
  - `vu0_compiled_find` picks a program by entry address and checks every micro word the program
    covers against the words it was built from. The check is cached per `ps2_vu0_code_version`
    (OP2's signature) and runs on every call when the code is not the game's VU0 micro memory.
  - It requires VI0 == 0.
  - `DHD_VU0_COMPILED=0`, `DHD_VU_INTERPRETER` or `DHD_VU_TRACE` turn it off.
  - The words are filled from .vutext at load, as for VU1.
- Calls go through a `switch`, not a function-pointer table. Pointers put the 17 functions into
  the wasm table and moved the table indices the game keeps in RAM by 17. The first state trace
  failed on 10 RAM words from field 1453 because of this.
- `port/tools/build_wasm.py` generates the programs and adds `-DPS2_VU0_COMPILED`.

### Exactness

- `port/tools/vu0c_diff.py` + `port/runtime/vu0c_diff.c` (new) build vu0_interp.c twice on x64
  with build_host64's flags: once compiled, and once as the original interpreter, the referee.
  - Every entry runs on random raw-bit states, mixed with zeros, denormals, extremes, infinities
    and NaN encodings. The whole ps2_cpu and data memory must come out bit-identical.
  - Result: 17 entries × 100,000 cases MATCH.
  - Every covered word changed in turn makes the program decline, and so does VI0 != 0.
  - `--negative` swaps one madd for an msub in 08D0, and the check catches it.
- State trace, `state_trace_run.py race` (5000 fields, seed 81257), base main e63bc6835 vs
  rr/SP1 bdf130b5a (main merged): **identical in all 13 sections**, mask unchanged: PASS.

### Measured

These are race timing runs of the x64 exe with the VU1R recipe (DHD_FPS=144, scale 2,
DHD_BENCH=7400-14600, full pedal, seed 81257), on a hidden desktop under game.lock, with runs
alternated. The PC was busy with other lanes (CPU 27-100 %), so expect noise. Values are EE busy
ms/field, with EE cpu ms/field in brackets.

| run | base main | SP1, VU0 compiled off | SP1 |
|---|---|---|---|
| 1 | 8.961 (6.864)* | 6.606 (6.807) | 5.345 (5.952) |
| 2 | 6.527 (6.757) | 9.405 (7.654)* | 4.520 (5.212) |

\* Outlier runs (p99 85-110 ms, 77-84 fields/s) from contention.

- Without the outliers: base and off are 6.5-6.6 ms, SP1 is 4.5-5.3 ms.
- So the change saves about 1-2 ms of EE time per field, and 0.8-1.5 ms of EE CPU time.
- Fields/s go from 139-141 to 158-184.

On native x64, the harness timed the interpreter against the compiled programs per call. For
example, 08D0 went from 3.7-6.8 µs to 0.15-0.18 µs, 0180 from 0.72 µs to 0.06 µs and 0830 from 3.8
µs to 0.26 µs. The native routines and the compiled programs for the same entries timed about the
same, so the natives stay first.

### Tried, not kept

- Inline COP2 register moves (qmtc2/qmfc2/cfc2/ctc2) in recomp.py, instead of the ps2_exec
  templates. The hot function 0021B8A8 has 47 such calls around its vcallms. Dropping the
  templates removed ps2asm_aot programs and .rodata strings. That moved the wasm table indices
  by -37 and the strings the game stores, so the trace differed in RAM and CPU from field 25.
  Making it pass would need a wider mask, which is not allowed, so the change was reverted
  (bdf130b5a). The macro-mode arithmetic in ps2asm.c belongs to SP2's area and was left alone.

### Risks

- The MAC U/O bits are never set, as already assumed in vu0_native.h. The fuzz includes
  extremes and still matches, because the clamp keeps results finite.
- In the browser, the SIMD and scalar float paths must round the same way under the VU0 CSR.
  The browser builds and passes check_web_layout, but nothing measured or traced the browser.
- Any change that adds function pointers or removes templates moves layout the state trace sees.
  Keep the switch dispatch.

### Next

- Inline the COP2 moves while keeping the layout: leave the template strings and table entries
  in place and only skip calling them.
- Compile vu0_micro.c's entries (0x0C0, 0x830, 0x978, called from ps2asm.c), or move the
  vcallms there to the compiled programs as well.
- Measure the browser, and a 240 race now that the EE is near 4.5 ms.

## SP9: ps2_asm hot functions

2026-10-26, branch `rr/SP9` (on rr/SP2 `d653e7fcc`, main `0c29546e4` merged as `d0dcfa6b4`). Race,
seed 81257, bench window 7900-10900 (3000 fields), hidden desktop, native renderer, scale 2,
`DHD_FPS=144`, unpaced, sound off; TSC 3.79 GHz.

### Numbers first

`DHD_ASM_HIST=1` histogram (WIP commit `d7ee9d92f`, on main `e63bc6835` + SP2): every
`ps2_asm`/`ps2_exec` entry with its instruction count and rdtsc cycles, per call site and path.

| path (per field) | entries | instructions | cycles (ms) |
|---|---:|---:|---:|
| `ps2_exec` (recompiled code), generated sequence | 37,337 | 37,337 | 11.38 M (3.00) |
| `ps2_asm` (game C inline asm), generated sequence | 9,552 | 35,927 | 4.86 M (1.28) |
| `ps2_asm`, `ps2asm_fast.h` template | 5,817 | 25,995 | 2.14 M (0.56) |
| `ps2_asm`, interpreter (`run()`) | 102 | 349 | 0.13 M (0.03) |
| total | 52,808 | 99,608 | 18.51 M (4.88) |

- Nothing hot is interpreted: 99.8% of entries already ran generated code (ps2asm_aot.inc) or a
  fast template. The cost is the call itself: lookup, operand bind/write-back, the call chain.
- Of the `ps2_exec` cycles, 7.2 M/field (1.9 ms) are the 13 hot `vcallms` sites: VU0
  microprograms (SP1's compiled VU0 has since cut that part). The other ~28,000 entries/field are
  single MMI/VU0 instructions at 80-100 cycles each, e.g. `pcgth` x2 at 2,148/field
  (func_00213C08), `psubh`/`pmulth`/`pmfhi`/`pmflo`/`paddw` at 1,652/field each and `pextlw` at
  979/field (func_00220DA0), `vmulax/vmadday/vmaddz` triples in func_00213C08.
- Hottest `ps2_asm` sites: the 15-instruction matrix-direction fast template (580/field, 0.56 M
  cycles), the 3-instruction bounds template (2,233/field), 4-instruction vector add/sub.
- The histogram adds ~140 cycles per entry (EE busy 8.86 -> 10.82 ms/field with it on), part of
  it inside the measured spans, so the cycle column overstates; the shares below are from a
  sampled profile without it.

Sampled profile of the EE thread (debug exe of the same source, histogram off, 30 s, 1 ms; % of the
thread's busy samples): `lookup` 5.2, `ps2_asm` 4.7, `ps2_exec` 4.1, `run_fast_program` 2.9: 17%
dispatch, plus 9.0% in the generated bodies. So the share is real (well over 5%). `hist_enabled`
itself took 3.3% while off (an out-of-line call per entry), so the histogram was dropped again after
measuring (git history keeps it, `d7ee9d92f`).

### Change: recompiled instructions call their generated code (PS2X)

- `recomp.py` (wasm build only, `WASM_EMIT`) writes `PS2X(ps2x_<sha1 of text>, "text")` where it
  wrote `ps2_exec("text")`; `recomp.h`'s PS2X is a direct call.
- `ps2asm_aot.py` reads the PS2X sites and defines each `ps2x_*` beside the generated
  sequences: `static Prog *p; if (p) aot_run_N(p, 0); else p = exec_first(text, aot_ins_N, n);`.
  The first call runs the text through `ps2_exec` (unchanged) and keeps its `Prog` only when the
  run-time parse equals the generated sequence byte for byte (the same test
  `select_aot_program` uses); otherwise, or with `DHD_ASM_GENERIC=1`, every call stays on
  `ps2_exec`. A text the generator cannot parse gets `{ ps2_exec(text); }`. In this build all 216
  distinct texts are generated (449 sites); the 526 generated sequences and the program table
  are unchanged.
- Code: +62 lines (`ps2asm.c` 15, `ps2asm_aot.py` 25, `recomp.py`/`recomp.h` 8), minus the
  histogram (~170) and two SP2 cuts.

### SP2's cuts

- Dropped: the lazy `$zero` clear (a load and a branch per instruction instead of one store, not
  shown to help) and the `ps2_exec` single-instruction branch (`ps2_exec` is the fallback now).
- Kept, each a few lines in paths the profile shows hot: the one-entry lookup cache (`lookup`
  serves `ps2_asm` only now), the fence quick reject in `ps2_memory_address` (every recompiled
  store), the inlined `ps2_recomp_enter` test (every recompiled entry) and skipping
  `run_fast_program` when `fast_kind` is 0.

### Effect

Profile after the change (same recipe, pre-merge build): `ps2_exec` and `hist_enabled` gone,
`lookup` 5.2 -> 2.1%, the `ps2x_*` functions 0.8%; dispatch 20.1 -> 10.4% of EE busy.

| run | base main `0c29546e4` EE busy ms/field | SP9 `d0dcfa6b4` EE busy ms/field |
|---|---:|---:|
| pair 1 | 5.794 | 5.168 |
| pair 2 | 6.538 | 6.114 |
| mean | 6.166 | 5.641 (-8.5%) |

Before the main merge, on main `e63bc6835`, a pair measured +1.6% and -17% (the second with the
whole PC quieter): the direction holds, the size is in the noise of one PC; the profile's ~6-7% of
EE is the best estimate.

### Checks

- `build_wasm.py -j 2` (offset scan clean), `package_release.py --jobs 2`, browser
  `port/web/build.py --renderer rust` + `check_web_layout.py build/web`: OK (core, graphics,
  renderer), all on the merged source.
- State trace, `state_trace_run.py race` (5000 fields), base main exe against SP9,
  `trace_columns.py`, unchanged mask (7 masked words): all 13 columns identical, both before the
  main merge (base `e63bc6835`) and after it (base `0c29546e4`). The template literals move from
  the recompiled units to ps2asm.c, but the program table and every literal stay, and the trace
  shows no layout effect (SP1's attempt removed templates; this does not).

### Relation to SP11

rr/SP11 (`8e940e31c`, not on main yet) writes the COP2 moves (qmtc2/qmfc2/cfc2/ctc2) and vcallms as
C in `recomp.py`, before the `ps2_exec` fallback. With both merged, SP11's C takes those
instructions and PS2X only the rest (VU0 macro arithmetic, MMI, the control registers SP11 leaves on
ps2_exec): no instruction gets two treatments. The merge conflicts only in `ps2asm_aot.py`: keep
SP11's `EXEC_CALL` (`ps2_exec(?:_kept)?`), add `DIRECT_CALL`, and test `'ps2_exec' not in text and
'PS2X(' not in text`.

### Next (EE thread after SP1 + SP9, % of busy samples)

- Recompiled game functions 25.7% (0021B8A8 6.6, 00213C08 4.9, 002628A0 4.4, 00213658 2.1):
  decompiling or hand-fixing these is the biggest single item.
- `ps2_asm` dispatch 11.8% (`ps2_asm` 5.4, `run_fast_program` 4.0, `lookup` 2.4) plus 11.2% in
  generated bodies: the same direct-call scheme for the game's inline asm (transform.py emitting
  a per-template call, `ps2asm_aot.py` folding constraint kinds and operand sizes into the
  bind/write-back). 77 of the 685 asm-using units are C++, so the declaration needs `extern "C"`
  at file scope. That is about 2.4% for the lookup alone, more with the bind folded.
- `ps2_vu_madd` 5.5% (exact VU multiply-add, called from the generated bodies), the slow memory
  path `host64_memory_address_slow` 4.3%, `memmove`/`memory_copy` 3.7%.
- `common_getenv_nolock` 0.5% of the EE thread: some getenv still runs per field or per call.

## SP10: indexed native vertices

2026-10-26, `rr/SP10`, main `0c29546e4` merged in. Native mode only. Exact mode, the WGSL, the
vertex layout and every locked file are unchanged.

### Change

- **C front end** (native_core.inc). In an indexed field, each draw command's first/count is a
  range of the field's u32 indices.
  - A strip's first two vertices are checked against the previous triangle's last two. A fan's
    are checked against that triangle's first and last.
  - A vertex is reused only when all 32 bytes match, and otherwise stored. Lists store all three
    vertices.
  - A sprite, line or point is four corners and six indices (0 1 2, 2 1 3, as before).
  - Equal bytes draw the same, so the triangles, their order and their vertex bytes are those
    of the expanded picture.
  - Off with DHD_INTERP: its mix pairs segments and averages motion by recorded vertex counts,
    so it keeps the expanded vertices. Also off with `DHD_NATIVE_INDEXED=0` (the A/B switch).
  - The browser never interpolates, so it is always indexed.
  - The DHD_NATIVE_TRACE fields CSV gained an `indices` column.
- **ABI.** The call is a new export, `dhdr_native_picture_indexed(n, picture, indices,
  index_count, out)`, declared in the new `port/render/include/dhd_render_ext.h`.
  - The frozen `dhd_render.h`, `abi.rs`, `ffi.rs` and `lib.rs` are locked, so the export lives
    in render.rs and repeats ffi.rs's picture checks.
  - `build_graphics.py` reads the DHDR_FN declarations of both headers.
  - Folding `indices`/`index_count` into a v2 DhdrPicture is the cleaner form once the header is
    reopened.
- **Rust renderer** (arenas.rs, render.rs).
  - An index buffer (INDEX | COPY_DST) grows from 2^18 indices by doubling. Every draw pass
    binds it, and `draw_indexed` replaces `draw`.
  - The GREATER-with-writes second draw reverses index triples instead of copying vertices.
  - The existing merge of consecutive draws with nothing set between them works on index ranges
    unchanged. That is the "merge draws sharing pipeline + bind group" idea: draws that share
    both state and contiguous ranges were already merged, and wider merges need constants per
    draw (WGSL).
  - The non-indexed path stays for native_replay (.ntrace) and for interpolation.
- **Unrelated fix.** The GS test programs (replay.c, test_native_*.c) no longer linked on main:
  UI1's `gs_picture_rect` call is in present.c, which they don't link. Under DHD_GS_TEST_BUILD,
  gpu_rust_native.inc now has a no-op.

### Effect

| Race (x64, 10,000 fields, seed 81257) | expanded | indexed |
|---|---:|---:|
| stored vertices per field | 89.9k | 42.7-43.1k |
| indices per field | 0 | 89.9k |
| vertex + index bytes per field | 2.88 MB | 1.73 MB (-40%) |

- **Golden picture g+3600:** 94,497 -> 44,790 vertices, 3.02 -> 1.81 MB.
- **C recording cost** (the vertex part of native_record, rdtsc in the replay build, 3 runs
  each): 130-160 cycles per primitive expanded, 109-123 indexed. Fewer bytes are stored; the
  compares cost less than the stores they save.
- **Timing, two pairs under game.lock, same dhd64.exe**, `DHD_NATIVE_INDEXED=0` as the base.
  SP8's run.sh setup: hidden desktop, native, scale 2, 144 fps, frame clock, fields 7900-17900.

  | CPU ms/field | pair 1 base | pair 1 indexed | pair 2 indexed | pair 2 base |
  |---|---:|---:|---:|---:|
  | native (render thread) | 1.651 | 1.559 | 1.326 | 1.118 |
  | gs | 4.101 | 4.348 | 3.810 | 3.195 |
  | game / vu | 5.892 / 4.006 | 6.060 / 3.985 | 5.370 / 3.642 | 4.384 / 2.981 |
  | fields/s | 157.9 | 149.1 | 177.9 | 218.2 |

  In pair 2 the base ran every thread, the EE and VU included, about 20% faster. These runs
  can't resolve a 0.1 ms effect on a shared PC: the indexed run measured lower in pair 1 and
  higher in pair 2. The render-thread wall time per picture (`native_picture`) didn't move
  beyond that noise: 1.18 against 1.20 ms in pair 1.
  - On a desktop GPU, the render thread's cost is wgpu-core's per-draw encode and the driver
    (SP8). 1.1 MB less to copy is about 0.05-0.1 ms.
- **Expected elsewhere** (not measured; the phone is AND2's):
  - Browser: writeBuffer cost 1.17 ms/field for about 3 MB (finding 089), and recording and the
    renderer share the graphics worker there. 40% fewer bytes should be roughly 0.4 ms/field.
  - Phone: the native thread's kernel time (staging memory) and memcpy scale with the bytes
    written. Expect part of the 59% kernel share to shrink. Measure on the Mi 11i: `native`
    ms/field, base about 8.5.

### Checks

- Native replay g+3600, plain and DHD_INTERP=1: 4/4 golden items identical each.
- `test_native_transfers.py`: PASS, 33 pictures. `gs_golden.py compare native-transfers`:
  198/198 identical.
- Same replay with `DHD_NATIVE_INDEXED=0` against 1: pictures and VRAM byte-identical.
- The capture is the original r48-plain `gs-g+3600.gs`.
- `package_release.py --wasm build/wasm/dhd_core.wasm --jobs 2`: PASS (source 864509523; this findings commit changes no code).
- `port/web/build.py ... --renderer rust`: PASS; graphics.wasm imports
  `dhdr_native_picture_indexed` and the renderer exports it. `check_web_layout.py build/web`:
  OK.
- The browser's indexed pictures were not run: there is no headless native runner.

### Risks and notes

- The goldens cover one race picture and the synthetic transfer scenes; menus, the HUD and
  movies are not in them. The rule is byte equality, so a missed sharing case costs bytes,
  never pixels.
- u32 indices: u16 with a base vertex per draw would save another 0.19 MB per field, but draws
  over 65,536 vertices would have to split, and the contiguous-draw merge would need equal base
  vertices.
- SP8 (not merged) conflicts textually in arenas.rs and render.rs (`picture`). Conceptually the
  two fit: the belt should also copy the index bytes (`write_indices`), and import batching is
  untouched by this change.
- An older dhd_render library without the new export fails to link with this C. The .ntrace
  recorder/replayer (NTRACE.md) still holds expanded pictures.

## SP11: EE toward 4.17 ms

Branch `rr/SP11` from main `0c29546e4` (after SP1), main `e1c06e6fb` (SP9) merged in. Goal: the EE
thread under 4.17 ms a field in races (240 fps). A sampled race of main `0c29546e4` (debug build,
`sample_profile_raw.py` from 5 s after GO for 40 s, 21,138 game-thread samples) set the order of the
work.

### What the profile showed

- `wasm_quietf` 7.5% and `wasm_quiet` 4.3% of the game thread's samples (plus `wasm_fabs`
  0.4%): about 16% of its busy time. wasm2c calls them on every `f32.promote` and
  `f64.demote` (the game's float code converts float and double all the time), and in
  floor, ceil, trunc, nearest, sqrt and fabs. They are `if (isnan(x)) quiet; return x`.
  Under the UCRT headers `isnan(x)` is `fpclassify(x) == FP_NAN`. So each conversion did a
  compare with zero, a NaN compare, an infinity compare and a normal/subnormal compare.
  `-ffp-model=strict` keeps every one of those compares, even the ones whose result nothing
  uses, because a compare may raise a flag. The disassembly in `ps2_vu_madd` shows all four
  ahead of each `cvtss2sd`.
- `ps2_exec` 2.0% self plus its share of `lookup` (3.6% with `ps2_asm`'s): the recompiled
  functions' COP2 moves and vcallms (0021B8A8 alone has 47 of them around its vcallms).
- The three vu0_micro.c entries (0x0C0, 0x830, 0x978) do not appear in the profile at all.

### Changes

1. **wasm2c's NaN tests as builtins** (`port/tools/prepare_wasm2c.py` `builtin_float_tests`,
   applied to the `*-impl.h` of every host build). `isnan(` becomes `__builtin_isnan(` and
   `signbit(` becomes `__builtin_signbit(` (14 and 2 sites).
   - Both give the same answer for every input, so every value computed stays the same.
   - x64, Linux and Android hosts get this. The browser runs the wasm itself, so it is
     unchanged.
2. **Recompiled COP2 moves and vcallms in C** (`port/tools/recomp.py` `cop2_move`).
   - `qmtc2`, `qmfc2`, `cfc2` and `ctc2` (the plain, `.i` and `.ni` forms) become the
     statements that `ps2asm.c`'s `run_ins` runs for them. `qmfc2` of vf0 gives (0,0,0,1.0),
     `cfc2` zero-extends, and `ctc2` truncates to 16 bits for VI registers.
   - These stay on `ps2_exec`/PS2X: the control registers with side effects or float views
     (R, I, Q, FBRST, VPU-STAT: 20, 21, 22, 28, 29), moves into `$zero`, and any other
     spelling.
   - `vcallms N` becomes `ps2_vcallms("text", N)`. In `ps2asm.c`, run_ins's V_CALLMS body moved
     into `vcallms()`, which both callers share. It sets the VU0 CSR, runs the registered native
     routine or else `ps2_vu0_run`, and restores the CSR. The text is used only to name the call
     site if the microcode fails.
   - Each site also writes `ps2_exec_kept("text")` (`recomp.h`: an empty asm with the literal
     as an input). `ps2asm_aot.py` reads `ps2_exec_kept(` as well, so the template set, the 526
     generated sequences and the function table do not change. The table has 37,152 entries,
     identical by name against main (checked with a wasm section compare). SP1's attempt
     dropped the templates and the race trace then differed.
3. **Merge with SP9.** SP9's PS2X (recompiled code calling its generated code directly) and this
   lane do not overlap. `recomp.py` tries `cop2_move` first, so the moves and vcallms are C,
   and PS2X takes the rest: VU0 macro arithmetic, MMI, and the control registers left out above.
   In `ps2asm_aot.py`, EXEC_CALL (`ps2_exec(?:_kept)?`) and DIRECT_CALL sit side by side.

### Exactness

- State trace, `state_trace_run.py race` (5000 fields, seed 81257), unchanged mask (7 masked
  words), `trace_columns.py`: **all 13 columns identical, PASS** twice.
  - Main `0c29546e4` against SP11 `8e940e31c` (changes 1 and 2).
  - Main `e1c06e6fb` (SP9) against SP11 after the merge (`efd94bb91`).
- After the merge the wasm data section differs from main's (252,850 against 252,226 bytes):
  the move literals stay in the recompiled units, while PS2X keeps its other literals in
  ps2asm.c. The table is identical. Before SP9, the data section was byte-identical too.

- Builds after the merge: `build_wasm.py -j 2` (offset scan clean), `package_release.py --jobs 2`,
  browser `port/web/build.py --renderer rust` and `check_web_layout.py build/web`: OK (core,
  graphics, renderer, threaded).

### Measured

These are race timing runs of the x64 exe with the VU1R/SP1 recipe: DHD_FPS=144, scale 2,
DHD_BENCH=7400-14600, full pedal, seed 81257, hidden desktop, under game.lock, base and change
alternated. The PC was busy (CPU load 65-100% from the other lanes), so expect noise. Each cell is
EE busy ms/field, with EE CPU ms/field and fields/s in brackets.

| pair | base | SP11 |
|---|---|---|
| change 1 only, base `0c29546e4`, run 1 | 5.03 (5.64, 161/s) | 3.79 (4.82, 194/s) |
| change 1 only, base `0c29546e4`, run 2 | 5.39 (5.78, 155/s) | 4.70 (5.47, 165/s) |
| final, base `e1c06e6fb`, run 1 | 5.33 (6.06, 153/s) | 4.46 (5.62, 164/s) |
| final, base `e1c06e6fb`, run 2 | 4.78 (5.79, 160/s) | 4.05 (5.05, 179/s) |

- The change takes about 0.7-1.2 ms of EE busy time a field (15-25%), and 0.5-0.8 ms of EE
  CPU time.
- Most of it should be change 1. Change 2 was not timed on its own. It is about 2-3% of the
  profile at most, and one base-against-(1+2) pair before the merge was lost in noise: base
  5.65/6.57 ms against SP11 5.72/4.65 ms.
- On this loaded PC the EE is at or under 4.17 ms in some runs (3.79, 4.05), but not reliably.
- **240 fps** (DHD_FPS=240, same recipe, GO at field 11603, DHD_BENCH=12500-24500):
  - 216 fields/s, p50 4.30 ms, p99 8.9 ms.
  - EE busy 3.24 ms/field, vu 3.43, gs 3.11.
  - So at 240 no single thread is saturated. What is left is the threads waiting for each other
    and the load from the other lanes.

### Not done, and why

- **vu0_micro.c through vu0_aot.py**: vu0_aot.py already compiles 0x0C0, 0x830 and 0x978, but
  using those programs would not help.
  - The native routines in vu0_micro.c are already straight C, and they do not show in the
    profile.
  - They also write only the registers the EE reads back. A compiled program writes the full VU0
    state (temporaries, ACC, VI15, MAC/status flags), and that state is in the trace's cpu
    column, so swapping them would change the trace without a gain.
- The getenv still running every field (`common_getenv_nolock` about 0.6%): `present.c`'s
  `capture()` reads 5-6 variables (DHD_SCREENSHOT, DHD_CAPTURE_*, DHD_RACE_SYNC) on every vsync
  on the EE thread. It was left alone (port/gs is SP3's area). Caching them is the cheap next
  fix.

### Next

- The slow memory path (`host64_memory_address_slow` + `full` + `alias_crossing`, about 5% of
  the game thread's samples). The likely source is the uncached RAM aliases (0x20/0x30
  windows, at 0x28/0x38 in the module): every access through them is an out-of-line call. A
  one-test inline fold for "inside 0x28/0x38, not crossing" would keep the same addresses.
- `ps2_vu_madd` (3.8%, after this change mostly its clamps) and the remaining recompiled game
  functions (0021B8A8, 002628A0, 00213C08: about 12%).
- The VU0 CSR is set twice per recompiled vcallms (`vcallms()`, then `ps2_vu0_run` again;
  `host_fp_csr`/`host_fp_set_csr` 0.8%). The inner set could be skipped when the CSR already
  holds the VU0 value.

## SP12: compiled VU0 in the browser

2026-10-26, branch `rr/SP12` from main `0c29546e4` (SP1 merged). No code change: compiled VU0 stays
on in the browser build. The question was whether SP1's compiled VU0 programs, checked bit-exact on
x64, also match in the browser's wasm engine, where `port/web/postlink.py` rewrites every scalar and
vector float operation of the core to the `web_*` helpers of `port/web/float.c` (which read the FP
control word the core keeps at 0x07800000 and, under the VU0 rounding mode, round toward zero
through binary64).

### Trace path

A browser trace path already exists: `port/web/run_headless.mjs` runs the browser build's
`dhd_core.wasm` (threaded, graphics worker included) under Node's V8/JSPI with the same
`DHD_STATE_TRACE` recipe as x64 (`port/tools/state_trace_run.py` race: 5000 fields, seed 81257, the
same NAV and race-clock pad, the unchanged mask `port/tools/state_trace_mask.txt`), and
`port/tools/trace_columns.py` compares the TSVs column by column. The race recipe is passed with
`--env DHD_NAV=... --env DHD_RACE_SYNC=1 --env DHD_RACE_PAD=... --env DHD_RACE_SEED=81257`; `--env
DHD_VU0_COMPILED=0` turns the compiled programs off. No new code was needed.

### Results

- Browser compiled on vs browser compiled off (`DHD_VU0_COMPILED=0`): **identical in all 13
  columns** (menu, phase, clock, ram, scratch, hw, cpu, vu0, gs_stream, gs_regs, gs_vram, sound,
  code), 5000 fields each. The compiled programs and the interpreter agree bit for bit in the
  wasm engine.
- Browser (either setting) vs the x64 exe of the same main: differs in `vu0`, `gs_stream` and
  `gs_regs` from field 29, in `ram` and `hw` from field 2954 and in `sound` from field 2955;
  menu, phase, clock, scratch, cpu, gs_vram and code are identical over all 5000 fields. With
  compiled VU0 off the differences are the same fields, so this is a pre-existing
  browser-vs-x64 difference, not a compiled-VU0 difference.
- The field-29 dumps (`DHD_STATE_DUMP=29`) locate it outside VU0: all 30 differing words of the
  vu0 section are in VU1 data memory (offsets 0xC000-0xFFFF of the dump) or in the VU1 register
  snapshot after it, each one float ULP from the x64 value; VU0 micro and data memory
  (0x0000-0x7FFF) are identical, and the first differing gs_stream word is one of those values.
  That is the graphics reactor's VU1 path (`PS2_VU_IEEE` in vu0_interp.c: plain IEEE
  round-to-nearest for the picture, by design), which the state trace happens to hash through
  VU1 memory and the GS stream; it feeds no game state (cpu and clock never differ, and the
  field-2954 RAM words are graphics bookkeeping of the same kind). x64 runs VU1 at toward-zero,
  so the two builds cannot match those columns until VU1 in the browser is exact, which is a
  separate, larger task (the `--exact-float` graphics build exists for it).

### Float layer check

SIMD and scalar round the same way under the VU0 mode, which is why the compiled (SIMD) and
interpreted (scalar) VU0 agree above.

### Browser fps

Under `game.lock`, headless Chrome (check_browser.mjs, own port, race route to field 4500): both
settings sit at the 60 fields/s display cap in the race window (compiled 56.0, off 58.0 mean
fields/s over fields 3550-4500, one run each on a shared PC). The uncapped Node runner
(run_headless.mjs, race window from field 3600) gave compiled 74.2 and 80.0 against off 84.6 and
77.6 fields/s in two alternated pairs: the ranges overlap, so no fps difference is measurable either
way in the browser; the x64 gain does not obviously carry over, and compiled VU0 costs nothing
measurable there either.

### Risks and next

- The browser-vs-x64 trace difference is pre-existing and VU1-shaped; do not widen the mask
  for it. A browser trace gate should compare browser against browser (as here: on vs off),
  or run the graphics reactor with `--exact-float` before comparing against x64.
- Chrome itself was used for fps only; the trace ran in Node's V8, the same wasm engine
  family, but a real-Chrome trace (the page has no state-trace export) remains untested.

## SP4: browser threaded build, VU1 on its own worker

2026-10-26, branch `rr/SP4` (main merged up to `0c29546e4`, SP1 included). Browser only
(`port/web`), plus one switch in `port/hle` that only the threaded browser sets.

### Where the time went (main, race, unpaced, Chrome on a hidden desktop)

CPU profile of both workers over ~800 race fields (52.3 fields/s in the profile window; 56-60
unprofiled):

| worker | busy | waiting for the other | per field |
|---|---|---|---|
| game (`worker.js`, the core) | 87% | 12.8% (on graphics) | ~16.7 ms busy |
| graphics (`graphics.js`) | 83.5% | 16.5% idle | ~16.0 ms: VU1 34%, VIF 4.9%, GS 28.9%, WebGPU 8.9% |

Both were near the limit, the graphics worker by a hair. VIF1/VU1 was 39% of it, so the x64 build's
split (a VU thread feeding the GS thread) was the biggest single step: (a).

### Changes

1. **VIF1/VU1 in a third worker** (`www/vu.js`, `vu.wasm`). The core's queue (`web_queue.h`
   block 1, 0x07A00000) now goes to the VU worker. It runs the VIF and VU1 calls
   (`build_graphics.py` marks them `WQ_VU_CALL`, 0x200 in the packet's kind). Everything else
   goes on through a second ring (block 2, 0x07A00800) to the GS worker:
   - `WQ_GIF`: the words of each `graphics_gif_feed` call, copied, one packet per call, so
     the GS sees the same packet boundaries (the state trace's gs_stream);
   - `WQ_REF`: a pointer to the block-1 packet of a GS call, PATH3 feed, frame, vsync or env;
   - `WQ_MARK`: a block-1 position.

   The GS worker stores block 1's `consumed` at each MARK, so the core's side (gqueue.wasm,
   its waits, readbacks and results) is unchanged. The VU worker marks:
   - after each waited call;
   - whenever it catches up with `written`;
   - at least every quarter ring.

   `vu.wasm` sits in its own span (0x58000000-0x5C000000) with its own FP control word
   (0x07800080) and helper stack (0x079A0000). It imports only `memory` and
   `host_wall_seconds`; `check_web_layout.py` checks it as the `vu` role, threaded builds only.
   The one-worker build (`--no-threads`) is unchanged: one graphics module with both sides.
2. **Exact fast paths for the EE FPU's binary64 add, sub and mul** (`double.c`). The EE's
   float ops compute in binary64 from promoted floats (`ps2float.h`). In EE mode (round
   toward zero) these went through the software helpers. Two float-like operands (24
   significant bits, normal float exponent) have:
   - an exact product (48 bits);
   - an exact sum when their exponents are at most 28 apart.

   An exact result rounds the same in every mode, so the plain wasm op gives the right answer.
   `double_fuzz.c` ops 7-9 draw such operands (the exponent gap around the edge half the time):
   1M results per op, identical to SSE with MXCSR=FFC0. A gap of 31 fails op 7 (the control).
   Float-helper time in the core went from 11.75% to 9.55% (dmul 3.55% to 1.56%).
3. **The threaded core skips the waits that only order queued graphics work**
   (`DHD_GRAPHICS_ORDERED`, set by `worker.js` and `run_headless.mjs` in threaded runs; `hle.h`).
   Two waits are skipped: the vsync's second wait after the VBLANK handlers, and
   `sceGsSyncPath`'s. The workers run every queued call in order anyway, and
   `host_service_gs_vsync` snapshots the GS MMIO pages at queue time.

   Every read still waits:
   - GS calls that return a value;
   - `ps2_graphics_sync`, before the EE touches the mapped VU1, VIF1, GIF or GS pages;
   - the vsync's first wait, before the VBLANK reads.

x64 never sets the switch, and `DHD_SCREENSHOT` runs keep every wait.

### Measured

Race fields 3800-4800, unpaced (`host_pacing_config` period 1 ms), Rust renderer, 2x scale, Chrome
on a hidden desktop. Base and change ran one at a time under `game.lock`, interleaved, with other
lanes building on the PC (CPU load 15-80% before runs).

Before SP1 (main `e63bc6835` merged; change = 1+2+3), fields/s:

| pair | base | change |
|---|---|---|
| 1 | 59.42 | 74.89 |
| 2 | 66.29 | 57.35 |
| 3 | 68.10 | 65.28 |
| 4 | 58.15 | 70.02 |
| mean | 63.0 | 66.9 |

After merging main `0c29546e4` (SP1's compiled VU0 included), fields/s, with the CPU load at the
start of each run:

| run | base | change |
|---|---|---|
| 1 | 57.17 (load 90%) | 54.42 (load 97%) |
| 2 | 57.07 (46%) | 77.27 (30%) |
| 3 | 56.40 (71%) | 71.77 (32%) |
| mean | 56.9 | 67.8 (+19%) |

Base stays at ~57 whatever the load. The change is 72-77 (+26-35%) when the PC has cores to spare,
and a little slower (-5%) on a PC at 97% load, where a third busy thread has no core.

Profile of the change (before 3, before SP1): the game worker waits 10.3% (was 12.8%). It is now the
limit at ~16.3 ms busy, while the GS worker is idle 47% and the VU worker busy ~42% (~7.6 ms per
field).

**Waits.** A counting gqueue (scratch) shows, per race field:
- 4-7 graphics waits that find work still pending;
- 3-4.5 of them blocking in `wait32`;
- about 1 waited call (the CSR read).

So the core's remaining wait is real graphics latency, not wake-up cost. A deeper queue alone
(option b) would not buy more than those waits. Skipping the ordering-only waits (3) is the safe
part of (b).

### Checks

- Race state trace, Node (`run_headless.mjs`, 4300 fields, the race NAV/env), base = the same
  merged core with main's `port/web` (DHD_GRAPHICS_ORDERED unset), change = this branch
  (split, double fast paths, ordered waits): `trace_columns.py` PASS, all 13 columns identical.
  VU counters are equal: `vu_native` 4702923, `vu_steps` 84550237.
- Ordered against unordered on one core: identical. Before the merge, the core with the switch
  against the older base core showed one `cpu` row (field 4248) differing. That row was only the
  classification of one host pointer in r8; the raw GPR lanes were identical (`DHD_STATE_GPRS`).
  The cause is the core's data layout (one more global), not the switch: the same row appears
  with the switch off.
- Browser race picture: OK; readbacks 891, as in base.
- `build.py --renderer rust`, threaded and `--no-threads`: OK.
- `check_web_layout.py`:
  - threaded: core, graphics, renderer, vu;
  - `--no-threads`: core, graphics, renderer, and a stale `vu.wasm` is refused.
- `--no-threads` Node run: 600 fields OK.
- x64: `package_release.py --wasm ... --jobs 2` builds (`port/hle` touched).
- Note for main: the race trace of main `0c29546e4`'s core differs from the one of `80b851d90`'s
  in `ram` and `cpu` from field 25 (same mask, same `state_trace.c`). Both of this lane's builds
  are on the new core, so this comes from main's merges (UI1/SP1), not from SP4.

### Risks

- 3 workers: one more thread on small machines (2 cores) may slow down rather than speed up.
  `vu.js` blocks in `Atomics.wait` and yields after 0.1 s idle.
- PATH3 feeds and GS calls go to the GS by reference to the block-1 packet. Block 1 is only
  reused after the GS worker's MARK, so the data is still there. The VU's own GIF words are
  copied (cost: one copy of the VU1 XGKICK words per field).
- The `DHD_VU_SUSPECT` diagnostic watch (`gs.suspect_draws`) is off in threaded builds: the VU
  module has no GS.
- The ordered waits rely on every graphics read being waited or fenced. A new host call that
  reads graphics state without a result or fence would see stale data in the browser only.
  `?env.DHD_GRAPHICS_ORDERED=` turns it off.

### Next

- The core is now the limit:
  - the float helpers (`web_helper_sum` from `ps2_vu0_run`; SP1's compiled VU0 may already
    take most of it);
  - `web_copy` from `ps2_asm`.
- The GS worker is idle ~47%: room for work moved off the core (e.g. the PATH3 and texture copies).
- A batch of VIF1 calls per packet (one ring packet per DMA chain instead of per call).

## SP14: EE runtime helpers

Branch `rr/SP14` from main `70fd3eeb5`, main `c4a2161be` (SP11, SP12, SP4, AND2, SP7) merged in.
Goal: the EE thread toward 4.17 ms a field. This lane covers the runtime helpers around the
recompiled code, not recomp.py, ps2asm_aot.py (SP11) or transform.py (SP13).

### What the profile showed

Debug build of main `70fd3eeb5`, `sample_profile_raw.py` on the hidden desktop, race seed 81257.
Shares are of all game-thread samples (`idle_wait` 26%):

- `ps2_vu_madd` 4.2%. It was out of line (1443 bytes): every lane went float -> double and back,
  with wasm2c's NaN-quieting promote/demote helpers around each conversion.
- `host64_memory_address_slow` 3.3%. That is OP1's cold arm, the out-of-line alias fold.
- `memmove` 1.4%, `memory_copy` 1.4%, `memcpy_repmovs` 0.5%.
- `common_getenv_nolock` 0.4%, from `gs_vsync` -> `present.c` `capture()`. It ran six getenvs
  per field, each scanning the whole environment.

A census build counted the slow-path calls by window, size and caller, and the `memory.copy` calls
by size and caller. It used a local instrumentation header, not committed. Totals over the race run:

- **Slow path**: 528M calls, about 48k per field.
  - By window: 84% the low view (window 0, at or above 4 MiB), 16% the 0x38000000 mirror,
    0x28000000 negligible.
  - By caller:
    - recompiled code 50%;
    - game C 26.5% (mostly `func_00204A98` and `func_00206E48`);
    - runtime helpers 22% (`ps2_store128` 11%, `ps2_store32` 4.4%, ldl/ldr/lwl/lwr about 6%);
    - `dma_poll` 1.7%.
- **memory.copy**: 93M calls.
  - 77M are 4-byte copies from `ps2_asm`'s operand bind and write-back (a variable-size
    `memcpy` that wasm lowers to `memory.copy`).
  - About 3.6M are `deliver()`'s register-bank copies of 128-512 bytes: `save_registers` and
    `load_registers` around `sif_poll`, on every kernel entry.

### Changes

1. **`ps2_vu_madd` in single precision** (`port/runtime/ps2float.h`).
   - The old double version stays as `ps2_vu_madd_reference`. The new one clamps the inputs
     (`ps2_float_input`), does the product in float, clamps it, then does the add or subtract
     in float and clamps the result.
   - Why it is exact:
     - With clamped inputs, the double product of two floats is exact, so the float product is
       the same single rounding of it.
     - A float add or subtract equals the double one rounded to float. Toward zero, truncating
       twice is truncating once. To nearest, double rounding is harmless because 53 >= 2*24+2.
     - An overflowing add gives FLT_MAX (toward zero) or inf, and the clamp maps both to FLT_MAX.
   - The one case where results can differ is an overflowing product (magnitude at or above
     0x7f7fffff after rounding). That case takes the reference, under `__builtin_expect`.
   - `ps2_vu_arith4` does ADD, SUB, MUL, MADD and MSUB on four lanes at once with clang vector
     types. `ps2_float_input4` is the bitwise clamp: denormals go to signed zero, inf and NaN to
     signed FLT_MAX. MADD/MSUB return 0, and the caller takes the scalar path, when a product in
     a dest lane overflows.
   - `ps2asm.c` `vu_arith` uses it for those five ops, OPMULA for its MUL, and OPMSUB for its
     MSUB.
2. **Canonical RAM in the runtime's address helpers** (`ps2asm.h` `ps2_ram_canonical`, used by
   `ps2_memory_address` and `recomp.c`'s ldl/ldr/lwl/lwr/sdl/sdr/swl/swr).
   - A guest address in the low view [0x00400000, 0x02000000) or the mirror [0x38000000,
     0x3a000000) becomes 0x08000000 + (address & 0x1ffffff): the same bytes on every host.
     The x64 accesses then stay on OP1's inline arm.
   - One bit test rejects every other window. The fold is only under `__wasm__`: native test
     builds pass host pointers.
   - This covers the recompiled code (its loads and stores go through `ps2_memory_address`) and
     the store helpers.
3. **Cheaper slow path** (`port/host64/memory_access.h` `host64_memory_address_slow`).
   - What still reaches the slow path (game C's own pointers, `dma_poll`) checks the two
     common views first: a span inside the low view or the 0x38 mirror folds directly, by the
     full adapter's own rule.
   - Everything else, crossing spans included, goes to `host64_memory_address_full` as before.
   - `host64_selftest.py --optimized --negative-controls` PASS.
4. **`ps2_asm` operand copies** (`ps2asm.c` `operand_copy`). The bind and the GPR/LO/HI
   write-back copy 4, 8 or 16 bytes with constant-size `memcpy`, which compiles to plain loads
   and stores. Before, a variable-size `memcpy` became `memory.copy` -> `memmove`. Other sizes
   keep the variable copy.
5. **`deliver()` skips `sif_poll` with no RPC in flight** (`port/hle/kernel.c`). With
   `sif_next_event()` at its "none" value, `sif_poll` does nothing. The two register-bank copies
   around it (about 1.5 KB of memmove per kernel entry) are skipped with it.
6. **`capture()` reads `DHD_SCREENSHOT` once** (`port/gs/present.c`). Every branch of `capture()`
   needs that variable. When it is unset, `capture()` returns before the six getenvs. SP13 had
   not taken this.

### Differential test of the madd

`port/tools/vu_madd_diff.py` builds `port/runtime/vu_madd_diff.c`, which compares raw bits of the
new scalar and vector paths against the reference. It is built and run three ways:

- x64 with build_host64's strict FP flags, under the EE MXCSR (0xffc0: toward zero, FTZ, DAZ),
  round to nearest (0x1f80), and two more modes (0x9fc0, 0x7f80);
- x64 at -O2 without the strict model;
- wasm32 + SIMD128 (wasi-sdk, build_wasm.py's flags) under node.

The cases cover:

- zero, denormals, inf, NaN, FLT_MAX;
- values near the top and bottom of the range, and products near FLT_MAX and FLT_MIN;
- short mantissas, everyday magnitudes, random raw bits;
- near-cancelling accumulators and the broadcast forms.

Result: 1,000,000 cases per mode, all **MATCH**. `--negative` builds a vector MADD without the
overflow fallback, and the test catches it.

### Profile after

Debug build of the merged branch, same recipe. Shares of all game-thread samples, with base in
brackets:

- `ps2_vu_madd`: gone from the profile (inlined; only the reference stays out of line) [4.2%].
- `host64_memory_address_slow`: 0.68% [3.28%].
- `memory_copy` 0.02% [1.41%], `memmove` 1.95% [1.43%], `memcpy_repmovs` 0.68% [0.53%].
  Together 2.65% [3.37%]. The 4-byte copies are gone. What remains is bulk copies, whose share
  grows as the rest shrinks; their callers were not traced further.
- `common_getenv_nolock`: gone [0.39%].
- `deliver`: 0.42%.

### Timing

Race seed 81257, bench fields 7900-10900, `DHD_FPS=144`, scale 2, native renderer, hidden desktop,
one run at a time under `game.lock`. Base is main `c4a2161be`; change is `rr/SP14` after the merge.
Pairs alternate:

| run | EE busy ms/field | EE CPU ms/field | fields/s |
|---|---:|---:|---:|
| base a | 4.002 | 5.140 | 151.7 |
| SP14 a | 3.451 | 4.671 | 167.4 |
| SP14 b | 3.532 | 4.859 | 162.2 |
| base b | 3.959 | 5.140 | 156.6 |

EE busy time drops from 3.98 to 3.49 ms a field (-0.49 ms, -12.3%). EE CPU time drops from 5.14 to
4.77 ms a field (-7%). Fields/s rise by 6.5%.

### Exactness

- x64 state trace: `state_trace_run.py race` (5000 fields), base main `c4a2161be` against
  `rr/SP14` `8fd57573b`, unchanged mask (7 masked words), `trace_columns.py`. **All 13 columns
  identical, PASS.**
- Browser: the race state trace of the threaded browser build (`build.py --renderer rust`) in
  Node (`run_headless.mjs`, the race recipe, seed 81257, 4300 fields). Base main `c4a2161be`
  against `rr/SP14`, `trace_columns.py`: **all 13 columns identical, PASS**. VU counters are
  equal (`vu_native` 4242717, `vu_steps` 79008641).
- `vu_madd_diff.py`: MATCH in every mode (above). `host64_selftest.py --optimized
  --negative-controls`: PASS.
- Builds:
  - `package_release.py --jobs 2`: OK (0.4.0 zip, source `8fd57573b`, only Windows DLL imports);
  - `port/web/build.py --renderer rust` plus `check_web_layout.py`: OK (core, graphics,
    renderer, vu).

### Not done

- **Game C's own low-view pointers.** These are 26.5% of the census, in `func_00204A98` and
  `func_00206E48`. They are plain C dereferences, so the fold would belong in transform.py,
  which is SP13's area.
- **`dma_poll`'s DMA tag reads** (1.7%) still take the slow path. Its tag addresses are taken
  from the guest as-is.
- **`wasm_quietf`/`wasm_quiet`** are SP11's area and already merged.

### Risks

- **Browser float semantics.** The browser runs f32 ops through the postlink helpers that
  emulate the EE float mode. The exactness argument holds for both roundings. The wasm32 run of
  `vu_madd_diff` (plain IEEE) and the Node race trace check it.
- **`ps2_ram_canonical`** changes the address that recompiled code and the store helpers access,
  not the guest values in registers. It folds only under `__wasm__`. Code that compared the
  host address it gets back against a low-view or mirror range would now see canonical RAM. No
  such code exists today: the fenced MMIO ranges lie outside both views.
- **`deliver()`** depends on `sif_next_event()` returning its "none" value exactly when no RPC
  is pending. A future SIF change that queues work without an event would need the old path.

### Next

- Fold low-view pointers in game C at transform time (SP13's area). That is the largest
  remaining slow-path share.
- Inline the two-view fold in the cold arm of the hottest functions, instead of a call.
- Canonicalize `dma_poll`'s tag and source addresses once per chain.
- Trace the remaining bulk `memmove` callers per field.

## SP13: inline asm direct calls

2026-10-26, branch `rr/SP13` from main `e1c06e6fb` (SP9), main `fbf0dd6f4` (SP4, SP10, SP11, SP12)
merged as `e82fefc1b`. Main with SP14, SP7 and AND2 merged again as `b7c6af824`. SP9's next item:
the game C's inline asm (transform.py's `ps2_asm` calls) paid a lookup, a generic operand
bind/write-back loop and an indirect call per statement.

### Change

- **transform.py**: an asm statement with a literal template becomes
  `PS2A(ps2a_<sha1 of template, constraints, count>, "template", "constraints", n, __o)` instead of
  `ps2_asm(...)`. The operand array and the input copies in the game function stay as they were.
  Templates a macro builds from its arguments (14 sites, e.g. 00148F60's `VADD(d,a,b,op)`,
  00214BA0's `VOP`) keep `ps2_asm`.
- **port.h**: `PS2A` is `ps2_asm` unless `PS2_ASM_DIRECT` (build_wasm.py sets it for the game
  units only; build.py and the tools' test programs keep `ps2_asm`). Then it declares the function at
  block scope with an asm label and calls it with the same four arguments.
- **C++ units** (77 of the asm units): a block-scope declaration inside an `extern "C"` function has
  C linkage, elsewhere C++ linkage, so 3 units failed with "different language linkage".
  transform.py now puts one file-scope `extern "C" { ... }` declaration of every `ps2a_*` of a C++
  unit in front of it, followed by `#line 1`, so line numbers stay.
- **ps2asm_aot.py** reads the PS2A sites (into the same template set: 694 templates, 526 sequences,
  the program table unchanged) and writes one function per distinct (template, constraints, count):
  478 functions for 2,199 sites. Each binds the inputs and writes the outputs back operand by
  operand, the way `ps2_asm` does, with the kinds, ties and directions fixed at generation time
  (`asm_bind_gpr`/`asm_put`: the sizes 4, 8 and 16 as fixed copies, any other size as before).
  It then runs `run_fast_program` when the program has a fast kind, else its `aot_run_N`, as
  `run_program` would.
- **Checked once at run time**: the first call goes through `ps2_asm` (lookup, parse, fast kind,
  AOT selection: unchanged). `asm_first` then keeps the program only when the run-time parse is the
  generated one byte for byte: instructions, plus each operand's kind, tie, input and output flags.
  Otherwise, and with `DHD_ASM_GENERIC=1`, every call stays on `ps2_asm`. The 3 templates the
  generator does not parse (`1: bc0f 1b`, `1:`, and one whole-function asm that reads `$a0`) are
  plain `ps2_asm` calls.
- The caller's literals are passed in, so the lookup table keeps the same keys and every template
  literal stays in its game unit. recompiled.txt is identical to main's.
- **getenv** (SP9 and SP11's leftover, `common_getenv_nolock` ~0.5% of the EE thread):
  `present.c`'s `capture()` read 6 variables on every vsync. Every one of its branches needs
  `DHD_SCREENSHOT`, which only the launch sets, so it now reads that once and returns at once
  when it is unset. With it set, nothing changes. SP14 made the same change (`0f67cff3d`); the
  merge keeps its copy.
- Code: transform.py +25, ps2asm_aot.py +55, ps2asm.c +45, port.h +11, present.c +5,
  build_wasm.py +1, test_runtime.py (regex also takes `PS2A(`).

### Exactness

State trace, `state_trace_run.py race` (5000 fields, seed 81257), unchanged mask (7 masked words),
`trace_columns.py`: **all 13 columns identical, PASS**, both for main `fbf0dd6f4` against SP13
`e82fefc1b` and, after the second merge, for main `6a59044a0` against SP13 `b7c6af824`. A pre-merge
exe against `e1c06e6fb` was built but not traced. The lock was busy, so only the merged pair was
run.

### Measured

Race, seed 81257, bench window 7900-10900, hidden desktop, native renderer, scale 2, DHD_FPS=144,
unpaced, sound off, under game.lock, release exes alternated (`fbf0dd6f4` against `e82fefc1b`):

| run | base EE busy ms/field (CPU, fields/s) | SP13 EE busy ms/field (CPU, fields/s) |
|---|---|---|
| pair 1 | 4.773 (6.46, 141.9) | 3.972 (5.86, 158.6) |
| pair 2 | 4.194 (5.90, 156.5) | 4.082 (5.98, 154.9) |
| mean | 4.48 | 4.03 (-10%) |

Pair 1 has a high base. The profile's share, below, is the better estimate.

Sampled profile, debug (PDB) builds of the same sources, 30 s from 10 s after GO, 1 ms; share of the
game thread's busy samples (5,686 base, 4,518 SP13):

| function | base | SP13 |
|---|---:|---:|
| `ps2_asm` | 6.37% | 0.09% |
| `lookup` | 3.11% | 0.11% |
| `ps2a_*` (binding, write-back, inlined bodies) | n/a | 4.78% |
| `run_fast_program` (fast templates) | 3.68% | 3.43% |
| `aot_run_*` (their own frames) | 8.13% | 8.15% |
| inline asm in total | 21.3% | 16.6% |
| `common_getenv_nolock` | 0.53% | 0 |

Dispatch (`ps2_asm` + `lookup`, now `ps2a_*` minus the bodies inlined into it) went from 9.5% to at
most 4.8% of EE busy, and the getenv is gone: about 5% of the EE thread in all.

### Checks

`build_wasm.py -j 2` (offset scan clean, 0 failed units), `package_release.py --jobs 2`, browser
`port/web/build.py --renderer rust` and `check_web_layout.py build/web`: OK (core, graphics,
renderer, vu), on both merges. After the second merge, recompiled.txt is again identical to main's.

### Next

- `run_fast_program` (3.4%) is now the biggest dispatch cost. It is noinline (ps2asm_fast.h keeps
  fast-template temporaries out of `ps2_asm`'s frame). The generator could pick the fast kind at
  generation time and call the fast routine directly, without the switch.
- The 14 macro-built templates (`VADD`/`VOP`/`VEC_OP`/`VU_SETX` ...) still take the lookup and the
  generic bind. A per-site cache, or expanding the macro uses in transform.py, would cover them.
- The `aot_run_*` bodies (8%) are the interpreter's code with constant `Ins`. `ps2_vu_madd`
  (4%) and the memory fence calls inside them are next. Recompiled 0021B8A8/00213C08/002628A0
  (~17%) and `host64_memory_address_slow` (5%) are still the biggest single items.

## SP17: main's race trace after the round

2026-10-26, branch `rr/SP17` (from main `6a59044a0`). SP4 reported that the race trace of main
`0c29546e4`'s core differs from `80b851d90`'s in `ram` and `cpu` from field 25.

### Where it changes

| commit | x64 vs 80b851d90 | Node vs 80b851d90 |
|---|---|---|
| e63bc6835 (SP5b) | identical | - |
| 2b6d29715 (UI1) | identical | - |
| 0c29546e4 (SP1) | identical | identical |
| 315462880 (SP9, SP10, SP11, SP12 in) | identical | - |
| fbf0dd6f4 (SP4) | ram 25-27, cpu 25-107, 143-1112, 1444-1452 | the same rows |
| c4a2161be (main later) | the same rows | - |

So the first changing commit is **fbf0dd6f4, the SP4 merge**. SP5b, UI1 and SP1 do not change it,
alone or combined. In both runners the differing rows are the same and every other section (scratch,
hw, vu0, gs_stream, gs_regs, gs_vram, sound, code, menu, phase, clock) is identical over all 5000
fields; from field 1453 on, every section is.

### What differs: layout only

Dumps (`--dump 25,26,27,108,150,1000`, `DHD_STATE_GPRS=1`), 80b851d90 against fbf0dd6f4:
- **ram**: one unmasked word, 0x0093C624: 0x00000000 against 0x80808080. It is the upper word of
  the a3 slot of func_00254D68's (sprintf) vararg spill (`sd $a3, 0xB8($sp)`, sp 0x0093C568);
  the low word 0x0093C620 is already masked. func_00208900's message has one `%s`, so a3 is never
  read. Raw, the other differing words are the masked ones: 0x00760A10 (SIF `pending`),
  0x0093C3F0 (module C stack), 0x0093C620, and the t0/t1 slots 0x0093C628-0x0093C637
  (0x70 / 0x088AEE38 against 0x3263666D7100313B / 0x0101010101010101, OP2's two values).
- **cpu**: the `rest` hash (everything but the GPRs: FPU, VU0 registers, COP0) is equal in every
  row. The GPRs differ only in a0's upper lanes (2-3) in all 1062 rows: "I.PTR;1\0" against
  ".ni $2, " (lane 0-1 equal), and in t1/t2 in 16 rows: ";1\0qmfc2" and 0x0101... against 0 and
  1.0f. These are bytes that strcpy (func_00254544: ld/lq once aligned, the 0x8080.../0x0101...
  constants) read past the NUL of the `";1"` literal that func_00208900 appends to the sound bank
  path. The GPRs are the recompiled code's register file; nothing reads them before they are
  rewritten (RAM, VU and GS stay identical, and so does everything after field 1452).
- **Cause**: `";1"` is a plain C literal, and the linker tail-merges it into
  `"cdrom0:/MOD/LIBSD.IRX;1"`, followed in .rodata by the template `"qmfc2.ni $2, $vf31..."`.
  In 315462880's core the literal is at byte 6508016 of the module, in fbf0dd6f4's at 6508129
  (+113: SP4's `graphics_ordered` and its `"DHD_GRAPHICS_ORDERED"` literal). The new alignment
  sends strcpy down its word path, which reads the next .rodata bytes. The same literal caused
  OP2's four masked words and SP1's failed template removal.

No game state differs: positions, timers, sound, the GS stream and all RAM but this dead spill are
equal. It is a trace artefact of the module's data layout, not a behaviour change.

### Fix: remove the dependence, not mask it

- `port/patches.py`: func_00208900's two path literals (`"\SND\\"`, `";1"`) become
  `static const char [16] __attribute__((aligned(16)))` arrays, as 0021F400.c already declares
  its module paths. Whatever strcpy and strcat read past the NUL is now zero, in every layout.
  The strings and the path are the same, so the game is too (a port-side patch; src/ unchanged).
- With it, the t0/t1 slots hold 0x000000000000313B / 0x0101010101010101 and a3's upper word
  0x80808080 in every build, so `state_trace_mask.txt` drops 0x0093C628, 0x0093C62C, 0x0093C630
  and 0x0093C634 (7 words -> 3). Nothing new is masked; 0x0093C624 needs no entry. The three
  left (0x0093C3F0, 0x0093C620, 0x00760A10) are host addresses or computed from one, as listed.

### Checks

- x64, 5000 fields, 80b851d90 + the patch against main 6a59044a0 + the patch: **all 13 columns
  identical**, with the old 7-word mask and again with the new 3-word mask: PASS. The two cores'
  layouts differ (SP4, SP9-SP12, SP7, SP14 and AND2 lie between them).
- Node (threaded browser core, `run_headless.mjs`, 5000 fields, new mask), the same two patched
  builds: all 13 columns identical, PASS.
- After merging main `962b6fc24` (SP13 in), x64 (dhd.exe 09c88c5e...) against 80b851d90 + the
  patch, new mask: all 13 columns identical, PASS.
- Raw dumps of the two patched builds (fields 25, 150, 1000): only 0x00760A10, 0x0093C3F0 and
  0x0093C620 differ; the GPR logs are identical in every row.
- The patch against the unpatched build of the same commit changes only the leftovers above
  (a0's upper lanes now 0, the spill words), so a base for a trace compare must have it.

### Risks and next

- Other plain game literals read by the word-wise string routines would show the same kind of
  difference after a layout change; the race trace shows none left. If one appears, the same
  patch (an aligned, zero-padded array) is the fix, not a mask entry.
- 0x0093C3F0, 0x0093C620 and 0x00760A10 stay masked: host addresses, or computed from one.

## SP15: recompiler codegen for the hottest functions

Branch `rr/SP15` from main `9ead4173a`; main `c4a2161be`, `6a59044a0` (SP14) and `962b6fc24` (SP13)
merged in. Goal: the EE thread reliably under 4.17 ms a field, through what `port/tools/recomp.py`
(and then wasm2c) makes of the hottest recompiled game functions (0021B8A8, 00213C08, 002628A0: ~12%
of EE samples after SP11).

### What the generated code did

- Every guest register lived only in `ps2` (linear memory). Each read was a load and each write a
  store, and every guest store (`sw`/`sd`/`sq` were out-of-line calls to `ps2_store32/64/128`;
  `sb`/`sh`/`swc1` stores through a pointer) made the compiler assume `ps2` may have changed, so
  the registers were loaded again after it. In 002628A0 (a Hermite curve, 170 instructions) the
  wasm2c C reloaded `$sp` after each of its three `sd`s and stored every intermediate FPU value.
- `ps2_store32/64/128` themselves were 3.9% of the EE thread's busy samples (base profile below).
- `mul.s`, `add.s`, `sub.s` (and `mula.s`/`madda.s`/...) computed in double: two promotes, the
  double operation, two double compares, a demote. `div.s`, `sqrt.s`, `cvt.w.s` were out-of-line
  calls (`ps2_fdiv`/`ps2_fsqrt`/`ps2_cvt_w_s`) to the same inline ps2float.h forms.
- Loads tested `INTC_STAT` and then the fence range separately (`ps2_load_address`).

### Changes (wasm build only: `recomp.WASM_EMIT`; the i686 output is unchanged)

1. **Guest registers in locals, store-through** (`recomp.py` `cache_registers`, applied to each
   `rc_` body in `write()`). The GPRs (low 64 bits), FPRs and the FPU accumulator a body names get
   C locals, loaded at entry. A write sets the local and `ps2` (`ps2.gpr[5].d[0] = rg5 = ...`), a
   read takes the local. The locals are read back from `ps2`:
   - after every call (`rc_`, native, `jalr`): `RC_RELOAD_ALL`;
   - after a PS2X instruction, for the GPRs its operands name (`reload_named`; an FPU operand
     would reload everything, none has one);
   - after a line that writes `ps2.gpr[n]` itself (`lq`, `por`, `qmfc2`, `cfc2`, a quadword result).
   So `ps2` always holds every register, exactly as before. Nothing else needs a sync: the slow
   memory paths that can run game code (an `INTC_STAT` read, an MMIO store: interrupts, fiber
   switches) save and restore the registers around it (kernel.c `run_handlers`, `deliver`,
   `reschedule`), the state trace reads `ps2` there, and VU0 (vcallms) never touches EE registers.
   Unused loads and back-to-back stores fold away in clang. The upper GPR halves, LO/HI and the VU
   registers stay in `ps2`.
2. **Inline guest stores** (`recomp.h` `rc_store32/64/128`): outside `[0x10000000, 0x13000000)`
   `ps2_store*` only store, so the store is inline there; inside, the original function runs.
   Loads test one range (`rc_load_address`: `INTC_STAT` and every fence lie in
   `[0x10000000, 0x12001100)`). Since the SP14 merge both inline paths (load and store) take
   SP14's `ps2_ram_canonical` on their fast side, so the low view and the `0x38000000` mirror fold
   to the canonical RAM address there as they do in `ps2_load_address`/`ps2_store*`.
3. **Single-precision EE MUL/ADD/SUB** (`recomp.h` `rc_float_mul`, `rc_ee_add`, `rc_ee_sub`):
   the same bits as ps2float.h's double forms on the clamped operands, in every rounding mode,
   with or without FTZ/DAZ (the double product of two floats is exact; after `ps2_ee_align` the
   double sum is exact; overflow becomes the largest magnitude through `ps2_float_input` as through
   `ps2_float_result`; denormal results flush either way). DIV, SQRT, CVT.W call the inline
   `ps2_ee_div`, `ps2_ee_sqrt`, `ps2_float_to_int` directly. The browser now runs one float helper
   per operation instead of promote, double op and demote.
4. **mtc1 is opaque** (`rc_opaque32`, an empty asm): with registers in locals, more constants reach
   the FPU ops, and clang would fold an operation on two constants in round-to-nearest while the
   EE rounds toward zero.
5. **The VU0 FP word** (`ps2asm.c` `vcallms`, `vu0_interp.c` `ps2_vu0_run`): the EE's mode
   (fp_mode.c `HOST_FP_EE`) is the VU0 word but for the sticky exception flags, which nothing reads.
   Under it the word is left alone: four host calls per recompiled vcallms become two reads
   (`host_fp_csr`/`set_csr` 1.40% -> 0.84% of EE busy). Any other mode is set and restored as
   before.

The template literals, the 526 generated sequences and the function table are untouched: table
37,152 entries identical by name, data section byte-identical against the base (before the main
merge). The core wasm is 8% smaller (7.84 -> 7.21 MB): the store-through code is shorter than the
reload/store traffic it replaces.

### Exactness

`state_trace_run.py race` (5000 fields, seed 81257), unchanged mask (7 words, both sides built
from the same mask and patches), `trace_columns.py`: **all 13 columns identical, PASS**, five
times:
- base main `9ead4173a` against changes 1-4 (`01d523dca`);
- the same base against changes 1-5 (`4140e5d5c`);
- main `c4a2161be` against the merged branch (`702893231`);
- main `6a59044a0` (SP14) against the merged branch with the canonical fold (`61a133f8f`);
- main `962b6fc24` (SP13) against the merged branch (`1ed867dbf`); table identical by name
  (37,152 entries), data section identical (275,074 bytes).
(Main moved on to `dbd6ab944`, SP17's padded literals and 3-word mask, after this trace; per the
(The merged branch against the old base differs in ram/cpu from field 25: main's own merges moved
data; main against the merged branch is identical.)

### Measured

Sampled profile (release exe, link-map names, `sample_profile_raw.py` 30 s from 5 s after GO, same
race recipe; % of the EE thread's busy samples, waits excluded):

| | base `9ead4173a` | SP15 `4140e5d5c` |
|---|---:|---:|
| all recompiled `rc_` functions + `ps2_store*` | 23.8 + 3.9 = 27.7 | 22.6 + 0.2 = 22.8 |
| 0021B8A8 | 7.24 | 7.46 |
| 00213C08 | 4.78 | 4.29 |
| 002628A0 | 3.20 | 2.46 |
| 00213658 | 1.88 | 1.57 |
| `host_fp_csr`/`host_fp_set_csr` | 1.40 | 0.84 |

So about 5.5% of the EE's busy time goes, roughly 0.2 ms a field at 4 ms.

0021B8A8 did not move: 43% of its samples sit on one instruction, the `ld $t0, 0($s5)` that reads
the next command of the list it walks (`.L0021B9A4`). 273 of those 335 samples are after the
out-of-line `host64_memory_address_slow` call: `$s5` points through an uncached RAM alias, and the
load waits on memory. That is the slow memory path (SP14's lane), not codegen.

Timing, hidden desktop under game.lock, VU1R/SP1 recipe (DHD_FPS=144, scale 2, DHD_BENCH=7400-14600,
seed 81257), alternating; EE busy ms/field (EE CPU ms/field, fields/s):

| pair | base | SP15 |
|---|---|---|
| s1 run 1 (base `9ead4173a`, SP15 `4140e5d5c`) | 4.11 (5.25, 177/s) | 3.99 (5.36, 171/s) |
| s1 run 2 | 5.59 (6.14, 108/s)* | 4.07 (5.48, 165/s) |
| s2 run 1 (base main `c4a2161be`, SP15 merged `702893231`) | 4.65 (5.23, 160/s) | 4.18 (4.92, 148/s) |
| s2 run 2 | 5.12 (5.46, 139/s) | 4.57 (5.14, 148/s) |
| s3 (base main `962b6fc24`, SP15 merged `1ed867dbf`) | 3.20 (4.21, 197/s) | 2.96 (4.09, 194/s) |

\* contention outlier (p99 74 ms).

s3, on top of SP13 and SP14, one pair on a quieter PC: EE busy -0.25 ms a field (-7.7%), EE CPU
-0.12 ms. p50 4.98 / 4.99 ms, p99 9.1 / 10.7 ms. The VU (3.71 / 3.77) and GS (3.53 / 3.82) threads
now set the pace, so fields/s does not move.

- In all four pairs the EE is busier on the base: -0.12, -1.5*, -0.47, -0.55 ms; without the
  outlier about -0.4 ms a field (~9%), EE CPU -0.1 to -0.3 ms. The profile's 5.5% (~0.2 ms) is the
  more careful estimate; the PC was loaded by other lanes in both sessions.
- fields/s follow the VU and GS threads, which moved by up to 20% between runs on their own.
- **240 fps** (DHD_FPS=240, DHD_BENCH=12500-24500): s1 base 4.01 ms EE busy, 182 fields/s, p50
  5.09, p99 10.6 ms; SP15 4.08, 172/s, p50 5.47, p99 11.5. s2 (merged) base 3.60, 196/s, p50 4.75,
  p99 10.6; SP15 4.28, 154/s, p50 5.90, p99 17.0, with its VU and GS threads 0.8-0.9 ms busier as
  well (load from other lanes during that run). At 240 the EE no longer sets the pace: all three
  threads sit at 3.6-4.5 ms, and these single runs cannot resolve 0.2 ms.
- Before SP13 the EE was at or under 4.17 ms only in some runs on this shared PC. With SP13 and
  SP14 merged (s3) it is at 2.96 ms, well under, and the VU/GS threads are the limit. The
  remaining big EE items are outside recompiled codegen (below).

### Not done / tried

- Write-back registers (no store-through): the stores would move into every guest access's cold
  path (an MMIO store or `INTC_STAT` read must see all registers in `ps2`), about 14,000 sites times
  ~20 registers of cold code. The profile says the stores are not where the time is.
- present.c `capture()` getenvs: SP14 did it (`0f67cff3d`), left out here.
- Browser: built (`--renderer rust`) and `check_web_layout.py --require-renderer` OK (core,
  graphics, renderer, vu), last on the merged `1ed867dbf`; not traced or timed.
  Its float helpers implement every IEEE operation exactly under the EE word, so the
  single-precision forms keep their bits there too.

### Risks

- The no-sync argument for the cold paths rests on the HLE kernel saving and restoring the EE
  registers around all game code it runs from an INTC_STAT read or an MMIO store. A new HLE path
  that runs game code there without `save_registers`/`load_registers` would see current `ps2`
  (store-through) but the interrupted body would keep its locals: still the right values, as the
  registers are restored, unless that new path means to change the interrupted code's registers.
- `ps2.fpr[...]` written directly in a body is refused by the recompiler (`Fail`), as is a write to
  `$zero` through `GPR(0) =`; nothing emits either today.

### Next

- The alias load in 0021B8A8 (and `host64_memory_address_slow` at ~5% of EE busy overall):
  SP14's `ps2_ram_canonical` now folds the low view and the `0x38000000` mirror inline (also on
  SP15's inline paths); a fresh profile of the merged main should say whether the `0x2xxxxxxx`
  uncached window this load goes through is still on the slow path, and fold it the same way.
- The VU0 macro ops in the hot functions (vmula/vmadd/vopmsub triples in 00213C08, 00213658) run as
  PS2X calls to their generated code; their cost is ps2_vu_madd (SP14).

## FPS1: animations at high frame rates

Branch `rr/FPS1` from main (`c98563e73`). The high-rate modes (finding 090 SP5b) run the game update
at the selected rate and move the 60 Hz field clock 60/rate per update: at 240 there are four real
updates per field. Code that advances a phase, angle, scale or age by one **per update** while the
retail game advanced it once per **field** therefore runs four times too fast at 240. SP5b converted
the timers that the decompiled C reads through `port_step()`/`port_whole_step()`; the remaining
counters are plain increments in ported C or in recompiled code, and reading every one of them by
hand is not reliable. This section measures them instead, fixes the two that are real, and proves
the fixes at 60 and 240.

### The rate audit

`port/hle/rate_audit.c` (off unless `DHD_RATE_AUDIT=<file>`; it reads its env once on the first tick
and only checks a flag after) runs at the top of the game frame tick `func_00238C00`, the update
boundary, where the field clock advances. Each update the whole 32 MB of EE RAM is compared with the
previous update, two words per load, and every word accumulates

- `travel`: the sum of |change| over the sampled updates,
- `net`: the signed sum of the changes,
- `changes`: the number of sampled updates at which it changed.

A change counts as a float when both values look like floats and as an integer otherwise (small
integers and addresses are denormal or absurd floats). The window is gated to a game scene (the menu
id at `0x0829DED8` and the race phase at `0x0843F348`), so it starts when the title or the race is
up, and the header records the field range, the menu/phase at both ends and how often the gates
stopped holding inside the window (a flapping menu id cannot silently shorten a run).

`port/tools/rate_audit.py` runs the pair with the state trace's deterministic headless recipe
(virtual clock, no movies/sound/memcard, native renderer, no settings):

python port/tools/rate_audit.py run boot  --rate 60  --name title-60  --window title python
port/tools/rate_audit.py run boot  --rate 240 --name title-240 --window title python
port/tools/rate_audit.py compare title-60 title-240

The windows are:

- `race` (menu 9, phase 2-4; a running race): skip 10 and 600 samples at 60, scaled by rate/60 for
  the high run (40 and 2400 at 240), so both runs cover the same 600 fields.
- `title` (menu 15, the boot title): skip 30 and 120 samples at **both** rates, because the title's
  own timeline is update-driven; both runs then stand at the same scene phase. The reference ratio
  is the same either way (at 240 a field lasts four updates).

`compare` ranks by **net travel per update relative to the 60 run**: 1.000 is a per-update
accumulator (the same net travel per update at any rate), 0.250 at 240 is correct time-based motion.
Travel per update is shown beside it, and a word whose travel exceeds its net is an oscillator (a
field-parity word flipping `0x7900`/`0x7908` travels but has no net). The "changed" filter applies
to the 60 Hz run only: a word fixed to game time changes in only a quarter of the 240 updates, and
dropping those (as the first version did) hid exactly the words the tool is used to verify. A word
the high run no longer reports moved by less than its minimum travel and is time-based too.

The scan is a full-memory compare per update, so it is for audits only; the game a player runs never
turns it on.

### Attributing a word to its writer

The detector cannot tell source state from scratch: the snapshot at the frame-tick top also sees
transient buffers that the VU1/VIF path writes each field, and its addresses are EE words (the form
`port/tools/state_trace_mask.txt` uses). Attribution used `DHD_WATCH`, which arms hardware
watchpoints on the game thread and now also prints the hit's RVA against `__ImageBase` (`rva
0012AB34 (map)`), looked up in `build/host64-exe/dhd64.map` (nearest symbol; generated functions
repeat, so the RVA was always checked against the function's code). VU/DMA-thread stores produce no
hit, which is itself the diagnosis for a buffer the game thread never writes during the window.

What the audit's top ranks turned out to be, besides the two fixes:

- `0x0029E160`, the animation clock: it changes unit with the mode (1 per update at 60 in fields,
  256 per update at 240 in 1/1024 field), and every consumer converts with
  `port_animation_fields()`. The `0x015D..`, `0x0162..`, `0x017D..`, `0x00A1..` and `0x002E..`
  timestamps beside it are object copies in the same 1/1024 units.
- `0x0029E218`, `0x0029DA54`: monotonic stamps (LRU/object ids), new values, not time.
- `0x004C8944`: the SIF RPC counter.
- parity words at `0x08279468`, `0x0829E284`, `0x084CCB00`, `0x084CCF40`, `0x0832A174`,
  `0x08660244`: two values alternating per field.
- `0x00C50D14`, `0x00B996B0`, `0x00B751D0`: oscillating floats whose sign flips (travel without
  net).
- the `0x0062....` block in the title window: VU1 code/data scratch written by the VU thread
  (the game-thread watch saw only scene-load stores); `0x006D....` in a race is the same kind of
  packet memory (its value read as an int is not state).
- the small-travel float words left in the race list (per-update travel below 0.1 against values
  around 10^3, e.g. `0x002B31A4`, `0x007104F4`, `0x0076....`, `0x0078....`): the two rate modes
  place them at slightly different values (the sampled trajectories differ a little between
  rates, finding 067's equivalence limits), so their travel ratio says nothing; a per-update
  phase would sit at a clean 4.000, these land between 1.4 and 2.3.
- `D_0029E10C`, the render/double-buffer frame counter: must stay per update (one picture per
  update is what the rate selects), so it is not "fixed".

### The two fixes

Both sit in `port/patches_fps.py`, in the style of the SP5b fixes, and both keep the retail
arithmetic exactly at 60 (`port_whole_step()` is 0 or 1, and `port_fps_high()` is false):

1. **`func_00228BE8`: `D_0029DBAC`, the particle/effects frame clock**:
   `D_0029DBAC += port_fps_high() ? port_whole_step() : 1;`
   Everything that reads it treats it as a 60 Hz frame count: the dust, snow and spray waves
   (`001DF920` `Wave`, the scroll offsets), the glow phases (`0022E9A0`), the effect matrix
   wobble (`0022E3D0`), the spawn stamps (`00162690`) and the parity reads in `00143538`/
   `0022F040`. At 240 it advanced four times per field, so those animations ran 4x too fast.
2. **`func_001BBF30`: `obj->x60C`, the sound/skat queue owner's age**:
   `obj->x60C += port_fps_high() ? port_whole_step() : 1;`
   `func_001BBF30` is called four times per update, and `func_001BC250` returns `x60C*1000/60`
   milliseconds. Counting per update at 240 aged the owner four times too fast (16 increments per
   field instead of 4), so the ms it reports drifted.

Measured with the audit (`compare`; net travel per update, "x60" is the after value divided by the
60 run's, so 0.25 is correct time-based motion; before is the build at `c98563e73`, after
`9f0a2123e`):

| word | writer | scenario | 60 | 240 before | 240 after | x60 after |
| --- | --- | --- | --- | --- | --- | --- |
| `0x0029DBAC` | `00228BE8` | race | 1.000 | 1.000 | **0.250** | 0.250 |
| `0x0029DBAC` | `00228BE8` | title | 1.000 | 1.000 | **0.250** | 0.250 |
| `0x0039E5A0` | `001BBF30` | race | 4.003 | 4.004 | **1.002** | 0.250 |
| `0x0039E5A0` | `001BBF30` | title | 4.000 | 4.000 | **1.000** | 0.250 |

`0x0039E5A0` is four increments per update at 60 by design (the four calls), so correct behaviour at
240 is 1.00 per update = 4.00 per field, a quarter of 60's per-update travel, which is what it now
measures. Raw rows behind that: race `0x0029DBAC` travel 2,400 over 2,400 updates at 240 before and
600 after; race `0x0039E5A0` 9,610 before and 2,404 after. After the fix the race values are in the
60 run's range (`0x0029DBAC` 2,370 against 2,371; `0x0039E5A0` 13,107 against 13,757, where before
it was 52,457, four times 60).

### Verification

- State trace on the new build against the base build at `c98563e73`, same recipe and the same
  pinned seed: boot 600 fields and race 5000 fields, **all rows match in every column** (600/600
  and 5000/5000), `port/tools/state_trace_mask.txt` unchanged at its 3 entries. Re-run on the final
  merged build (below) with the same result. The trace notes that the exe hash differs (the fix
  changes code), the section values are what is compared.
- Merged main through `6ad0ffe41` (browser default settings only): the core's wasm is
  byte-identical (`sha256 3a4c5415...`), so this is the same code that was measured.
- x64 release packaging from the merged build (`package_release.py --wasm build/wasm/dhd_core.wasm
  --jobs 2`): `build/release/DownhillRevived-0.4.0-win.zip` (sha256 `6645c95c...`), dhd.exe x64 on
  the Rust renderer, all imports Windows DLLs, source `57e3ef83f`.
- Browser: `port/web/build.py build/wasm/dhd_core.wasm --renderer rust` (threads, 137 imports
  matching `host_api.h`) and `check_web_layout.py --require-renderer build/web`: **web layout: OK
  (core, graphics, renderer, vu)**.

To reproduce: the four `rate_audit.py run`/`compare` commands above (`build/rate-audit/t4-*`,
`t5-*`, `r5-*`, `r6-*` are the runs used here), then `python port/tools/state_trace_run.py boot
--name x` / `race --name y` and `python port/tools/state_diff.py base-boot.tsv x.tsv` against traces
of the base build.

### Left open

- The audit ranks, it does not size: a fixed word is only proven time-based, not that its
  animation looks right on screen. The two here are particle scroll/glow phases and a sound queue
  age; a visual check of dust, snow, spray and glow at 240 is worth doing once (no screenshot
  comparison exists for them).
- `DHD_RATE_AUDIT` runs on the whole 32 MB every update, so it is not usable together with the
  timing benches; no cost was measured for it and none is intended in release builds (it is off).
- The `0x0062....` scratch shows up because the snapshot is taken at the tick top; a variant that
  scans only words the game thread writes during the update (watchpoint-driven) would remove that
  noise, at the cost of a much slower run.

## ZC1: port-only C for hot translated functions (tried, dropped)

2026-10-27. A port-only switch compiled the `#ifdef NONMATCHING` C of func_00213C08 and
func_00213658 (collision) and new C for func_002628A0 (NGA animation curve) instead of their
recomp.py translations. Dropped, branch deleted:
- Slower: one ABBA session (hidden desktop, scale 2, uncapped, race fields 7400-14600, shared
  PC): game thread busy 2.40 / 2.48 ms a field on main against 2.65 / 2.60 with the C, 233.7 /
  229.7 against 224.3 / 225.9 fields/s. The SP13-SP15 work had already made these translations
  cheap, and the game thread is no longer the limit (VU 3.3 ms, GS 3.0 ms busy).
- Not checkable with the state trace: the translations keep their stack frames and scratch
  registers in guest memory, C keeps them in locals, so `ram`/`cpu` differ in dead stack on every
  field. The collision pair also changed real state: func_00213658 reads a hit normal's w lane it
  never writes (stack leftovers: -25.0 on main, 0.0 with the C, fields 3332-3398).
- The C stayed non-matching (none of it is byte-exact), so it was not kept as decompilation.
- Its C for 00213658 declared func_00213250 with the two floats in the middle; on the EE ABI
  floats travel in $f12/$f13 whatever their position, so any port-side C of these functions must
  declare them in the callers' order.

## GP1: GS thread profile and upload counters

2026-10-27, branch `rr/GP1`, based on main `766435a80`. Measurement only: no code change is
committed and no speedup is claimed. The point is to size three options for the GS path: (a) moving
the whole GS onto the GPU, (b) a GPU-side upload cache, (c) moving uploads ahead of draw batches.

### Method

The core wasm was rebuilt from main (`build_wasm.py -j 2`, 294 s, objcache 10,737 hits / 0 misses);
`build_host64.py --debug` (PDB for the sampler, codegen unchanged) produced
`build/host64-exe/dhd64.exe`. Every run reached GO at field 7139.

One clean profile (`prof1`, 24 s) and one counter run (`cnt2`, 12 s) are reported. The counter build
is the same code plus the env-gated scratch patch `gp1_counters.patch` (`DHD_GP1_COUNTERS`,
`DHD_GP1_UPLOADS`; off by default), which only reads counters and writes CSV. A rebuild of the
counter binary reproduced `cnt2` exactly on all 10,887 common field rows and 78,648 common upload
rows (`cnt3`), and `cnt1`/`cnt2` agree on every shared counter, so the counters are deterministic.
The profile is one sample on a shared PC: treat the ms/field values as ±5-10%, not a confidence
interval (the two runs' GS CPU is 4.729 vs 4.510 ms/field).

Sample-to-ms conversion: `ms/field = self samples / all thread samples × thread CPU ms/field`
(12,821 samples, 4.729 ms/field); `% work` uses the 9,249 non-wait samples. Wait samples are
reported separately. The counter window is fields 8200-8700 (501 fields): after GO, in the steady
race, where both upload sets are present.

### GS thread

`prof1` (fields 7900-10900): 164.323 fields/s, p50 5.739 ms, p99 11.525 ms; GS thread CPU 4.729
ms/field, GS busy 4.406 ms/field (timeline: wall − gs_idle). 12,821 samples, 3,572 of them in waits
(27.9%, 1.318 ms/field), 9,249 work samples = 3.411 ms/field.

| Bucket | Samples | % work | ms/field | Contents |
| --- | ---: | ---: | ---: | --- |
| gif | 2,761 | 29.9% | 1.018 | `gs_write`, `packed`, `gif_qword`, `gs_feed`: GIF packet parse and register decode |
| prim | 2,764 | 29.9% | 1.019 | `native_record`, `vertex_kick`, `native_draw`, `native_index_of`: primitive recording, vertex conversion, batching |
| localmem | 1,853 | 20.0% | 0.683 | `native_exact`, `pixel_address`, `write_pixel`, `gs_write_pixels`, `gs_read_pixel`, `image_data`: VRAM access (uploads, CLUT and texture reads, local copies) |
| texkey | 755 | 8.2% | 0.278 | `load_clut`, `gs_palette`, `native_mix`, `native_page_hash`, `gs_mark_pages`: texture key and CLUT |
| gpudrv | 470 | 5.1% | 0.173 | `nvwgf2umx.dll`, `D3D12Core.dll`: driver calls made on the GS thread |
| libc | 234 | 2.5% | 0.086 | `memmove`, `memset`, `memcpy` |
| state | 102 | 1.1% | 0.038 | `native_texture`, `native_state`, `native_command` |
| decode | 99 | 1.1% | 0.037 | `gs_color`, `native_decode` |
| rust | 93 | 1.0% | 0.034 | wgpu/allocator |
| other | 82 | 0.9% | 0.030 | — |
| upload | 36 | 0.4% | 0.013 | `native_import_page` |
| waits | 3,572 | — | 1.318 | handoff to the render thread (not work) |

Top functions by self samples:

| Function | Samples | ms/field | Bucket |
| --- | ---: | ---: | --- |
| `gs_write` | 1,048 | 0.387 | gif |
| `native_record` | 1,040 | 0.384 | prim |
| `vertex_kick` | 1,024 | 0.378 | prim |
| `packed` | 748 | 0.276 | gif |
| `gif_qword` | 567 | 0.209 | gif |
| `native_exact` | 530 | 0.195 | localmem |
| `pixel_address` | 492 | 0.181 | localmem |
| `nvwgf2umx.dll` | 370 | 0.136 | gpudrv |
| `load_clut` | 355 | 0.131 | texkey |
| `gs_feed` | 338 | 0.125 | gif |
| `native_index_of` | 210 | 0.077 | prim |
| `memmove` | 193 | 0.071 | libc |
| `native_draw` | 185 | 0.068 | prim |
| `write_pixel` | 182 | 0.067 | localmem |
| `gs_write_pixels` | 157 | 0.058 | localmem |
| `gs_palette` | 153 | 0.056 | texkey |

The front end splits roughly in thirds: GIF decode 1.0 ms, primitive recording/batching 1.0 ms,
local memory 0.7 ms; texture keys and the CLUT add 0.28 ms.

### Upload counters

`cnt2`, fields 8200-8700 (501 fields; 179.271 fields/s, GS 4.510 ms/field, native 1.395 ms/field, EE
4.432, VU 4.114; timeline GS busy 4.127).

| Metric | Per field |
| --- | ---: |
| host→local uploads (`GS_TRXDIR=0`, `image_data`) | 30.76 |
| upload bytes | 964,935 B |
| — 128x128 psm 0 (65,536 B each) | 12.43, 814,818 B |
| — 64x32 psm 0 (8,192 B each) | 18.32, 150,089 B |
| content already uploaded in the previous field (size + FNV-1a hash) | 30.75 (100.0%), 964,759 B (100.0%) |
| same destination *and* same content as the previous field | 4.92 (16.0%), 40,322 B (4.2%) |
| distinct contents in the window | 28 (13 × 128x128, 11 × 64x32, 4 one-off tiny) |
| distinct destinations in the window | 31 |
| destination/content rotation period | 39 fields |
| uploads before the field's first draw | 0 |
| batch breaks caused by an upload | 28.81 |

The uploads are the same 24 recurring images every field (28 distinct contents in the window,
including four one-offs), written to a rotating set of 31 VRAM addresses with a 39-field period.
That is why a content-only hash matches 100% while an (address, content) cache over the previous
field matches only 16% of uploads / 4.2% of bytes.

Batches and breaks:

| Metric | Per field |
| --- | ---: |
| batches | 468.70 |
| break: first draw of the field | 2.04 |
| break: upload (batch closed by an upload) | 28.81 |
| break: draw state change (non-CLUT) | 410.68 |
| break: CLUT only | 0.00 |
| break: texture context | 0.00 |
| break: primitive type | 27.18 |
| commands (1 + passes per batch) | 483.39 |
| primitives recorded | 38,611.8 |
| primitives per batch | 82.4 |
| passes per batch | 1.03 |

The CLUT split uses the strict test "every `state_version` bump since the batch opened is a CLUT
entry change". None of the 410.7 state breaks passes it: every batch that spans a CLUT change also
spans a non-CLUT state change. CLUT loads are therefore a consequence of texture state changes, not
an independent batch-break source. For the record: 616.2 CLUT loads per field (272.1 16-entry, 344.1
256-entry) and 108,420 changed entries per field (176 per load).

GPU bytes per field:

| Stream | Bytes | Share |
| --- | ---: | ---: |
| vertices (`NativeVertex`, 32 B × 58,476) | 1,871,233 | 72.4% |
| indices (4 B × 121,787) | 487,150 | 18.9% |
| other (draw constants 123,748 B + imports/alpha ~98,305 B) | 222,053 | 8.6% |
| decoded textures | 2,665 | 0.1% |
| total | 2,583,100 | 100% |

2.58 MB/field at 179.3 fields/s is 463 MB/s. Textures are essentially all cache hits in the race
(0.07 decodes/field); the uploaded pages are almost never re-decoded as textures in this window (2.7
KB/field) — the remaining GPU-side traffic they cause is page imports, about 1.5 × 64 KiB per field.
Upload traffic is 965 KB per field, 37% on top of the 2.58 MB of GPU buffer traffic, but it is
CPU→VRAM, not a GPU upload.

### Implications

**(a) Whole GS on the GPU.** Measured GS work is 3.411 ms/field (4.406 ms busy including waits), 79%
of the 5.58 ms frame at 179.3 fields/s. The buckets a GPU GS could absorb are prim 1.019, localmem
0.683, texkey 0.278, gpudrv 0.173 and the small state/decode/upload buckets, about 2.2-2.4 ms/field;
the GIF front end (1.018 ms) still has to parse the game's packets unless the GPU consumes GIF
directly, and the render thread would grow by whatever the GPU rasterizer costs (not measured here).
Rough saving: 2.2-3.4 ms/field, 40-60% of the frame, with the full 4.4-4.7 ms GS CPU as the ceiling
if the handoff waits go too. This is the only option with a multi-millisecond prize, but it is a
rewrite of rasterization, blending, CLUT and VRAM behaviour.

**(b) GPU-side upload cache.** 100% of the 965 KB/field repeats the previous field's content, but
only 4.2% repeats at the same address; destinations rotate with a 39-field period. A cache keyed by
(address, content) over one field is worth almost nothing (40 KB/field). To capture the bytes it
must be content-addressed (24 recurring images, about 1 MB) or keep 39 fields of destination history
(about 37 MB). The measurable CPU saving is the upload write path: `image_data` 0.043 +
`gs_write_pixels` 0.058 + `write_pixel` 0.067 + the upload share of `pixel_address` (241k of ~375k
calls) 0.116 ≈ 0.29 ms/field (6% of GS CPU), plus 28.8 fewer batch breaks ≈ 0.09 ms/field of
render-thread CPU (native thread 1.395 ms/field over 468.7 batches = 2.98 µs/batch), so about
0.35-0.4 ms/field (6-7% of the frame). Correctness is the hard part: the uploaded pages are read as
render-target initial content (imports) and could be read back, so a cache that skips the VRAM write
must serve those readers too, not only texture decode.

**(c) Uploads ahead of draw batches.** All uploads happen after at least 7 batches (of 468.7), i.e.
they are interleaved with draws; each closes the open batch. Moving them to the top of the field
removes the 28.8 upload breaks (6.1% of batches) but not the 965 KB/field of writes: about 0.086
ms/field on the render thread + 0.003 ms/field on the GS thread, roughly 0.09 ms/field (1.6% of the
frame). It is cheap, but it needs a hazard check: an upload that overwrites a render target after it
was drawn would change what the draws see if moved earlier. The counters show the uploads are not at
the field start, so that check is required per upload, not optional.

Verdict: (b) is the best return for a contained change, but only in its content-addressed form; (c)
is a small add-on worth about 0.09 ms/field if the hazards check out; (a) is the only
multi-millisecond option and should be its own project. These are counter-derived estimates, not
measured speedups.

## GS2: emulate only the GS local memory that is read

2026-10-27, branch `rr/GS2` (code `45ddb5ed4`, merged with main `067fb5061` as `353c261cd`). GP1 put
about 20% of the GS thread in emulated local memory. This lane first listed who can read it in
native mode, then cut what no reader needs and made the rest cheaper. All bytes, counters and calls
the readers see stay the same.

### Who reads the emulated local memory (native mode)

| Reader | What it reads |
| --- | --- |
| `native_page_hash` (texture keys, `native_texture`) | every page of every texture a draw uses: whole 8 KB pages, swizzled, each time `page_writes` changed. The streamed uploads (GP1: 30 a field, 965 KB) are all read here every field. |
| `native_decode`, `native_convert_texture` | texels of textures missing from the cache (GP1: 0.07 decodes a field) |
| `native_import_page` | a colour page as a surface's starting content (about 1.5 a field) |
| `load_clut` | 64 B or 1 KB at CBP per CLUT load (GP1: 616 a field) |
| `gs_read_image` / `gs_defer_read_image` | readbacks: the lens-flare Z probe (GPU-written pages from the exact device, the rest from the CPU image), colour reads |
| `local_copy` fallbacks (`native_local_copy`) | the source rectangle |
| exact Z device (`gpu_exact.inc` `upload_pages`) | before each exact draw, every CPU-written page not yet uploaded, whether the draw touches it or not |
| state trace (`host_service_gs_snapshot`) | all 4 MB (`gs_vram` column), plus `pixels_written`, `state_version` and the GIF path words (`gs_regs`) |
| capture, `gs_display`, texture packs (`texpack.c`) | captures, the CPU scanout, the pack hash |

Because the texture keys and the state trace hash the uploaded pages, the CPU copy has to stay
complete in every field. Skipping or deferring the CPU writes themselves would make every key lookup
wait on them, so no upload is dropped. Only one reader needs less: the exact Z device, which draws
only the probe's Z-writing primitives (watch rectangle, band, exact targets).

### Changes

- **Exact device uploads only the pages it touches** (`gpu_exact.inc`, `upload_lazily`; set by
  `native_install`, so exact mode is unchanged). Before, the first exact draw of a field
  uploaded every CPU-written page (about 120 pages, 965 KB, through the queue) and marked them all
  texture-dirty. Now `draw` uploads only the pending pages it writes or reads (frame and Z inside
  its clipped box, including Z under ZMSK, and its texture's pages, mip levels included). The
  others wait in `g->host_dirty`. Invariant: the GPU never draws into a page that has CPU content
  waiting, so `cpu_dirty` and `host_dirty` never overlap. The readbacks (`sync_range`,
  `stage_pages`, `sync_vram`) read only `cpu_dirty` pages and so never need a waiting one.
  `uploaded` follows `pixels_written` only once nothing is left waiting. `readback_all`,
  `local_copy` and `exact_display` still upload everything.
- **PSMCT32 uploads written a row run at a time** (`memory.c` `write_ct32`, page-aligned base
  pointers, which covers the game's uploads). Within a 64-pixel page column a row lands in one
  physical page, so the sync check, dirty bit and `page_writes`/`pixels_written` counts happen
  once per run. The pixels go to `pixel_address`'s word offsets, split into row and column bits.
  The words, counters and the order of `sync_range` calls match `write_pixel`.
- **IMAGE data in runs** (`gs.c` `gs_feed`): a run of whole IMAGE qwords goes to `image_data` in
  one call instead of one call (with its `flush_draw`) per 16 bytes. The path keeps the last
  qword in `words`, as before. Off when `record_transfer` is set (captures see every qword) or
  under `image_reference`.
- **CLUT loads without a branch per entry** (`gs.c` `load_clut`, CSM1): the entries are
  gathered, then compared and stored with a branch-free count of changed entries. The CLUT and
  `state_version` (hashed by the state trace) end up the same. CSM2 keeps the old loop (its
  32-bit entries can overlap).

Not done: caching the palette part of the texture key (SP3's next idea; it needs a CLUT version in
`Gs` and changes the key function). Lazy swizzling (keeping raw upload bytes until a reader needs
the page) would not pay off, because the texture key reads every uploaded page in the same field.

### Checks

- `test_gs` (clang-cl -m32 as `test_runtime.py`): 0 failures. The upload fuzz now runs 6000
  transfers (half page-aligned PSMCT32, some pages pre-dirtied), fast against `image_reference`.
  It compares VRAM, counters, `host_dirty`, the GIF path state and the `sync_range` call
  sequence. Two deliberately wrong versions (one column bit, sync on every run) fail it.
- State trace, x64, 60 Hz, seed 81257: race 5000 fields and boot 600, base main against rr/GS2,
  `trace_columns.py`: **PASS, all 13 columns identical**, before the merge (base `348567dcc`
  against `45ddb5ed4`) and after it (`067fb5061` against `353c261cd`). The race includes the
  lens-flare probe every field, whose answers feed `ram`. The mask was not touched.
- Builds: `build_wasm.py`, `package_release.py` (x64) and `port/web/build.py --renderer rust` +
  `check_web_layout.py --require-renderer`: OK (web layout: core, graphics, renderer, vu).

### Timing

Same core wasm for both. Base main `067fb5061`, new rr/GS2 `353c261cd`.

| Run | Fields/s | p50 / p99 ms | busy ms/field game / vu / gs | CPU ms/field gs | CPU native |
| --- | ---: | ---: | ---: | ---: | ---: |
| base-1 | 176.7 | 5.56 / 10.74 | 3.53 / 4.09 / 3.98 | 4.20 | 1.67 |
| new-1 | 171.9 | 5.85 / 10.39 | 3.73 / 4.50 / 3.57 | 3.92 | 1.73 |
| new-2 | 175.6 | 5.64 / 10.51 | 3.74 / 4.34 / 3.47 | 3.86 | 1.82 |
| base-2 | 166.4 | 5.90 / 11.92 | 3.82 / 4.30 / 4.20 | 4.33 | 1.90 |
| mean base → new | 171.5 → 173.7 | | 3.68 / 4.20 / 4.09 → 3.74 / 4.42 / 3.52 | 4.26 → 3.89 | 1.78 → 1.78 |

The GS thread is 0.57 ms a field less busy (-14%) and uses 0.37 ms less CPU (-9%). Both new runs are
below both base runs. The busy time drops more than the CPU time, consistent with the GS no longer
blocking in the queue writes of 965 KB a field to the exact device. Fields/s did not move beyond
noise (+1.3%). The PC was busier than in the 2026-10-27 reference (about 230 fields/s, VU 3.27 ms;
here VU 4.1-4.5 ms busy, base fields/s 166-177), and the VU thread was the limit in every run. One
session on a shared PC, not a confidence interval.

### Left

- The GS front end (GIF decode, primitive recording) is still most of the GS thread. The
  palette-key cache above is worth about 0.05-0.08 ms (GP1's `gs_palette` + part of `native_mix`).
- The texture-key page hash still reads every uploaded page each field. A key built from the
  upload's content hash (GP1's content-addressed option) could drop that read, but it changes
  the key function and needs a rule for partly written pages.

## HO1: deferred vsync, the EE no longer waits for the drawing at each field

Goal: on main a race field takes about 4.4 ms though no thread is busy more than about 3.3 ms; cut
the waits between the game (EE), VU and GS threads without changing anything the game can observe.

### Where the waits were (main, DHD_TIMELINE, x64, hidden desktop, fields 7400-14600)

The timeline now prints each EE wait site as `exe+0x...` so it can be looked up in `dhd64.map`
(`port/hle/graphics_timeline.h`). Main: 216.8 fields/s; busy EE 2.72, VU 3.43, GS 3.22 ms/field; the
EE waited 1.89 ms/field (1.43 for VU, 0.46 for GS; 1.09 spinning, 0.78 parked), VU was idle 1.18 and
GS 1.39. By site: the full waits at vsync (two per field) and at `sceGsSyncPath` (four per field,
1.74 ms) made the EE wait for the whole of its own field's drawing every field, so the three threads
ran one after the other rather than overlapped. The rest is the Z probe (`sceGsExecStoreImage` and
its guarded-page fault, about 0.74 per field, 0.5 ms), a readback the game uses the next frame,
which must stay a full wait.

### What changed

- **Deferred vsync** (native hosts with the graphics workers, `host_service_graphics_threaded()`
  returns 2). The design is finding 048's (f80b4d61, reverted then because VU was the bottleneck),
  close to PCSX2's MTGS, which queues the vsync as a ring packet and lets the EE run a bounded number
  of frames ahead. At vsync the EE queues the VBLANK as a marker behind the drawing
  (`graphics_queue.c`, run in order on the GS thread: `gs_vblank`, CSR VSINT/FIELD, the readback
  field). `gs_vsync` queues the picture the same way (`present.c`: the display registers are copied
  and handed to the render thread when the GS reaches them). The EE goes on with the next field. At
  most two fields are in flight: vsync N waits until the picture of field N-2 has been handed on
  (`DHD_FIELDS_IN_FLIGHT=1`: N-1). `sceGsSyncPath` and the second vsync wait only ordered graphics
  work and are skipped, as in the browser's `DHD_GRAPHICS_ORDERED`.
- **The CSR page stays exact.** The game reads CSR from its MMIO page, which only the EE writes. At a
  deferred vsync the host writes a provisional CSR there, the last value with the VBLANK applied:
  exact except for SIGNAL/FINISH, which only the GS knows once it has drawn. The next
  `host_service_graphics_wait` (every fence, every kernel or HLE wait) writes the exact CSR the
  VBLANK left, which is the value a full wait at vsync would read. Unfenced, the game reads only
  CSR.FIELD (func_0020BED0, and sceGsSyncV's load at 0x00243D2C, emitted unfenced by
  `port/tools/recomp.py`), and the provisional value has FIELD exact. With SetVSyncFlag, and up to
  the first exact CSR, vsync waits as before.
- Off (old full waits) with `DHD_SYNC_STRICT=1`, `DHD_SCREENSHOT`, `DHD_GS_CAPTURE`,
  `DHD_RACE_GSDUMP`, `DHD_PROFILE`, `DHD_VU_SUSPECT`, with a movie or a non-native renderer
  picture, and in bench pictures. The browser build is unchanged (its host reports 1). Android uses
  the same code as x64, so it gets the deferral too.
- `DHD_STATE_NOWAIT=1` (state trace): no wait for the workers at each row, so the fields overlap as
  in play; only the EE's sections are hashed (the others are written as 0).
- Nothing spins more: the new waits use the queue's `idle_wait` (short spin, then park).

### The race-trace failure and its fix

The first version failed the race trace: only `ram` differed, from field 1453 (race start), and it
differed even with `DHD_SYNC_STRICT=1`. The dump showed 10 words, all 0x12904 against 0x12905, in an
array with a 0x7BD0 stride: a function-table index the game keeps in RAM, shifted by one. That
version had made two new core functions address-taken (`graphics_sync`, `csr_settle` as
`ps2_graphics_sync`/`ps2_gs_csr_sync`), which added a wasm table entry. The game's behaviour was the
same, but the state trace compares those indices. The fix moved the CSR settle into the host
(`host_service_graphics_wait`, `host_service_graphics_vblank`), so the core takes no new function
addresses and `ps2_graphics_sync` is `host_service_graphics_wait` as on main. The `ps2asm` fence
plumbing went back to main's.

### Exactness (x64, against a fresh build of main 656e65677)

| trace | result |
|---|---|
| race, 5000 fields, seed 81257 | PASS, all 13 columns identical |
| boot | PASS, all 13 columns identical |
| race with `DHD_STATE_NOWAIT=1` (fields overlapping) | menu, phase, clock, ram, scratch, cpu, sound, code identical for all 5000 fields; GS/VU/hw columns not hashed in that mode |

`port/tools/state_trace_mask.txt` is unchanged. Browser: `port/web/build.py --renderer rust` and
`check_web_layout.py --require-renderer` pass (138 imports).

### Timing (one ABBA session, x64, hidden desktop, race fields 7400-14600, DHD_TIMELINE on)

| run | fields/s | p50 / p99 ms | busy EE / VU / GS ms | EE wait VU / GS ms | VU idle / GS idle ms | EE / GS spin ms | busy cores |
|---|---|---|---|---|---|---|---|
| A1 main | 225.6 | 4.33 / 8.68 | 2.68 / 3.31 / 3.12 | 1.32 / 0.43 | 1.12 / 1.31 | 1.03 / 0.37 | 2.69 |
| B1 HO1 | 242.0 | 3.92 / 9.04 | 2.61 / 3.33 / 3.20 | 0.47 / 1.04 | 0.63 / 0.94 | 0.78 / 0.27 | 2.80 |
| B2 HO1 | 245.5 | 3.85 / 8.69 | 2.59 / 3.28 / 3.14 | 0.47 / 1.01 | 0.64 / 0.94 | 0.76 / 0.27 | 2.80 |
| A2 main | 242.5 | 3.99 / 8.15 | 2.29 / 3.13 / 2.90 | 1.44 / 0.39 | 1.00 / 1.22 | 1.10 / 0.39 | 2.63 |
| extra: HO1, `DHD_FIELDS_IN_FLIGHT=1` | 259.2 | 3.65 / 8.37 | 2.18 / 3.07 / 3.16 | 0.49 / 1.19 | 0.79 / 0.70 | 0.92 / 0.19 | 2.72 |

ABBA: main 234.1 fields/s on average, HO1 243.8, **+4.1%**. p50 is 0.3-0.4 ms lower, p99 about the
same. Busy cores rise from 2.63-2.69 to 2.80 (more overlap; spinning is lower, not higher). The
extra one-field run came last, when the PC was quietest; with no pair of its own it does not show
whether one or two fields in flight is faster. Two stays the default.

### What is left

- VU and GS now idle 0.6-0.9 ms/field, and the EE waits about 1 ms/field at the vsync bound plus
  0.5 ms at the Z probe. The probe (about 0.74 full waits per field) drains the whole pipeline,
  whatever the depth. That readback, which the game uses the next frame, is what keeps the field
  above the busiest thread's 3.3 ms. Lifting it would mean making the probe read itself cheaper or
  earlier, not dropping the wait.
- The GS thread now also does the picture handoff that used to run on the EE (about +0.1-0.3
  ms/field of GS work); the GS is the busiest thread with VU.
- The timeline's site attribution points inside the `host_service_*` wrappers, so it splits the
  waits only into "full wait" and "vsync bound".
