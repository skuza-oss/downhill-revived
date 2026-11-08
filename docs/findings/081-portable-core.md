# 081: A portable 64-bit core (plan)

Status: partly superseded by 088 (the phase status; the 32-bit description is the then-state)
Checked: 2026-11-04

Written during phase 1 (2026-10-24, branch `port/portable-core`). Every phase is done since: the
64-bit build is the only build and shipped as 0.4.0 (finding 088).

## The problem

The port was 32-bit Windows then (`i686-pc-windows-itanium`, port/tools/pctarget.py, both gone).
That wasn't just a build setting. The design depended on it:

- **The game code needs 4-byte pointers.**
  - The decompiled game code in `src/` keeps pointers in its structs.
  - The game's data image is loaded from the player's disc with 4-byte pointer words (fixed up by
    port/tools/data.py).
  - The recompiled asm functions (port/tools/recomp.py) load 32-bit words and use them as
    addresses.

  If `src/` were compiled as normal 64-bit C, every struct with a pointer would change size and
  no longer match the game's data or the recompiled code.
- **A PS2 pointer is a host pointer.**
  - PS2 RAM is mapped at host 0x08000000, with uncached mirrors at 0x20000000|base and
    0x30000000|base, plus a "low view" at the PS2 addresses themselves.
  - The scratchpad, VU memory and hardware registers sit at their PS2 addresses
    (port/runtime/memory.c).
  - Code all through port/ casts between `uint32_t` and pointers.
- **Function addresses live in game data**: function tables and callbacks are 32-bit code
  addresses fixed up at link time.

The usual 64-bit cleanup (`base + offset` everywhere) would mean rewriting every pointer in the
decompiled game, which is ~3,800 units. That breaks rule 1 for `src/` and is far too big.

General ILP32-on-64-bit ABIs aren't an option on our targets:
- x32 exists only on x86-64 Linux, and is usually disabled;
- arm64_32 exists only on watchOS;
- `__ptr32` is MSVC-only, and every pointer would have to be annotated.

## The design: the game core as a wasm32 module

WebAssembly's wasm32 is exactly the ILP32 environment the game needs: 4-byte pointers, gcc/Itanium
struct layout, and 8-byte alignment of 64-bit types (like the EE). Compile the **whole core** to
wasm32 with the same clang: `src/`, the recompiled functions, port/runtime, and the HLE layers that
touch game memory. Then:

- **Web:** the browser runs the module directly.
- **Windows x64, Linux, Android, iOS:**
  - **wasm2c** (WABT) or w2c2 translates the module to plain C, which is compiled natively for
    each 64-bit host.
  - Guest memory is `base + address`, done by the translator for every load and store, with
    guard pages instead of bounds checks.
  - Nothing in `src/` changes.
  - Firefox ships this route (RLBox) for sandboxed libraries.
- **Host layer (native, per platform):**
  - SDL3 for the window, input, audio and the app lifecycle;
  - wgpu for graphics;
  - sockets, files, time, fibers and threads.

  The module imports these as a small, explicit API. Host code reaches guest memory only through
  `base + guest_address` helpers.
- **Same layout as today:** PS2 RAM stays at linear address 0x08000000, with the scratchpad, VU
  and registers at their addresses. Pointer values in game memory are therefore identical to the
  32-bit build, which keeps the state comparison exact.
  - **Mirrors on native hosts:** we allocate the linear memory ourselves and map the same 32 MB
    three times inside it.
  - **Mirrors on the web:** the browser can't alias memory, so this needs another solution
    (masking at the few sites that use uncached pointers). That is deferred to the web phase.
- **Fibers (EE threads):** the game's threads are cooperative.
  - On native hosts, wasm2c code runs on the real C stack, so the host switches native stacks
    (Windows fibers; ucontext or a small assembly switch elsewhere) and saves/restores the
    module's `__stack_pointer` global per fiber.
  - Web: JSPI or Asyncify, deferred to the web phase.
- **Code addresses:** function pointers in wasm are table indices. The data fixups become
  relocations that wasm-ld resolves into table indices; recompiled dispatch maps targets the
  same way.
- **The 32-bit Windows build stays** as the reference and the shipping build until the new one
  is proven.

Expected cost: wasm2c code is usually 1.1–1.5x the time of native code. The 32-bit x86 build is
itself not the fastest baseline (only 8 general registers), so x64 through wasm2c may come out
close. Phase 1 measures this.

## Float modes (from audit H6)

The VU and EE arithmetic depends on the x86 MXCSR mode: round toward zero, FTZ and DAZ. The mode is
set per EE fiber (`kernel.c` `ee_float_mode`) and around VU0/VU1 runs and V_CALLMS. The PS2's
software doubles and the host GS work run in IEEE round-to-nearest.

WebAssembly has none of these modes, and SIMDe drops them: wasi-libc's `fesetround` does nothing.
So:

- **Native hosts (x64, arm64): the hardware mode is kept.**
  - The module imports `host_fp_mode(mode)` at exactly today's MXCSR switch points. The host sets
    MXCSR on x64, and on arm64 FPCR.RMode=RZ and FZ=1 (denormal inputs and outputs flushed).
  - wasm2c output is plain C float code, so it runs in that mode at no extra cost.
  - The generated C must be compiled with `-frounding-math` (FENV_ACCESS) so nothing is folded
    or moved across the mode calls.
  - arm64's FZ and x86's FTZ+DAZ may differ in tininess-detection corner cases. The state trace
    on an arm64 device checks this.
- **Web: a software float layer** (round toward zero by TwoSum/error-term correction, explicit
  flush to zero), inserted by rewriting f32 `fadd`/`fsub`/`fmul`/`fdiv`/conversions at the LLVM
  IR level. Only for the web build, so deferred to the web phase. It can be checked exhaustively
  on x86 against MXCSR RZ hardware.
- No approximate intrinsics (`rcp`/`rsqrt`) are used anywhere, so rounding and flushing are the
  only exactness issue.
- **Risk:** NaN sign. On x86, 0/0 gives 0xFFC00000. Wasm leaves the NaN sign
  nondeterministic, while wasm2c on x64 follows the hardware. Check this if the trace shows it.

## The proof: state traces, not images

Both builds run headless with the same scripted pad input (autopad) in a deterministic fixed-step
mode. After every game frame each build writes hashes of:

- PS2 RAM in 64 KB pages, the scratchpad, and VU0/VU1 code and data;
- the GS packet/register stream;
- the sound output.

`state_diff.py` reports the first frame and page that differ, and dumps the differing words,
classified:
- a code address on both sides that names the same symbol counts as equal;
- anything else is a failure.

This proves more than images do: one wrong byte of game state fails, at the frame where it starts.

Test set:
- boot to the menus;
- a full race on each track with scripted input;
- split screen;
- a memory card save and load;
- a movie;
- the online lockstep loopback.

Plus the existing runtime unit tests, built for both targets.

## Phases

0. **Audit (running):**
   - the wasm32 compile census of all units;
   - the platform API inventory;
   - the guest-memory access audit;
   - the state-trace design;
   - the wasm2c toolchain spike (memory mirrors, fibers, SIMD, speed).

1. **State trace in the 32-bit build:** implement the trace and diff tool. Two runs of today's
   build must give identical traces (this proves determinism first).
2. **Host API split:** move all Windows API use behind `port/host/` interfaces (memory, fibers,
   files, time, threads, audio, input, window, GPU, network). The 32-bit build must still
   produce identical traces.
3. **Core to wasm32:** build the module and link it with wasm-ld; data fixups become relocations;
   SSE paths get portable or simd128 versions with identical results.
4. **Native 64-bit host on Windows:** wasm2c, our memory allocator (mirrors), fibers, and the
   existing D3D11 renderer through the host API. The state traces must match the 32-bit build
   frame for frame over the whole test set.
5. Then the platforms: wgpu renderer, Linux, web, Android, iOS (see the earlier overview).

## Phase 1 result (2026-10-24)

`DHD_STATE_TRACE` (port/hle/state_trace.c) together with `state_trace_run.py` and `state_diff.py`
now reproduce run to run. Two boots of 600 fields and two races of 5000 fields from one build match
in every field and every section, with an empty mask. A different race seed diverges at the race
start (field 1360), which is the positive control.

Three sources of nondeterminism were found and removed:
1. **Uninitialised locals.** The game reads some locals before writing them and picked up host
   leftovers. Game and host code are now built with `-ftrivial-auto-var-init=zero`.
2. **Stack placement.** The game keeps addresses of its stack locals in its own memory (a menu
   stack around 0x0093C3F0–0x0093C620), and Windows placed the fiber stacks differently each
   run. Under a trace, EE fibers now run on fixed stacks at 0x06000000 (256 KB per slot, lowest
   free slot), reserved by the launcher. They must stay below 0x10000000, because the game masks
   addresses with 0x0FFFFFFF; at 0x5A000000 a race crashed. The wasm build gets the same rule for
   free: its stacks live in linear memory, so put them in the same range.
3. **Normalising host addresses by value doesn't work.** Fiber stacks and the heap sit at
   0x04xxxxxx, and so do genuine game constants (VIF codes such as 0x04200E00 in D_00278044), so
   the normalisation hid real data in some runs. It is now opt-in only (`DHD_STATE_NORM=host`)
   and unused.

For the i686 vs 64-bit comparison, the remaining expected differences are words that hold host code
addresses (function pointers: recompiled functions, callbacks). `state_diff.py` compares those by
symbol name using a per-build symbol map.

## Phase 2 plan: the module/host boundary

Every port file goes on one side. The **module** is wasm32 later and is i686 today: game code, the
recompiled functions, the runtime, and the HLE that is game logic. The **host** is native 64-bit
later.

- **Module:**
  - runtime: ps2asm, vu0_micro, vu0_interp (VU0), recomp, libc_shims, and the data-image part
    of memory.c;
  - hle: kernel (EE threads), timer, boot, sif, cdvd, game_data, iop, mc, dma, the EE side of
    graphics.c (sceGsExecStoreImage and the feeds), pad, menu_page, pc_settings, widescreen,
    exit_menu, puppet, the game-logic parts of online.c and online_menu.c, autopad, state_trace,
    the sound mixer (sound.c up to sound_tick), and game_options.
- **Host:**
  - graphics: graphics_queue.c and gs_queue.inc (worker threads), VIF1/VU1 execution, all of
    gs/ (GS, GPU, present, texpack, font);
  - devices and OS: sound output (WASAPI), gamepad (SDL3), keyboard and text input, movie
    decoding, net/ (transport), update, splash, file I/O, time and pacing (the wall-clock side
    of fps.c), fibers, threads, and the memory mapping (RAM views, mirrors).
- **Crossings** (module → host imports, host → module exports) become one header,
  `port/host/host_api.h`. Its functions take guest addresses (`uint32_t`), never host
  pointers.
- **Host-side guest access:** host files reach guest memory only through `GUEST(addr)` and
  `GADDR(ptr)` (`port/include/guest.h`). Today these are the identity (base 0); later the base
  is the module's linear memory.
- **Order:** do it all in today's i686 build first. Every step must keep the state trace equal
  to the baseline. Then compile the module side for wasm32 (phase 3).

**Code pointers across builds:** after any code change, host function addresses move. Guest
memory holds such addresses (data-image fixups, callbacks), so raw RAM hashes differ between
builds that behave identically. The trace therefore gets a pointer map:
- EE addresses known to hold code addresses: the code-valued fixups, plus addresses learned
  from dump diffs;
- the words at those addresses are left out of the ram hash and logged separately when they
  change, then compared by function name using each build's symbol map.

The same mechanism handles wasm table indices in phase 4.

## Open design issues (2026-10-24)

- **Indirect calls with mismatched types.** The module has 21,909 `call_indirect` sites. Some
  function pointers in `src/` are called with a type that differs from the target's definition.
  The PS2 and i686 tolerate this, but wasm traps on it. Direct calls are already handled by the
  generated ABI thunks (build/wasm/abi_thunks.tsv). For indirect calls, candidates are:
  - binaryen's `--fpcast-emu` (FuncCastEmulation: every table call uses one uniform signature);
  - per-type adapter entries in the table.

Pick one by measuring the dispatch cost (host64_bench.py).

  Done (W7f): per-type shadow tables with i686-slot adapters, port/tools/wasm_postlink.py, run
  after the link. fpcast-emu was measured and rejected. See finding 082, section "W7f".
- **Return values of void callees.** The ABI thunks return 0 where the caller reads a value that
  the callee doesn't produce. The 32-bit build reads whatever the callee left in eax or `$v0`.
  These must reproduce the i686 value, or the trace comparison shows them (W5 round 3).
- **A build-dependent float in game memory.** EE 0x00760854 holds a value of about 2.98 that
  differs slightly between builds, so something build-dependent leaks into game state. W1 is
  finding its writer with a hardware watchpoint.
- **Host callbacks as host function pointers.** Six imports from host_win32.c (`host_fiber_create`,
  `host_fault_guard`, ...) and `gs_capture_request` take C function pointers. In the module a
  function pointer is a table index, so these need wrappers in the host glue that call back
  through the module's table (next host-glue round).
- **Committed memory.** The module's initial memory is 0x70C00000 (1.9 GB), because the
  scratchpad sits at 0x70000000. host64/guest_memory.c commits every declared byte. Only a few
  ranges are used: RAM, the hardware windows, the stacks and the module data. The rest should
  stay reserved, not committed, to keep the commit charge small (and the web build needs another
  answer anyway).

## Baseline b2 (2026-10-24 06:47)

From commit 6789c48e on, the trace compares code pointers by function identity (W1). This covers the
1,348 learned code words in RAM and the GPRs (`put_gpr`), plus a `code` column. The cdvd.c leak is
fixed: sceCdSearchFile's reply copied host stack bytes into EE 0x00760854.

The new reference traces are `build/state-trace/base-boot.tsv` (600 fields) and `base-race.tsv`
(5000 fields), from the i686 build of 6789c48e. Two runs of each match in **every column**, ram and
cpu included.

Against the old reference (mg-*): gs_stream, gs_regs, gs_vram, sound, scratch and hw are identical
in every field. vu0 differs from field 1, but this is only the format: the vu0 section includes
VU1's register file through `serialize_cpu`, which now hashes GPRs through `put_gpr`.

From here on, a refactor or the x64 build is compared with base-* on every column, without
`--ignore`. The exception is cpu between builds with different code layouts: derived temporaries
(e.g. gpr[9] = code address + offset) can differ there. Those fields are checked with state_diff.py.

Check of the merged build d42da166 against base-*: ram, scratch, hw, vu0, gs_* and sound are
identical in all 600 boot and 5000 race fields. cpu differs from field 150. A dump at field 150 of
both builds (state_diff.py) shows **one** differing word: gpr[1] ($at) lane 1, the upper 32 bits of
the 64-bit register. It is 0x003B6284 in the base build and 0x003B5E74 in the new one (-0x410). $at
is the assembler temporary, and its upper lane is never stored to RAM (ram stays identical). Open:
which recompiled template leaves a build-dependent value there. To check in the x64 run. 
