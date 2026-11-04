# 082: Independent wasm2c x64 core host runtime

Status: partly superseded by 099 ("unproven" status, per-OS build scripts)
Checked: 2026-11-04

This is the 64-bit host every release runs on since 0.4.0 (finding 088): the game's state traces
match and it is faster than the 32-bit build was (findings 089, 090). Changed: one build script
for every target, `port/tools/build_host64.py --target win-x64|linux-x64|macos-arm64|android-arm64`
(finding 099; `build_host64_linux.py` is gone), fibers in one module (`port/host64/fiber.c`), the
VU0 entries compiled (`vu0_aot.py`, finding 090 SP1). The rounds below are the log of how it got
there.

The runtime supplies aliased linear memory, fixed guest stack fibers with wasm SP/call-depth saves,
and EE/IEEE FP switching. Real-game equivalence and whole-core speed parity were unproven at this
first round.

Round 2 replaces the full barrier around every wasm2c access with span-aware host address folding.
Only the two exact RAM mirror windows fold to canonical RAM. Guest pointer values, non-RAM addresses
and original-address bounds checks stay unchanged. Normal accesses have no asm. Boundary-crossing
accesses retain OS views and the old barrier; crossing memory.copy snapshots its source for hidden
physical overlap. OS mirrors remain required. The allocator's enforced 0x70010000 minimum is exposed
to LLVM for valid bounds inference. Every generated unit must pass through
port/tools/prepare_wasm2c.py before native optimization.

Median ns/iteration, clang-cl /O2, strict FP, Ryzen 5 7600, 24 timed samples per
workload in three rotated cycles:
| Policy | int | float | table | mat4 | GPR | copy |
|---|---:|---:|---:|---:|---:|---:|
| native i686 | 1.467 | 4.563 | 1.659 | 27.203 | 2.996 | 59.273 |
| no fix (unsafe) | 1.435 | 3.616 | 2.061 | 27.080 | 1.273 | 52.679 |
| full barrier (previous) | 1.438 | 3.617 | 1.996 | 28.174 | 3.116 | 53.204 |
| pointer opacity, no clobber | 1.460 | 3.595 | 2.097 | 28.081 | 3.172 | 54.889 |
| selective pointer opacity | 1.429 | 3.427 | 2.157 | 28.618 | 3.063 | 56.466 |
| fold start only (unsafe) | 1.479 | 3.487 | 2.114 | 28.462 | 1.351 | 54.505 |
| span folding, no size fact | 1.437 | 3.553 | 2.016 | 28.425 | 1.312 | 54.589 |
| selected folding + size fact | 1.429 | 3.525 | 1.927 | 28.834 | 1.203 | 52.249 |

Selected GPR traffic is 2.59x faster than full barriers. The unweighted geometric mean time ratio is
0.830 of i686; table access remains 16.2% slower and mat4 6.0% slower in this run. This is a
synthetic summary, not proof of game-speed parity. Reproduce with `python
port/tools/host64_bench.py`; all 27 return bit patterns per workload match i686. GPR source
volatility retains traffic into wasm so this tests native register recovery, not actual nonvolatile
recompiled-function timing.

`python port/tools/host64_selftest.py --negative-controls` passes mirror/non-RAM preservation,
scalar/bulk boundary accesses, physical copy overlap, stored pointer values, four generated OOB
traps, three physical guard faults, float bits, table calls, fibers, growth and cleanup. Omitting
preparation at /O2 fails the required mirror assertion; missing SP/depth controls fail as required
too.

H3 reports 42 src mirror sites and 12 port uses. recomp.py does not mask mirrors: it emits ADDR/GPR
accesses through runtime helpers that preserve address values. A recomp-only mask cannot cover
ordinary C, HLE and bulk accesses. Dynamic mirror frequency is unmeasured; no game was run. Earlier
source/wasm optimizations still require validation by the real-module state trace.

No external implementation or proprietary data was copied into the repository.

## Dispatch and SIMD parity (round 3, 2026-10-24)

The earlier Table workload is **random memory access, not indirect calls**. It remains in the suite;
dispatch is a separate new workload. No universal MIPS-address hash lookup exists in recomp.c:
recomp.py's `jalr` first tries `ps2_recomp_scalar_call`, then casts the relocated address and passes
UW(4)..UW(11), writing the result to GPR(2). Scalar dispatch compares relocated target addresses;
`jr` uses a switch over local MIPS label addresses. wasm relocations turn native code pointers into
table indices. The synthetic dispatch kernel uses eight u32 arguments, GPR-shaped state, a
guest-address-to-relocated-callback selection, an indirect call and a local-label-shaped switch in
each target. Eight functions share its signature; four are hot. The explicit hot-wrapper list is
known from this synthetic workload, **not measured in the game**.

A compile-time assertion rejects a non-SIMD wasm backend. Separate packed multiply/add and lane
shuffle model VU0 macro FMAC work; unequal lane biases and a four-lane FNV result avoid a zero XOR
after convergence. The driver sets RZ/FTZ/DAZ for VU0 on both targets and restores the previous
MXCSR. This is a representative synthetic kernel, not a full VU interpreter or proof of exceptional
VU semantics.

Final ns/source iteration, median of two independent run medians (24 timed samples/run/kernel; 48
total). Each run rotates three variant orders, interleaves variants **per kernel**, and uses one
warmup plus eight measured repetitions per process. CPU 0 affinity and high priority are required by
the driver. Integer uses 50M iterations to reduce short-sample noise; other counts remain 5M, Mat4
0.5M and copy 0.1M. All builds finish before timing. The native reference keeps round 2's i686 /O2
strict-FP flags. Before uses the round-2 prepared-address policy and /O2, with SIMD now enabled in
the wasm source for a fair pipeline comparison. Tuned adds O3, x86-64-v2 and Zen 3 scheduling,
guarded dispatch, and post-call size facts; it uses neither LTO nor PGO.

| Kernel | i686 | Before (SIMD enabled) | Tuned x64 | Tuned / i686 |
|---|---:|---:|---:|---:|
| Integer | 1.457 | 1.448 | 1.448 | 0.994 |
| Float/sqrt | 4.663 | 3.242 | 2.790 | 0.598 |
| Table (memory) | 1.794 | 2.066 | 1.736 | 0.968 |
| Mat4 | 28.428 | 17.654 | 16.728 | 0.588 |
| GPR traffic | 3.130 | 1.262 | 1.261 | 0.403 |
| Copy | 62.738 | 55.195 | 55.856 | 0.890 |
| Dispatch | 11.280 | 12.635 | 10.195 | 0.904 |
| VU0 SIMD | 2.457 | 2.873 | 2.388 | 0.972 |

All eight aggregate medians meet <= i686. All 54 return bit patterns/kernel match i686. The original
non-SIMD Mat4 result was 28.834 ns versus i686 27.203; enabling module SIMD closes that old gap
before native tuning. The new Before column is therefore not the old round-2 binary. Table memory
goes from 15.2% slower to 3.2% faster than the contemporaneous i686; dispatch from 12.0% slower to
9.6% faster; VU0 from 16.9% slower to 2.8% faster. Tight parity is subject to shared-host variation:
one earlier single-run integer median was 0.1% slower. No timed samples or failing runs were
removed. The two final run medians, hashes, raw logs, assembly, tool versions and flags are in
build/host64-bench. This is a synthetic parity result, not a whole-core or frame-time promise.

### Generated-C decisions and safety

- WABT 1.0.42 already has a bounds check and pointer-type comparison on its hot
  `CHECK_CALL_INDIRECT` path. Only type-pointer inequality takes the SHA-256
  memcmp slowpath, which also checks nulls. Interning it again cannot remove a
  hot memcmp: none is present. No check was deleted or replaced.
- prepare_wasm2c.py now promotes small type groups to guarded direct calls.
  WABT's original CHECK_CALL_INDIRECT runs first. Each promoted branch compares
  the actual native function pointer to the known typed wrapper, then passes
  the **actual entry's module_instance**, not the caller instance. An unknown
  pointer retains DO_CALL_INDIRECT. Table mutation, foreign contexts, equivalent
  noninterned type hashes and ordinary traps preserve their previous behavior.
  There is no assumption of an immutable or in-range table. Defaults promote
  only complete groups of at most four; wider groups stay indirect unless a
  caller supplies at most four hot wrappers of that signature with repeated
  `--dispatch-target wrap_...` options. Use `--no-dispatch` for A/B measurement.
  Split output units without local element metadata remain unpromoted; explicit
  target names must exist in the processed unit. Real large-table hot selection
  still requires real-module profiling.
- WABT reloads cached memory size after indirect/imported calls. The round-2
  adapter annotated only declarations, leaving redundant checks in the dispatch
  loop. Annotating **every size assignment** exposes the same allocator-enforced
  minimum after calls too. Growth cannot violate that minimum. Arbitrary-address
  bounds checks, original effective addresses and boundary alias fallbacks stay.
- The SIMD output lowers through SIMDe again. Baseline SSE2 emulates unsigned
  max in the operand clamp; x86-64-v2 provides SSE4.1 `pmaxud`. The address loop
  also suffers awkward multiply/shift/mask lowering (three LEAs for the second
  index). O3 and `-mtune=znver3` unroll it four ways and overlap independent
  address work. Inspection corrected the initial hypothesis: the address
  expression was not eliminated. Zen 3 tuning schedules instructions; the ISA
  stays x86-64-v2 (SSE4.2 maximum, no AVX). The benchmark rejects any V-prefixed
  vector instruction in generated assembly. Tuning is measured on Ryzen 5 7600;
  other CPUs still need measurement. Strict FP and contraction-off remain on.
- **ThinLTO rejected:** the full optimized self-test caught FTZ failure at float
  case 2, EE mode. Cross-unit inlining/constant propagation folded an exactly
  representable multiply into a subnormal constant, bypassing runtime FTZ,
  despite strict FP. Native noinline reference still flushed it. No expectation
  or safety check was weakened. O3 without LTO passes every float bit and negative
  control. An earlier PGO/ThinLTO trial improved synthetic Mat4 but did not close
  dispatch or Table by itself; that configuration is not retained. Binaryen was
  researched but not installed or needed. No WABT installation was edited.

### Validation and reproduction

```
python port/tools/host64_bench.py
python port/tools/host64_dispatch_test.py
python port/tools/host64_selftest.py --negative-controls
python port/tools/host64_selftest.py --optimized --negative-controls --out build/host64-selftest-optimized
```

All pass. Dispatch tests run both automatic small-group promotion and explicit hot selection,
checking selected and unselected targets, mutated table entries, a copied instance with a different
bias, an unknown native target, equal hash bytes at another address, wrong type, null,
table-size/UINT32_MAX indices and callee stack-depth exhaustion. Preparation remains idempotent. The
ordinary and optimized full self-tests retain the four generated OOB traps, three physical guard
faults, mirror/span/copy checks, all exact float bits, fiber state and the three required omission
controls. No src/ or include/ edits; no game, visible window, game data, downloads or new
dependencies.

Project code uses these patterns; no third-party implementation is vendored. Online references
consulted: [WABT SIMD support](https://github.com/WebAssembly/wabt/blob/main/README.md), [Clang
strict FP](https://clang.llvm.org/docs/UsersManual.html#controlling-floating-point-behavior),
[Binaryen options and distribution](https://github.com/WebAssembly/binaryen).

This tracked finding preserves the complete decisions and final numbers.

Round-3 implementation commits: c76da086 (representative kernels), 922c3f7a (guarded dispatch,
post-call size facts and safety checks), 4c64ecd2 (tuned codegen, interleaved two-run measurement,
explicit hot/cold targets).

## Round 4: native executable and generated boundary (2026-10-24)

The phase-4 executable scaffold now links and runs an independent fake module headless. This proves
the native boundary and runtime; a real-game boot, renderer run and frame-state comparison are still
pending. The work started with a fast-forward to port/portable-core (9b1ac1ad), bringing W2/W3/W4/W6
into W7.

### Generated imports and reverse calls

`port/tools/gen_host64_imports.py` parses all 54 declarations in `port/host/host_api.h`, checks the
actual WABT 1.0.42 import declarations, and emits `imports_gen.c`, `exports_gen.h`, and
JSON/Markdown ABI reports. Only the module's actual imports get wrappers. The checked-in generated
C/header under `port/host64/` are reproducible fake-module examples, not a fixed production import
list. The builder always generates fresh copies in its output directory; the launcher includes that
output header before source include directories. Unknown names/namespaces, scalar signature
mismatches, unparsed declarations, missing callback tables and callback C-signature drift fail the
build.

The wrappers call the actual host functions. Explicit uint32_t guest addresses pass unchanged. C
pointer arguments become bounds-checked GUEST() pointers; file, path, directory, sound and
scalar-output pointers have explicit span policies. Returned environment strings are copied into a
monotonic guest arena at 0x07000000-0x07800000, with stable addresses and an exhaustion trap.
`host_stack_ranges` marshals native uintptr_t pairs to wasm32 pairs, rather than copying eight-byte
host pointers into guest structs. The exported-function table is generated with exact WABT
prototypes; main() with either zero arguments or wasm32 argc/argv is supported (the latter receives
0,0).

Fiber callbacks use the exported indirect-function table, validate the 256-bit WABT type hash, and
call the entry's actual module_instance. Creation validates the index immediately; invocation
validates it again so mutation remains safe. Null, out-of-range and wrong-type callbacks trap. Guest
callback arguments remain 32-bit values. Native OS callbacks cannot silently be cast to wasm
functions.

Decisions/limits: asynchronous module callbacks (`host_thread_detach` and `host_heartbeat_start`)
are explicitly rejected in the generated ABI report. They need a protocol for the module's shared
mutable wasm stack pointer; a native thread calling that stack concurrently is unsafe. The remaining
52 synchronous API wrappers compile in the generator test. That compile is not a behavioral test of
all 52. `host_memory_regions` rejects native ranges not representable in 32 bits, and
`host_heartbeat_eip` calls the native service but returns 0 (unknown), not a truncated native PC.
The normalization-only diagnostic needs a wider ABI before it can enumerate the whole native
process. Directory/file/time and the synchronous fiber paths are the current native integration
boundary.

### Build and launcher

`port/tools/build_host64.py` accepts a .wasm, a wasm2c .c, or split .c units (module name core), and
produces dhd64.exe with clang-cl and lld-link. Installed WABT/wasi-sdk/SIMDe are read in place, with
no downloads. All 36 W6 host units compile for x86_64-pc-windows-msvc and are archived in
host64.lib. The fake exe selects the real host_win32 and sound_out_win32 objects; it does not
exercise the unused graphics, controller, movie or networking objects. A future module using
controllers also needs an x64 SDL3 library (--sdl-lib).

Native/generated code uses O3, x86-64-v2, Zen 3 scheduling, strict FP, contraction-off, zero
automatic locals, wrapv and no strict aliasing. No LTO, PGO, force-link, missing-function stubs or
bounds-check removal. Every WABT memory template is passed through prepare_wasm2c.py, including
split core-impl.h. The installed WABT and matching sources are unchanged. COMDAT/archive selection
keeps unreferenced legacy host/module dependencies out of the fake executable.

The old fixed-address child-process launcher is replaced: x64 reserves an ASLR linear-memory arena
itself; host_mem_reserved reports ranges inside it, and the legacy relaunch function is never needed
by memory initialization. EE wasm stacks remain 64 fixed 256-KiB slots at guest
0x06000000-0x07000000; native C frames stay on Windows fibers. The legacy shell's custom
security-cookie handler is renamed in its x64 object so it cannot pull the entire i686-era shell
into an otherwise independent link. The CRT retains its real cookie handler. Native diagnostics log
host varargs directly and name native device threads.

Reading runtime/memory.c also exposed three missing low RAM views. The allocator now installs six
aliases of one 32-MiB section: the existing complete RAM views and low views at 0x00400000,
0x20400000 and 0x30400000, section offset 4 MiB and length 28 MiB. The address adapter folds these
aliases too. Spans crossing the 4-MiB starts or 32-MiB ends retain their exact virtual byte
sequence, and copy operations preserve source-snapshot semantics. Ordinary/optimized safety tests
pass; synthetic performance parity was not remeasured after this change.

### Validation

```
python port/tools/host64_imports_test.py
python port/tools/build_host64.py --fake --num-outputs 2 --run --out build/host64-split-current --host-library build/host64-exe/host64.lib
python port/tools/host64_selftest.py --negative-controls
python port/tools/host64_selftest.py --optimized --negative-controls --out build/host64-selftest-optimized
python port/tools/host64_dispatch_test.py
```

PASS: 36 host units compile, PE32+ AMD64 executable links without unresolved symbols, and the fake
main returns 0. Its 20 generated imports exercise a 12-byte build-directory file read, monotonic
time, memory views/commits, complete and low RAM mirrors, boundary-spanning loads at both low-view
edges, fiber creation and two yields, guest local-stack placement/preservation, stack-range
marshaling, environment-string return, and the real sound ring's reset/write/tick calls. Sound
submission is exercised, not audible device output. DHD_HEADLESS=1 and DHD_SND_OFF=1 are set for
every builder-run executable. Four boundary negatives catch buffer overrun and
invalid/null/wrong-type callbacks. Split-output and externally supplied split-C paths both return 0.
Generator controls reject seven invalid interfaces and repeated output is byte-identical. The
ordinary and optimized existing self-tests retain all mirror/span/float/fiber/guard checks and all
three omission controls; guarded indirect-dispatch mutation/type/foreign-instance tests also pass.

### W5 real-module attempt

Tested size: 325,913 bytes; SHA-256
b1c2b6ad0735d7355db855ac860d26bc5cf05ae19cb944d05d7b555d2e3aa786. WABT translated it into 16 C
units, but the boundary validator correctly refused all seven imports below. None is an
env/host_api.h service. The generated header also has no main export. No module code or W5 files
were edited, and no substitute WASI implementations were introduced.

| Namespace | Import | wasm parameters -> result | Status |
|---|---|---|---|
| wasi_snapshot_preview1 | clock_time_get | i32, i64, i32 -> i32 | unsupported namespace/API |
| wasi_snapshot_preview1 | fd_close | i32 -> i32 | unsupported namespace/API |
| wasi_snapshot_preview1 | fd_prestat_dir_name | i32, i32, i32 -> i32 | unsupported namespace/API |
| wasi_snapshot_preview1 | fd_prestat_get | i32, i32 -> i32 | unsupported namespace/API |
| wasi_snapshot_preview1 | fd_seek | i32, i64, i32, i32 -> i32 | unsupported namespace/API |
| wasi_snapshot_preview1 | fd_write | i32, i32, i32, i32 -> i32 | unsupported namespace/API |
| wasi_snapshot_preview1 | proc_exit | i32 -> void | unsupported namespace/API |
| exports | main | absent | launcher entry missing |

Build contents stay local and are not included in commits.

Sources: the installed WABT 1.0.42 templates, wasm-rt.h and generated headers; local host_api.h,
runtime/memory.c and W6 host census. Online primary references: [WABT embedding and
callbacks](https://github.com/WebAssembly/wabt/blob/main/wasm2c/README.md) and [WABT funcref
definition](https://github.com/WebAssembly/wabt/blob/main/wasm2c/wasm-rt.h). No external
implementation was copied or vendored.

Implementation commits: b0be6916 (low aliases), f507ae42 (generated boundary), f08f1c70 (builder,
launcher and proof).

Final boundary hardening: a40d14b1 rejects a missing expected WABT type hash before comparing it;
the fake run and generator controls pass again.

### W5 refreshed artifact (same round, supersedes the first attempt)

W5 replaced the diagnostic artifact while this report was being finalized. A second actual build
attempt translated the refreshed 1,865,374-byte artifact (SHA-256
0794f30ee1313cffc093f63bdb42e3ce472774737680fa47caf44eab067db2fa) into 16 C units and again failed
closed at ABI validation. The WABT header contains 735 function imports: all 44 declared host APIs
match their scalar signatures, with no mismatches. The other 691 imports are outside host_api.h. The
complete per-import table is build/host64-real-latest/imports.md and is included in the local
executable audit. No stubs were added.

| Import group | Count | Result |
|---|---:|---|
| env functions declared by host_api.h | 44 | matched |
| env functions outside host_api.h | 675 | unknown; module/link boundary unresolved |
| wasi_snapshot_preview1 functions | 16 | unsupported namespace/API |
| Total | 735 | 691 unmatched; real executable not linked |

Unknown env imports include unresolved game functions/C++ names, _ri0/_rix register helpers,
library/runtime functions and host services that are still outside the boundary header
(graphics_feed, host_console_init, pacing helpers, update/splash and others). The refreshed artifact
exports __main_argc_argv rather than main. The launcher now accepts either generated entry
signature, passing guest argc/argv as 0,0; this export alias is therefore not a blocker for the
refreshed artifact. The earlier seven-import table remains a record of the first artifact only. The
synthetic proof and passing controls remain the executable result.

## W7e: host boundary completion (2026-10-24)

The latest W5 artifact was read again and copied to build/w7e/w5-latest/input.wasm for a stable
audit. SHA-256: 0794f30ee1313cffc093f63bdb42e3ce472774737680fa47caf44eab067db2fa. It still has 735
function imports: 44 existing contracts match and 691 do not. The complete table is generated by
port/tools/host_boundary_audit.py into build/w7e/classification.md (also appended to the local
executable audit).

The first audit (artifact 0794f30e, 735 imports) found **204 (a), 439 (b), 48 (c)**. W5 rebuilt the
module twice during this round. Classification: **8 (a), 62 (b), 63 (c)**
(build/w7e/classification-0417.md, from port/tools/host_boundary_audit.py).

The cause: W5 renames definitions to retail names but leaves these references mangled. The eighth is
rc_func_0023E288, a recompiled function the i686 link has and the module lacks.

(b), runtime: 20 names that the i686 build itself never has. link.py links each of them as a fatal
hle_missing stub (build/port/link/missing.c): the 16 libmpeg mpc.o _ri0_*/_rix_* routines, kCopy,
kFindAddress (libkernl), and the SDK functions func_0025A650 and func_0025F940 (syscall 0 and mtsa
have no translation). The module must match i686 exactly: the same named fatal stub. They are not
wasi-libc, libc++abi or host services, and recompiling them would *change* behaviour against i686.
Next come 22 port_* FPS entries (hle/fps.c), 16 WASI syscalls and clock() (wasi-libc),
ps2_fp_trap_enable and ps2_x87_checkpoint (runtime/fp_trap.h inline wasm definitions), and
host_pad_script (hle/autopad.c). The earlier artifact's __dynamic_cast, exception-handler, sha256
and state_trace imports are gone: W5 resolved them.

Each has a host_service_* (or host_console_write) entry in host_api.h. The exceptions are
host_movie_start/host_movie_update, which are now module code in hle/cdvd.c (ISO lookup), calling
host_service_movie_prepare/start/update. An automated check maps every (c) row to a header
declaration.

The 48 services now cross host_api.h through address-only contracts. New entry names are
host_service_*; guest callers use GADDR explicitly. GS calls select the host's GS instance, never
pass a module Gs pointer. NetRoomSettings and NetRange are decoded field by field from wasm32
records; nested strings/ranges are checked in generated wrappers. All new buffer/string arguments
have explicit generator span policies. The module formats wasm debug varargs and passes bytes to
host_console_write; the i686 safe formatter keeps its existing behavior.

Wall-clock pacing (work samples, deadline handoff and presenter wait) lives in host.c. The module
publishes its period and field, and retains the virtual clock and game-state FPS algorithms.
Scripted input and its keyboard mapping belong to the module: host_pad_script/host_pad_read moved to
autopad.c; hle/keyboard.c is pure code and may be compiled in both the separate module and native
host. Native x64 diagnostic threads read published scalar clock/register snapshots; they never call
the module to obtain them. Capture names are computed on the module stack before native
presentation. i686 retains its original capture path.

### Asynchronous callbacks and timing

The generated thread-detach wrapper gives a native producer only immutable module
identity/index/argument. It enqueues under an SRW lock; it cannot enter wasm or mutate its stack
pointer. host_callbacks_drain runs at kernel_poll and the INTC wait boundary, detaches a finite FIFO
batch and runs every job on its own host fiber/256-KiB guest stack. It resumes the draining fiber
before deleting the job's fiber. Reentrant drains are suppressed. Table identity, bounds, null and
the full type hash are validated again at invocation. Exhaustion traps; no dropped callbacks or
forged success. Heartbeat producers post the same way. The x64 heartbeat PC is unknown (0):
suspending the owner while it drains its own heartbeat would deadlock. A stalled module cannot drain
its heartbeat; native crash/watchdog reporting uses the last published state independently.

On i686 host_callbacks_drain is a no-op and the existing detached C threads and 30-second heartbeat
keep running on their own native stacks. Thus no i686 job was rescheduled to a different game field.
The sound output thread only consumes PCM and the transport threads update native queues; neither
invokes module code. The VU/GS record_packet callback did call module tracing from a worker. It now
copies each packet into a host FIFO. After the exact existing graphics_wait fence, the module
replays the unchanged packet hash operation, in order. A pointer-free GS prefix snapshot supplies
the unchanged trace serialization; no native function pointers cross it. Presenter pacing no longer
calls module code from its native presentation thread.

Open x64 limitation: online_menu.c's two detached jobs, hash_iso (SHA-256 of the whole ISO) and
fetch_list (blocking HTTP lobby list), are module code. On x64 they are posted and run inside
kernel_poll on the owner thread, so the menu stalls for the hash or the request. That is correct (no
module code on a native thread) but not responsive. The fix is two native async services, so the
blocking part runs on the host thread and only results cross: `host_service_file_sha256_start(path)`
/ `_poll(out32) -> 0 busy, 1 done, -1 failed`, and `host_service_net_list_lobbies_start(max)` /
`_poll(out, max) -> count or busy`. The module would poll them from page_update, as it already polls
list_state and iso_state. Not done here: x64 cannot run the game yet. i686 is unaffected (native
threads, as before).

### Exact W5 build_wasm.py follow-up (not applied here)

Against W5 04958b62 / artifact 81658567:

2. The 20 i686-stub names: put them in the abi_stage stub set, so stub_source
   emits the named trapping function. Derive the list from link.py's
   build/port/link/missing.c (or its census.json `undefined` list minus D_/.L/rc_),
   never from a hand list:
   `stubs |= i686_missing_names() - defined` next to `stubs = set(REFUSED) - defined`.
   A call then ends the run with that name, as hle_missing does on i686.
3. Game units call ps2_x87_checkpoint (patches.py and recomp.py output) without
   including fp_trap.h. Add `'-include', os.path.join(PORT, 'runtime', 'fp_trap.h')`
   to the game-unit and recomp compile flags, so the wasm inline no-ops apply.
   W7 changed hle/kernel.c to include it instead of using a local extern.
4. clock(): link
   `os.path.join(WASI, 'lib', 'wasm32-wasip1', 'libwasi-emulated-process-clocks.a')`
   after libc.a in wasm_link, and compile with -D_WASI_EMULATED_PROCESS_CLOCKS.
6. Canonicalize the seven _Z references above to their retail names, the same way
   as their definitions. Continue the recompile fixed point for rc_func_0023E288.
7. Keep game_data.c's export_name("port_disc_validate"). The generator rejects
   a module that imports host_service_host_disc_ready without it.
8. C++ runtime: no modern libc++abi import remains. If one appears, link
   WASI/lib/wasm32-wasip1/noeh/libc++abi.a and libc++.a after the objects. The
   retail __dynamic_cast is old libgcc tinfo2.o (0x00200A08) and must stay retail.

Sources consulted: the repository's config/libmatch.txt and symbol_addrs.txt;
https://github.com/WebAssembly/wasi-sdk/blob/main/README.md and
https://github.com/WebAssembly/wasi-libc (SDK/sysroot and C++ archive ownership). No external source
code was copied. No dependency was downloaded.

Additional boundary details: hle/vif0.c shares the proven VIF parser with vif.c under DHD_VIF0_ONLY,
excludes VU1/GS code, and is called immediately by wasm DMA for unit 0. Native x64 vif.c refuses
unit 0 and contains no references to module ps2 state; i686 retains its combined parser. The
module's VU0/VU1 register view and trace obtain fenced native VU1 state through fixed-width
snapshots. The wasm32 ps2_cpu prefix is 1892 bytes, its pointers are at 1892/1896 and cop0 is at
1900 (verified with clang record layouts). The 2096-byte wire snapshot zeroes pointer words and
copies the 196-byte cop0/control/P tail separately from the native struct. No eight-byte pointer or
native tail offset is copied as data. The VU0 64-qword view is returned through a separate 1024-byte
guest span.

Native keyboard code consumes the published rate on x64. On i686 its original FPS calls remain; i686
presenter pacing also retains the original live period query, including a rate change during pad
handling. Native fullscreen setup calls host_dpi_aware directly, and x64 widescreen/flat-limit
values are published by the module just before presentation.

The disc chooser's synchronous validation is a typed reverse export, port_disc_validate(u32 path,u32
error,u32 capacity)->u32, implemented by the existing module game_data_read. Generated native glue
copies the path/error through guest buffers and checks the owning OS thread. Native game_data_read
supports only the chooser's NULL ELF output; an unexpected request for a host ELF pointer traps. A
module importing the disc chooser without this exact export is rejected by the generator. W5 must
preserve the export_name attribute from game_data.c in its mirrored source; no additional
export-list guess is needed. The native watchdog now returns a pending-dump flag and the module
calls kernel_dump itself at the original boundary, eliminating another reverse call.

### W7e validation (final tree, 2026-10-24 04:33-04:42)

- `python port/tools/link.py` (LLVM from scoop on PATH): exit 0, dhd.exe
  rewritten at 04:35:49 (sha256 0fb10be1...). 23 undefined symbols, all stubs, as before.
- `state_trace_run.py boot --name w7e-boot`: 600 rows, exit 0. Then
  `trace_columns.py mg-boot.tsv w7e-boot.tsv --ignore ram,cpu`: **PASS** (every
  column other than ram/cpu identical: hw, vu0, gs_stream, gs_regs, gs_vram, sound, ...).
- `state_trace_run.py race --name w7e-race`: 5000 rows, exit 0. Then
  trace_columns against mg-race.tsv: **PASS**.
- `host64_census.py`: 36/36 units, 0 errors. `build_host64.py --fake --run` (eight
  boundary negatives, posted-callback drain), `host64_imports_test.py` (all 136
  wrappers compile, nine generator rejections, determinism) and
  `host64_selftest.py --negative-controls`: all pass.

Corrections in the takeover: the DHD_PACING_LOG field column is written after the vsync increment
again, as jit_wait did. kernel.c includes runtime/fp_trap.h instead of a local extern, so wasm gets
the inline no-op. The audit tool now takes i686's hle_missing stub set from link.py's missing.c. The
earlier w7e trace files were discarded; the ones above come from this final tree.

## W7f: indirect calls with mismatched types (2026-10-24)

Finding 081's open issue. The decompiled game calls some function pointers with a type other than
the target's. MIPS and i686 run the call, wasm traps on it.

**Which targets are reachable** (port/tools/wasm_indirect_audit.py, from the index
operand's producer, the data image's code fixups and the GCC 2 vtable layout):
- 19,400 sites are the inlined MMIO hooks `ps2_intc_read`/`ps2_graphics_sync`, with constant
  targets of the right type; 31 more use a constant index. 19,432 sites are proved safe.
- 262 vtable calls (`load16_s` delta at off-4, pfn at off) reach the code fixups at that slot.
- 301 recompiled dispatch sites reach any code fixup (959 functions).
- 85 callback sites are conservative.
- 1,603 (call type, target type) pairs are possible, 97 with evidence. The largest:
  - C virtual calls of 1-4 arguments into recompiled `(i32 x8)->i32` methods;
  - recompiled jalr `(i32 x8)->i32` into void C functions of fewer arguments.
- 831 vtable functions are reached by more than one call type, so replacing one entry per
  function can't work.
- The table also holds 501 of build_wasm.py's ABI thunks: fixups.c declares every
  data-image function `void(void)`, so vtables point at `f__as__v`, which passes 0 for
  every argument.

**Fix: port/tools/wasm_postlink.py**, a post-link rewrite of the module:
- one shadow table per call type, the size of table 0;
- every call_indirect moves to its type's table: only the table-index immediate changes, in
  the same LEB width, so a matching call runs as before;
- slot i holds table 0's definition (thunks unwrapped) when the types match, otherwise an
  adapter, or a clone for small prefix-compatible targets;
- null and out-of-range indices still trap;
- table 0 is unchanged for the host.

Adapters follow i686 cdecl stack slots:
- i32/f32 are one slot by bits, i64/f64 two; extra slots are dropped;
- missing slots are 0, a documented choice: i686 reads build-dependent stack garbage;
- a float read from an int or void callee is the x87 empty-stack real indefinite
  (0xFFC00000, measured on i686);
- f32/f64 results convert as x87 stores do.

- 16 shadow tables and 22,917 adapters, of which 3,180 are clones; 6,793,601 bytes;
- the static census, at wasm level and on the wasm2c C, finds **0 sites without a
  compatible entry and 0 that can reach another type** (20,265 C sites including clones);
- the prepared C (112.6 MB) compiles for x86_64 in 255 s to a 44.8 MB object; the unfixed
  module takes 180 s and gives 28.8 MB.

**Proof:** port/tools/test_wasm_postlink.py runs one C file natively for i686, and through
wasm2c unfixed and fixed:
- all 13 mismatched kinds trap unfixed and match i686 fixed, on 49 compared words and 20
  documented choices;
- the control and the null entry are unchanged.

**Alternatives rejected:**
- Per-entry adapters in table 0 are invalid (above).

**Speed** (host64_bench.py, 2 runs, all 54 bit patterns equal to i686):

| kernel | i686 | postlink | fpcast |
|---|---:|---:|---:|
| dispatch | 11.469 | 9.883 (0.862) | 12.931 (1.127) |

- Tuned dispatch without the fix is 0.855.
- The new mixed-arity kernel is 1.042 cold, 1.001 with its targets hot-promoted. The
  adapter is fully inlined; one call-depth check remains.

**W5 hook**, in build_wasm.py main() right after `write(os.path.join(OUT, 'link.txt'), link_txt)`:

__import__('wasm_postlink').postlink(WASM, WASM, eax=i686_eax_table(), report=os.path.join(OUT,
'postlink.json'))

It must run before wasm2c and raises if a type trap remains. Passing the eax table the ABI stage
already computed saves a second objdump pass. Optional: don't make `__as_` thunks for address-only
references.

Sources: WebAssembly core spec (binary format: tables, element segments); WABT 1.0.42 wasm2c
templates; binaryen FuncCastEmulation (github.com/WebAssembly/binaryen); Intel SDM vol. 1 (x87 stack
fault, masked response); i386 cdecl.

## W7g: the first real 64-bit run (2026-10-24)

dhd64.exe now links strictly from the real module and boots headless with the state trace. It runs
to **field 21 of 600**. Fields 1-20 match the i686 baseline (mg-boot.tsv) in every compared section.
The boot stops on a W5 build_wasm.py gap (below).

**1. Function-pointer imports.** host_api.h gives the module guest-safe contracts:
`host_callback_fiber/_detach/_fault_guard/_memory_regions/_heartbeat(fn, ctx)`. fn is
`HOST_CALLBACK(f)`: the address on i686, the table-0 index in wasm. ctx is a guest value
passed back as the first argument. Also `host_stack_ranges_copy(out, max)` (u32 pairs) and
`host_service_gs_capture_request(name)`. Module callers: kernel.c, graphics.c,
state_trace.c, online.c, online_menu.c, autopad.c. i686 adapters sit beside the natives in
host_win32.c and capture.c. On x64 the generated glue validates the table entry's full type
- fibers on fiber_x64.c slots at 0x06000000;
- posted jobs and the heartbeat drained at kernel polls;
- the fault guard inside the faulting access on the owner thread.

The generator refuses the six native-only names. The old host_stack_ranges wrapper leaked bridge
arena on every call; that wrapper is gone.

**2. Committed memory.** guest_memory.c commits only:
- the module region from HOST64_MODULE_BASE (0x70100000);
- the root shadow stack;
- the RAM views;
- EE stack slots, each with a reserved guard page;
- the bridge arena, page by page as it is used;
- what the module asks for through host_mem_commit (scratchpad, 0x10000000, 0x11000000,
  0x12000000).

Everything else stays reserved. A last vectored handler reports any access to it with the guest
address and the callers' RVAs (host64_rva.py) and ends the process. It is committed above a reserved
null page. host64_selftest.py --negative-controls checks the policy and passes.

**3. Executable.** `build_host64.py <module> --num-outputs 8 --sdl-lib <x64 SDL3>`: 133 generated
imports, no stubs, 34 MB. The x64 SDL3 static library was built from the already-unpacked 3.4.18
source with sdl3.py's options (no download). --sdl-lib now adds SDL's Windows libraries.

**4. Boot.** `state_trace_run.py boot --exe build/w7g/exe/dhd64.exe` runs it with the unchanged
recipe (it has --exe, so no host64_trace_run.py was needed). Stops and fixes:

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | exit 71, no message | wasi-libc environ start-up `_Exit(EX_OSERR)` on the adapter's ENOSYS; also no trap reporting (jmp_buf never armed, fibers can't longjmp to main) | module `getenv` = host_getenv, read live as on i686; `WASM_RT_TRAP_HANDLER=host64_trap`; getenv results interned in the arena |
| 2 | `state-trace: cannot write` | wasi-libc opens files only below a preopen; there was none | preopen fd 3 "/", host paths unchanged, files from fd 4 |
| 3 | write to guest 0 in pads_begin_field | module wrote the renderer's `gs_native_view` (undefined there, so 0) | `host_service_gs_native_view` |
| 4 | read of guest 0 at field 21, func_00202388 | `D_00279468 __attribute__((section(".data")))` not rewritten by build_wasm.py, so 0 in the diagnostic link | **open, W5** |

trace_columns.py mg-boot.tsv against the 20 complete x64 rows, `--ignore ram,cpu`: menu, phase,
clock, scratch, hw, vu0, gs_stream, gs_regs, gs_vram and sound are all identical. ram and cpu differ
from field 1 (ignored). Its FAIL is the row count only (600 vs 20). So no section has a first
differing field yet.

**build_wasm.py changes needed (W5).** The objects hold 104 references to 81 data names that are
never rewritten. The diagnostic link resolves each to 0, which the strict link reports and the
module hides:

| Pattern | Refs |
|---|---:|
| short names (`D_2DC8`) | 53 |
| `extern "C"` declarations in C++ | 41 |
| recompiler `A_D_…` labels below VUTEXT (jump tables) | 6 |
| function-pointer declarators | 3 |
| `__attribute__` after the declarator | 1 |

Changes:
1. extern_statements: skip only `extern "C" {` blocks.
2. DECLARATOR: allow a trailing attribute and function-pointer declarators.
3. Short names and the code-region labels: addresses from the image symbols i686 uses.
4. Fail the build on any undefined data symbol.
5. Also: drop `--stack-first` (or keep the stack at [0, 1 MiB) knowingly).

**Validation (final tree).**
- link.py: exit 0, dhd.exe 06:58.
- state_trace_run boot and race + trace_columns vs mg-boot/mg-race `--ignore ram,cpu`:
  both **PASS**.
- build_host64.py --fake --run: 8 negatives.
- host64_imports_test.py: 137 wrappers compile, 16 controls.
- host64_selftest.py --negative-controls: PASS.

Noted, not changed: the WASI adapter's path_open truncates whenever O_CREAT is set (fopen "a").
postlink reports `eax_constants_used: 0` although 84 i686 eax constants were found.

## W7h: full-column integration (in progress, 2026-10-24)

`codeids.wasm_code_rows` derives trace identities from this module's table 0 and name section,
excluding null and unwrapping address-only ABI thunks. It applies i686's C symbol prefix before
hashing. `host_code_range` in the generated native boundary returns `[1, table.size)` and writes two
checked guest u32 spans; native uintptr_t outputs would overwrite the wasm caller's four-byte
variables. All 139 host API wrappers compile; generator rejection controls pass with the current W7g
fake fixture (`host64_imports_test.py --build build/w7g/fake`). `host64_selftest.py
--negative-controls` passes every positive and omission control.

### Exact W5 build_wasm.py follow-up (not edited here)

1. Include a generated codeids.c object in `all_objs`. Initially use
   `codeids.write_table([], [], path)`, link, derive rows with
   `codeids.wasm_code_rows(WASM)`, regenerate/compile/link until the generated
   text is unchanged. Run wasm_postlink only after this fixed point. This
   resolves dhd_code_count/table and dhd_data_count/table; diagnostic address
   zero is invalid for those objects. The local strict link exits 0.
2. Handle numeric local labels `.L[0-9A-Fa-f]{8}` in the same path as absolute
   data labels, including recompiler alias `A__L...`; preserve their literal
   retail value below the data-image range. Example: `.L001FE0F4` is absolute
   0x001FE0F4 in i686's map, not an unresolved function/data import. The local
   generated-source workaround in rc_func_001FE050 is
   `#define A__L001FE0F4 ((char *)0x001FE0F4u)`.
3. `I686_ABS` values are already guest addresses. Do **not** pass them through
   `data.absolute` again. Use one helper at both `rewrite`'s macro value and
   `fixup_source`'s operand value (the current two `data.absolute(data_address(...))`
   sites):

def guest_data_address(name): if name not in IMAGE.labels and name in I686_ABS: return '0x%X' %
I686_ABS[name] return data.absolute(data_address(name))

4. In `rewrite`'s function-pointer branch, change
   `cast = '(%s %s)' % (base_name, ptr)` to
   `cast = '%s %s' % (base_name, ptr)`. The body format supplies the cast's
   parentheses already. The extra pair produces invalid expressions such as
   `(*((void (**)(int)))(unsigned)0x829DA94)`. It excludes 00215B68.c,
   002013F8.c and 00231628.c from the wasm build; the fallback recompilation
   is not a substitute for their working i686 C. The title-sequence x64 run
   crashes in rc_func_002185C0, which belongs to that excluded 00215B68 unit.
5. Generated preprocessor directives must start on a new line. For example,
   `extern "C" { #undef D_00412510` in 0016EB40.cpp is invalid. Prepend a
   newline to the replacement when the extern declaration starts mid-line.
6. Preserve the original numeric-data macro after transform's merged-unit
   alias cleanup. 00187438.c has a later `#undef D_003F52D0`; after the extern
   became a numeric macro, that undef removes its only definition, breaking
   later functions. Restore the first numeric macro after a cleanup undef
   when it is not followed by another definition of that name. The local
   wrapper implements this on generated text, without modifying transform.py.
7. Append `codeids.wasm_fixup_source(DATA_OUT/fixups.c)` to the generated
   codeids.c object at each identity-link iteration. `state_trace.c` uses
   its explicit function-relocation addresses on wasm. Scanning fixup values
   by the table-index range incorrectly adds **49 absolute scalar words**
   (examples: D_52 = 82, D_79 = 121) to the code-word list. That zeros real
   RAM data in the hash. Function provenance gives 2,437 fixup words, matching
   i686, rather than the erroneous 2,486. Raw GPR dumps now accompany dump
   fields to expose value-only classification collisions without masking them.
8. Remove the placeholder `obj/data/codeids.c.o` when the real generated
   identity object is supplied. With `--allow-multiple-definition`, leaving
   the placeholder first silently selects its zero counts even though the
   strict link succeeds. The local wrapper now filters that placeholder.
9. Canonicalize C++ **definitions and references**, including after ABI source
   rebuilds, before `symbol_kinds` and the recompile fixed point run.
   `cpp_linkage` labels prototypes but leaves unprototyped definitions such as
   `_Z13func_00235488P5Queueiii` mangled. The retail name is then recompiled
   although the C++ unit compiled successfully. At field 25 the fallback's
   16-byte array cookie shifts the heap; i686's C++ QueueItem array uses a
   4-byte cookie. EE 0x0039E598 is 0x088A0414 on i686 and 0x088A0420 in the
   faulty module; the first heap chunk is 0xE4B1 versus 0xE4C1. This is a real
   state divergence, preceding the later null object at the title sequence.
   The local compile hook calls `wasm_retail_names.rename(obj)` on every
   successful C++ compilation, including ABI rebuilds. This performs the
   i686 `link.rename_cpp` symbol policy on wasm import strings, linking
   symbols and debug names. Code/data and function indices stay byte
   identical. Compatible same-object import/definition aliases merge symbol
   records; relocation symbol indices follow that merge, with offsets and
   addends unchanged. Incompatible prototypes get distinct `base__as_SIG`
   imports and explicit adapter requests for the ABI stage; they are never
   retargeted to a function with another wasm type. LLVM objcopy cannot redefine wasm symbols (it rejects the
   operation), hence the small format-aware helper.

`test_wasm_retail_names.py` links a synthetic C++ definition, mangled C import, namespace data and a
function-table entry to a C companion. The unrenamed link fails; the renamed module executes through
wasm2c and returns 48 exactly as native i686, including compatible merged aliases and an
incompatible prototype that requires an explicit adapter. Code/data sections are unchanged;
rerunning the rename is byte-idempotent. No dependency was installed. Array cookie source consulted:
LLVM clang/lib/CodeGen/ItaniumCXXABI.cpp,
https://clang.llvm.org/doxygen/CodeGen_2ItaniumCXXABI_8cpp_source.html . Object linking/relocation
format: https://github.com/WebAssembly/tool-conventions/blob/main/Linking.md .

Merged-unit typed views also require replacing a self-reference such as `#define D_003F52D0
(*(Global_89120 *)&D_003F52D0)` with its numeric guest address. The earlier extern was a real
variable on i686; after becoming a macro its self-reference is deliberately not recursively expanded
by the preprocessor, leaving an undeclared identifier. Local generated text uses `(*(Global_89120
*)((void *)(unsigned)0x083F52D0))` and restores the canonical Game view after the cleanup undef. All
game units except the two known variadic wrappers now compile (3,813/3,815).

The original baseline has another gap: EE 0x004D029C holds func_00206E40 at field 1 but is absent
from the learned pointer-word list. Both maps prove the same identity: native 0x4012AC80 versus wasm
table index 0x31. The base RAM hash includes that native address as raw data, so it cannot equal the
wasm hash with the current list. The unmodified reference comparison remains the acceptance result;
supplementary comparisons are explicitly separate diagnostics.

### Correction to finding 081's field-150 interpretation

Fresh i686 link (exit 0; exe SHA starts 05063649f286671f) matches base-boot in all columns except
cpu from field 150. The field-150 CPU difference at byte 28 is **a serialized data-symbol offset**,
not `$at` lane 1. GPR records have variable length: raw = five u32 words, symbol = six. GPR[0] takes
20 bytes; GPR[1] begins with tag 2, id 0x073718BD (`_deferred`), offset at byte 28. The current
`_deferred` start 0x4148E5D0 plus offset 0x003B5E74 equals 0x41844444. The old offset 0x003B6284
plus old start 0x4148E1C0 gives the same 0x41844444. Its buffer really is over 4 MiB, so the float
falls inside it. The writer is rc_func_0022B708: `lui $at,0x4184; ori $at,$at,0x4444; mtc1 $at,$f2`.
Thus the runtime creates a constant float, not a host pointer. Value-only identity classification
mistakes its bits for an address inside the renderer snapshot. Preserving the baseline hashes
preserves this collision; removing that classification changes the reference's CPU serialization.

Research: WebAssembly Core Specification, tables and element segments,
https://www.w3.org/TR/wasm-core/ . No external code copied or downloads made.

### W7i: cross-build trace protocol and what W5 still has to take

Merged port/portable-core d4d233a7. The merged build_wasm.py builds 3815/3815 units with no
wasm-only recompiled functions.

1. The doubled PS2_RAM_BASE (item 3 above) is still present, now at three sites:
   `rewrite`'s value, `fixup_source`'s operand, **and the new `keep_macros.self_alias`**
   (`data.absolute(data_address(m.group(1)))`). Use `guest_data_address` at all three.
3. Link with `--table-base=0x12100` (73984). The state trace classifies a GPR lane or a
   learned code word as a code identity *by value*. With the default base 1, every
   small integer (8, 0x14, 0x85) is also a table index, so wasm tags plain integers as
   code where i686 (code at 0x40xxxxxx) hashes them raw: the cpu column differs from
   field 1 and code from 25 (EE 0x0093C870 = 8 in both builds). The range
   0x12100..0x13C84 contains one round number (0x13000). Cost: (base+7045) funcrefs
   x 32 bytes x 18 tables (wasm_postlink's shadow tables copy table 0's limits)
   = about 47 MB of untouched, zero-filled virtual memory. Postlink rows are
   index-aligned and its segments start at the first used index, so nothing else changes.
4. Every wasm link needs `dhd_code_fixups` (state_trace.c's wasm branch).
   codeids.write_table now emits an empty one under `#ifdef __wasm__`, so the placeholder
   link works; the real table needs `write_table(..., wasm_fixup_source(fixups.c))`.

Host side: host.c (host library) now calls `state_trace_watch_spec`, which is module code in host64;
bridge.c defines the host copy (same rule). `host_code_range` is no longer imported (the wasm trace
takes code words from the fixup list); `host_service_gs_native_view` goes through the generic scalar
glue.

**The comparison against base-*.tsv cannot be exact in ram/cpu for any second
compiler.** The reference hashes raw i686 values that depend on i686 code generation:
- host C-stack addresses (fixed EE thread stacks at 0x06000000) that the game keeps:
  `&local` stored in a global by func_00187048 (EE 0x003E3C34/38), and registers spilled
  by recompiled libc printf (rc_func_00254D68 chain) below the guest sp;
- an uninitialised read in matching source: func_00158680 passes `int zeros[3]` as a
  Vector* to func_00209618, whose 16-byte COPY_VECTOR takes w from the frame slot after
  the array (i686 `0xC(%esp)`, never written: residue such as 0x0829CCF8). It lands in
  D_004D5DE0[k].pos.w;
- i686's own value collision (float 0x41844444 tagged as _deferred data, field 150) and
  the unlisted code word 0x004D029C (W7h).
Protocol used instead: both builds run with the same settings, `--codeptrs
--env DHD_STATE_GPRS=1`; trace_columns.py without --ignore on that pair; cpu proven per
settings still equals base-* except the known cpu word, so the settings account for the
whole reference delta. state_trace.c's `DHD_STATE_NORM=fibers` (ram/scratch/hw/cpu only)
and `DHD_STATE_GPRS` are opt-in; default hashes are unchanged.
6. **f32 constants as double literals (race field 1360).** WABT 1.0.42 writes some f32
   constants as 9-digit decimal literals with no `f` suffix (`var_f2 = 6.10351562e-05;`,
   437 in the module). Under the host build's `-ffp-model=strict`, clang converts that
   double to float at run time, in the dynamic rounding mode. In the EE mode (toward zero)
   2^-14 becomes the float below it: one ulp off in func_00222B68's random range for 17
   objects. Fix (committed): `prepare_wasm2c.exact_f32_literals` rewrites each such
   constant as the exact hex float of its round-to-nearest value (WABT's 9 digits round-trip),
   and `build_host64.py` applies it to every unit.
8. **EE float rounding in the wasm build.** Native float multiplies and the helpers' float
   add/sub round in the run-time mode (toward zero on the EE). wasm clang folds constant
   operands in round-to-nearest, though. Race field 1443: `0.48888889f * 60.0f` was folded
   to 29.333334, where i686 computes 29.333332. The wasm shim (`shim_ee_float`) should
   compute fadd/fsub/fmul as an exact double result rounded toward zero by bits (DAZ
   inputs, FTZ output, FLT_MAX on overflow), and `fmul float` should be rewritten to
   `ps2_native_fmul` like the other ops. fdiv keeps its run-time double division (i686 runs
   it in the EE mode too) behind an empty asm barrier against folding. Checked natively:
   bit-identical to SSE at MXCSR 0xFFC0 on 2 x 20M random pairs, with the host in either mode.
   Open: the shim's double helpers assume round-to-nearest, but host64's EE fibers run
   toward zero.
10. **Uninitialized frame read in func_001AE358** (race field 2953, not a host64 bug).
    `func_00221AB8(matrix, &c, basis)` reads 4 rows from `&c`, one local Vec. Rows 1-3
    are whatever each compiler puts after `c`: on i686, a never-written slot, a spill, blend
    and a temp. The camera's angles.x (CameraAngles xA0) then differs per build. The fix
    belongs in the source (c, d, e and a 4th row in one array, as GCC laid them out). W7i
    masks the 10 xA0 words in its cross-build traces.
11. **Invalid bitcasts.** A same-unit alias with another return type than its definition
    (00228BE8.c: `int func_0022C350__c11(...) asm("func_0022C350")` vs the `unsigned char`
    definition) is called through wasm-ld's `.L<name>_bitcast_invalid`, which traps (x64 race,
    field 3249). The build should fail on any `_bitcast_invalid` in dhd.map; build_wasm.py
    declares that alias `unsigned char`.
    - Boot, 600 fields: every column identical except cpu, which differs from field 29.
      w7i-cpu-proof proves 599 fields. Field 99 r2 lane 3 is func_00158680's uninitialized
      pos.w (masked in RAM).
    - Race: x64 completes 5000 fields. Every column is identical through field 3431, except
      ram from 2968 (camera) and cpu (classification).
    - i686 check: with the base's codeptrs (codeptrs-b3.txt), boot and race equal base-* except
      cpu from field 150 (the known $at word).
    - Speed, race recipe with the trace off (DHD_STOP_FIELD=5000, headless, one run at a time):
      i686 221 s, x64 317 s (x64 1.43x slower).

## Linux host (2026-10-24)

The same runtime now runs on Linux x86_64; aarch64 paths exist but are untested (no device). Windows
behaviour is unchanged: the split keeps every Windows line, and the Windows self-test still passes
in both modes.

### Layout

- `guest_memory.c` is shared policy only (views, commit-on-demand,
  fault-on-uncommitted, allocate/grow/free orchestration). The OS mapping is
  behind `guest_memory_os.h`: `guest_memory_win32.c` (the moved Windows code)
  and `guest_memory_posix.c` (new). The policy cannot drift between hosts.
- `guest_memory_posix.c`: one 8 GiB `PROT_NONE` reservation (`MAP_NORESERVE`,
  so it charges no swap), the 32 MiB RAM section from `memfd_create`,
  aliased at all six guest views with `MAP_FIXED|MAP_SHARED`; commit is
  `mprotect` (preserves contents like `MEM_COMMIT`); release is `munmap`.
  `SIGSEGV`/`SIGBUS` on an alternate stack (`sigaltstack`) report the guest
  address plus a `backtrace_symbols_fd` trace, restore the default disposition
  and re-raise, so the process dies with the signal as an unhandled fault
  would. Foreign addresses re-raise without reporting.
- `fp_mode.c` needed no change (already MXCSR/FPCR). `selftest_host.c` checks
  protection against `/proc/self/maps` and has FPCR expectations on aarch64;
  `launcher.c` reports traps with `backtrace_symbols_fd`; `diagnostics.c`
  uses `pthread_setname_np`. `bridge.c` and `memory_access.h` are portable
  as written.
- `host64_selftest.py --linux` (the default when run on Linux; run it from
  inside WSL) builds with clang/ELF and checks guard deaths by `SIGSEGV`
  instead of `0xC0000005`. Everything else, including all three negative
  controls, is the same test.

### Bounds-check default (the one Linux-only fix)

`wasm-rt.h` defaults Linux to guard-page mode (`WASM_RT_MEMCHECK_GUARD_PAGES`), which compiles the
explicit OOB checks out and relies on WABT's own mmap allocator plus its signal handler to convert
faults into traps. Our mirror allocator replaces both, so the first `bounds()` access died with a
real `SIGSEGV` instead of trapping. Linux compiles therefore pass
`-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1`, which keeps every generated OOB access a trapping check. Any
future Linux host64 link (e.g. `build_host64.py`) needs the same define; Windows keeps its proven
default.

### Toolchain and validation

```
python port/tools/host64_selftest.py --negative-controls             # Windows
python port/tools/host64_selftest.py --optimized --negative-controls # Windows
python3 port/tools/host64_selftest.py --linux --negative-controls    # WSL
python3 port/tools/host64_selftest.py --linux --optimized --negative-controls  # WSL
```

All four pass: 4 generated OOB traps, all exact float bits, 3 fibers, 3 guard faults, cleanup, and
the no-mirror-fix/no-sp/no-depth controls. The fiber checksums (`13fe05c0`, `c673d5c0`, `78e9a5c0`),
lowest locals (`0603a580`, `0607a580`, `060ba580`) and every float-bit line are identical on Linux
and Windows.

Sources: the installed WABT 1.0.42 `wasm-rt.h`, wasm2c templates and `wasm-rt-impl.c` (Apache-2.0,
read in place, not vendored); glibc/POSIX `mmap`/`memfd_create`/`sigaction` documentation. No
external implementation was copied; the minicoro-style switch was written fresh, so no licence
applies. No game data, no new Windows dependency.

## Linux headless (2026-10-24)

The first headless Linux x86_64 `dhd64` that boots the real game and writes a state trace:
`build/host64-linux/dhd64` (41 MB, clang, WSL Ubuntu), 600/600 boot fields, exit 0. Recipe unchanged
(`port/tools/state_trace_run.py boot --linux`, `DHD_HEADLESS=1`, `DHD_FRAME_CLOCK=1`, same
env/ISO/NAV); only path spellings adapt to the module's WASI layer (below).

### Module choice

Reason, checked by import-set comparison (`wasm-objdump -x -j Import`): the w5 set equals the module
behind the reference x64 boot trace (`w7i/exe5`, 134/134 matched, `imports.json` clean) exactly,
while the newest w7 wasm is currently unbuildable on every host: it newly imports
`env.state_trace_field/init/pcm`, which no host provides yet (w7's own newest attempt fails the same
way: `w7i/exe7/failure.log` reads "unmatched imports"). That refactor is w7's in-flight work, not
Linux-specific, so the fallback the task names is the right module: any trace delta against ref3 is
then host-caused by construction.

### Build fixes (all keep Windows byte-identical)

- `gen_host64_imports.py --linux`: the Linux `game_data_read` prologue
dropped the shared body lines (unbalanced braces, `function definition is
not allowed`). Fixed by dedenting the body to the shared level; the Windows
(`linux=False`) output is still byte-identical to the committed generator
(stash-compare, `cmp` clean).
- `port/hle/keyboard.c`, `port/hle/vif.c`: four `#if defined(_WIN64)` /
`#elif` / `#if !defined(_WIN64)` guards that select the host64-native branch
(module's published rate, `GUEST()` views, abort on the unhosted VIF unit)
now read `#if defined(_WIN64) || defined(__x86_64__)`, the convention
`host.c` already uses. Without this, Linux compiles the in-module branch
and the link fails on direct game references (`port_fps_high`,
`func_0012DF48`, `D_0029E3D0`, `ps2`, `ps2_vu0_run`). i686 is unaffected
(both macros false there: same branch as before). `host64_census.py` stays
36/36 with 0 errors; the Windows full-trace run below re-verifies x64.
- Headless gamepad is a null backend (`gamepad_start` 0,
`gamepad_vibrate`/`gamepad_read` no-ops); rumble/headless input stays
autopad-driven. `host_posix.c` file I/O matches `host_win32.c` and the
`host_api.h` contract write-for-write (`r+b` without `TRUNCATE` fails on
missing files on both, per the contract).

### The one Linux-specific runtime behaviour: WASI strips `/`

The first boot died at once: `state-trace: cannot write ...tsv`, exit 3. Instrumenting
`host_file_open` (reverted afterwards) showed the module's in-module wasi-libc `__wasi_path_open`
shim delivers `tmp/dbg-trace.tsv` for a `/tmp/dbg-trace.tsv` prefix: the leading slash is stripped
(preopen- relative resolution), while the disc open (via `port_disc_validate`, not wasi) keeps its
absolute path. The layered fix, without touching the recipe or the module:

- `DHD_STATE_TRACE` is passed as a plain relative name (`--env
DHD_STATE_TRACE=w8-linux-boot` overriding the absolute default after the
recipe sets it); the recipe runs with cwd = the trace dir, so the `.tsv`,
`.ptrs.tsv`, `.gprs.tsv` and `-f*` files land exactly where
`state_trace_run.py` reads them (rows counted, exit path unchanged).
- `DHD_STATE_MASK`/`DHD_STATE_CODEPTRS` are passed as relative names the
same way (`--mask "" --codeptrs ""` neutralises the absolute defaults,
`--env` supplies the names); the files are byte copies of
the W7i protocol (same bytes, `DHD_STATE_NORM=fibers`, `DHD_STATE_GPRS=1`)
is unchanged.

### Trace results

Linux `dhd64` sha256 `b800db18fe61bdec...`, 600 rows in 162 s, exit 0, 164 masked words
(`w8-linux-boot.tsv`; x64 oracle below is w7 `exe5/dhd64.exe` sha256 `c2ef0551e37bf926...` run
through the same w8 recipe on Windows).

- `trace_columns.py base-boot.tsv w8-linux-boot.tsv`: menu/phase/clock/
scratch/vu0/gs_stream/gs_regs/gs_vram/sound identical; ram from field 1, hw
from 25, cpu from 28, code from 1. The baseline
`base-boot.tsv w7i-ref3-x64-boot.tsv` reports the identical four
first-fields, so every i686 delta is the documented second-compiler delta
(§W7i), none of it Linux-specific.
- `trace_columns.py w7i-ref3-x64-boot.tsv w8-linux-boot.tsv`: everything
identical except ram from field 28 and code from field 1. Both are version
drift, not Linux, proved two ways: (a) the field-1 code-word set in the
Linux `.ptrs.tsv` (2615) is a strict superset of ref3's (2504) with zero tag
diffs on shared words — the 111 extra words are exactly the newer
`w7i-codeptrs.txt` (1465 vs 1354 listed; the file postdates the ref3 trace);
(b) a fresh 28-field x64 run of `exe5` with the same mask/codeptrs bytes
agrees with Linux on all 15 columns (incl. `combined`, `stackwords`) for
all 28 rows, while disagreeing with ref3 at the same two spots.
- `trace_columns.py w8-x64-boot.tsv w8-linux-boot.tsv` (fresh exe5,
600 fields, same inputs): **all columns identical, PASS** — zero
Linux-specific differences over the whole boot. Linux runs are
deterministic (600-field boot vs 28-field dump agree on rows 1-28).

(b) Two early attempts died in `recomp.py write()` with `OSError EINVAL`, Win32 1224
(`ERROR_USER_MAPPED_FILE`) on different files each run: transient host AV/indexer holds on freshly
written `.c` files, not a repo bug; a rerun passes. The newest w7 wasm still needs host
`state_trace_*` (all hosts, not Linux).

### Newest-module follow-up (this round): w7 wasm boot on Linux

The w7 `dhd_core.wasm` refreshed at 12:52 (6,227,768 bytes, the module `w7i/exe7` built at 12:54;
that dir holds a 33 MB exe and no `failure.log`) generates cleanly here: 134/134 imports matched, no
unmatched-import error, so this round used it instead of the w5 fallback. Binary
`build/host64-linux-w7/dhd64` (41.7 MB), a separate out dir so the w5 build in `build/host64-linux/`
keeps running untouched.

Three fixes on top of the committed Linux host, all Windows-invisible:

- `port/hle/host.c`: a POSIX `host_pad_vibrate` in the existing `#else`
branch (same dedupe and `DHD_VIBRATION` rule, XInput tail omitted;
`gamepad_vibrate` is already a no-op there). Without it the link fails:
the import handler `host_service_host_pad_vibrate` is live.
- `port/tools/build_host64_linux.py`: compile with
`-ffunction-sections -fdata-sections`, link with `-Wl,--gc-sections` —
the `/Gy` + `/opt:ref` mirror. The host keeps legacy guest-extern
helpers (`D_*`, `func_*` in `fps.c`) that only some builds reference;
GNU ld without GC fails where lld-link silently drops them.
- `port/host/host_posix.c` `host_file_open`: if a relative open fails,
retry with `/` prepended. Second sighting of the §WASI-`/` strip, now at
the layer that fixes every module-opened absolute path (trace `.tsv`,
mask, codeptrs) instead of one recipe variable: the shim eats exactly
one leading slash, so the retry only fires after the plain open already
failed and cannot change any working open.

Run: `state_trace_run.py --linux boot --name linux-boot --exe build/host64-linux-w7/dhd64` (stock
recipe, absolute defaults): 600/600 rows in 148 s, exit 0, exe `c0d2e70ca38c61f6...`.
`trace_columns.py` first-differing-field table (all pairs 600x600):

- `base-boot.tsv` vs `linux-boot`: ram 1, cpu 28; rest identical
(incl. code).
- `w7i-x64-boot.tsv` vs `linux-boot`: ram 1, cpu 1, code 1; rest
identical.
- control `base-boot.tsv` vs `w7i-x64-boot.tsv` (both Windows):
ram 1, cpu 1, code 1 — the exact linux-vs-w7i pattern.
- same-host module control `w8-linux-boot.tsv` (w5) vs `linux-boot`
(w7): ram 1, hw 25, cpu 28, code 1; rest identical.
- cross-platform cross-module `w8-x64-boot.tsv` (exe5) vs `linux-boot`:
ram 1, hw 25, cpu 28, code 1 — the exact same-host pattern.

Conclusion: zero Linux-specific differences. Every differing field is explained by module drift
between the w5 and w7 builds (w7-owned; note `base` agrees with the newer module on code and on cpu
to field 28). The longer w7i runs (`w2` 1453 fields, `w3` 2955) diverge from field ~195 in
menu-driven sections — different run shape/route, present regardless of platform, not compared
field-for-field here.

Windows gates verified this round on the same tree: `host64_census.py` 36/36 with 0 errors; i686
`python port/tools/link.py` exit 0 (`dhd.exe linked ..., still undefined: none`). Both need the
documented local-only inputs staged next to the tree (`asm/`, `assets/vutext.bin`, `iso/SCUS_971.77`
per `port/tools/build_clean.py`; all untracked/gitignored, nothing committed): the earlier "cannot
run" was missing local data, not a code blocker.

## Static hazard census (W8c, 2026-10-24)

Every race divergence so far came from one of six construct classes that i686 tolerates and wasm
does not (w7h-boot.md sections 9-21). The census finds every remaining instance statically instead
of one race at a time. Detector: `port/tools/wasm_hazards.py` (stdlib only; run `python
port/tools/wasm_hazards.py all --abi-table <abi_thunks.tsv> [--postlink <postlink.json>] [--w2c
<dir>] [--tsv out.tsv]`). Status is `handled:` with the covering fix, or `open:` with what is
missing.

Research: WebAssembly tool-conventions linking format (object symbol reading), WABT wasm2c output
shape, MSVC dumpbin COFF disassembly. No external code copied, no game data committed.

| class | open | handled |
|---|---|---:|
| void_value (void def, caller reads a value; sec. 9, 14) | 28 | 20 |
| falloff (non-void ends without return; sec. 19) | 2 | 1 |
| byval_sret (aggregate across a disagreeing boundary; sec. 20) | 9 | 1 |
| f32_fold (decimal literal under the wrong rounding; sec. 16, 17) | 386 | 0 |
| frame_pun (own frame read as another type; sec. 21) | 49 | 0 |
| variadic (fixed decl of a variadic def; sec. 15) | 0 | 49 |
| arity supplement (fewer/more fixed args; sec. 14) | 0 | 740 |

Method notes. void_value: the 48 ABI-table `return 0` rows plus a source-level cast/declare scan (no
new inflate-shaped casts). i686 eax at every open callee's ret is dynamic — verified by following
cross-object tail calls through the link map (e.g. func_001C33D0 → func_00201B68 loads 3 extra
objects, still dynamic) — so each thunk's 0 differs from i686 garbage by an unpredictable value.
`postlink.json` confirms `eax_constants_used=0`: no thunk carries a proven constant.

New since section 20: seven more by-value AnimationHandle (16 bytes, `include/animation.h:68`
asserts the size) definitions in `src/unsorted/00202E00.cpp`
(func_002038E0/00203B68/00203C28/00203D18/ 00203F40/00203FE0/00204170) with 0 by-value against
15/10/29/0/0/1/1 pointer-or-scalar external declarations (`Vec *`, `Key *`, `Handle *`, `void *`).
Same invisibility as section 20: the wasm types coincide, so abi_stage cannot see the disagreement.
One call site shows the shape: `00126B20.cpp:74` passes `&(Sound copy)` where the callee reads 16
struct bytes — i686 reads stack words starting with the pushed pointer, wasm reads the Sound bytes.
Per-caller verdicts are w7's job; the rows carry the declaration counts.

Reachability. The first track hits the 16 P0 rows below (already fired in a w7 boot/race run) plus,
by construction, the 14 recomp-dispatch void_value rows (the value reaches GPR(2) of recompiled MIPS
the race executes) and any AnimationHandle caller on the race path (triage per row). Menu-only and
unreached paths need the per-row caller unit in the TSV; the 48 general frame_pun candidates are
mostly triage.

| # | class | function (site) | fix |
|---|---|---|---|
| 1 | void_value | func_002189F8 (`0021A5C8.c:223`, `r = func_002189F8(...)` result used; variadic C def is void) | return the real value like func_00241E70, or prove the use dead |
| 2 | void_value | func_0023E188 (`recomp/func_0023E288`; sret Color + missing arg + return-0 triple) | same SCALAR_ABI wrapper as #6 |
| 3 | falloff | func_00119790 (`00119790.c:6`) | w7i FALL_OFF: declare func_001177F8 `void *`, `return` its value |
| 4 | falloff | func_00117828 (`00117828.c:6`) | same FALL_OFF pattern |
| 5 | byval_sret | func_00161868 (Color by value; race field 2953) | w7i SCALAR_ABI: take `unsigned int`, rebuild the union |
| 6 | byval_sret | func_0023E188 (Color sret) | static body plus `unsigned int` wrapper |
| 7 | frame_pun | func_001AE358 (`001AE358.cpp:60`, `&c` into 4-row reader) | pass the row array (src change) or keep the mask |
| 8 | f32_fold | func_0011D488 (`0011D488.c`; field-1443 writer) | exact-hex f32 literals + RZ-by-bits shim + `fmul`-rewrite |
| 9 | f32_fold | func_00222B68 (`00222B68.c`; field-1360 writer) | same float fix |
| 10 | f32_fold | func_002224E0 (w2c top literal count; race-set reader) | same float fix |
| 11 | f32_fold | func_00161868 / func_001AE358 / func_001B5DE8 / func_00209618 / func_00230258 / func_00236088 (race-set float terrain) | same float fix, writer-first |
| 12 | byval_sret | func_00203C28 (3 by-value vs 29 pointer external decls) | scalarize the boundary like #5, or align every caller to by-value |
| 13 | byval_sret | func_002038E0 (0 vs 15) | same as #12 |
| 14 | byval_sret | func_00203B68 (0 vs 10; `00126B20.cpp:74` passes `&Sound`) | same as #12, check the first word is used as a pointer |
| 15-20 | void_value | 14 recomp-dispatch rows (e.g. func_00148560 ← func_001B40A0, func_0022A168 ← func_0022B708, func_001C6A80 ← func_001B40A0) | per-callee i686-eax proof (all dynamic so far) or a real return; each value reaches GPR(2) |

The 14 src-CHECK void_value rows (e.g. func_00123350 ← `00122E68.cpp:31`, the field-1360 writer
unit; func_00190188 ← `00187438.c:479`) follow behind the table: same thunk-0-vs-garbage shape,
caller-by-caller verdicts. f32 remaining: 25 top w2c functions plus ~360 raw-lane units in the TSV;
frame_pun remaining: 48 candidates with the read stride in evidence.

## W15: the windowed x64 "stall" at "Preparing the graphics..." (2026-10-24)

**There was no deadlock.** An lldb all-threads backtrace of the minimised dhd64.exe 15 s after
"Preparing the graphics..." (`lldb -b -o "process attach --pid N" -o "thread backtrace all"`,
relinked with `/debug` for public symbols) has the game thread in `gs_vsync -> gs_gpu_init ->
shader_compile -> D3DCompile`, still compiling raster.hlsl. The SDL thread pumps events, the splash
thread waits in GetMessage, no thread waits on a host event.

The cold compile is just long: `main` (cs_5_0) 15-17 s, `hardware_pixel` (ps_5_0) 9 s, the rest
milliseconds; "Ready" at 27 s. The same in a standalone x64 or i686 program, with the default or the
EE MXCSR (0xFFC0), so not an x64, fiber or FP-mode problem (the EE mode only changes the bytecode a
little: 162560 vs 162476 bytes for `main`). The shader cache
(%LOCALAPPDATA%\DownhillRevived\shaders, gpu.c) keys on d3dcompiler_47.dll's size and date, and
dhd64.exe loads System32's copy where dhd.exe loads SysWOW64's: the first x64 start never has a
cached shader, even on a PC where dhd.exe has run. With a warm cache "Ready" comes 0.01 s after
"Preparing the graphics...".

**Two fixes.**
- 613641de: since 44bf55d2 SDL3 owns the window, and the x64 SDL3 library the package linked
  (build/x64-sdl3, copied from w7g) was built with SDL_VIDEO off, so dhd64.exe logged
  "gs: SDL video did not start" and ran with no window at all. sdl3.py now builds any target
  (`python port/tools/sdl3.py --x64`: build/port/sdl3/3.4.18-x86_64, link.py's options), and
  package_x64_test.py uses that library unless --sdl-lib is given.
- gpu.c logs each shader it compiles and how long it took ("gs: compiling shader main
  (cs_5_0), not cached yet..."), so a first start is no longer taken for a hang. Cache hits
  print nothing.

With a warm cache the same nav step comes inside 30 s. Against base-boot both differ the same way
(ram from field 1, cpu 28, code 25), the module-side cross-build differences above (table base,
stack words), not host ones. i686 (gpu.c is shared): link.py exit 0 (23 stubs), boot trace equals
base-boot except cpu from field 150.

Not done: compiling `main` and `hardware_pixel` on two threads would bring the first start from
about 27 s to about 17 s; precompiled bytecode in the exe would remove it.

## W17: the Linux x64 SDL3 window, input and audio (2026-10-24)

The Linux x64 build (WSL Ubuntu, `port/tools/build_host64_linux.py`) gets the SDL3 window, input and
audio, like the Windows build. There is no simple SDL present path for non-D3D11 hosts in the tree
(sdl3.py builds with SDL_RENDER=OFF, gpu_null.c has no device), so the window opens for input, sound
plays, and the renderer logs that it is missing.

### SDL3 for Linux x64

`python3 port/tools/sdl3.py --linux` (WSL only) builds the same pinned 3.4.18 source as Windows
(same URL/SHA-256, same OPTIONS: window, audio and controllers; no GPU/renderer/ camera) as a static
`libSDL3.a` under `build/port/sdl3/3.4.18-linux-x86_64/install`, with the backends CMake
auto-detects from installed -dev packages. Installed for this round, on top of what WSL already had
(X11, Wayland, PulseAudio, ALSA, dbus, xkbcommon): PipeWire, wayland-protocols, the X11 extras,
libudev, libusb, libdrm/libgbm (`libpipewire-0.3-dev wayland-protocols libxext-dev libxrandr-dev
libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxxf86vm-dev libxtst-dev libxinerama-dev
libudev-dev libusb-1.0-0-dev libdrm-dev libgbm-dev`; `libibus-1.0-dev` was already present). The
first configure hard-failed without XTEST (`libxtst-dev`); SDL's X11 check fails closed on it, so it
is installed too.

All backends are compiled in (`nm`: defined `X11_bootstrap`, `Wayland_bootstrap` (+preferred),
`OFFSCREEN/DUMMY_bootstrap`,
`PULSEAUDIO/PIPEWIRE(+preferred)/ALSA/SNDIO/DISK/DUMMYAUDIO_bootstrap`), but SDL dlopens the backend
libraries (configure logs `dynamic libX11 -> libX11.so.6`), so the installed `sdl3.pc` only names
`-L<install/lib> -pthread -lm`. The first version of the backend gate read `sdl3.pc` and failed
closed wrongly; the gate now reads the configure result (`SDL_X11`/`SDL_WAYLAND=ON` in CMakeCache).
System link libs still come from `sdl3.pc`.

### Builder wiring (`build_host64_linux.py`)

- Sound follows link.py's switch: `DHD_SOUND=sdl` at build time selects
  `port/host/sound_out_sdl.c`, else `port/host/sound_out_null.c`. SDL audio implies the SDL
  window/input (like `build_host64.py --sdl-lib` on Windows); `--sdl-lib/--sdl-include`
  give the library explicitly and default from `sdl3.linux_build()` under `DHD_SOUND=sdl`.
- SDL on: `port/host/sdl_host.c` joins the host units, and the five SDL users
  (`hle/gamepad.c`, `hle/text_input.c`, `gs/present.c`, `host/sdl_host.c`,
  `host/sound_out_sdl.c` — link.py's SDL_USERS) compile with `-DDHD_HAVE_SDL` and the SDL
  include. The static lib plus the `sdl3.pc` system libs join the link.
- `--sdl-libs` is one quoted string (shlex-split): argparse treats dash-led values as flags.
- Also added the missing `-DWASM_RT_MEMCHECK_BOUNDS_CHECK=1` (the §Bounds-check requirement;
  the committed script lacked it — without it the first `bounds()` access dies with SIGSEGV
  instead of trapping).

### Window without a renderer (`port/gs/present.c`, shared with Windows)

- The SDL window helpers (`WindowSpec`/`make_window`/`display_of`) move from the `_WIN32`-only
  region to shared `PRESENT_SDL` (after the includes and `sdl_window`; Windows behaviour is
  unchanged, same code). A first attempt placed them before the includes and broke the Windows
  build (`fprintf`/`hle_log` undeclared); the x64 control build below caught it.
- New `#elif defined(PRESENT_SDL)` `init()` for hosts without D3D11: opens the SDL window on
  the SDL thread, logs `gs: SDL window WxH (video DRIVER)` (from `make_window`) then
  `gs: no renderer on this host yet (headless scanout)`, and returns 0 — no swap chain, so
  `gs_vsync` keeps its bench path. `DHD_HEADLESS` skips the window entirely.
- `gs_pump_messages()` now runs before the `disabled` check, so the close button still ends
  the game with no renderer (a no-op otherwise).

### Proof (same module throughout, 6,227,660 bytes)

- (a) The Linux build links both ways: default `PASS: 134 generated imports; 35 Linux host
  objects (null audio, no window)`; SDL `PASS: 134 generated imports; 36 Linux host objects
  (SDL window/input, SDL audio)`.
- (c) Non-headless smoke of the SDL build (60 fields, exit 0,
  `SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy`, no WSLg window):
  `989snd: SDL audio output started (48000 Hz stereo PCM, backend dummy)`,
  `gs: SDL window 896x672 (video offscreen)`,
  `gs: no renderer on this host yet (headless scanout)`. No errors; `/tmp/w17-sdl-smoke.tsv`
  holds the 60 rows.
- (d) `python port/tools/link.py` exits 0 with the shared files touched: 3815/3815 units
  compile with 0 errors; `dhd.exe linked with 23 stubs; still undefined: none`; code table
  matches. Staged `asm/` (copy) plus `iso/SCUS_971.77` and `assets/vutext.bin` (same-volume
  hardlinks) from the sibling tree, read-only; all three are gitignored and left in place,
  none committed.

### Left for Linux (renderer)

- Presenting: `gs_vsync`'s non-native branch decodes into `pixels[]` but `resize()` returns 0
  on hosts without a device, so nothing presents. Either add an SDL3 streaming-texture present
  path (needs SDL_RENDER or raw texture upload — currently OFF/nowhere) or take the wgpu swap
  chain behind `DHD_RENDER`. The window, input and audio are ready for it.
- `gs_window_apply()` is a stub on Linux (DHD_WINDOW size/mode changes are ignored).
- Keyboard input needs window focus (`keyboard_focused`); fine interactively, absent under
  dummy/offscreen drivers (autopad/DHD_NAV still drive).

## W23: the shared object cache (2026-10-24)

- **Key.** SHA-256 of the compiler (resolved path, size, mtime, `--version`), the command
  line without the output path, the working folder, Python's version, the preprocessed
  source (the same command with `-E`, or `/E` for clang-cl), and for the two-step game units
  (build.py, build_wasm.py) the text of the tools that rewrite the IR between the steps (the
  builder itself and eefloat.py). Any change to a source, header, flag, compiler or rewriter
  gives a new key; nothing matches partially.
- **Direct mode.** Running `-E` for every unit costs about half a warm build. A manifest
  (`m/` in the cache) is keyed on the command and the text of the files on it (the source,
  `-include`). It lists every file the preprocessor read (its line markers) with that file's
  SHA-256, plus the paths on earlier `-I`/`-isystem`/`/imsvc` folders that must stay absent,
  because a header added there would shadow one that was used. If every file matches and no
  absent path exists, the key comes from the manifest without `-E`; otherwise the build
  preprocesses and writes the manifest again. The one case it misses is a new header next to
  an including header that shadows one found on the `-I` path (`#include "x.h"` searches the
  includer's folder first). `DHD_OBJCACHE_DIRECT=0` always preprocesses.
- **Writes and hits.** An entry is written to a temp file and renamed, so a parallel builder
  sees a whole object or none. A hit is a copy, not a hardlink: link.py (`rename_cpp`) and
  build_wasm.py (`wasm_retail_names`) edit objects in place after the compile, and the cache
  holds the object as the compiler wrote it. Compiler warnings are not stored, so a hit
  prints none (the builds compile with `-w` anyway).
- **Size.** Each build that stored something prunes at the end: over `DHD_OBJCACHE_GB`
  (default 30), the least recently used entries go (a hit touches its entry) until the cache
  is at 90%. `python port/tools/objcache.py stats` and `prune` do the same by hand.
- **Off.** `DHD_OBJCACHE=0` compiles everything and reads and writes nothing. Each build
  process prints one line: `objcache: N hits, M misses, K not cacheable`.
- **Comparing exes.** lld stamps the link time in the PE header, so two links of the same
  objects have different sha256s. `python port/tools/objcache.py pe-sha EXE` hashes the file
  with the COFF TimeDateStamp, the checksum and the debug-directory stamps zeroed.

### Proof (LLVM 22 clang, `build.py -j 12`)

|---|---|---|---|---|
| i686 build.py + link.py | 282 s | 204 s | 121 s (23 + 98) | 267 s (31 + 236) |
| i686 pe-sha | f589de8e | f589de8e | f589de8e | f589de8e |
| i686 hits | - | 5529 / 10298 | 10298 / 10298 | 10298 / 10298 |
| build_wasm.py | - | 487 s | 354 s | - |
| dhd_core.wasm sha256 | - | 9592bf65 | 9592bf65 | - |
| build_host64.py | 192 s | 156 s | 9 s | 9 s |
| dhd64.exe pe-sha | eb45118c | eb45118c | eb45118c | eb45118c |

- The cold i686 build already hits 5529 times: link.py's recompile fixed point compiles every
  recompiled function again in each round.
- Fixed on the way: build_host64.py built its `-ffile-prefix-map` flags from a set, whose
  order changes per Python process, so two builds of one tree never shared a key.

## W18: dhd64.exe CPU time (2026-10-24)

Goal: at least i686 speed, with the same behaviour.

### Tools

- `port/tools/sample_profile.py`: an external sampling profiler. Every tick it suspends the
  threads that used cycles, reads RIP/EIP (and RSP for `--callers`) and resumes them. Symbols
  come from llvm-symbolizer `--inlines`, which needs `build_host64.py --debug`. `--child`
  profiles i686 dhd.exe's relaunched child.
- `port/tools/bench_x64.py` (since merged into `bench_race.py`, finding 088 CL9): menu and race
  timing, one minimised run at a time. It uses
  state_trace_run's recipe with the trace off, the port's `DHD_BENCH` field timer and per-thread
  CPU ms per field. In `flat` mode (`DHD_FRAME_CLOCK=1`) the game runs unpaced.

### Where the time went (game thread unless noted)

- **Menu, base exe:** the VU1 thread spent its time in the VU1 interpreter. The words of the AOT
  blocks are filled by `ps2_vu_aot_load`, which is called from `ps2_image_load` inside the
  module, so the host's copy was never filled. `aot_find` also compared a host pointer against
  a guest address and never hit.
- **Race, after the host fixes:** w2c_core_run took 55% of the game thread. That is ps2asm.c's
  inline-asm interpreter: about 74% of its samples sit at the opcode switch, and every operand
  is decoded through linear memory. ps2_vu0_run took 10%, lookup 2% and ps2_asm 2%. By self
  time, i32 loads took 42% and `host64_memory_address` 12.8%.
- **Race, now:** ps2_vu0_run is 29% of the game thread. The VU0 microprograms at 0180, 08D0,
  0C50, 0CD0 and 0BD0 run on the interpreter (`DHD_VU0_STATS`); i686 interprets them too.
  graphics_wait and idle_wait now show up, so the game thread waits on the other threads part
  of the time.

### Fixes

| Commit | Change | Effect |
|---|---|---|
| 4a70c171 | The host loads the VU1 AOT words itself; `aot_find` compares `GADDR(words - pc*2)` | menu 10.5 -> 41.7 fields/s paced |
| e7505912 | `host64_memory_address` fast path for a span inside one non-alias 32-MiB window (proven equal to the full test over 207M cases) | part of the 37.8 -> 19.8 ms menu step |
| c45a25e7 | Plain global call-depth counter instead of TLS; `--debug`; parallel module compile | menu flat 32.1 fields/s |
| 6d59ea70 | WABT guard pages under clang-cl (`__clang__` added to the `__GNUC__` tests) instead of explicit bounds checks | menu game thread 21.1 -> 17.7 ms |
| f3027e27 | The module runs the native VU0 routines (`PS2_VU0_NATIVE`, word arrays from vu1_aot.inc, no microcode copied) | race game thread 46.2 -> 24.5 ms (with e5f39bcc) |
| e5f39bcc | Module port units built with the compiler builtins (memcpy, fabsf, sqrtf were wasi-libc calls) | see above |
| ee2f3b0d | Generated code for every inline asm template and recompiled COP2 instruction (`ps2asm_aot.py`, below) | race game thread 25.8 -> 11.4 ms |

**Inline asm AOT (ee2f3b0d).** ps2asm.c's per-instruction body is now `run_ins`, always inlined;
`run()` loops over it. At build time, `build_wasm.py` step 5a reads every `ps2_asm` and `ps2_exec`
literal in the transformed game units, headers and recompiled functions. That is 694 templates and
526 distinct instruction sequences. `ps2asm_aot.py` parses each one as `compile()` does (it reads
the opcode, kind and mnemonic tables from ps2asm.c), then emits one function per sequence. That
function calls `run_ins` on each constant `Ins`, so clang folds the decoding away and the
interpreter's own code runs without it.

At run time a program uses a generated function only when its parse is byte-identical
(`select_aot_program`, a memcmp of the `Ins` array). Any parser drift just leaves a template on the
interpreter. A native check that ran ps2asm.c's own `lookup` on all 694 templates matched 691; the
other 3 are 1 bad template and 2 empty ones. `DHD_ASM_GENERIC=1` still selects the plain
interpreter. The i686 build does not define `PS2_ASM_AOT`; for i686 only the `run_ins` refactor
changes.

### Trace results

- **Every module change (f3027e27 + e5f39bcc, then ee2f3b0d):** checked with a 600-field x64
  boot trace against my own pre-change x64 trace.
  - post2 re-traced with the current mask vs aot: identical in all 13 columns (PASS).
- **Host-side fixes (4a70c171, e7505912):** the fix2 race trace matched the pre-change trace up
  to field 4123, when it was stopped under the fewer-tests rule.

### Speed (flat = unpaced, minimised, same recipe)

| Build | Race fields/s | Race game ms/field | Menu fields/s | Menu game ms/field |
|---|---|---|---|---|
| i686 (own AOT link) | 69.5 | 8.0 | 35.3 | 8.4 |
| x64 pre2 (before the module fixes) | 15.4 | 46.2 | 31.1 | 18.3 |
| x64 post2 | 32.1 (36.3 in this run) | 24.5 (25.8) | 33.6 (38.8) | 11.4 (11.5) |
| x64 aot (ee2f3b0d) | 68.4 | 11.4 | 38.8 | 10.5 |

The paced menu went from 10.5 fields/s on the base exe to 41.1 after the guard pages (6d59ea70);
i686 0.2.8 gives 40.2. The x64 race now runs at i686 speed with the game thread at 11.4 ms per
field.

### Left

- Most of the remaining gap to i686's 8 ms is the VU0 micro interpreter (ps2_vu0_run, 29%).
  i686 interprets the same entries natively. A VU0 counterpart of vu1_aot.py's generated
  blocks, checked against the uploaded words like the native routines, would cover 0180, 08D0,
  0C50, 0CD0 and 0BD0.
- Each `ps2_vu0_run` memcmps up to 132 words of the native routines (w2c_core_memcmp, about 4%).
