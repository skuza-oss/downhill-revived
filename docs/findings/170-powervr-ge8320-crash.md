# 170: PowerVR GE8320 phones close at start

Status: in progress

Branch `fix/powervr-crash` from `main` fce8a1f (0.7.12), started 2026-11-06. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Issues #56 (OPPO CPH2269, Android 11) and #60 (Infinix Hot 11 Play): phones with a PowerVR Rogue
GE8320 GPU (Helio G35/P35-class chips; driver "PowerVR Rogue Vulkan Driver 1.13@5776728") close
right after the renderer starts. The shared log (Share log, finding 152; kept by the lead) shows
`crash: SIGSEGV at 0000000000000008 in thread game` with the frames inside the PowerVR driver's
`libufwriter.so` (its shader compiler / USC code writer), right after
`gs: renderer dhd_render 1.0 wgpu 29.0.3 ... on PowerVR Rogue GE8320`. Finding 153 fixed a similar
start crash in Mali's r54 compiler (no texture passed into a shader function).

Goal: the game starts and plays on these phones with the native renderer, one code path for every
GPU (a narrowly scoped driver workaround only if no common form works), pictures unchanged on other
GPUs (`test_gs_wgpu`, the native pictures byte-identical where the shaders change).

There is no PowerVR phone here. Find the cause from evidence: which pipeline is built when it
crashes (the log has no pipeline trace; finding 153's test patch logged each pipeline before
vkCreate*Pipelines), what PowerVR's compiler is known to choke on (wgpu/naga issues, other
emulators' PowerVR workarounds: PPSSPP, Dolphin, DuckStation, Skyline/yuzu-android, Vita3K, PCSX2
Android forks; Imagination's offline compiler PVRShaderEditor/`glslangValidator`-based tools if
they can compile SPIR-V for Rogue), and our SPIR-V (`port/render/tests/native_spirv.rs` writes every
stage as wgpu-hal makes it). Rank the candidates, implement the most likely common fix, and prepare
a test APK for players: release-signed (signing variables from the lead's notes, never print the
passwords), with the pipeline trace so a tester's Share log names the crashing pipeline if it still
crashes.

Acceptance: the cause or the ranked candidates with evidence; the fix; `check.py port` passing;
the shaders' SPIR-V checks (`native_spirv`); a test APK path for the lead (a release candidate
build plus the trace patch on a local test branch); on the Mi 11i (Adreno) the test APK starts and
draws the title (the lead or the lane, phone rules apply).

## Progress

- [x] 2026-11-06: finding reserved
- [x] 2026-11-06: the crashing pipeline. The shared log (0.7.11) symbolised against that release's
  unstripped `libmain.so`: `dhdr_exact_create` -> wgpu `create_compute_pipeline` -> the driver.
  `dhdr_exact_create` makes three compute pipelines (exact/pipelines.rs); the crash is the second
  call site, whose label is 4 bytes long and which passes three constants: raster.wgsl `main` (raster
  key 7). `scanout` was built just before it and compiled. Nothing else compiles at that moment: the
  native renderer, its render thread and its pipeline worker start later.
- [x] 2026-11-06: Imagination's offline compiler does not reproduce it. The PowerVR Profiling
  Compilers (2025 R2, free from Imagination's developer site) have a target for this very GPU, BVNC
  22.87.104.18 "Series 8 XEP GE8320, MT6765 Helio P35". They compile every raster.wgsl stage as
  wgpu-hal makes it, `main` included: 17,203 instructions, 140 temporaries, 114 MB of compiler
  memory, against scanout's 302 instructions, 7 temporaries and 2 MB. A newer compiler, as malioc
  was for finding 153.
- [x] 2026-11-06: the cause, read in the driver (Result): the driver's SPIR-V front end is LLVM 9.0;
  its InstCombine turns raster.wgsl `texel()`'s `select(0u, tw - level, tw > level)` into
  `llvm.usub.sat`, which the driver's UniFlex code writer cannot lower in a compute shader: it reads
  a null table entry at offset 8.
- [x] 2026-11-06: the fix. `texel()`'s two clamped subtractions pass the difference through
  `keep_u()`, an OR with `batch.w` (a uniform that is always 0, as `keep()` already relies on), so
  no compiler sees `a - b` in the select. Only `main`'s SPIR-V changes: every other stage (native,
  present, scanout, local_copy) is byte-identical on all three device kinds. The new `main` compiles
  in the GE8320 offline compiler.
- [x] 2026-11-06: the check. `native_spirv` now also writes raster.wgsl's stages (scanout, `main`
  for the 8 raster keys, `local_copy` for both shifts, with their layouts' binding numbers) and scans
  every stage for the three forms LLVM 9 turns into an intrinsic this driver cannot lower
  (`llvm_saturating_forms`; `llvm_saturating_forms_scan` proves each detector on a small module). On
  main's raster.wgsl it reports the two `texel()` selects in all 24 `main` stages and nothing else
  in any module; with the fix it passes.
- [x] 2026-11-06 checks: `check.py port` OK. The exact renderer's pictures: `test_gs_wgpu.py
  --skip-hardware --only gpu` with main's raster.wgsl and with this one gives the same bytes in both
  VRAM logs (WARP, with and without early depth). gs_golden's test-gpu goldens could not be used:
  they belong to an older NVIDIA driver and compare refuses another (as in finding 153). State
  traces, a game built with main's raster.wgsl against this one: boot, all 600 rows match; race,
  all 5000 rows match.
- [x] 2026-11-06: the test build for players: this branch plus the pipeline trace patch (finding
  153's, a local test branch), built as the 0.7.12 APK is (package_release.py, the phone profile,
  the release key): versionCode 712, so it installs over 0.7.12 and 0.7.12 over it.
- [x] 2026-11-06: the test build on the Mi 11i (Adreno 660): it starts and draws the title screen.
  Its log has the trace of all 64 pipelines built by then: the render probe, scanout, `main` (key 7),
  local_copy, then the native renderer's. Afterwards 0.7.12 as shipped was installed again.

## Decisions

- **One code path.** The form changes for every GPU; there is no PowerVR switch. Pictures and Z
  cannot change: `batch.w` is 0, so every value is the old one (the OR adds one instruction per
  `texel()` call).
- **`keep_u`, not `tw - min(tw, level)`.** LLVM 9 does not fold `a - umin(a, b)` into usub.sat (a
  TODO in its source), but later LLVM versions do. A driver with a newer LLVM and the same code
  writer would crash again; no compiler sees through the OR with a uniform.
- **The scan reads naga's SPIR-V** (what every driver gets), not the driver's LLVM IR. It finds the
  forms as written, conservatively (loads of one variable count as one value, any unsigned compare
  over the same operands), not ones LLVM could build after inlining and SimplifyCFG. Every module was
  also read for such forms: none.
- **The driver stays out of the repository and the tests.** The Result says how to find and verify
  a copy of the library; only its symbol tables and code were read, it was never run.

## Surprises

- **Not native.wgsl** (finding 153's module): the exact renderer is built first even with
  RENDER=native (it answers the Z probe), and its rasterizer is the pipeline that crashes.
- **Not the thread-safety problem** of Godot (#123614) and cinnabar's wgpu fork (#17), where GE8320
  phones crash in `libufwriter.so` with pipelines compiled on several threads. Here one thread
  compiles, and the crash is a lookup that misses every time.
- **The driver compiles SPIR-V through LLVM 9.0** (its strings: "llvm-mc (based on LLVM 9.0.0)";
  its intrinsic IDs are LLVM 9's), plus 75 intrinsics of Imagination's own, `llvm.usc.*`, from ID
  5771. Its optimiser canonicalises patterns into generic intrinsics that its own code writer does
  not know.

## Result

The cause, with the evidence:
1. **The pipeline**: raster.wgsl `main` (Progress). In the driver, the frames run from
   `vulkan.mt6765.so` to `libufwriter.so` `BILParseStream` (its SPIR-V front end), eight frames
   deeper to a read at address 8.
2. **The driver code.** The GPU driver of MT6765 phones is shared across makers. A public firmware
   dump of another MT6765 phone (a Redmi with the same PowerVR Rogue 1.13 driver) has the player's
   `libufwriter.so` layout: its exported `BILParseStream` spans `0x9cb03c`-`0x9cb138` (the log's
   frame 11 is `+0x9cb10c`), and every frame's call site is a `bl`/`blr` into the next frame's
   function. The crash pc `0xbed204` is `ldr w23, [x24, x8]`.
3. **What that code does**: it lowers an LLVM call to an intrinsic. It reads the callee's intrinsic
   ID (`Function+0x24`), handles some IDs itself (assume, bswap, copysign, ctlz, ctpop, cttz,
   lifetime.start/end, the six `*.with.overflow`, most `llvm.usc.*`), and looks every other one up
   in a 22-entry table: ceil, cos, exp2, fabs, floor, fma, fmuladd, log2, maxnum, minnum, pow,
   round, sin, sqrt, trunc and seven `llvm.usc.*`. The table maps each to a UniFlex opcode
   (`PVRUniFlexGetInputInstDesc`). When the ID is not in the table, the entry pointer stays null.
   Only one program kind (2) has a fallback. Kinds 3 and 4 read the entry's word at offset 8, the
   others at 4, so a miss in a kind 3 or 4 program reads `[null + 8]`: the log's
   `SIGSEGV at 0000000000000008`, in a compute pipeline.
4. **The intrinsic**: LLVM 9's InstCombine (`InstCombineSelect.cpp`, `canonicalizeSaturatedSubtract`)
   turns an unsigned `(a > b) ? a - b : 0` into `llvm.usub.sat` (LLVM 9 ID 235). The driver carries
   it: the one place that creates ID 235 is an IRBuilder call followed by the fold's negation for
   the swapped form. ID 235 goes to the table and is not there. Nothing expands it first: the only
   other code naming 235 is LLVM's analyses and its cost model.
5. **Where in our shaders**: raster.wgsl `texel()`, `tw = select(0u, tw - level, tw > level)` and
   the same for `th`, reached from `main` only (`draw_pixel` -> `sample_texture` -> `texel`). The
   scan finds this form in every `main` stage and nowhere else. scanout, compiled just before `main`
   on the phone, has none.

Candidates, ranked:
1. `llvm.usub.sat` from `texel()`: the above; fixed.
2. Other intrinsics LLVM 9 makes that this writer cannot lower in compute: `llvm.uadd.sat` (from
   `(x < ~c) ? x + c : -1` forms) and `llvm.fshl` (rotates, `x << a | x >> b`) are in no stage
   (scan). `llvm.memcpy`/`memset`: the driver carries neither MemCpyOpt nor LoopIdiom (the passes
   that make them from stores and loops; its pass list is in its strings), and a `memcpy_loop` name
   beside its own legalisation suggests a lowering of its own. bitreverse and canonicalize: LLVM 9's
   InstCombine makes only byte swaps (handled) from bit tricks.
3. Excluded. Concurrent compilation (Godot, cinnabar): one thread compiles here. `num_workgroups`
   (only `main` reads it; `llvm.usc.ugetnumgroups` is unhandled too): no driver code creates that
   intrinsic, by ID or by name. The compiler's size or memory: the fault is a lookup miss in the
   front end, not an allocation.

After `main` the phone builds `local_copy` and the native renderer's pipelines; all scan clean. If
one still crashes, the test build's pipeline trace names it.

Left for a phone test (the lead, with the players of #56 and #60): the test build on a GE8320 phone
(any Helio G35/P35/P22-class phone with "PowerVR Rogue GE8320" in its log). It should reach the
title and the menus, then a race. If it still closes, Share log: the last `pipeline trace:` line
names the pipeline the driver could not build, and the test build's kept `libmain.so` maps its
frames. The same build on the Mi 11i (Adreno) is in Progress.

How to look again (no driver binary or path in the repository): GitHub code search for
`vulkan.mt6765.so` finds MT6765 firmware dumps; take one whose `vendor/lib64/libufwriter.so` exports
`BILParseStream` at `0x9cb03c` with size `0xfc` (NDK `llvm-nm -D -S`). Its code reads with the NDK's
`llvm-objdump`; LLVM 9's `Intrinsics.td` (generic intrinsics, sorted by name, from 1) gives the IDs.
