# 083: Direct wasm core in Node (first web-route proof)

Status: partly superseded by 087 (the i686 core link, the JS renderer)
Checked: 2026-11-04

The browser route is current, rebuilt since: the core comes from `build_wasm.py` alone (no i686
link; the i686 traces below were the referee then, finding 088 retired that build), the graphics
side runs in its own worker on shared memory (finding 087 BS2), and the browser draws only with
the Rust renderer (`gpu.js` and the WebGPU compute rasterizer are gone, finding 088 CL5). Today's
build is `python port/web/build.py build/wasm/dhd_core.wasm`.

The core now runs directly in Node 24.21.0 with JSPI, without wasm2c, a browser, window, sound
device, or an ISO copy. The existing host GS/VIF1/VU1 C runs in a second wasm reactor sharing the
core's WebAssembly.Memory.

## Reproduction and input provenance

```powershell
python port/web/build.py build/web/input.wasm
node port/web/run_headless.mjs --trace build/web/proof-boot --env DHD_STATE_GPRS=1
```

SHA-256: `f87b5ac951517016a13ac624af8fef0a09bc66d630fdc83a74f8bddd7a77cf50`. This avoids making
another divergent game compiler driver. Input bytes are unchanged; the web postlink appends helpers
and changes instructions while preserving guest layout and table-index values. The builder writes
input/output hashes in build/web/manifest.json.

LLVM/MSVC from their existing locations build only the independent SSE oracle. No dependency was
installed. Node on this machine exposes WebAssembly.Suspending/promising both with and without
`--experimental-wasm-jspi`; the tested invocation needs no extra flag. Binaryen's `--all-features`
also enables compact imports, which Node 24 cannot decode; the builder explicitly passes
`--disable-compact-imports`.

The default recipe is state_trace_run.py's 600-field boot: virtual frame clock, headless, movies
skipped, memory card disabled, sound device off, native renderer selection, settings=none and
`DHD_NAV=0:triangle|cross`. Additional `--env NAME=VALUE`, `--fields`, `--module`, `--graphics`,
`--imports`, `--iso` and `--trace` arguments support isolated diagnostics.

## RAM mirrors and cost

port/web/postlink.py uses the existing wasm_postlink.py binary decoder, never a textual replacement
of generated game C. It changes 181,819 scalar/SIMD memory instructions and 351 bulk instructions.
Original functions and table entries keep their indices before optional Binaryen optimization; guest
pointer values and the code/data identity tables are unchanged.

The complete windows 0x28000000..0x29FFFFFF and 0x38000000..0x39FFFFFF fold to
0x08000000..0x09FFFFFF. Low views 0x00400000..0x01FFFFFF, 0x20400000..0x21FFFFFF and
0x30400000..0x31FFFFFF fold to 0x08400000..0x09FFFFFF. Non-RAM addresses stay unchanged. Effective
address addition is checked before folding, including memarg offsets and SIMD lanes. Crossing a
window boundary folds each byte separately. Bulk copy preserves a source snapshot even when distinct
virtual ranges physically overlap.

Reserved web-only scratch: FP mode word 0x07800000; scalar/SIMD boundary buffer 0x07810000; private
helper stack [0x07820000,0x07920000); bulk boundary snapshot [0x50000000,0x52000000). A
boundary-crossing bulk copy above 32 MiB explicitly traps; this is a documented implementation
limit, not a general wasm memory semantic. Atomics and memory.init fail the rewrite rather than
remaining silently unadapted. Neither occurs in the tested modules.

```powershell
python port/web/build_memory_test.py
node port/web/check_memory.mjs
node port/web/check_memory.mjs memory-fixture-shared-stack
```

The positive fixture checks all mirrors, low-view edges, non-RAM storage, unaligned scalar and
SIMD/lane boundary loads/stores, stored pointer bits, physical overlap, bulk boundary copies/fills,
effective-address overflow and OOB traps. The shared-stack omission control must fail its leaf-frame
canary. The fixture constructs an unpublished leaf frame explicitly; the installed compiler happens
to publish SP for the original C fixture.

The first tight 10M-iteration load/store loop measured 0.300 ns raw versus 12.454 ns folded (41.6x);
an O3 Binaryen trial measured 0.358 versus 9.985 ns (27.9x). This is substantial overhead, not a
performance win. Shared-machine reruns vary; the later private-stack fixture measured 0.267 versus
10.988 ns. The final optimized fixture passes the same checks at 0.314 ns raw versus 8.688 ns folded
(27.66x). The runner's whole-boot speed is the useful end-to-end result below. Further work should
inline a cheap canonical-address path without weakening bounds or boundary semantics.

## Fibers and inserted-call stacks

JSPI suspends host_fiber_switch and resumes a separately promising table callback. The scheduler
saves/restores __stack_pointer and FP mode on every switch. Root fiber 65 keeps its original stack;
fibers 1..64 get the lowest free 256-KiB slot beginning at guest 0x06000000, zeroed on allocation.
Their lowest page is left unused by convention; wasm has no per-page OS protection. Deletion refuses
the current/root fiber. Continuations are resumed via a microtask after the old wasm activation has
suspended, never recursively on the old C stack. The real boot creates seven fibers and deletes
five.

**A separate helper stack is required.** A wasm leaf can address locals below the global stack
pointer without publishing a new pointer. An inserted call that pushes onto that same stack can
corrupt its locals. Helpers therefore use a new mutable global with their own stack at 0x07920000.
They import no functions, cannot yield, and complete before any fiber/GS boundary, so this stack is
neither shared with game frames nor suspended across a switch. The field-143 investigation exposed
this with newly inserted double helpers: Traversal records acquired helper integer scratch values. A
private helper stack removes the corruption; the synthetic omission control independently checks the
hazard.

## Float modes, including raw f64 instructions

The rewrite covers raw f32 add/sub/mul/div/sqrt, demotion and signed/unsigned 32/64-bit integer
conversions, plus f32x4 arithmetic/sqrt/integer conversion. Helpers inspect the current CSR word.
IEEE host work keeps ordinary wasm RN; EE work implements RZ, DAZ, FTZ and native NaN handling by
bits. Existing game ADD/SUB guard-bit alignment and DIV/SQRT nearest-result helpers remain in the
input module; the raw instruction lowering models the surrounding native arithmetic rather than
replacing those PS2 policies.

Verification is against actual SSE instructions with MXCSR=0xFFC0, streamed through a binary Node
child-process pipe. Every output word is compared, not just hashes. Ten f32 operations each pass 20M
random input pairs: **200M exact results** (add/sub/mul/div/sqrt, f64 demotion, i32/u32/i64/u64
conversion). Seven supplementary f64 operations each pass 1M pairs: **7M exact results**
(add/sub/mul/div, i64/u64 conversion, sqrt). NaNs, infinities, denormals and overflow/underflow are
present in the uniform random-bit corpus.

```powershell
python port/web/build_float_test.py
node port/web/check_float_exact.mjs
python port/web/build_float_test.py --double
node port/web/check_float_exact.mjs --double
```

Its bit-rounded f32 helpers fix game arithmetic, but native wasm2c also executes raw f64 in the
active MXCSR mode. Direct wasm does not. First, HLE next_event had a one-cycle rounding difference,
visible in Timer 1 from field 25. Lowering HLE doubles fixed it. Then runtime/wasi-libc math
differed in RAM at 143 and GS packet floats at 150. Lowering original scalar f64 arithmetic, sqrt,
promotion and integer conversions, while keeping the inserted helpers' own arithmetic RN, fixes this
second class. There are 14,923 rewritten scalar f64 operations, 7,442 scalar f32 operations and 71
f32 SIMD operations in the core. The resulting pass models the **current module's native
execution**.

The census also finds 27 packed f64 arithmetic instructions and 31 packed f32-to-f64 promotions in
the input. Those are not lowered by this first pass. They occur in eight game functions and
sincos_980. The boot comparison checks their observable behavior for this workload; it does not
prove every packed double/denormal case. Extending packed-double lowering and its own random-bit
oracle is explicit remaining work before claiming general float-mode parity.

## Host boundary and GS

gen_imports.py derives signatures from host_api.h with gen_host64_imports.py's declaration/type
parser, compares them with actual wasm import types, and generates metadata for all 134 imports.
Unknown namespaces, undeclared services, native-only callback APIs and signature mismatches fail
generation. The JS runner binds only checked names. Unsupported headless services throw when called,
rather than reporting success. Offline device/presentation services are explicit no-ops; sound
mixing itself remains in the core.

Files use descriptor/position records with Node fs and BigInt i64 returns. Environment strings are
interned in the same 0x07000000..0x07800000 bridge arena used by the native host. Memory views are
refreshed after memory.grow. The host declines OS page guards, selecting graphics.c's existing
immediate readback fallback; it does not claim to protect wasm pages.

build_graphics.py compiles the existing gs.c, GS memory layout, VIF1 parser and VU interpreter using
wasi-sdk. Static data/heap begin at 0x40000000, disjoint from EE RAM and core data at 0x70100000.
The reactor shares core memory, but has its own C stack, allocator and function table. Its exports
implement the actual GS registers, GIF parser, VRAM transfers, snapshots and VU1 execution. The
core's wasm-only VU1 forwarding branch must be disabled in this **host** copy, or host_vu1_file
recursively forwards to itself. Generated copies only are changed under build/; original runtime/VIF
sources are untouched.

Host graphics executes in IEEE mode, VU1 temporarily selects EE mode, and the EE caller's mode is
restored afterward. Packet boundaries follow the native default GS worker, despite serial execution.
Empty feeds are skipped as the native FIFO's while(count) does. Otherwise GS stream hashes first
differ at 29 even though the final GS state agrees. Presentation and device output stay disabled;
the headless native path also never initializes a GPU rasterizer.

## Trace evidence

**Both final runs reached 600/600 fields, module exit 0.** The normal recipe is
build/web/proof-boot.tsv.

```text
rows: A 600, B 600
  menu       identical
  phase      identical
  clock      identical
  ram        differs from field 1
  scratch    identical
  hw         identical
  cpu        differs from field 28
  vu0        identical
  gs_stream  identical
  gs_regs    identical
  gs_vram    identical
  sound      identical
  code       identical
FAIL
```

The supplementary run is build/web/proof-protocol.tsv. That is 164 masked EE words and 2,614
code-address words (2,437 fixup words, 1,465 listed; overlaps deduplicated). No new mask or pointer
word was added. Invocation, in addition to the ordinary recipe:

```powershell
python port/tools/trace_columns.py build/state-trace/w9-x64-latest.tsv build/web/proof-protocol.tsv
```

Against that trace, **all 13 columns are identical in all 600 fields: PASS**, including RAM and CPU.
This is the direct-wasm versus wasm2c proof for the same input module. There is no remaining
web-specific trace difference in this boot.

A fresh i686 run (exe SHA starts 2df4aafc357d5edc) with the same protocol writes
build/state-trace/w9-i686-protocol.tsv. Against the web trace every column is identical except CPU
from field 29 (99 rows): FAIL.

```text
fields: 600; proven: 599; unproven: 1
register differences proven: 32 same raw value classified differently, 40 code identities, 0 data identities, 60 fiber-stack addresses, 132 zero-padded argument registers
cpu column rows differing: 99 (first 29)
  field 99: r2 A 00000000:0:00000000:0 00000000 00000000 0829CCF8  B 00000000:0:00000000:0 00000000 00000000 00000000
```

The latest x64 trace has the same result against i686. The original baseline still fails RAM/CPU as
shown above; the diagnostic PASS does not replace it. Full command outputs are
build/web/base-comparison.txt, x64-comparison.txt, i686-comparison.txt and cpu-proof.txt. Dumps at
25, 28, 29, 150 and 600 accompany both web proofs and the fresh native references.

**Speed:** ordinary web boot 1,074.068 s, **0.558624 fields/s**; protocol web boot 1,081.819 s,
0.554622 fields/s. A matched i686 run with the same GPR output and five dumps takes 37.933 s,
**15.8172 fields/s**: the ordinary web run is **28.31x slower**. W7 exe7's fresh protocol run takes
149 s (about 4.03 fields/s). These are headless end-to-end wall times including startup/ISO loading,
not just rendering or a warmed kernel. The two web proofs ran concurrently on the shared development
machine; no affinity or isolated performance claim is made. The large measured address-folding cost
is still a priority for follow-up.

Final artifacts: core 5,783,092 bytes, SHA-256
`424190d0c5f3431b8278419a5be62e4672cfd93bd38df00a2b9cad43a23f6d79`; host GS reactor 132,240 bytes,
SHA-256 `0c7f7fb87e4930f713d52d0b02e5d6778baad640c01dcb6545ac19ab38f98112`.

Independent controls were not changed: trace_columns.py, state_diff.py, state_trace_mask.txt,
state_trace_codeptrs.txt and the original baseline. The ordinary baseline result remains visible
even when it fails for known cross-compiler identities.

## Research and scope

JSPI API and boundary behavior: https://v8.dev/blog/jspi-newapi and V8's upstream
test/mjsunit/wasm/jspi.js,
https://chromium.googlesource.com/v8/v8.git/+/refs/heads/main/test/mjsunit/wasm/jspi.js . Binary
format decoding follows the existing wasm_postlink.py and the core specification:
https://www.w3.org/TR/wasm-core/ . Existing repository GS queue, VU and host ABI implementations
were reused, not reimplemented in JS. No external implementation was copied. Compiler-rt builtins
are linked from the installed SDK, not vendored. Browser engines, full races, online services, sound
devices, rasterization and per-page stack guards are not proven by this Node boot. The fixed web
scratch ranges and long-running GS heap limits need review before extending the proof to those
workloads.

## W9 performance follow-up (2026-10-24)

Decision: keep stored guest pointer bits unchanged. The game can store, compare and later inspect
uncached pointer values, so removing their ORs at creation is not a general bit-exact substitute for
access adaptation. The web postlink now proves literal effective addresses and canonical base
intervals instead. The interval test accounts for the full fixed memarg offset without wrapping;
large-offset indexed reactor globals are included. Unknown pointers and alias boundaries retain the
complete old checked fallback. 56,273 of 181,819 core accesses bypass adaptation statically; the
other 125,546 use specialized wrappers. No checker, baseline or pointer mask changed.

--debug-memory independently checks bypassed accesses through the alias map and traps if any byte
would fold. It first excludes overflow and widths >16; no alias window is shorter than 28 MiB, so
unchanged folded endpoints prove the complete single-instruction span. This checks both static
bypasses and dynamic fast branches. check_fast_memory.mjs rejects all five alias classes, an
unaligned alias boundary and a known-base effective-address overflow. The existing
mirror/SIMD/bulk/OOB fixture and private-stack omission control still apply. Contiguous bulk
transfers use wasm memory.copy/fill; spans containing an entire alias are explicitly excluded even
when both endpoints look canonical. The new whole-alias copy/fill control passes.

Float decisions: normal f32 sums with exponent gap <=24 are exact in binary64; larger gaps choose
the dominant operand or its magnitude predecessor directly. Exceptional inputs retain the old path.
f64 division uses RN followed by an exact 106-bit integer product comparison instead of 54
long-division steps. The unchanged SSE oracles pass 200M f32 and 7M scalar f64 results again.

JSPI already returns no Promise for a same-fiber request. The full baseline boot records 1,399 real
switches and zero same-fiber requests. It is not justifiable to change scheduling to remove those
real guest switches. --trace-off removes DHD_STATE_TRACE and stops at the requested host field
context; this is the beginning of the last field, whereas a traced run stops after its row. It
performs no state hashing/dumps/GPR output. --profile-imports adds optional inclusive host-call
timers; ordinary timing runs leave these timers off. All games here run serially in this checkout.

The first 100-field traced profile takes 43.464 s (2.301 fields/s): memory adaptation 55.836%,
memcpy 22.029%, interrupt delivery including inlined trace work 15.863%, direct hash samples 2.080%,
bulk adaptation 1.960%, GS reactor 0.509%, JS/runtime/startup 0.186%, JSPI scheduler 0.004%. These
self samples are not independent additive speedups: trace copying and hashing themselves call the
adaptation helpers. The same early 100 fields with trace off take 0.431 s (232.176 fields/s), not
representative of the later boot.

The complete baseline trace-off 600 fields take 731.008 s (0.820784 fields/s). CPU self samples: GS
memory adaptation 60.227%, GS interpreter/reactor 28.459%, GS exact float helpers/builtins 5.355%,
GS bulk adaptation 0.462%, core 2.415%, core memory adaptation 2.341%, core bulk 0.296%,
JS/runtime/startup 0.444%, JSPI below the sampling resolution. Inclusive graphics-feed calls account
for 691.857 s. GS helper names were recovered by rebuilding the old module with names: its code
section is byte-identical to the measured old GS module, so the function-index attribution is
checked, not guessed.

An intermediate 600-field run (literal/initial interval bypasses, bulk lowering and f64 correction,
before the large-offset base fix and f32 shortcut) takes 559.610 s (1.072175 fields/s). It still
spends 55.651% in GS memory adaptation, 36.047% in the GS interpreter/reactor, 6.168% in GS float
helpers/builtins. This exposed the missed large-offset indexed bases and led to the final interval
correction.

The indexed-base/f32-fast-path version takes 511.530 s for the same profiled 600 fields (1.172952
fields/s). Its GS adaptation still costs 55.477% of self samples. Inspecting the reactor's actual
global initializer exposed its C stack at 0x00100000: below the low mirror, but outside the initial
canonical fast ranges. Including [0,0x00400000), extending the middle safe interval to
[0x02000000,0x20400000), and including [0x3a000000,0xffffffff) removes that miss without assuming
where the stack is or changing its address. Memargs are still included in the interval proof,
including their full byte width.

The resulting complete profiled 600-field boot takes **221.428 s, 2.709684 fields/s**, 3.301x the
original trace-off baseline. Self samples: GS interpreter/reactor 51.269%, guarded GS memory
accesses 27.702%, exact GS float helpers/builtins 14.116%, core 3.273%, core memory adaptation
1.497%, core float helpers/builtins 0.234%, bulk adaptation 0.114% across both modules, JS
imports/boundary 0.643%, other JS/runtime/startup 1.151%, named JSPI scheduler 0.002%. Engine JSPI
internals are not isolated from the other JS runtime samples; all JS combined is only 1.796%. There
are 5,851,122 counted imports; graphics-feed calls consume 208.666 s inclusive of their wasm work.
The optional import timers themselves account for about 0.197% of self samples; the separate
uninstrumented repetitions below omit them. The residual memory category includes the
canonical-range guards/wrapper calls, not just actual folding. `run_micro` alone accounts for
102.356 sampled seconds. The reactor currently compiles the existing interpreter, not the native
build's optional generated VU AOT/native geometry path. Removing remaining memory/float overhead
alone would still leave substantial interpreter work; reaching 60 requires a larger GS/VU execution
change. **The requested 60 fields/s is not achieved.**

Final release proof (`perf-final-proof.tsv`): all 13 columns identical to the previous W9
`proof-boot.tsv` in **600/600 fields**, unchanged trace_columns.py PASS. GPR output enabled; no new
masks, pointer words, ignored columns or baseline edits. It takes 321.359 s (1.867073 traced
fields/s). This is the same W9 result, including its documented differences against the original
i686 baseline; it does not redefine those older comparisons as PASS.

Full two-reactor alias-debug proof (`perf-final-debug-proof.tsv`) also reaches 600/600 with exit 0,
no assertion trap and all 13 columns identical to W9: unchanged trace_columns.py PASS. Diagnostic
cost is 1,013.734 s (0.591871 fields/s); it is intentionally not a speed result. Both proofs record
1,399 real fiber switches and zero same-fiber requests. The shared-stack omission control still
fails its unpublished-leaf canary, as required.

Neither partial run is a completed performance sample. The final debug builder forwards its flag to
both the core and GS postlinks; the optimized GS module is checked to retain the assertion function.
A fresh default build after the certificate change is byte-identical to the measured/proven release
modules (hash check in perf-final-rebuild-hashes.txt): core
c59cb7c094a363f24cd325dea2beea9952261f063914a67865dbeaf389c6f2e4, GS
e6fc0bff49a34c2828ddf9bc91cb640c3afab78dfd1f8b14b4414ea3ed31cc59.

Uninstrumented final 600-field repetitions, run serially after all builds and debug validation, with
trace off and no CPU profiler/import timers:

| Run | Wall seconds | Fields/s |
| --- | ---: | ---: |
| perf-final-off-0 | 295.277423 | 2.031987 |
| perf-final-off-1 | 223.948057 | 2.679193 |

**Final observed range: 2.032–2.679 fields/s**, a 0.647 fields/s spread. Both runs have identical
import-count dictionaries to the final profiled run, 5,851,122 counted imports and 1,399 real/0
same-fiber switches. The profile-derived 3.301x improvement is a separate profile-to-profile
comparison, not a replacement for this measured spread.

Saved intermediate modules were also timed twice each, serially, with the same runner and trace off,
stopping at field 180. These are **early-boot prefix samples**, not substitutes for the 600-field
workload or estimates of race speed. In order, the snapshots add interval bypasses, bulk intrinsics,
f64 quotient correction, literal bypasses, normal-f32/indexed-base changes, and the low-stack
correction. Each core filename below has its paired `-graphics.wasm` reactor. All 14 runs reached
180 fields with exit 0.

| Step / saved core | Run seconds (two samples) | Fields/s range | Median fields/s |
| --- | --- | ---: | ---: |
| Original / baseline.wasm | 46.009, 45.749 | 3.912–3.934 | 3.923 |
| Initial range bypass / interval-only.wasm | 36.676, 38.384 | 4.689–4.908 | 4.799 |
| Bulk intrinsics / bulk-interval.wasm | 43.426, 40.844 | 4.145–4.407 | 4.276 |
| f64 correction / float-interval.wasm | 53.121, 51.332 | 3.388–3.507 | 3.448 |
| Literal bypasses / release.wasm (intermediate) | 53.241, 63.981 | 2.813–3.381 | 3.097 |
| f32/indexed bases / basepointer.wasm | 56.616, 67.713 | 2.658–3.179 | 2.919 |
| Low-stack correction / lowstack.wasm (final) | 24.841, 28.749 | 6.261–7.246 | 6.754 |

The intermediate results are non-monotonic. The final prefix is faster, but the much heavier later
boot explains why its rate must not be extrapolated to 600 fields. The full-boot timings and
exactness evidence above remain the gates.

The final existing mirror fixture passes at 0.298 ns raw versus 5.767 ns adapted (19.34x); a
preceding check measured 0.289 versus 4.174 ns (14.45x). These are medians from the fixture's
existing six timed repetitions, showing remaining per-access guard/call cost rather than claiming it
is eliminated. Both retain the same hash. The complete original mirror/boundary/SIMD/OOB checks, new
known-base/whole-alias controls and narrow-certificate controls pass; the shared-stack negative
control fails its leaf canary as expected.

Final reproduction (reuse the same input module described above):

```powershell
python port/web/build.py build/web/input.wasm
node port/web/run_headless.mjs --fields 600 --trace-off --trace build/web/speed
node port/web/run_headless.mjs --fields 600 --env DHD_STATE_GPRS=1 --trace build/web/exact
python port/tools/trace_columns.py build/web/proof-boot.tsv build/web/exact.tsv
python port/web/build.py build/web/input.wasm --debug-memory
node port/web/run_headless.mjs --fields 600 --env DHD_STATE_GPRS=1 --trace build/web/debug-exact
python port/tools/trace_columns.py build/web/proof-boot.tsv build/web/debug-exact.tsv
```

Build the default recipe again after a debug run to restore normal speed. No dependencies were
installed, no game data committed, no matching-source or include/ changes made, and no browser or
visible game window was used.

Sampling: node --cpu-prof --cpu-prof-dir=build/web --cpu-prof-name=NAME.cpuprofile
port/web/run_headless.mjs ...; summarize using python port/web/profile.py PROFILE --core EXACT_CORE
--graphics EXACT_GRAPHICS. Named optimized modules are retained with wasm-opt -g; all artifacts
remain ignored under build/web. Research rechecked: the WebAssembly memory execution rules
(https://webassembly.github.io/spec/core/exec/instructions.html#memory-instructions) require
non-wrapping effective-address/OOB handling; V8's JSPI API description
(https://v8.dev/blog/jspi-newapi#returning-without-suspending) confirms that only a returned Promise
suspends. No external implementation was copied.

## W21: the game in Chrome (2026-10-24)

The game runs in a browser page with a picture, sound, keyboard and gamepads. The game loop runs in
a Web Worker on JSPI fibers. GS drawing is done by a WebGPU compute rasterizer (the same raster.wgsl
and present.wgsl as the wgpu backend) through `port/web/gpu_web.c` and `port/web/www/gpu.js`. Memory
card saves are kept in IndexedDB (`port/web/www/vfs.js`).

### How to start it

```
# 1. the core module (then needed an i686 link for vu1_aot.inc; build_wasm.py now makes it alone)
python port/tools/build_wasm.py -j 8 --no-wasm2c
# 2. the browser modules (core postlink + the GS/VIF1/VU1 reactor, into build/web)
python port/web/build.py build/wasm/dhd_core.wasm
# 3. the local server (COOP/COEP; 127.0.0.1 only)
node port/web/serve.mjs                         (or --port 8178)
```

Open **http://localhost:8177/** in Chrome (JSPI and WebGPU are on by default in Chrome 137+). Pick
the ISO (SCUS-97177) and press Start. Keys: arrows, X/Space = cross, C = circle, Z = square, S =
triangle, Enter = start, Tab = select, A/D = L1/R1, Q/E = L2/R2. Standard-mapping gamepads work too.

Headless check, used during development (no window; it serves the ISO to localhost only): `node
port/web/check_browser.mjs --iso <iso> --field 600` saves `build/web/browser.png` and prints the
status line. Add `--query env.DHD_NAV=<script>` to drive menus, using the race_compare.py NAV
format. `node port/web/run_headless.mjs --fields 300 --trace-off --trace build/web/diag` is the same
pair of modules in Node, with stub GPU imports.

### Speed (fields/s; the machine was busy, so these are noisy)

| step | Node boot, 300 fields | Chrome title | Chrome race start |
|---|---|---|---|
| first picture (guarded memory, interpreter VU1) | 2.1 | - | - |
| unguarded reactor memory | 8-11 | ~3.5 | ~17 (intro) |
| arena writes through a staging copy (writeBuffer straight from the 2 GB wasm buffer ran at ~40 MB/s) | - | ~5 | - |
| VU1 translated blocks and native kernels in the reactor | 20.3 | ~11 | - |
| f32x4 add/sub/mul helpers in SIMD (exact binary64 sum/product, low 29 bits dropped, demoted; other lanes scalar) | 34.2 | ~16.9 | - |

The VU1 fix: `ps2_vu_aot_load` fills the AOT block words from the image (0x263BC0). The page loaded
the words in `web_vu_ready`, but only from `host_service_vif_feed`, and only once the first four
image words were nonzero. VIF1 data actually comes through `host_service_graphics_feed`, and that
section starts with zero words, so the words stayed empty. `aot_find`'s memcmp therefore never
matched, and every VU1 step went through the interpreter (374M steps per 300 boot fields, now 9.9M;
every one now runs as a block). The load now runs on all three feed paths and looks for data in the
first 4 KB. It also bumps `ps2_vu1_code_version`, which drops the cached misses. `build_graphics.py
--vu-probe` adds a `web_vu_probe` export that counts the blocks that match micro memory now, for
when this breaks again.

### Works / doesn't

- Works: boot, title screen, menus, race start and the race picture (checked by screenshot),
  sound (the status line counts the seconds written: 10.1 s by field 607), keyboard and
  gamepad mapping, memory card saves (IndexedDB).
- Not at full speed yet: about 17 fields/s on the title screen against 59.94. Before the SIMD
  helpers, a Chrome profile of the title screen had the f32x4 helpers vadd/vmul at 34%, the scalar
  `sum` at 10%, run_micro at 8%, writeBuffer at 3% and draw at 4%. The SIMD fast path was checked
  against the scalar path on 20M random lanes, near-equal exponents included: every bit was equal.
- Not checked: steering a whole race with a real player (the headless run only reached the start
  line), and Firefox/Safari (no JSPI).

### Round 2: CPU-side speed (2026-10-24)

The race start is measured from field 3500 on with the race_compare NAV. Node runs `node
port/web/run_headless.mjs --fields 4200 --trace-off --env DHD_NAV=<NAV> --from 3500 --cpu-profile`,
which prints the window's fields/s and writes build/web/node.cpuprofile. Chrome uses
check_browser.mjs.

| change | Node race start | Chrome race start |
|---|---|---|
| start of the round | 16.7 | 18.7 |
| FP control word in wasm (the reactor defines host_fp_csr/set_csr/mode on 0x07800000; the core links to them with no JS frame) | 19.0 | - |
| lean SIMD add/sub/mul paths, SIMD vdiv and vi32 | 27.6 | - |
| zero lanes and zero results on the SIMD paths | 30.1 | 25.7 (up to 28) |

- Every SIMD helper path is bit-identical to the scalar helpers on 20M random lanes, on 20M
  opposite-sign lanes with exponent gaps of 18 to 60 near powers of two, on every exponent pair
  with edge mantissas and signs, and on 20M div/i32 lanes. The test sources live in
  build/web/vtest; they are scratch and not committed.
- Item (2), "untranslated VU1 steps", was a misreading of the counter. vu_steps counts
  dispatches, and vu_blocks == vu_steps, so every VU1 dispatch already runs a translated block
  or a native kernel.
- What remains in the Node race profile: vmul 15%, the VU1 dispatch loop (run_micro, with
  ps2_vu1_run inlined) 10%, vadd 10%, ps2_vu0_run with its native routines 6%, then vif/gs
  feeds, clip and ps2_asm at 2-3% each. The vector helpers now cost about 6-7 ns per call against
  2 ns for a plain f32x4 op. What is left is the binary64 promote/demote and the call itself.
- Chrome timings depend on the GPU being free. While another process (bb-probe.exe) held the
  GPU at about 90%, writeBuffer waited on it and the page fell to 0.1-1 fields/s.
