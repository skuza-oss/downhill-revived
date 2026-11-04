# 021 - Native renderer

Status: partly superseded by 088 (the D3D11 window and present, i686/x87 details)
Checked: 2026-11-04

The GS core (the first section: `port/gs/` memory, registers, GIF parsers, CLUT) and the many
game/ABI fixes below are current. The renderer parts are the retired 32-bit build's: Win32 + D3D11
presentation, `gpu.c` / `raster.hlsl` compute rasterisation, x87 diagnostics. Today the Rust wgpu
renderer draws (`port/gs/gpu_rust.c`, `port/render`; findings 085, 088), the window is SDL3's
(`port/gs/present.c`, finding 099), and the shaders are WGSL (`port/gs/raster.wgsl`, finding 084).
Read those sections as the round log they are.

## GS core (2026-10-13)

`port/gs/` owns 4 MB of GS local memory, general and privileged register state,
three streaming GIF parsers, image transfers and the internal CLUT. `dma.c`
sends normal and source-chain GIF DMA to PATH3; VIF1 DIRECT/DIRECTHL sends PATH2.
`gs_xgkick` accepts PATH1 packets from wrapping 16 KB VU1 data memory.

The memory address permutations cover CT32/24/16/16S, T8/T4, T8H/T4HL/T4HH,
and Z32/24/16/16S. Base pointers are 256-byte blocks; depth's block XOR happens
**after** adding the base pointer. High-byte and 24-bit writes preserve other
bits. Host transfers retain subpixel state across IMAGE/HWREG boundaries.
Local-to-local transfers follow TRXPOS direction. Local-to-host readback is not
implemented yet.

PACKED, REGLIST (including odd counts), IMAGE, A+D, PRE, ADC and EOP are handled.
Primitive assembly retains strip/fan vertices across packets; a draw callback
receives completed primitives. CLUT loads obey CLD/CBP0/CBP1, CSM1/CSM2, CSA,
TEXCLUT and TEX2 masked updates. Palettes are snapshots, not live VRAM views.
Privileged register helpers implement CSR acknowledgement and GS reset;
the host still maps the game's MMIO stores directly, so display registers are
sampled at presentation rather than intercepted per store.

Reference sources (PCSX2, GPL-3.0+, PCSX2 Dev Team):
- https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSLocalMemory.h
- https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSTables.cpp
- https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSState.cpp
- https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSClut.cpp
- https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSDrawingContext.cpp

`test_runtime.py` now also builds `port/gs/test_gs.c`. It checks independently
computed PCSX2 address hashes over four pages for all 13 formats (unaligned base,
page stride and VRAM wrapping), read/write preservation, fragmented GIF packets,
24-bit and nibble uploads, VU1 packet wrapping, palette loading and CSR status.
Existing 462 template checks and 6,000 bitwise VU0 comparisons pass, as do the
GS tests. No proprietary packets, assets or dumps are included in these tests.

## Presentation plumbing (D3D11, retired)

`present.c` creates a Win32 window and D3D11 swap chain (hardware, then WARP if
unavailable). Each kernel vsync samples the privileged display registers and
`display.c` reads the selected swizzled framebuffer, including DBX/DBY and MAGH/MAGV.
The current scanout selects one enabled circuit; PMODE two-circuit compositing
and interlace field reconstruction remain to be validated.
`sceGsSyncPath` now polls the kernel as well as DMA, so tight render loops deliver
vsync and window messages. Linking passes d3d11/dxgi libraries to lld-link and
fails on driver errors instead of silently leaving a stale executable.

`DHD_HEADLESS=1` disables the window. `DHD_SCREENSHOT=<absolute BMP path>` saves
one decoded frame after `DHD_CAPTURE_AT` seconds (default 10). These captures
belong under build/, never in git. D3D11 initialized at feature level B000 in a
16-second smoke run. `build/port/presentation.bmp` is black: primitives have no
raster backend yet, and the first live VU1 run exposes a program termination
problem (17E0). Presentation is wired; a visible menu is not established yet.

## VU1 execution and the first visible screen

The shared micro interpreter now accepts a separate VU1 register file, 16 KB data
and code memories, PC wrap, TOP/ITOP, and an XGKICK callback. MSCNT resumes at the
instruction after the E delay slot. VIF1 BASE/OFFSET/FLG double buffering is active;
STCYCL zero means 256. Added EFU/P operations used by overlay A (ERCPR, ERSQRT,
ERLENG, ESUM), plus the other algebraic EFU operations, MFP/WAITP and TOP reads.
The IALU branch bypass delay applies to integer arithmetic, MTIR and incrementing
loads/stores, not to flag reads or integer loads. VU0's existing behavior remains
covered by the 6,000 native comparisons. All code is C with SSE and no contraction.

A live failure initially looked like a VU1 infinite loop. Captures proved that
VU1 code memory was still zero at the first MSCAL. DMA was delayed until kernel
calls, and later launches could replace its registers before the first packet
ran. Native CHCR stores now call `ps2_store32` through the build-time transform;
recompiled SDK SW/SD use the same MMIO helpers. The HLE consumes the transfer when
STR is written. This also avoids losing PATH3 packets before their storage is
reused. Matching sources are untouched. Reentrancy is guarded in the DMA walker.

With this fix, the game reaches and draws its memory-card warning: background,
Downhill Domination logo, and RETRY/CANCEL controls. A 30-second run had no VU1
failures or crash. The captured overlay-B 17E0 initialization and 1CB0 MSCNT
continuation both replay successfully; the continuation emits two GIF tags and
two GS primitive kicks.

Diagnostics, local only:
- `DHD_VU_CAPTURE=<existing build directory>` saves the first state for each
  overlay/entry. Files contain proprietary code and data; never commit them.
- `DHD_VU_REPLAY=<capture>` makes `test_vu1.exe` replay one captured call.
- `DHD_VIF_TRACE=1` logs a bounded command/upload trace.

Tests cover VU1 memory/PC wrap, E and branch delays, EFU timing/results, TOP/ITOP,
MSCNT, XGKICK wrapping, VIF1 MPG/UNPACK/double buffering/DIRECT, and all three
native DMA-store syntax forms. This establishes interpreter bring-up, **not**
bit-exact validation of all 32 retail entry points. Only overlay B's first-screen
path has live input coverage so far. A PCSX2 VU/GS trace corpus for the remaining
entries is still needed; synchronous XGKICK does not model GIF arbitration or
writes overlapping an in-flight kick. The existing host-float arithmetic also
needs edge-case comparisons against PS2 arithmetic before lockstep claims (lockstep was later
dropped for state sync, finding 044).

## GPU rasterisation (D3D11 compute, retired; the WGSL port is finding 084)

`gpu.c` and `raster.hlsl` use D3D11 compute shaders at native GS coordinates.
A dispatch handles one completed point, line, triangle, strip/fan triangle or
sprite. The GPU buffer is the same 4 MB swizzled address space as the CPU GS
memory. Uploads synchronize pending draws; scanout reads back completed VRAM.
Each primitive samples a pre-draw snapshot, so render-to-texture works without
an unsynchronized texture cache. Masked halfword writes use compare/exchange:
two threads drawing neighbouring CT16 pixels must not lose each other's writes.
This is intentionally a slow reference backend, not the eventual fast renderer.

Implemented shader state: flat/Gouraud color, STQ/UV, direct/indexed textures,
TEXA and CLUT, nearest/bilinear sampling, CLAMP modes, texture functions, fog,
ALPHA's (A-B)*C/128+D equation, PABE, alpha test/AFAIL, destination alpha test,
Z tests/writes, FRAME masks, FBA, dithering, COLCLAMP, SCISSOR and SCANMSK.
HLSL is embedded at link time; the executable has no shader-source file dependency.
The device requires feature level 11.0, with a WARP fallback. Keyboard input now
also works while the game's new window owns focus (X confirm, C cancel).

WARP tests check uploaded/indexed/framebuffer textures, bilinear STQ with Q=2,
points, lines, both triangle orientations, strips, fans, sprites, fog, depth,
blending, alpha-fail preservation, scissor and atomic CT16 framebuffer writes.
The GS tests now cover INT+FFMD scanout: a half-height source is line-doubled,
rather than reading the adjacent framebuffer as the bottom half of the picture.

Remaining correctness work:
- Compare all 32 VU1 entry points and representative frames against PCSX2 captures.
  Current synthetic tests and first-screen execution are not that proof.
- Model asynchronous XGKICK/GIF path arbitration if captures show dependence on it.
- Implement mip/LOD selection and antialiased line/edge coverage. Current lines
  use a simple one-pixel coverage rule, and textures sample level zero.
- Validate two-circuit PMODE compositing and interlace field offsets. Current
  scanout chooses one enabled circuit and line-doubles INT+FFMD.
- Review extreme-coordinate triangle edge overflow and full-range Z32
  interpolation (current shader uses 32-bit edges and float interpolation).
- HLSL sampling warnings are resolved by converging return paths (see follow-up below).

Validation: runtime suite passes (462 templates; 6,000 bitwise VU0 comparisons;
GS, VU1, VIF1 and WARP fixtures). All 3,797 game units compile. The Windows link
retains 23 pre-existing SDK stubs, with no unresolved symbols after stub linking.
`configure.py`, Ninja and `check_image.py` still report MATCH for all eight PS2
sections. Only port/ and documentation have been changed.

Final 30-second smoke run: no crash or VU1 failure; the memory-card warning is
visible at 640x448 with the corrected scanout. Local framebuffer capture:
`build/port/native-renderer.bmp` (20 seconds), log: `build/port/native-renderer.log`.
At capture, 3,132 PATH1 / 4,701 PATH2 / 45,071 PATH3 packets had been consumed,
with 244,824 primitive kicks and four image uploads. PMODE=8066, SMODE2=3 and
DISPFB2=948C confirm that circuit 2 uses interlaced frame mode. These local
artifacts contain game content and are intentionally untracked.

## Font/scanout correction and clean shaders (2026-10-13)

The apparent f-to-t and S-to-B substitutions were lost/misordered field samples,
not wrong glyph indices. A local draw trace decoded the exact expected ASCII
atlas cells; independently decoding the PSMT4HL VRAM with PCSX2 GSTables showed
intact glyphs. Full 64x64 atlas-tile GPU tests now cover all five indexed formats,
including high-byte/nibble formats, away from the texture origin.

The game alternates XYOFFSET.Y between 0x7900 and 0x7908 for its half-height fields.
Presentation previously sampled DISPFB before the VBLANK handler chose the next
buffer, then line-doubled each field. Presentation now latches after that handler
and weaves complementary field lines; the initial field uses bob. CSR FIELD and
VSINT now live in the same GS state used by write-one-to-clear acknowledgements.
Regression tests check field alternation, FIELD preservation on VSINT ack, and
retention of complementary scanlines. Live capture `build/port/font-latch.bmp`
shows FOR, SAVE and SLOT correctly. General two-circuit compositing remains open.

FXC's potential-uninitialized warnings came from nested early returns through
indexed sampling. Single converging returns remove them without suppressions;
fog's nonnegative division uses unsigned arithmetic, removing X3556. Runtime and
WARP tests pass with no HLSL warnings. The compiler source label is now the absolute
`port/gs/raster.hlsl` path. The old root path was only a diagnostic label: no shader
file was written there. Embedded headers remain under build/port/.

`DHD_GS_TRACE=<existing build directory>` saves bounded draw coordinates/state and
the first VRAM image for each TEX0. These contain game content; keep them local.

## Scripted controls and the engine clock

`DHD_AUTOPAD=5:1:triangle,10:0.5:cross` supplies active-high button intervals in
host seconds from boot, independently of window focus. Chords use `+`; intervals
include their start and exclude their end. Invalid scripts fail explicitly.
Parser tests cover boundaries, overlaps, release, and malformed input.

Padman was receiving buttons but the engine did not advance its input frame:
Timer 1 COUNT remained zero. The kernel now advances ungated BUS, BUS/16 and
BUS/256 counters from the same clock as vsync. MODE=0x82 is the game's BUS/256
clock; COUNT wraps at 16 bits and sets overflow status. Tests cover divisors,
fractional ticks, count resets and enable transitions. Compare interrupts,
gated modes and HBLANK counting remain unimplemented.

The warning offers Cross to retry and Triangle to cancel. Scripted Triangle now
leaves it. The following FMV startup crashes at `rc_func_0024C630`: its native
wrapper has lost the five incoming arguments passed through `func_0023C1D0`.
MPEG SDK services are also among the existing stubs. Local reproduction log:
`build/port/triangle-cancel.log`. Title/menu validation remains pending this path.

## Independent VU1 comparisons

`python port/tools/test_vu1_pcsx2.py [local-captures...]` downloads six public,
SHA-256-checked files from PCSX2 commit
[`0f71e668`](https://github.com/PCSX2/pcsx2/tree/0f71e668b8adc950db7de1b1b15c2f5080c102e9/pcsx2).
The test-only adapter compiles PCSX2's actual VU instruction implementations,
flag logic and micro-interpreter pipeline. Platform includes and the unrelated
VU0 macro entry points are excluded; the native game executable does not link
PCSX2. The downloaded GPL-3.0+ sources, binaries and retail code stay under build/.
Set `DHD_PCSX2_VU=1` to include this comparison in `test_runtime.py`.

The matrix checks VF/VI/ACC, Q/P/I/R, flags, final PC, all 16 KB of data memory,
and ordered GIF bytes. Each of 32 entry points runs one fixed and 32 seeded
fixtures, with float mantissas varied while retaining small integer counts.
A linked-particle fixture supplies an actual terminating two-point list; the
sprite fixture supplies its preserved GIF header. Initialization is independently
compared before every entry. Two live first-screen captures (17E0 and 1CB0) also
match: **1,586 cases, zero differences**. Results are in local
`build/port/vu-oracle/results.json`.

These comparisons exposed and fixed:
- VU1 arithmetic rounding: scoped SSE truncate/FTZ/DAZ, restoring host MXCSR.
- Whole-register stalls where only disjoint VF components were pending.
- Upper Q reads before the lower WAITQ stalled their shared instruction pair.
- FCSET's missing four-cycle latency and stale clip state restored by FMACs.
- Division exception status published before the divider completed.

Runtime regressions cover WAITQ with a simultaneous upper Q read, exact 1/3
truncation, MXCSR restoration, FCSET visibility and final clip state. All existing
6,000 VU0 comparisons still pass.

Coverage limit: the 16 B mesh table entries execute their shared setup and stop
at E; their draw bodies run later through MSCNT. Only the first-screen 1CB0 draw
continuation has live capture coverage. Random mantissa fixtures are not arbitrary
bit-pattern arithmetic testing or representative world meshes. Further live
continuations, overflow/denormal arithmetic and asynchronous GIF overlap remain
necessary before claiming PS2 bit-exactness or deterministic online play.

## GS coordinate and constant-depth regressions

Triangle determinants no longer overflow signed 32-bit multiplication. Signed
magnitudes are subtracted as unsigned integers before conversion to interpolation
weights, retaining exact coverage signs and zero edges over the GS coordinate
range. An explicit bounding test excludes samples outside that range. WARP
regressions render full-range triangles in both windings through a small scissor.
Constant-depth lines/triangles retain their integer Z32, including 0xFFFFFFFF;
they no longer lose the low eight bits through float conversion. The new Z-buffer
regression checks the stored GS pixel. Nonconstant full-range Z interpolation,
mip/LOD, AA coverage and two-circuit compositing remain open.

### FIELD polarity with the running frame clock

Rechecking after the timer fix exposed the remaining polarity error: CSR.FIELD
is the inverse of zero-based scanline parity. PCSX2's
[MTGS VSync dispatch](https://github.com/PCSX2/pcsx2/blob/0f71e668b8adc950db7de1b1b15c2f5080c102e9/pcsx2/MTGS.cpp)
passes `CSR & 0x2000 ? 0 : 1` to the renderer. `gs_weave` now accepts hardware
FIELD and applies that inversion. Its regression explicitly checks FIELD=1
supplies even lines, and FIELD=0 odd lines. The earlier readable capture alone
was insufficient while the game clock was frozen. Fresh capture with the clock
running: `build/port/font-inverse.bmp`, with smooth, legible FOR/SAVE/SLOT and
PlayStation. This supersedes the earlier font capture as verification.


## Shell boot and deferred IOP replies

`DHD_AUTOPAD=5:1:triangle DHD_SKIP_MOVIES=1` now leaves the no-card screen,
loads/decompresses the shell resources, and reaches a black title screen with
`PRESS X TO START` (`build/port/shell-async.bmp`). The title scene is not correct
yet: its first complex VU/GIF draw stalls, and additional live VU captures expose
arithmetic differences beyond the previously passing synthetic matrix.

Startup MPEG/IPU playback is still unavailable. `DHD_SKIP_MOVIES=1` explicitly
bypasses only the startup movie sequence; the default still attempts playback.
Port-only source transformations relocate the slot-zero loading NGP's 1 MB
scratch pointers, adapt the sound handle aggregate ABI (including declarations
in merged C++ units), and reconcile float/integer argument order at sound
heading callers. The PS2's separate float/integer argument banks hid these
source declaration disagreements; i386 stack arguments require one order.

SIF RPC previously completed immediately. That let the higher-priority stream
loader pass ReleaseWaitThread before its decompression helper reached its initial
SleepThread. Replies now queue for a synthetic 1 ms IOP latency. Synchronous
requests wait on a semaphore; NOWAIT retains the buffers and exposes the SDK's
busy packet until the callback completes. Kernel interrupt delivery preserves
EE registers, processes replies, then reschedules. FlushCache is also a kernel
poll boundary so native sound RPC wait loops receive completions. Behaviour
reference: [ps2sdk SIF RPC](https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/src/sifrpc.c).
This fixes the double-buffer loader's lost wakeup without changing ReleaseWaitThread.
New SIF tests cover deferred server/callback execution, busy packet layout,
synchronous semaphore ordering, and full/reused request queues. Runtime tests,
including 6,000 VU0 comparisons and D3D11 WARP, pass without shader warnings.
`DHD_DUMP` now reports thread waits and semaphore counts as well as CPU state.


### VU1 shell captures and exceptional operands

The newly reachable shell produced 19 distinct entry captures. Ten initially
failed the independent oracle despite the synthetic matrix passing. Corrected
VU1 arithmetic operand clamping (exponent 255 to signed max finite, denormals to
signed zero), FMAC result clamping, raw-bit MIN/MAX and CLIP comparisons, FTOI
saturation by exponent/sign, and RSQRT signed-zero/invalid cases. VU0 behaviour
is unchanged. Redistributable synthetic tests cover the exceptional operands;
retail captures remain ignored under `build/port/vu-captures-new/`.

The full synthetic matrix plus those 19 new live captures passes **1,603 cases,
all 32 entries, zero differences** against the pinned PCSX2 interpreter, including
raw GIF bytes. The original pre-fix captures also pass. This remains validation
against PCSX2's interpreter with normal overflow clamping, not proof over every
possible PS2 input or asynchronous VU/VIF overlap. Full runtime tests pass,
including all 6,000 VU0 comparisons and the GPU tests, with no shader warnings.
Shell screenshots still lack the background scene; traced triangles have zero
XY, so the next investigation is their input/packet path rather than asserting
that the rasterizer or shell is complete.


### Title/menu controls and the remaining scene problem

Cross advances the title prompt to the main menu. `build/port/menu-followup.bmp`
shows SINGLE PLAYER, MULTI-PLAYER and OPTIONS plus navigation hints; the text is
too dark and the background scene remains missing. Reproduce with
`DHD_SKIP_MOVIES=1 DHD_AUTOPAD=5:1:triangle,12:1:cross,17:1:start` and capture at
20 s. This is progress to the menu, **not a completed title/menu renderer**.

The first shell capture of entry 0x0010 already contains all-NaN bone matrices
at VU qwords 864-867. The later 0x0590 input contains enormous invalid transformed
positions, and its raw GIF XYZ output degenerates near zero. The independent
PCSX2 interpreter reproduces the output for these inputs. The next problem is
to trace the EE/VU0 animation/matrix producer and its VIF upload, then recheck
GS parity with valid scene data. This evidence does not yet identify which
producer is wrong. Other GS parity limitations listed above remain open.

Some apparent stalls were caused by `DHD_DUMP` itself: the watchdog suspended the
game inside `getenv`, then called CRT functions that needed the same lock. It now
copies CPU/stack state without CRT calls, resumes the thread before reporting,
and requests the kernel thread/semaphore report at the next safe kernel entry.
A timed dump at 10 s followed by button transitions and a screenshot at 20 s
verifies that execution continues. `DHD_GS_TRACE_SKIP=N` skips initial draws
before the bounded trace; each draw now includes FRAME/ZBUF/TEST/ALPHA/SCISSOR.
The test oracle can save one replay's local outputs with `DHD_ORACLE_DUMP` set
to an ignored prefix under `build/port/` (`.gif` and `.mem`); these contain game
data and are never committed.


## Final verification for the follow-up

Full `build.py` / `link.py`: 3,797 compilations succeed, 23 existing SDK stubs,
no unresolved symbols or duplicate definitions. Complete runtime suite passes;
independent PCSX2 suite reports 1,603 cases / 32 entries / zero differences.
No root `raster.hlsl` exists. Fresh captures from that full build:
`build/port/font-correct.bmp` (readable card-screen font, 9 s) and
`build/port/main-menu.bmp` (main menu with missing scene/dark text, 22 s).
Both runs continued after the 8 s watchdog dump and were terminated by the
harness timeout; neither logged a crash or a VU execution failure.


## Animation NaNs: native/recompiled callback ABI

The first bad shell rotation is already in the animation channel array read by
`func_00209618` (node type 17), before `func_00222040` loads it into VU0. Channels
1-6 were `FFC00000`; the Euler conversion and `func_00221930` propagate those
values into the matrices subsequently uploaded to VU1 qwords 864-867. This is
not a VIF swizzle or VU1 skinning error.

The 13 assembly-only interpolation handlers in `D_004D0278` take two integer
arguments (`$a0`, `$a1`) and animation time in `$f12`, returning the interpolated
float in `$f0`. `recomp.py` inferred `void(pointer, int)` from their declarations
in `src/unsorted/00207630.c`. Its generated wrappers neither set `$f12` nor return
`$f0`. The native animation evaluator expects a float, so it stored invalid
x87 return values into the channel array. Explicit, assembly-verified signatures
in the port recompiler now marshal time and return the result; they also override
stale signature caches. No matching source was edited.

Source issue for the decompilation owner: `func_00206E48` and its `Handler` typedef
in `00206E48.c`, plus the handler declarations in `00207630.c`, omit the float
argument/result. Callers in `00202E00.cpp` correctly expect three arguments and a
float. The retail dispatcher at 0x206E48 preserves `$f12` across its `jalr` and
leaves the callback's `$f0` untouched. The current clang dispatcher is an indirect
tail jump, so it likewise preserves the native stack arguments and float return;
the failing boundary was specifically the generated callback wrappers. The
source declarations still deserve reconciliation: relying on this tail call is
not a portable C contract. They remain unchanged as requested.

After the wrapper correction, the captured qwords 864-866 are finite, near an
identity basis; qword 867 is approximately (-9.343, 6.118, 5.837, 1). The title's
mountain background and logo now appear in `build/port/menu-animation.bmp`,
although the scene is still too dark. Brightness is a separate open investigation.
The same pinned PCSX2 interpreter agrees on the new live VU1 captures (188 cases
including the reduced two-seed synthetic sweep, all 32 entries, zero differences).
The full runtime suite passes, including a new redistributable 845-case callback
ABI test with synthetic EE instructions, stale cache input, and exact float
results. Full link: 23 existing SDK stubs, no unresolved symbols or duplicates.


### What the movie bypass does

`DHD_SKIP_MOVIES=1` is an explicit renderer-test option, off by default. The
port transformation returns success immediately from `func_001EBFD8`, skipping
the startup movie sequence and its playback loop. It does not decode a movie,
replace the movie with a still, or implement MPEG/IPU hardware. Menus and races
still load their ordinary disc resources.

Real playback needs the movie-call ABI reconciled first: native
`func_0023C1D0(void)` calls `func_0024C630(void)` although the retail path forwards
register arguments. Then the port needs the MPEG demux/decode path (16 libmpeg
symbols remain stubs), IPU commands/FIFO/DMA and colour conversion, or a native
MPEG-2 decoder integrated with the game's stream callbacks. Decoded frames must
reach GS memory/presentation with the game's timing, and movie audio needs its
IOP/SPU2 path or a native replacement with audio/video synchronisation. Merely
removing the bypass cannot supply these pieces. The source ABI mismatch is
recorded here for the decompilation owner; src/ remains unchanged.


### Rider-help pointer and longer captures

Single Player reached the rider-selection transition, then faulted in
`func_0020FBB0` reading 0x00294A68. The caller `func_001F5398` was drawing the
first string in `D_00275728`. Splat encoded that table's first pointer as
`.asciz "hJ)"` (the little-endian pointer bytes), so the port data converter
left it unrelocated while relocating the following `.word` entries.
`data.py` now recognises this verified pointer slot, checks the original bytes,
and emits a relocation to `D_00294A68`. It does not reinterpret arbitrary text
as pointers. A regression checks the pointer, identical text at another address,
and rejection when the verified slot's contents change. The game now passes the
previous fault and draws the rider-help text (`build/port/rider-help-007.bmp`).

For longer runs, `DHD_CAPTURE_INTERVAL=N` records a numbered BMP sequence every
N seconds after `DHD_CAPTURE_AT`, using DHD_SCREENSHOT as the filename prefix.
The old single-screenshot behaviour is unchanged without the interval.
`DHD_GS_TRACE_COUNT` sets the bounded trace length; `DHD_GS_TRACE_FULLSCREEN`
filters to broad untextured sprites, useful for following screen fades. Trace
vertices now include their fog factors. Captures and traces stay under build/port.


### Source blocker at rider confirmation

The 325-second run now reaches the rendered rider-selection scene and remains
alive, but repeated Cross presses cannot confirm the rider. The offending
source call is in `func_001F24B8` (`src/unsorted/001EB1D8.cpp`, near line 3512):
`func_00203B68(&D_004C6370, 0.9f, 1)`. Its declaration near line 94 orders the
arguments `(Value *, float, int)`, whereas the actual definition in
`src/unsorted/00202E00.cpp` is `(Handle *, int apply, float t)`.

Retail instructions at 0x1F3074-0x1F3094 put 0x3F666666 (0.9) in `$f12` and 1 in
`$a1`. The generated i386 caller instead pushes 1, 0x3F666666 and the handle
before calling the native definition. Thus the callee receives
`apply=0x3F666666`, `t=bits_float(1)` (about 1.4e-45), restarting the camera track
near zero instead of seeking to 90%. Further Cross presses repeat that branch.
This is a decompiled caller/definition disagreement, not a VU or rasterizer bug.
No source edit or port transformation was made for it in this round, per the
requested source boundary. The same file has other mixed-order declarations
and an explicit incompatible cast around line 3195 that merit the owner's audit.

Furthest captures: `build/port/rider-help-007.bmp` shows PLAYER ONE / SELECT RIDER
and the rider; `rider-help-010.bmp` shows the animated rider and terrain after
another confirmation attempt. The terrain incorrectly obscures the lower rider;
GS depth/mipmap parity remains open. No loading screen, track selection or race
has been verified in this round. The 325-second run logged no crash or VU timeout.


### Settled menu brightness and verification

The longer isolated main-menu run distinguishes a slow fade from a fixed shader
brightness error. `D_0029DFF4` (menu text alpha) rose from 84 to 222, then the
145-second watchdog read 252. The screenshot at 160 seconds shows bright gold
selection text, grey/white other entries, the logo and mountain background:
`build/port/main-menu-settled.bmp` (also `menu-settle-003.bmp`). Earlier 20-50 s
captures were still partway through the game's fades. No gamma boost or colour
multiplier was added. This backend currently advances roughly one game frame
per second in the shell on this machine, so frame-based fades and camera tracks
are very slow in wall time. Renderer performance is a separate open problem.
This observation is not a claim of full GS pixel parity; the terrain occlusion
visible at rider selection and the earlier listed depth/mipmap limitations remain.

Final checks for this round: full build 3,797/3,797; full link 23 existing SDK
stubs and no unresolved symbols or duplicates. Runtime tests pass, including
6,000 VU0 comparisons, 845 generated callback ABI cases, pointer/text conversion,
GS/VU1/VIF1, controller/timer/SIF and D3D11 WARP. HLSL emits no warnings. The full
independent PCSX2 sweep plus 44 new live captures passes **1,628 cases / all 32
entries / zero differences**. The 325-second rider run and 165-second settled
menu run have no crash or VU timeout; both stop at the harness deadline.

Convenient captures: `build/port/main-menu-settled.bmp` and
`build/port/rider-selection.bmp`. Only port/ and docs/ changed; src/, asm/, config/
and tools/ were not edited. Generated objects, traces, retail captures and game
data remain ignored. The remaining progression blocker is the source argument
order documented above, which is handed back without modifying matching code.
# Cross-unit EE argument ordering (2026-10-13)

The owner's c8c73997 corrects rider confirmation's source declaration and call
without changing the PS2 image. The remaining matching-source permutations are
handled by `port/tools/abi_check.py`: clang resolves typedefs, explicit casts and
asm aliases across all 3,797 units. C++ method definitions include the implicit
`this` when checking C callers. The audit compares 19,341 calls against actual
definitions and generates **180 fixes in 78 units, targeting 30 functions** in
`port/abi_calls.json`. This includes the eight older hand-written port patches
and all four remaining c62/c16 calls in 001EB1D8.cpp.

`transform.fix_abi_calls` applies file/line/column sites before other patches.
It checks the original call's SHA-256, preserves order within each EE register
bank, reorders the arguments and casts the callee to the corresponding local
prototype. Swapping arguments alone would cause conversions through the old
prototype. Nested calls are rewritten recursively and line counts stay stable.
No source unit is changed. Run `python port/tools/abi_check.py --check` to
regenerate the audit and verify that the committed manifest remains current.

Other differences are **flags, not inferred repairs**: the complete local report
is `build/port/abi/report.json` (1,769 other mismatches). Thirty repaired calls
also have a return-type mismatch; their return declarations remain unchanged
and are still flagged. Calls without a source definition and indirect calls
are counted explicitly; this tool cannot infer an asm-only function's real
signature or a runtime function pointer's target.

Validation: all 3,797 PC units compile and the full link retains 23 known SDK
stubs with no unresolved or duplicate symbols. Runtime tests pass, including
new synthetic typedef/alias/cast/nested-call, stale-location, arity and precision
checks, plus execution of the reordered calls. The independent PCSX2 VU1 check
still passes 1,628 cases across all 32 entries with zero differences.


## Profiling and GS page coherence (2026-10-13)

`DHD_PROFILE=build/port/name.profile` enables a game-thread sampling trace and
five-second GS timing/counter reports. `port/tools/profile.py` resolves samples
with the matching lld map; candidate stack addresses are explicitly not an
unwound call tree. The first profile recorded game-time ticks separately from delivered fields;
see the counter correction below before interpreting its rates. Baseline main-menu intervals: 0.6-1.2 game-time ticks/s, 27-29 fields/s,
about 17,000 full 4 MB texture snapshots/s, and 767-791 ms/s waiting in GPU
readback. VU1 accounted for about 3% of samples; recompiled/inline-asm work was
small. Vsync pacing was not the dominant limitation.

The backend now tracks conservative physical 8 KB pages dirtied by GPU draws.
A texture snapshot is refreshed only when its TEX0/CLAMP footprint overlaps
those writes (including region clamp/repeat outside nominal dimensions).
Read-only CLUT loads avoid synchronization when their CPU pages are current.
Framebuffer feedback and GPU-written palettes still force synchronization.
Unaligned base blocks and VRAM wrap are included. This follows the coherence
principle in [PCSX2's texture cache](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/Renderers/HW/GSTextureCache.cpp),
without introducing converted texture/target aliases. Scanout separately uses
independent X/Y address components for PSMCT32/24/16/16S and copies duplicate
field lines instead of repeating per-pixel swizzle work.

With these changes the same menu reaches 3.0-4.1 game-time ticks/s and 45-48 fields/s;
roughly 66,000-72,000 draws/s need only 73-74 snapshots/s. Screenshot:
`build/port/menu-cache-isolated-003.bmp`. Remaining GPU readback waits consume
about 500-580 ms/s: single-primitive dispatch remains expensive. New tests
check page coverage against the existing swizzle oracle for all 13 formats,
GPU-written CLUTs, region-clamped feedback, and scanout against scalar decoding
across offsets, stride, wrapping and field modes.

Concurrent sound edits first failed COM compilation, then stalled boot; the
renderer measurement uses `build/port/renderer-link/dhd.exe`, built with the
last committed kernel/IOP sources in an ignored build directory. The working
sound files are preserved. The latest combined runtime run passes the renderer,
ABI and VU tests up to an unrelated new sound-bank assertion; standalone GS
GPU verification is run separately. No source/PS2-build changes in this step.


## Ordered batching, GPU scanout and VU execution (2026-10-13)

The next bottleneck was one dispatch per GS primitive. Draws now accumulate
in batches of at most 256, with ordered primitive lists per 8x8 tile. A pixel
executes its list serially, preserving blending and Z order. Register/palette
changes, texture feedback, CPU access, and aliased color/depth surfaces flush
the batch. Cached pipeline state avoids rebuilding the palette per triangle.
Normal scanout decodes GS memory on the GPU and copies directly to the swap
chain; only an explicitly requested screenshot reads the display back to CPU.
`DHD_GS_BATCH=1` and `DHD_GS_CPU_SCANOUT=1` retain diagnostic reference paths.

After these changes VU1 became the main CPU cost. The interpreter uses bounded
32-bit cycle counters, a circular pending-flag queue, specialized VU0/VU1 paths,
and SSE2 four-lane arithmetic/flag evaluation. Operand clamping, signed zero,
rounding, operation order and separate multiply/add remain unchanged. VIF
trace/capture environment settings are read once rather than on every packet.
The independent PCSX2 interpreter still agrees on **1,628 cases / 32 entries /
zero differences**. All 6,000 native VU0 comparisons also pass.

In the combined production build, main-menu intervals now deliver 59.7-60.0
fields/s and roughly 42-44 game-time ticks/s, versus 27-29 fields/s and
0.6-1.2 game-time ticks/s initially. Some sampled intervals fall below that while
other work runs on this machine. The field-rate goal is met in steady menu
intervals; These numbers do not establish the rendered frame rate; see below. VU1 dominates the
remaining CPU samples, with GS submission second; GPU readback is no longer
the menu bottleneck. Evidence: `build/port/race-progress.log`, `.profile` and
its matching `.map`; the menu is captured in `race-progress-001.bmp`.

GPU regressions compare the complete 4 MB VRAM after 1,024 mixed ordered draws
with batch limits 1 and 256, including blending, depth, halfword targets,
CPU uploads and masked depth reads aliased with color writes. GPU scanout is
compared with scalar swizzled decoding for all four display formats, offsets,
strides, wrapping and successive interlaced fields. Runtime and WARP tests
pass; HLSL emits no warnings. The combined build now includes the owner's
committed audio implementation. Only port/ and docs/ changed in this step.


## Sound-name corruption: SN integer widths (2026-10-13)

The audio owner's unordered-word fallback exposed an older `rewrite_long`
bug, not an aggregate shuffle introduced by the 180 float/int permutations.
A probe compiled by the matching SN toolchain reports `sizeof(int)=4`,
`sizeof(long)=8`, **`sizeof(long long)=16`**, and `sizeof(double)=8`.
The transform previously collapsed both long forms to the host's 8-byte
`long long`. `00201B68.c` spells its 16-byte sound name `typedef long long
u128`; this became an 8-byte parameter and member. Its request's trailing
fields consequently occupied the wrong offsets. Additionally, many callers
of 00201B68/00201BD0 declared the preceding 64-bit bank-name argument as int,
shifting the x86 argument stack. EE register allocation hid that discrepancy.

The transform now retains 128-bit long-long types and LL/ULL literals via
TI-mode typedefs, while single-long types and L/UL literals remain 64-bit.
The three affected explicit long-long source units are 0011D270, 0018EB50,
and 00201B68. Audited 989snd producer declarations (including merged aliases)
normalize bank-name and callback-data widths before native compilation.
The matching source files remain untouched.

The new regression compiles both actual request producers against a synthetic
RPC receiver. It checks all four name words in order at +16..+28, volume/pan,
flags/shorts, both numeric handles and full 64-bit bank names, and callback data.
It also verifies signed/unsigned 128-bit operations, literals, size and member
alignment. Runtime tests and all 3,797 PC units pass; full link has the same
23 SDK stubs, no duplicates and no unresolved symbols. The regenerated ABI
audit still has 180 reorder sites; **1,739 other mismatches remain flagged**.

Live `DHD_SND_TRACE=1` in `build/port/track-progress.log` confirms S_SELECT at
+16/+20, volume 0x400 at +8, and S_ENV_FAR_L contiguous at +16/+20/+24.
The audio owner can remove the unordered name-group fallback in sound.c:
ordinary `snd_bank_find(..., p + 16)` now receives the correct name. No audio
implementation or audio tests were edited or committed by this worker.


### Frame-counter correction

D_0029E3D8 is a game-time tick count: func_00238C00 can add three ticks per
render, and other loops also advance it. The early logs labelled that value
"game frames/s", incorrectly. All early rates above are now labelled ticks.
D_0029E10C is incremented once in the main render loop and is now the profiler's
frame count. DHP2 traces distinguish the new meaning; profile.py keeps the old
DHP1 traces labelled as ticks. Counter resets are reported rather than wrapping
to a huge unsigned rate.

The corrected main-menu measurement is **about 14-15 rendered frames/s and
59.8-60.0 fields/s**, not 42-44 rendered frames/s. `race-final.log` uses the
correct frame counter. Baseline 0.6-1.2 was ticks/s, so a precise before/after
rendered-FPS comparison is not available from that old capture. VU1 remains
roughly 77% of CPU samples. Reaching 60 rendered frames/s remains open.


## Rider confirmation and bike inventory (2026-10-13)

After c8c73997 and the generated permutations, rider confirmation still chose
the wrong animation clip: 001EB1D8.cpp declared func_00203590's resource ID as
64-bit long, while its definition takes int. On x86 this shifted both following
indices. The separately audited port patch corrects that declaration (20 call
sites); the generic float/int audit continues to flag width mismatches rather
than guessing repairs.

Confirmation then exposed ASCII-looking pointer words in menu string tables.
The original rider-help slot plus 45 additional verified slots are now relocated
by data.py, with strict source-byte guards. The tables are explicitly char*[]
in 001EB1D8.cpp. Tests cover every slot and retain genuine text unchanged.
This gets through START NEW GAME, SINGLE PLAYER GAME MODE and SINGLE EVENT RACE.

Entering the inventory called raw PS2 address D_002618D8 as a timer callback.
Its disassembly is just a stack frame and call to func_00140CE8, with the same
argument and return. The port redirects that verified thunk to the native
function. Bike selection then exposed func_0018EB50's decompiled 128-bit return:
its callee 0018EAC8 actually returns a pointer-sized table entry, and the vtable
caller provides no hidden aggregate-result pointer. Only the zero store at
+0x490 is 128 bits. A port-only patch makes the return/call 32-bit while retaining
the full clearing store. The regression exercises both return paths, all 16
cleared bytes, and the surrounding bytes. No source, asm, config or tools/ file
was changed; func_0023E288 was left alone.

The resulting run confirms the rider and reaches the bike inventory without
crashing, including repeated selection. Captures: `build/port/race-final-002.bmp`
(RIDER SELECTED), `race-progress-004.bmp` (game modes), `race-progress-005.bmp`
(race types), and `race-final-006.bmp` (bike inventory). The inventory's START
route is the next progression test. Texture/depth artifacts remain visible;
this does not establish GS parity or a playable race.


## Timer thunks and member-function pointers (2026-10-13)

START exposed a second timer callback boundary: func_00262728 was generated
as void(void) from its address-taking declaration. Its assembly only saves ra,
calls func_001F8558, restores ra and returns. The native timer supplied the
object, but the wrapper never marshalled it; a stale EE a0=0x4c caused a write
to 0x54c. The recompiler now recognizes the complete seven-instruction unchanged
forwarding pattern and inherits the callee's known ABI, including its return.
It refreshes stale declaration caches and does not infer through unknown
callees, changed delay slots or forwarding cycles. Thirty forwarding signatures
are identified in this build. Synthetic tests reproduce the stale-a0 failure
and verify argument/return forwarding and rejection of modifying delay slots.

That repair reaches SELECT LEVEL, MT. MIDOULE FR, then selecting the level
loads LOADBAR/LALPSE and PTS/ALPINE.PED. The next failure was a call to FFFF0000
from func_001224F0 at its D_0026DD10 member-function table. SN's representation
is {signed short this_delta, signed short virtual_index, 32-bit target}; index
-1 means a direct function. The host Itanium representation treated the first
word (FFFF0000) as the address instead of using the relocated second word.

The port now uses that SN layout for all six source units containing member
pointers. It handles direct calls, native method assignments, and virtual calls
through the stored signed vptr offset and index, combining both this adjustments
as the retail dispatch does. Fixtures check the eight-byte layout, direct and
virtual dispatch, negative offsets, and assignment of a native method. The
matching source and data remain unchanged. `build/port/race-thunks-006.bmp`
records level selection before this repair; the next run tests track loading.

Validation: 3,797/3,797 units compile; full link retains 23 known SDK stubs with
no unresolved symbols or duplicates. Runtime tests pass, including both new
ABI suites, 6,000 VU0 cases, the existing 845 animation callback cases and GPU
regressions. The generated argument manifest still verifies 180 sites, with
1,738 other mismatches flagged. No proprietary trace or game data is committed.

## Track object registration (2026-10-13)

Track loading next failed in func_002623C0, called by State::func_001933A0.
The retail call at 001933B0 forwards the incoming object in a0; its delay slot
only saves that value to s0. The decompiled no-argument declaration lost this
on x86. A guarded PC patch supplies this explicitly. The regression runs the
actual registration body with a synthetic SN vtable and checks both object
arguments, flag clearing and the indexed registration result. Runtime tests pass.

The next live run (`build/port/alpine-this.log`) passes this failure and loads
rider/bike animation resources. It then exposes an unrelocated particle-default
pointer at 0027C414, which splat emitted as a subnormal `.float`. Race drawing
is still pending that data-conversion repair. Matching sources are untouched.

## Speed references and synchronization audit (2026-10-13, preparation)

Read online before the next speed implementation:

- [PCSX2 MTVU.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/MTVU.cpp)
  and [MTVU.h](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/MTVU.h):
  a single-producer/single-consumer ring copies micro/data/register writes and
  VIF unpack inputs, then execution requests. Publication uses acquire/release;
  space pressure wakes the worker and waits. PATH1 completion has a separate
  handoff to GS, and SIGNAL/FINISH/LABEL changes return to the EE. This informs
  the intended ownership and ordering, not merely a thread around MSCAL.
  The requested wiki page returned HTTP 403; the source was available.
- [PCSX2 microVU analysis](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/x86/microVU_Analyze.inl):
  lane-specific VF dependencies, flag liveness, VI branch backup and exact
  incoming pipeline state matter at block boundaries. An AOT entry function
  still needs branches/loops; only individual basic blocks are straight-line.
  Static schedules must distinguish incoming pipeline states or use the
  interpreter when those states cannot be resolved. Generated game-derived
  code must remain in build/, produced locally from the user's microcode.
- [Play! VuBasicBlock](https://github.com/jpd002/Play-/blob/master/Source/ee/VuBasicBlock.cpp)
  computes FMAC stalls and integer branch delays before emitting instructions;
  its first block instructions also account for incoming pipeline writes.
  [VuExecutor](https://github.com/jpd002/Play-/blob/master/Source/ee/VuExecutor.cpp)
  keys cached blocks by their contents. These support separating decode,
  dependency analysis and emission, with code-content guards on AOT dispatch.
- [PS2Recomp VU1 core](https://github.com/ran-j/PS2Recomp/blob/main/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp)
  is an interpreter with explicit FMAC/divider/EFU/flag state; its
  [VU translator](https://github.com/ran-j/PS2Recomp/blob/main/ps2xRecomp/src/lib/vu_translator.cpp)
  provides EE VU instruction emission. It is not a drop-in implementation of
  our 32 fixed microprograms. Keep our separately PCSX2-checked interpreter as
  the oracle rather than changing arithmetic while optimizing dispatch.

No code from these projects was copied or adapted in this preparation; these
are design references. Implementation and licence review are still pending.
The static source audit finds sceGsSyncPath before VIF1 submission in
func_00218720 and repeatedly around render/flip paths in 001B4DD0 and 001EB1D8.
func_0020BED0 reads CSR.FIELD and polls sceGsSyncPath in mode 1 before consuming
the display-buffer ring. func_0019C220 performs a GS store-image/readback.
A worker must preserve these results and cannot expose host-dependent completion
as deterministic game state. Direct mapped loads currently bypass an MMIO-read
hook, and VPU_STAT currently returns idle because execution is synchronous.
The remaining audit must cover SDK/recompiled paths, indirect addresses,
FBRST/VIF reset, VIF FIFO/status and direct VU1-memory reads/writes. No claim
that the game never reads VU1 state has been established yet. Race progression
remains the prerequisite for implementing the requested threading/AOT work.

## Particle default pointers (2026-10-13)

The crash at func_0022CFB8's default-vector copy was the raw pointer 0027BD30
read from table slot 0027C414. Retail assembly loads that word from
D_0027BFC0 + type*0x40 + 0x14 and dereferences it; the matching C also declares
that member as a pointer. Splat had emitted the slot as `.float 3.649452037e-39`,
so the PC data converter preserved the number instead of relocating it.
Twenty-six vector/matrix fields in thirteen records have this classification.
They now receive explicitly audited, byte-guarded relocations in data.py.
Synthetic tests cover all slots, genuine subnormals outside the table, and
rejection of a changed literal. Full link and runtime tests pass. This repair
is based on our matching declarations and retail disassembly; it changes only
the PC image construction, with no source/data changes to the matching build.

The SDK assembly tightens the synchronization result: func_002433A8 actually
loops on D1/D2 CHCR.STR, VIF1.STAT, VPU_STAT bit 0x100 and GIF.STAT, then returns
zero; it does not use the purported mode/timeout arguments. Thus an ordered
queue fence at this HLE boundary can preserve the current return value even
for the caller passing mode 1. func_00243308 (called from 001B7198) writes
VIF1 FBRST/ERR, changes VU FBRST, waits on VPU_STAT and writes two VIF1 FIFO
qwords. That reset path must also fence and feed the worker. These two SDK
routines contain the VU1 busy/control-register accesses found by the assembly
search. A literal source search found no direct VU1 memory/status/FIFO address
uses; it cannot rule out indirect accesses or register-computed addresses.

## Race lighting and animation ABI (2026-10-13)

`alpine-particles.log` gets through ALPINE's track/rider resources and reaches
race setup. func_002211F0 then passed only its object's position to the
hand-written light-grid query func_00220DA0. In retail, a1 still holds the color
output pointer; the query saves it to t8 and writes three floats through it.
On x86 the omitted argument was a return address. The PC patch supplies the
color pointer explicitly. A regression compiles the real caller and verifies
both the no-light path and the resulting RGB/luminance conversion.

`alpine-light.log` passes lighting and reaches race animation. Its next crash
is func_00206E48 treating a float time bit pattern as a node pointer. The
matching dispatcher is declared void(node, values), but the assembly leaves
f12 untouched for the table handler and returns its f0. Its PC declaration,
handler type and return are now explicitly float(node, values, time). The
recomputed audit automatically adds the time-first call in 00202E00.cpp:1454:
**181 reorder sites in 78 units**, with **1,736 other mismatches flagged**.
The new fixture exercises every dispatcher table position, its two integer
arguments, float input and float return. The existing 845 real animation
callback cases still pass. Full PC compile remains 3,797/3,797; the runtime
suite and GPU regressions pass. This corrects the dispatcher in addition to
the thirteen generated handler wrappers fixed earlier; sources stay unchanged.

## Data-labelled timer callbacks (2026-10-13)

The first race transition now draws GET READY TO RACE with rider geometry:
`build/port/alpine-dispatch-004.bmp`. It is an incorrect close-up, not evidence
of a playable track. The next timer callback jumped to raw PS2 address 00260898.
That alabel is inside an assembly unit and is a complete unchanged forwarding
thunk to func_0011C8F0. Six other data-labelled timers have the same pattern.

The PC image builder now inspects each requested D_ text entry's complete
labelled body, including both delay slots. A verified forwarding thunk becomes
a COFF alternate name for its native target; other text/data addresses retain
their previous treatment. This resolves all seven timer labels and replaces
the earlier one-off 00141188 source patch. The synthetic regression accepts
the exact forwarding body, rejects a modifying delay slot and an interior
address, then links and calls the resulting data-named symbol to verify its
argument and return. Production map entries now resolve to native code.

The linker mechanism follows LLVM's public
[COFF alternatename regression](https://github.com/llvm/llvm-project/blob/main/lld/test/COFF/alternatename-lib.s)
and [Microsoft's directive description](https://learn.microsoft.com/en-us/cpp/build/reference/directives).
Only the mechanism was used; no upstream implementation code was copied.
The data-only directive object declares SEH compatibility and the regression
retains the linker's normal SafeSEH check.

## Camera-settings temporary alignment (2026-10-13)

`alpine-timers.log` passes the seven callback aliases, then faults on the
`movaps` store in func_00167800: its output is func_00167A00's float work[12],
which the native caller placed at an address ending in FC. Retail passes sp
itself (00167A4C) with a 16-byte-aligned EE frame. The callee performs a full
quadword assignment. A PC-only declaration aligns that local array to 16 bytes.
The regression compiles the actual caller and checks alignment, neighboring
keyframe pointers, emitted settings and output values. Runtime tests pass.

## Rider trail vector ABI and alpine world (2026-10-13)

The aligned camera-settings build reaches the alpine track intro: terrain,
trees and rocks are visible in `build/port/alpine-aligned-024.bmp`, labelled
MT. MIDOULE, ITALY FR. Depth/texture artifacts remain. After the intro,
func_001304F8 crashed writing its direction output through null. Its caller
passes seven pointers, but the native definition put three Vec4 arguments
inline on the stack. Retail 001304FC-00130534 loads those three vectors through
a3/t0/t1 and copies them to its own stack; t2 is the final output pointer.
The PC patch accepts pointers and makes the same local copies. A regression
compiles the real helper, runs its inline-assembly runtime, and checks the
geometry path, unchanged inputs, final output argument and early return.
Runtime tests pass. No matching source was changed.

The menu's D_0029E10C frame counter stops advancing during the track intro,
although the camera visibly moves. Therefore its reported zero race FPS is
invalid; a graphics frame-boundary counter is needed before race benchmarks.
The 14-15 FPS measurement above applies only to the main menu.

## GS depth readback (2026-10-13)

The race effects call sceGsExecStoreImage (00244228) for 4x4 PSMZ32
occlusion samples. The recompiled SDK polls GS FINISH and VIF1 FIFO occupancy,
which the initial renderer did not return. This costs millions of EE loop
iterations and cannot provide correct depth samples. A native SDK boundary
now executes its seven-qword VIF/GIF packet and reads the GS transfer into the
caller's exact byte range, then acknowledges FINISH and unmasks PATH3.

The GS local-to-host reader supports all thirteen existing pixel formats,
continuous byte/nibble packing, partial FIFO-sized reads and coordinate/VRAM
wrap. It synchronizes pending GPU writes before reading. Regression tests
check all formats against independently packed expected bytes and exercise
the SDK's actual 4x4 depth packet, including a destination overrun sentinel.
Runtime tests pass. Low-level reverse DMA/FIFO MMIO remains separate work;
this change implements the game's SDK readback boundary, not arbitrary FIFO
polling software.

Behavior reference: PCSX2's
[GSState Read/ReadFIFO](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GSState.cpp)
and our retail SDK packet/setup disassembly. No upstream code was copied.
This is a required EE-to-graphics synchronization boundary for future MTVU:
the game consumes returned depth values, so queued raster work must finish.

## Indexed rider float forwarding (2026-10-13)

The next race update reached func_001B3028 through func_001ACFA8 and treated
float bits as the entry index, crashing in func_001B2E50. The retail wrapper
computes `obj + 0x120 + index * 0x50` in a0 and leaves f12 untouched for the
leaf setter. Its matching definition omits that float entirely. The PC patch
adds and forwards it; the audit then fixes both float-first calls in
001ACFA8 automatically. There are now **183 reorder sites** and **1,722 other
flagged mismatches**. The real wrapper and leaf are regression-tested over
all thirteen indices, disabled entries and zero-value behavior. Runtime
checks pass. `build/port/alpine-vectors-005.bmp` records the alpine intro before
this repair; playable racing is not yet established.

## Rider handle callers (2026-10-13)

With depth readback and the indexed setter fixed, `alpine-readback` progresses
through the track intro but crashes in func_00203B68, called by 00179338.cpp.
The leaf copies a 16-byte handle through a0 (retail 00203B74-00203B90). Two C++
units, 00179338.cpp and 00126B20.cpp, still declare that parameter by value.
Their x86 calls passed the handle's first word as the pointer. Both PC-only
declarations now take a const reference; the existing leaf retains its local
copy. A fixture compiles the real rider caller and verifies all six handle
selection paths, the complete four-word handle and the integer/float inputs.
Runtime tests pass. This concerns the animation/control handle API around
00203B68, not the 989snd name ABI repaired earlier; sound.c stays untouched.

## Page-scoped CPU uploads and scanout measurements (2026-10-13)

Live track profiling reported 450-625 ms per second waiting for GPU readback.
Every incoming texture fragment synchronized and uploaded the whole 4 MB GS
image, even when it touched pages unrelated to pending render targets. CPU
writes now mark 8 KB pages and synchronize a page only when GPU writes overlap.
Uploads coalesce changed pages; GPU readback copies only GPU-dirty pages back
to the CPU, preserving unrelated CPU writes that have not uploaded yet.

The ordering regression interleaves an unrelated CPU texture write, an
intersecting framebuffer write and queued draws, then compares the complete
4 MB result with one-draw dispatches. Runtime/WARP tests pass. The design
follows the scoped local/video-memory invalidation used by
[PCSX2 GSRendererHW](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/Renderers/HW/GSRendererHW.cpp);
no implementation was copied. Required depth readbacks still synchronize.

Profiles now separately label menu-counter frames and changed scanout base
pointers. DHP3 traces record the latter, excluding repeated fields of the
same buffer. This measures displayed buffer changes, not simulation updates
or frames rendered repeatedly into a single buffer. The menu counters agree
in the repeated test. A new interpreter run measured about 6.6-8.4 changes/s
in its final intervals, lower than the earlier 14-15 menu sample; compare new
AOT results against this same build/scene interval, not the old headline.


## VU1 AOT block tier (2026-10-13)

`DHD_BUILD_VU_AOT=1` generates 652 straight-line C blocks from the two local
microcode overlays. The generated include is restricted to build/ and is
never committed. Constant instruction words let clang eliminate decode paths;
unknown/modified code, non-block entries and delay slots retain the interpreter.
XGKICK ends a block so callbacks cannot invalidate already-checked later pairs.
`DHD_VU_INTERPRETER=1` selects the original execution path for comparisons.

This is the first AOT tier, not the requested fully static pipeline analysis:
register hazards, Q/P and flags still use the reference runtime queues. The
PCSX2 comparison now also runs a separately compiled unchanged interpreter and
checks the full register state, data memory and emitted GIF stream. All 3,164
cases across 32 entries (including 44 live captures), plus synthetic timing and
fallback cases, pass with zero differences. See
build/port/aot-blocks-reference.log. The earlier research section's PCSX2
microVU and Play! block-analysis references guided the boundaries; no code
was copied from those projects.

With page-scoped uploads, the same executable forced to interpret measured
about 8-11 scanout-buffer changes/s in later menu intervals; AOT measured about
12-13 in the corresponding menu scene. VBLANK still ran near 60 fields/s.
Logs are menu-pages-interpreter and alpine-aot-blocks. This is a modest gain,
not full speed. Per-entry static pipeline specialization remains open.


The next track update exposed the same indirect-handle requirement in
0015A7C0.cpp and 0015B018.cpp, for both 00203C28 and 00204230. Retail
0015A7D4-0015A7F8 copies the complete handle to sp and passes sp in a0;
0015A83C-0015A840 passes another copy at sp+16. The PC declarations now use
const references, matching the existing callees' copied inputs. Real-caller
regressions cover complete 128-bit contents, short-circuit progress/status
queries, transition arguments and unchanged inputs. Full runtime checks pass.


A follow-up makes direct branch targets, branch fallthrough/return addresses
and post-E entries block boundaries (906 blocks). This avoids repeatedly
interpreting a loop's first pairs when its target lands inside a block.
The same 3,164-case matrix passes with zero differences, including the 44
post-animation-fix captures (build/port/aot-boundaries-valid.log). An exploratory
run also included older captures: two pre-fix files time out in PCSX2 itself
before native comparison, and are not counted as passes. No test limit was
weakened. The current ABI audit reports 183 repairs and 1,711 other flags.


## Optional copied graphics queue (2026-10-13)

`DHD_MTVU=1` moves copied VIF1 and GIF PATH3 packets to one FIFO worker.
VIF0 stays on the EE. The 8 MB ring uses Interlocked publication/retirement;
DMA sources may be reused after copying. Waits cover VU1 memory and mapped
VIF1/GIF/GS status accesses, CFC2 VPU-STAT/FBRST, reset writes, sceGsSyncPath,
GS store-image readback and presentation. SQ/SQC2 now notify FIFO MMIO writes.
The default execution stays inline; its memory hook is null.

Stress tests pass for 5,987 chunks, mixed paths, small-ring wrapping, 32-bit
counter rollover, backpressure, immediate producer-buffer reuse and shutdown.
The real VIF tests also pass queued, and runtime tests cover status/reset
fences and complete quadword FIFO notifications. Full PC build/link passes.
This follows [PCSX2 MTVU](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/MTVU.cpp)'s
copied-command/acquire-release design; no GPL implementation was copied.

The live queued run reaches Mt. Micoule race geometry and tutorial overlays
(build/port/alpine-mtvu-000.bmp through -008.bmp), then crashes at 4011FAE2;
its stack and registers are in alpine-mtvu.log. Rendering has large purple
polygons, texture/depth speckle and field combing, now prioritized for correction.
Conservative presentation waits also reduce VBLANK delivery; this opt-in tier
has not demonstrated a speed gain. Some benchmark intervals overlapped the
ABI/oracle builds, so they are not an isolated performance comparison.
Decoupling presentation and completing indirect native-read auditing remain
required before enabling it by default or claiming full determinism.


## GS replay captures (2026-10-13)

`DHD_GS_CAPTURE=build/port/<directory>` records four fields per legacy `.gs`
dump, starting at `DHD_GS_CAPTURE_AT` seconds (default 10); optional
`DHD_GS_CAPTURE_INTERVAL` repeats it. Native BMPs accompany each field.
The directory must already exist. Capture starts only between complete GIF
packets and synchronizes VRAM; proprietary dumps/images remain under ignored
build/. Adjacent transfers are coalesced before path switches and VSync.
PCSX2 2.8.2 opens and continuously replays a coalesced Mt. Micoule capture;
its software framebuffer output is in build/port/pcsx2-sw. The initial output
contains already-rendered native VRAM, so comparisons must use later fields.
The raw software framebuffer includes rows outside the displayed half-height
field; compare the displayed crop, not those unused rows.

Serialization was implemented independently from PCSX2's
[GSState freeze/defrost](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/GSState.cpp),
[GSDump](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/GSDump.cpp)
and [replayer](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GSDumpReplayer.cpp)
wire format (GPL-3.0+ reference; no implementation copied). Runtime regression
checks freeze size, register records, field sequence and fragmented transfers.

The replay parity fixture caught a serializer correction: GSVertex UV and FOG
are 32-bit internal fields, not their 64-bit GS register encodings. The version-8
freeze is 4,194,752 bytes. The serializer regression now checks that size.
`python port/tools/replay_gs.py <capture.gs> [build/port/<prefix>]` replays our
captures on the native GPU without the EE/VU, writing one PPM per field. This
lets the same packets be tested after renderer changes. Only our version-8
format is accepted; this is a diagnostic tool, not a general PCSX2 dump reader.


## GS mip levels and filtering (2026-10-13)

TEX1 now selects signed 12-bit K/16, constant or Q-derived LOD, L scaling,
MXL clamping and all six MMIN modes. MIPTBP1/2 are uploaded for both contexts;
levels 1-6 use their own TBP/TBW, halved dimensions and shifted CLAMP regions.
Nearest mip selection rounds at half a level; trilinear uses fixed LOD fractions.
Filtering selects MMAG at nonpositive LOD and MMIN at positive LOD. PCSX2's
PS2-tested exception is retained: MXL=0 forces MMAG. TEX0 writes also implement
MTBA's square-width allocation for levels 1-3 (TEX2 does not trigger MTBA).
Texture snapshot coverage includes every accessible mip and keys TEX1/MIPTBP,
so GPU writes into a mip cannot leave its sampled snapshot stale.

Reference behavior: [PCSX2 GSVertexTrace](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/Common/GSVertexTrace.cpp),
[GSRendererSW](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSRendererSW.cpp),
[GSDrawScanline](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSDrawScanline.cpp),
GSState/GetTex0Layer and [ps2tek](https://psi-rockin.github.io/ps2tek/).
No upstream implementation copied. Runtime/WARP tests pass all levels, half-level
rounding, blending, signed K, negative Q, L, TBW, shifted region bounds, MTBA and
mip-only GPU feedback. FXC reports no shader warnings.

`python port/tools/gs_fixture.py` generates a wholly synthetic dump under build/.
The identical packets replayed in native and PCSX2 2.8.2 software rendering have
zero differences in 49 checked pixels (all MMIN modes, levels 0-6, rounding,
trilinear and Q/L). `--check <framebuffer.png/ppm>` repeats the comparison.
Reference images: build/port/gs-fixture/pcsx2; native mipmap-native-final-003.ppm.
This establishes those sampling cases; the race's remaining speckle still needs
fresh field/Z comparisons rather than being claimed fixed by mipmapping alone.


## Current-field bob scanout (2026-10-13)

INT=1/FFMD=1 now reads the current half-height framebuffer and duplicates each
line on both CPU and GPU scanout. No old field lines are retained. CPU/WARP
regressions overwrite VRAM between fields and require both output parities to
contain the new frame, across display formats/strides and wrap. Runtime tests
pass (runtime-bob.log). Frame mode still reads the complete current framebuffer;
this port does not add a presentation weave there either.

[PCSX2 GSRenderer::Merge](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/Common/GSRenderer.cpp)
is the comparison reference. Its explicit bob mode uses the current field;
current PCSX2 automatic mode chooses a blend for FFMD rather than always bob.
The port intentionally uses the requested deterministic line doubling, with no
history or parity-dependent offset. The same captured packets can now be
replayed with replay_gs.py; purple-bob-000..003.ppm show stable paired scanlines.
PCSX2 crops for that packet sequence are purple-pcsx2-field003.png. Initial
VRAM still contains native-rendered geometry: those short four-field captures
must not be mistaken for a fresh software redraw of every polygon.


## Double-precision GS depth (2026-10-13)

Depth planes are now prepared with exact integer vertex Z converted to double,
then evaluated in double on the GPU. Integer conversion splits the result into
16-bit pieces with rounding correction, avoiding an implicit float32 conversion
and requiring only SM5's base double operations. The backend checks D3D11's
double-operation feature; WARP and the local RTX 3070 support it. Constant Z,
points and sprites retain their integer bypass. Z24/Z16 comparisons and writes
saturate to their format maximum, as PCSX2 does, instead of wrapping high bits.

Runtime/WARP tests cover small gradients near UINT32_MAX, all four Z layouts,
GEQUAL/GREATER repeat draws with different shading modes, line gradients and
ZMSK (runtime-depth-final.log). The synthetic `gs_fixture.py --kind depth` samples
the Z surface back as a depth-format texture. Native and PCSX2 software replay
agree in all 576 checked pixels (gs-fixture/pcsx2-depth and depth-native-final).
The reference fixture clears Z then draws with GEQUAL. PCSX2's ALWAYS fast path
can classify large close Z values as equal in GSVertexTrace and suppress its
horizontal gradient; GEQUAL invokes CorrectDepthTrace's integer check. The
port does not reproduce that float-derived equality shortcut.

Reference: [GSVertexSW](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/GS/Renderers/SW/GSVertexSW.h),
GSRendererSW/ConvertVertexBuffer, GSRasterizer and GSDrawScanline/TestZ (GPL-3.0+
behavior references; no implementation copied), plus Microsoft's
[double feature documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_feature_data_doubles).
The fog fixture has also been prepared for the next step; it confirms PCSX2's
256-F fog-color weight, which differs from the review's literal 255-F formula.

## Verified GS fog blend (2026-10-13)

Fog now uses `FOGCOL + (((COLOR - FOGCOL) * F) >> 8)` with a signed arithmetic
shift, matching the verified PCSX2 software replay. Equal colors stay equal;
F=0 returns FOGCOL, F=255 retains the GS's 1/256 fog contribution, and negative
differences round down. Alpha is unchanged. The WARP regression checks all 256
factors for differing and equal colors, with explicit assertions for those four
cases. Runtime tests pass (runtime-fog.log); no HLSL warnings.

The same synthetic `gs_fixture.py --kind fog` packets agree in all 512 sampled
pixels in native hardware replay (fog-native-003.ppm) and PCSX2 2.8.2 software
replay (gs-fixture/pcsx2-fog). Reference: GSDrawScanline's Fog/lerp16 path linked
above; independently implemented, no upstream code copied. Race geometry still
needs upstream tracing; correcting its final fog blend does not fix bad vertices.

## Purple geometry trace instrumentation (2026-10-13)

`DHD_GS_TRACE_AT` defers draw diagnostics until race time; optional
`DHD_GS_TRACE_FOGGED=1` selects triangles with FGE and all three fog factors zero.
The trace records exact Z, unsigned GS XY and signed XYOFFSET-relative positions,
MIPTBP/FOGCOL, GIF path, ADC skips, VU1 entry/TOP/ITOP and XGKICK PC/visible CLIP.
The optional interpreter hook only observes XGKICK; it does not change PS2 state.
`DHD_VU_CAPTURE_AT` defers input snapshots to the race. `DHD_GS_CAPTURE_FIELDS`
selects 1-600 fields (default four), permitting fresh geometry to be replayed
instead of relying on the initial VRAM image. These diagnostics contain game
data and their outputs must remain under ignored build/.

Regression checks the exact XGKICK PC, visible CLIP and address, unchanged
interpreter output, and both four/six-field dump lengths. Runtime tests pass
(runtime-trace.log). All 3,164 cases across 32 entries, including 44 live inputs,
still agree with pinned PCSX2 and the independent interpreter with AOT enabled
(aot-trace-reference.log). GSState/VertexKick and GSRendererSW references confirm
unsigned 16-bit XY followed by signed XYOFFSET subtraction, without a second
16-bit wrap. No upstream implementation copied. This adds provenance for the
next live capture; it is not yet a purple-geometry correction.

The rebuilt AOT game advances through Single Player to the Mt. Micoule intro
with later scripted input (Triangle at 45 s; the longer shader startup missed
the old 5 s press). A 120-field capture starts at 285 s:
`build/port/race-fog2-gs/frame-000.gs`. Field 119 is freshly redrawn in PCSX2
software, rather than simply displaying initial native VRAM. Displayed native
and reference images are `race-fog2-gs/frame-000-native-119.bmp` and
`race-fog2-pcsx2-field119.png`; `race-fog2-comparison.png` puts them side by side.
Both show fine rock/waterfall detail. They are not pixel-identical; this is a
same-packet diagnostic comparison, not a claim of complete race parity.

The later fully-fogged suspect draws start at 297.90 s, after this dump ended.
Trace `race-fog2-trace/draws.txt` identifies PATH1 triangle fans, PRIM=0x3D
(textured, fog enabled), VU continuation entry 0x1040, XGKICK PC=0x2430/qword=0x6D,
XYOFFSET=0x790000007000 and FOGCOL=0x715F65. Examples have tiny Q values
(3.10e-38), zero fog, and window-fixed-point vertices (-28503,34391),
(-28672,-30976), (2268,31459). Visible CLIP is nonzero (0x018410), with ADC
suppression elsewhere in the stream. They must be traced to their exact input;
discarding these triangles solely because fog=0 would hide valid GS behavior.
All 32 newly captured race inputs agree with the PCSX2 VU oracle, including
0x1040 (1,514 reference instructions, 2,128 matching GIF bytes). Its first
captured invocation has normal Q/fog values, so that comparison does not yet
cover the later bad invocation. Next capture needs periodic/exact input snapshots
and a GS dump spanning the bad draws.

This run later crashes at native PC=0x30303030 with ASCII-looking register values
(race-fog2.log), which remains open. The earlier 0x4011FAE2 crash maps to
func_0022C350+0x12: an aligned `movaps (%eax)` loading the unaligned 0x044CFC3C
temporary passed by func_0022CFB8. Its `float temp[4]` lacks the EE temporary's
quadword alignment; a port alignment repair and regression remain to be done.
No src/ or matching-build files changed in this round.

## Rider phase ABI and poisoned animation times (2026-10-13)

The protected keyframe crash in `func_002627A0` originates before interpolation.
Track diagnostics found NaN at +0x18, with zero speed and ordinary tick/start
values. The first bad rider phase in `func_001AD2B8` had an initially zero phase,
clip duration 120, and an infinite rate at controller +0x1C. The blend then
evaluated `infinity * 0`, producing NaN. `func_001ACAE0` copies that phase into
the main track, and `func_00203B68` copies it into related rider/bike tracks.

`func_001AD1C0`'s idle branch calls
`func_001AD580(ctrl, 1, ctrl->x50, 1.0f)`. Nine source call sites use that
four-argument shape, but the matching definition omits the unused integer
argument. Retail 001AD59C/001AD5A4 preserve f13/f12 as duration/target and never
read a1. Native x86 instead treated the integer bits of 1 as target and the
zero target as duration, causing the infinite rate. The port definition now
retains the unused integer stack slot. This repairs the ABI rather than
clamping bad animation times or discarding geometry.

`test_rider_time.py` compiles the real setter and idle caller and checks fourteen
cases, including ignored integer values, wrapped phases and fractional durations.
The original definition fails 28 checks; the patched definition passes all of
them. The test is part of `test_runtime.py`. Matching sources were not edited by
the port worker. The shared-header worker's concurrent source commits remain
separate from this port repair.

During the trace, [PCSX2 2.8.2 FPU.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/FPU.cpp)
was reviewed for exceptional division behavior. The live phase probe instead
established the argument shift above; no FPU arithmetic change or upstream code
copy was needed. Proprietary disassembly, diagnostic logs and images stay local
under build/.

The shared-header migration also exposed a transformer defect: an image global
as the first declaration inside `extern "C"` was mistaken for an existing
extern declaration. That left a second definition of `D_0029E160` and the link
correctly rejected it. `extern_image_data` now flattens the whole linkage or
namespace header before parsing declarations. A synthetic regression checks
the first global, nested namespaces, initialized globals, untouched function
locals and preserved line counts.

Protected progression then exposed `func_001825B0`: its three-word local was
passed to the same 16-byte lookup, which writes the short index at +0xC. The
port adds the missing index/padding word. The real six-lookup caller passes a
full-write receiver fixture with the protector enabled; the old local exits
with C0000409. A source audit identified the same local-only defect in
0013B5E8, 0013BA60, 0017FB18, 0018B3A8's Handle__c2 and 001F87B0. Those locals
are widened too; similarly shaped record arrays and embedded fields are not
changed. The regression also executes all five 001F87B0 lookups and checks
their complete handles and transition arguments.

At race entry, `func_00177DA0` next failed its stack canary. The 16-byte buffer
itself is sufficient for the speedometer's `%2.0f ` format and 0-99 clamp.
The source instead declares the float-to-double packer `func_001FD300` as
returning int. Its retail body unpacks the float and packs a 64-bit software
double. The recompiled sprintf varargs bridge also reads 32-bit native slots
into separate EE argument registers; it cannot marshal that host double.
An invalid formatted value can therefore exceed the speedometer's buffer.
This native caller now passes `(double)value` to `host_sprintf`, using the host
varargs ABI. Other EE formatter bridges and software-double callers are not
claimed repaired. The protected real-speedometer fixture checks six normal,
clamped and zero-speed cases, previous/current digit fields, the 64-bit node
flags, and mixed string/integer/double native formatting. It passes.

The speedometer fixture supplies the vector length through a scalar asm shim;
the existing runtime suite separately covers the EE instruction semantics.
The full runtime suite passed during this round, and the final protected
caller fixtures pass after all three repairs. Temporary seek/blend/phase/time
probes have been removed.

The remaining failure is a second source of NaNs, not the idle argument shift.
The protected race reached roughly 300 seconds, then an NPC controller's
+0x1C rate became NaN while its old phase and 120-frame clip duration were
finite. Related tracks acquired NaN offsets or speeds and `func_002627A0`
again indexed outside its keys. Trace the writers of the rate next, especially
`func_001AD1C0`/`func_001AD700` and their physics/tween inputs; its cause is not
yet established.

The full normal rebuild compiled 3,797/3,797 units and linked with 23 existing
SDK stubs, no unresolved symbols or duplicates. To avoid source/header changes
during verification, its inputs were frozen under `build/port/normal-snapshot`;
the local tools read immutable retail asm/assets from the original workspace.
Later shared-header commits are outside that snapshot's validation. A normal
350-second run logged no crash or overflow, but also acquired NPC NaN rates
and track times. Thus the remaining corruption is not confined to protector
builds. At countdown the rider is distorted, then the race becomes a solid
fog-colored field with PUNCH visible. Correct rider/bike rendering is still
open. Normal captures include anim-normal-013/014/015/017/030.bmp under build/port.

All sixteen newly flagged VU inputs match the existing pinned PCSX2 oracle
with zero differences in complete state and GIF output: thirteen 1CB0 and
three 1040 invocations. The live trace starts suspect draws at about 297 s;
1CB0 also emits zero XYZ/Q and fully fogged output. This keeps the investigation
upstream of VU1. The existing race-fog2-comparison.png is a track-intro
same-packet GS replay comparison, not a rider reference or proof of race parity.
The normal executable/map were restored to build/port/link after the probes
were removed and those two units were rebuilt normally.


## EE exceptional arithmetic and first-fault diagnostics (2026-10-13, round 2)

The reviewer's EE float lead is confirmed at a live game instruction. Trap run
`fp-trap-04` stops during ALPINE loading in recompiled `func_00119AD0+0x458`
(native EIP 4041C628): `subss` evaluates -max minus +max, with operands
FF7FFFFF and 7F7FFFFF. These are bounding-box sentinel values. Native SSE
raises overflow; the EE result remains finite. This is separate from the
previous idle setter ABI repair and is not yet proof that the rider is fixed.

`DHD_FP_TRAP=1` installs first-fault diagnostics and unmasks SSE invalid,
divide-by-zero and overflow on every game fiber. The linker map supplies the
function/offset/object; reports include the instruction bytes, all eight XMM
registers and the existing crash handler's GPR/stack snapshot. Both Windows
SSE multiple-fault codes (C00002B4/C00002B5) are handled. Host IEEE faults retry
one instruction masked, then restore the diagnostic mask using single-step.
VU1 interpreter/AOT arithmetic uses its existing clamping with host faults
masked locally, restoring the incoming MXCSR on return. It must not be counted
as a game fault. Thread-local retry state prevents cross-thread interference.

Behavior references (GPL-3.0+, no upstream implementation copied):
[PCSX2 2.8.2 FPU.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/FPU.cpp)
(`fpuDouble`, checkOverflow/checkUnderflow, DIV_S) and
[VUops.cpp](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/VUops.cpp)
(`vuDouble`, DIV/SQRT/RSQRT and FMAC operations). The port helpers flush
arithmetic denormals to signed zero, map exponent-255 operands to signed
7F7FFFFF and saturate exceptional results. Zero division uses the XOR of
operand signs; VU RSQRT additionally preserves its distinct zero/zero result.
Bitwise loads/moves retain their raw bits. This addresses exceptional-value
semantics, not all EE/VU rounding quirks or complete hardware equivalence.

The shared helpers cover generated recompiler add/subtract/multiply/divide
and accumulator forms, plus EE/VU0 macro FMAC and Q arithmetic. Float division
and square root wrappers now use the same mechanism. The old VU0 macro runtime
only guarded division by literal zero and otherwise used unchecked host math.
Standalone tests execute exceptional cases and all eleven generated arithmetic
forms with SSE faults unmasked; real macro instruction tests also exercise
clamping, negative square root and signed denormal division. The host-retry
fixture catches the next game fault after allowing an IEEE infinity.

The initial current-checkout runtime suite passed
(`runtime-fp-round2-verified.log`), including 6,000 existing VU0 microprogram
comparisons and protected rider caller tests. The unchanged VU1 output agrees
with pinned PCSX2 across 1,584 cases, all 32 entries, with AOT enabled
(`fp-vu1-oracle.log`). The final frozen suite passes its VU/GS/VIF/controller/
timer/SIF checks but stops at the audio fixture with 8,202 expected-PCM failures
(`fp-runtime-final.log`). Audio source/test inputs were captured during that
worker's concurrent work; the port worker did not edit audio. This is not a
claim that the final complete runtime suite passes. Standalone frozen float
and shared-header virtual-call tests pass.

Trap run `fp-trap-06` next stops at `func_00117A10+0x2E`, EIP 4000D48E:
`cvttss2si` converts finite bits 7D6C3A80, far outside int32. Native SSE raises
invalid and would produce its negative indefinite integer when masked. EE
CVT.W.S saturates positive overflow to INT32_MAX. Since the function then
clamps to 0..1023, this is an observable index 0 versus 1023 error. Its port
patch now calls the shared `ps2_cvt_w_s`; conversion maps exceptional operands
before comparison and truncates/saturates without a host fault. Real-caller
fixtures cover ordinary, positive/negative sentinel and zero-step inputs.
Reference: FPU.cpp/CVT_W above. No matching source was edited.

The earlier C6 event-dispatcher crash has a separate, established cause:
`EventQueue` moved into a shared header, outside the old `gcc_virtuals` scan.
Clang therefore put the vptr first and used native slots. The PC build now
mirrors project headers under build/ and applies the existing SN virtual
rewrite there. Its execution fixture verifies the 0x600 object, vptr at 0x5FC,
retail slots and signed this adjustments. Matching headers remain untouched.

Validation uses frozen source/header/port inputs in
`build/port/fp-round2-snapshot`; its local tools read retail asm/assets from
the original workspace. Later shared-worker changes are outside this proof.
The normal full rebuild compiles 3,797/3,797 and links with the same 23 SDK
stubs, zero duplicates and no unresolved symbols (fp-normal-build/link.log).
The executable SHA-256 is
144fd7b7959234f2c28d635d3fe8368149af8b72361e6f3ae6edc49bb4ec4def.
Input hashes are recorded locally in final-inputs.json.

`fp-trap-07` reaches the race and the 350-second limit with no game FP hit
and no logged crash. `fp-normal-08`, with trapping disabled, also reaches
350 seconds without a logged crash. Both are stopped deliberately at the
limit; exit 1 is taskkill, not a game crash. The normal run saves 34 frames
at the scheduled three-second intervals starting at 250 seconds. Selected
frames under build/port: fp-normal-08-011.bmp (countdown, about 283 s),
013.bmp (race start, about 289 s), 016.bmp (+10 s, about 298 s), 023.bmp
(+30 s, about 319 s), and 030.bmp (+50 s, about 340 s). These suffixes all
belong to the fp-normal-08 prefix.

The world no longer collapses to a flat fog field, but the rider/bike remain
badly distorted, with the view passing inside large rider/bike polygons.
There is no live controller-rate probe in the committed code, so absence of
FP faults is not proof that every stored rate or bone matrix is finite or
correct. The race rendering goal remains open. Next trace the NPC rate's
physics/tween producers and the first incorrect rider bone/camera matrices
before VIF upload, using independent retail EE data when available.

The new comparison tool replays the race-start dump at 298.004841 host
seconds, port vsync 14006, dump-local field 119. Output:
`build/port/compare/fp-round2-race-start/index.html` and
`moment-000-field-119.png`. RGB MAE 0.356352, pixels exceeding threshold 16:
0.007847%, block SSIM 0.998537, no vertical shift. Both render the same
incorrect rider geometry. This is same-packet PCSX2 GS replay, not an
independent retail race/camera/EE comparison. Screenshots, dumps, generated
code and logs remain ignored under build/. No temporary animation probes
were committed; matching sources and checks were not changed by this worker.


## Race state, scalar ABI and x87 diagnostics (2026-10-14, round 3)

Read-only external EE traces sampled race state/ticks, all rider positions,
velocities, crash counts, animation fields and body matrices once per second.
The original late Cross presses do not repeatedly restart the race: the menu
stays 9, the race counter advances, and only the ordinary rider crash counters
change. The quiet bootstrap also has just its loading/intro/countdown resets.
Thus the broken HUD clock is not evidence of repeated race restart.

The actual formatter/HUD fixture isolates a scalar ABI bug. The unsigned
64-bit division helper 001FBDD8 uses GCC mode(DI) typedef UDWtype. Its generated
native bridge classified the unresolved typedef as i32, truncating arguments
and result. Before repair, 11/31/51 format as `01`; afterwards they format as
11/31/51 and produce the corresponding HUD digit pairs. Canonical scalar typedef
resolution preserves real widths, explicit `(void)` versus unprototyped `()`,
and rejects stale signature caches. The signed/unsigned high-word native fixture
executes generated calls. References: [Clang AST introduction](https://clang.llvm.org/docs/IntroductionToTheClangAST.html)
and [Clang type implementation](https://clang.llvm.org/doxygen/Type_8cpp_source.html).

The SSE trap finds nine further EE-side sites:

- 0022C468 (in 00228BE8), EIP 4011FF8E+constructor offset 0x3BE:
  reciprocal 1/0 for a particle duration. Its four related zero-duration slopes
  use the shared finite EE divider too. Real-constructor tests cover zero,
  ordinary and signed durations with exceptions unmasked.
- 001E75D8's rider calibration Update, EIP 400E325B+offset 0x25B:
  an empty animation track has zero duration. IEEE reciprocal infinity followed
  by a zero track value can create NaN. The entire blend now uses finite EE
  divide/multiply/add and saturating conversion, preserving call/seek order.
  The real 43-entry initializer passes empty, ordinary, signed and overflow cases.
- 001F8920, native offset 0x20: completion ratio 0/0 while the total is unset.
  The shared finite divider handles it; the actual body fixture passes zero,
  ordinary and signed totals with exceptions unmasked.
- 00131998, native offset 0x503: rider angular substep divides by zero stopped
  angular speed during countdown. The shared divider preserves the large
  finite EE result; the real expression fixture covers stopped, ordinary,
  signed and zero-rate cases.
- 00208708, native offset 0x16: the float-tween progress evaluator divides
  elapsed 2 by duration 0 (XMM bits 40000000/00000000) at about 309 s after
  the scale-getter fix. Divide/multiply/add now use the shared finite helpers.
  Its real body passes instant, zero-slope, ordinary, signed and saturation
  cases with SSE faults unmasked, including 0 * finite-max = 0.
- 00111F98, native offset 0x1E7: flat-terrain interpolation divides -0 by
  zero before its integer conversion. The first instruction is DIVSS, not
  the following CVTTSS2SI. Finite subtraction/multiplication/division and
  saturating CVT.W.S preserve the EE result and signed 32-bit wrapping.
  The real body passes ordinary, flat, positive/negative signed-zero and
  sentinel-overflow cases with SSE faults unmasked.
- 002145F8, native offset 0x9A: segment/slab clipping multiplies bounding
  sentinel FF7FFFFF by reciprocal BFB89BC3 and overflows. Finite helpers cover
  segment and radius subtraction, reciprocal and both slab products. Its real
  body passes ordinary, stationary, expanded, signed-sentinel and saturated
  delta/product cases with SSE faults unmasked.
- 0011EDA8, native offset 0xF2: the proximity query leaves max-finite distance
  when no nearby rider is found, and the update unconditionally squares it.
  Shared finite multiply keeps the squared sentinel finite. Its real-body
  fixture covers missing-rider, ordinary and square-overflow cases with SSE
  faults unmasked, using both legacy and later shared query declarations.
- 00125178, native offset 0x16E: NPC distance adjustment adds a scaled distance
  to a maximum-finite sentinel and overflows. Finite subtract/multiply/add
  preserve the single speed query and saturated result. The real statement
  passes ordinary, signed, sentinel and zero-product cases with SSE traps.

These use the round-2 [PCSX2 FPU](https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/FPU.cpp)
semantics; no upstream implementation copied. The 0x54 animation-entry stride
and the unusual 001B1748 flag-clearing count were checked against retail asm;
neither is an invented structure-size or count fix. Host graphics execution
masks SSE exceptions while executing VIF1/GIF, then restores the incoming mask;
VIF0 remains a game diagnostic path. Queue fixtures pass in queued and inline modes.

The reviewer's x87 lead is also present in a live run. The earlier snapshot logs
SW 0021 before race reset, then SF/IE with changing TOP at the actual bone-cache
writer 0023AE78: examples 0861/1061/1861/3861, tags FFFF. Empty tags and nonzero
TOP show an invalid stack history, but do not identify the originating
instruction or 0015A968 as the sole live cause. The shared
worker's real negative fixture separately demonstrates its old void declaration
overflowing the native x87 stack. [Microsoft's x86 FP calling convention](https://learn.microsoft.com/en-us/cpp/cpp/floating-point-coprocessor-and-calling-conventions?view=msvc-170)
requires a floating return in ST0 and an otherwise clean coprocessor stack.

`DHD_X87_CHECK=1` samples status and tags around main-loop calls, before/after
001AD1C0/001AD700, and before/after the actual cache writer 0023AE78. The actual
race frame loop is inside 001B40A0 at 001B4128..001B45B4; those boundaries advance
diagnostic frame numbers and emit clean progress every 300 updates. fnstenv is
paired with fldenv. Fault logging temporarily saves the entire x87 register
state and restores it afterwards; no game status or stack is cleared. Native
fixtures preserve an empty stack, a live value, and a masked overflow exactly.

Host fiber wrappers also failed a real FP-context fixture: another fiber
overwrote a live x87 value. The port now uses floating-state fiber flags plus
FXSAVE/FXRSTOR on each yielding fiber's stack. It preserves both live values
and sticky faults, rather than concealing a broken caller. The old wrappers
fail; the corrected wrappers retain separate x87 values/faults and MXCSR.
Reference: [CreateFiberEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfiberex),
[ConvertThreadToFiberEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-convertthreadtofiberex).
On this machine the Windows flag alone does not retain the tested live x87 value.

The first run including shared float-return sweep 7bce00a0 reaches GO near
292 s, without an x87 log, but crashes at 319.9 s in 002013F8 while dispatching
a queued sound completion callback at data address 084B2828. The generated
native prototype of 00201BD0 passes the 128-bit SoundName as uint32_t, shifting
following arguments. Shared fix c226c3a8 adds true quadword scalar marshalling.
The alternate queue-probe run reaches 350 s, but logs two later x87 faults;
it is not a clean final proof. Probes remain in ignored alternate objects only.

The actual populated bone-cache rotations remain finite and close to orthogonal:
the first 319-s run's largest squared-length/dot-product error is 1.923e-6,
bone 18 near 294.6 s; the 350-s probe run's largest is 1.722e-6, bone 19 near
327.7 s. The first observed writer is 0023AE78 via 0016BC30, dispatched through
00216E28, not the previously suspected 00209618 traversal. No first bad scale
or shear was found. A rotation with the wrong angle can pass these checks, and
they do not validate final skinning/palette indices or the rendering camera.

The old frozen audio failure is exactly `test_sound`, 8,202 assertion failures
in fp-runtime-final.log. Of these, 8,008 are test_debug_capture at line 472:
every sample of the 4,004-frame stereo 48-kHz/16-bit WAV fails `pcm[i] == 4096`.
The remaining 194 are tone/stream rate, refill, clipping, level/end, volume
and loop PCM expectations. It is an expected-PCM failure, not compilation.
Audio sources were captured during that worker's edits; no mixer or audio test
fix was made by the port worker. The coherent round-3 snapshot's audio fixture
prints `test_sound: ok`.

The proof driver reads EE memory only and records the 26-entry bone cache too.
It preserves the established bootstrap, stops all input at countdown and never
sends a late START. Later fallback Cross is limited to selection/intro states.
File-backed autopad accepts atomic script replacement; Windows readers share
delete access. It selects the reserved-RAM game child, not the console helper,
and errors promptly on failed attachment instead of claiming an empty trace.
Screenshots, input hashes, queue/animation probes and dumps remain ignored.

The per-frame checkpoint now re-arms SSE traps even when the x87 observer is
disabled. Host libraries had changed MXCSR masks during earlier runs, so a
previous run with no trap reports was insufficient evidence of coverage.
The rearm regression resets host masks deliberately and then requires the
next game 0/0 to fail. It passes together with the host retry fixture.

The x87 first-fault experiment also unmasks x87 invalid, with host graphics
state saved/restored locally. The saved FPU instruction pointer resolves the
fault to native 4002DD09 in 00132918: FSTPS consumes the result of virtual
scale slot 4 after it returned no ST0. Status 81E3 has SF/IE, C1 clear and an
empty tag word. EIP 40026CAB in 0012DF48 is a later waiting FLDS, not the
originating fault. The initial guess of overflow at that return load was wrong.
Reference: [Intel SDM](https://cdrdv2-public.intel.com/835781/325462-sdm-vol-1-2abcd-3abcd-4.pdf),
deferred x87 exception handling and saved FPU instruction pointer. No code copied.

Retail 00261E48 writes float 1.0 to F0; 21 inspected vtables have this getter
at offset 0x2C, including D_00283A48. Its generated wrapper used an untyped
integer/eight-argument fallback and left ST0 empty for native float consumers.
The port now records a float return and one unused this argument. The actual
generated wrapper, reached via a float vtable slot, returns 1.0 with SW 0000
and tags FFFF across 16 calls. Restoring the old integer return produces
16 wrong results and SW 0041. Current-version stale caches are overridden.
The live instruction identifies the virtual-return path; the alternate target
logging run did not capture a target, so the selected live target is not
independently pinned by that probe.

Mirrored shared-header asm labels lacked the source-unit Win32 underscore
rewrite. New menu aliases therefore referenced func_001F7640/001F7680 while
their definitions exported _func_001F7640/_func_001F7680. Header mirrors now
apply the same label prefix; real alias calls and the existing SN virtual
fixture pass. Matching headers/source remain untouched by the port worker.

VU0 native and interpreted microprogram boundaries now mask host SSE faults
locally and restore the incoming MXCSR. Synthetic native division and
interpreted MUL overflow run with game masks unmasked, preserve their existing
IEEE results and restore the caller exactly. All 6,000 existing VU0 comparisons
still match the port's native reference. These are not a hardware oracle:
VU0 micro arithmetic still bypasses the VU1 operand/result clamp in several
paths. Round-2 EE/VU0 macro arithmetic is clamped; micro-mode parity remains
an explicit open item. The temporary first-fault/target logging builds remain
ignored; the normal executable rejects their marker strings.

The requested full normal rebuild after a2d562d2 compiles all 3,797 units and
finishes its 350-second run without a crash. Race counter 6 -> 1,881, no reset,
one player crash at 312.9 s, and the player moves downhill. Frames show
27/41/40 MPH and 00:03/00:16/00:30 near +10/+30/+50 wall seconds. The former
permanent 0 MPH/restarting-looking evidence changes, but large rider shards
remain. The engine delta is 2 and configured rate 30 throughout sampled race
frames. Race timer advances by that delta in 0019EE98; 00238D98 caps catch-up
to two ticks. Thus the timer and physics advance while simulation trails wall
time; a missing-progress/repeated-restart explanation does not fit this run.
The sampled cache rotations' peak squared-length/dot error is 1.732e-6.
Translations are held separately by 0023AE58; countdown bone positions stay
near the player root. No first scale/shear corruption was established.

Race-start GS replay after a2d562d2: MAE 0.0145, SSIM 0.99995, report under
build/port/compare/r3-round8-race-start. It replays native packets and initial
VRAM, so it does not validate retail EE bone, camera or skinning output. The
available comparison tool does not expose per-bone EE matrices.

Final trap run r3-final-trap6 reaches GO at 290.525 host seconds and stops
deliberately at 350.211 s (exit 1). There are no game SSE hits, x87 fault
reports or crashes. The last clean progress checkpoint is frame 2,100 after
259,469 samples. Race counter 4 -> 2,378, no resets; player crash counts change
at 304.7, 333.8 and 344.9 s. Sampled animation fields are finite and the selected
player's bone-cache index is 0. The cache's peak squared-length/dot error is
1.539e-6, bone 19 near 334.8 s. This does not establish correct pose angles or
final skinned vertices. A clean trap/status run is not a claim of race parity.

Validation is pinned to committed source/header head 71a18a1b (including the
shared float, scalar and quadword ABI work) in build/port/r3-final-snapshot,
plus owned runtime/header-mirror and subsequent finite-math overlays. Per-file
hashes and overlay commits are recorded in build/port/r3-final-exe/inputs.json.
Immutable retail asm/assets are read from the original workspace; generated
code, dumps and images are ignored. The final frozen runtime suite, including
all new real-path fixtures, exits 0 (r3-final-normal-runtime.log); test_sound
prints ok. No temporary first-fault or target-probe marker is in the normal
executable. The post-trap normal full build compiles 3,797/3,797; final link,
normal run and frames are recorded below after completion.

The post-trap normal link uses 4,545 objects, 5,561 retail data labels and 725
recompiled assembly functions. Strict linking succeeds with the established
23 SDK stubs, no duplicates and no unresolved symbols. The normal executable
SHA-256 is af7d8ca1d57843cdd9f13e077f432231925c0c89519ff6eebb7b7252a8c8b745.
Its hashes are in normal-inputs.json beside the executable.

Quiet normal run r3-final-normal reaches GO at 291.781 s and stops deliberately
at 350.162 s without a crash. Counter 26 -> 3,689, no resets. Rider 0 eventually
stops moving without a crash-counter change while the clock keeps advancing;
this run releases all buttons at countdown and supplies no pedaling.

The final moving proof r3-final-pedal uses the same normal executable. It keeps
START cleared after countdown and sends Cross only in active race states,
for two seconds at GO+3,+23,+43 (observed 294.297,314.289,334.377 s). No restart
or race-counter reset occurs. GO is 291.282 s; deliberate stop is 350.164 s.
Counter 3 -> 3,312, sampled rider 0 moves downhill and records two crashes near
303.5/305.6 s. Sampled animation fields stay finite. The HUD shows 39 MPH/00:03
near +10, 45 MPH/00:19 near +30, and 2 MPH/00:47 near +49; the next frame shows
23 MPH/00:49 and Riding Outside Boundary. Thus there is progressing simulation
and variable speed, not the old permanent zero-speed/restarting-looking clock.
Rendering remains wrong: large rider/bike shards exist already at countdown.
Later clock/physics behavior is not independent retail parity proof.

One moving-run matrix sample differs from a rotation: bone 6 at 305.550 s,
on the second crash, has peak squared-row-length/dot error 0.074043. The adjacent
304.546/306.555 samples are near rotations. The reader is asynchronous with
0023AE78's four quadword stores, so this could be a torn read or a blend;
it is not an established first bad game matrix and cannot explain earlier
countdown shards. Next instrument the source synchronously at 0023AE78 via
0016BC30/00216E28, then trace the first deformed skinned vertex/palette index.
No per-frame source-matrix or independent retail EE bone comparison is claimed.

Final representative native captures (every 3 seconds from 250 s):

- Countdown, 286.027 s: build/port/race-proof/r3-final-pedal/frame-012.bmp
- GO+0.746 s: build/port/race-proof/r3-final-pedal/frame-014.bmp
- GO+9.754 s: build/port/race-proof/r3-final-pedal/frame-017.bmp
- GO+30.739 s: build/port/race-proof/r3-final-pedal/frame-024.bmp
- GO+48.738 s: build/port/race-proof/r3-final-pedal/frame-030.bmp

PCSX2 GS replay of the final moving race-start dump at 294.060 host seconds
is pixel-identical (MAE 0, SSIM 1). Report:
build/port/compare/r3-pedal-race-start/index.html.
It replays native GS packets/initial VRAM and shares the bad geometry; it
does not establish correct retail EE pose/camera/simulation. Quiet-run replay
is also pixel-identical. All temporary probes remain outside production code;
the normal executable contains none of their marker strings. Matching source
and proprietary data were not edited/committed by this worker. Stop for review.

Owned round-3 commits: 0bb77329, 8e6410dd, 95667dfe, 24c6097b, c070ddc0,
b73a7e67, f1d0aeef, 73dcf4ea, ce843878, 068ebb15, 099524dd, 2ab08b9c,
71a18a1b, f83ffd06, bf71835a, 897031e1, 14f3f12a, ffd7a113, bd19fc4d,
233942e4, 5a8e198d, 7b1961d9, b9f1076e. Shared scalar/float/quadword commits
are credited separately above; none is represented as the port worker's edit.

## Rider shards: draw isolation, MIN/MAX and camera state (2026-10-14, round 4)

Draw isolation (`port/tools/race_proof.py --skip-program/--skip-vu/--skip-overlay`)
at the countdown and +10 s: skipping the low-entry skin palette jobs
(MSCAL 0000/0010/0020/0040) removes the large central rider/bike slabs and
leaves an upright start gate and horizon. Skipping particles (overlay A) does not.

The EE FPU and VU have no NaN. MIN/MAX order the raw 32-bit patterns as
sign-magnitude values, so an exponent-255 encoding is just the largest
magnitude; PCSX2 implements them with integer compares. The VU0 macro runtime,
the native VU0 micro helpers and recompiled `min.s`/`max.s` used MINSS/MAXSS or
fminf. They now share `ps2_float_minmax` (ps2float.h), which is also the scalar
VU interpreter's helper. Tests run identical exponent-255 pairs through the VU0
macro path and the VU1 SSE lanes (commit 8ae982fe). This did not remove the shards.

The 00216E28 4th-lane lead is covered by shared commit 208de939. On the EE
the lane is dead: every path in 0021B8A8 that stores the vector writes a
pointer over +0xC. The shards remain after it.

Read-only EE samples during the race (diagnostic sampler, output under
build/): the world is Z-up. At the static countdown the render camera
(`D_0075A9D0[0].camera`, position +0x10, rows +0x40) is 10.66 units from
rider 0, and its rows equal the rider body matrix. In the rider's frame the
camera sits 10.0 behind and 3.7 above, a plausible chase camera, and keeps a
constant 10.66 distance while racing. All riders share delta 2 / rate 30, and
all descend steadily in z. The earlier "upward" NPC speeds were an axis
misreading, not a physics fault. The camera state therefore does not explain the shards.

The VU1 skin job does. 00216E28 dots the source rows with (object - camera)
and yields (-0.0003, 10.0, -3.70) for the countdown frame. The VU1 input for
entry 0590 at the same static frame has root translation qw3 =
(-0.214, -0.012, -2.468): the side and height parts are close (allowing for a
bone offset), but the 10-unit forward component is missing. The object is thus
drawn about 2.5 units in front of the camera, crossing the near plane, which
gives the radiating near-plane triangles PCSX2's VU1 also produces from these
inputs. In 0021B8A8 that vector (vf4) is scaled by scratchpad 0x700003A0.x,
offset by the node vector, and processed by `vcallms 0x808`. The latter has no
native routine and runs in the VU0 interpreter, so VU0 micro-mode parity and
the 0x3A0 scale are the next checks.

## VU0 micro oracle and unaligned quadword locals (2026-10-14, round 5)

`port/tools/test_vu0_pcsx2.py` is the VU0 counterpart of the VU1 oracle. It
builds PCSX2's own
[VU0microInterp.cpp](https://github.com/PCSX2/pcsx2/blob/0f71e668b8adc950db7de1b1b15c2f5080c102e9/pcsx2/VU0microInterp.cpp),
VUops.cpp and VUflags.cpp (same pin, hashes checked) with
`port/runtime/pcsx2_vu0_oracle.cpp`. It runs each captured input through both
interpreters and compares VF, VI, ACC, Q/I/R, flags, the 4 KB data memory and
the VU1 registers. Inputs come from `race_proof.py --vu0-capture`
(`DHD_VU0_CAPTURE`: registers, 4 KB code and data, and the VU1 VF/VI file),
under ignored build/. Each capture is also run with 4 seeded variants of its
data floats. With no captures the test skips; `test_runtime.py` runs it next
to the VU1 oracle when `DHD_PCSX2_VU` is set.

VU0 micro code can read VU1's registers at data address 0x4000
([GET_VU_MEM](https://github.com/PCSX2/pcsx2/blob/0f71e668b8adc950db7de1b1b15c2f5080c102e9/pcsx2/VUops.cpp#L1135):
VF0..31, then VI0..31 one per quadword). The interpreter now maps that range
for VU0 to a snapshot of the VU1 state taken after the graphics fence.
Writes there are not copied back.

Race captures (r6): 13 entries, 0x0 to 0xCD0 from 0021B7E8/0021B8A8, plus
0x4F0 from func_00263B48, 8 captures each, 520 cases with the seeded
variants. Before the fix, 0808 and 0BD0 matched in all 40 cases each, 0C50 in
5, and the rest differed in the last bit of their results, e.g. entry 0CD0 vf20.x
native 3F7FFFF8 vs PCSX2 3F7FFFF7. The cause was rounding: our VU0 micro mode ran with
round-to-nearest and no operand clamp, while PCSX2 runs VU0 at its default
VU0FPCR (toward zero, FTZ/DAZ), as VU1 does. `ps2_vu0_run` now uses VU1's
MXCSR and the exponent-255 operand clamp. All 520 cases then match, and the VU1
oracle still matches 1584/1584. A synthetic MUL overflow now gives 0x7F7FFFFF,
as on the VU, instead of infinity.

So 0x808 matches PCSX2 for the inputs it is given. The 8 captured 0x808
calls are not the rider's, though: they have identity rows and a world-space
vf4 such as (2546.8, 1321.5, -6218.4). If the forward component is lost, it is
lost before the call.

One EE-side mismatch was found and fixed. 00216E28 stores the camera-relative
vector with inline-asm `sqc2`, which the runtime performs at the exact host
address. The recompiled 0021B8A8 reads it with `lqc2`, which, as on the EE,
ignores the low four address bits (recomp.h `M128`). The buffers
(`char local[0x10]` in 00215B68.c, `char buffer[16]` in 00218720.c) only
had byte alignment on the host. port/patches.py gives both 16-byte alignment.
This did not restore the lane. With the patch (r7), the 0590 root translation at
the countdown is still (-0.196, -0.008, -2.394), compared with (-0.214, -0.012, -2.468) before,
and the countdown and start frames still show large rider/bike slabs.
The next step is to capture the 0x808 input of the rider's own node, which
should have a vf4 of about 10 units.

`DHD_QUAD_CHECK=1` (`race_proof.py --quad-check`) logs each inline asm
program the first time it moves a quadword at an address that is not 16-byte
aligned. The r6 race logs 49 such programs, all on host stack locals: vector copy, add and
scale helpers (`lq $2,0(%1); sq $2,%0`, `lqc2 $vf11/$vf12`, `sqc2 $vf0`).
They are harmless while only inline asm or C touches the buffer. They break
only where recompiled code reads it back, as in 0021B8A8.

Recompiler check of 0021B8A8: `$gp` is an ordinary register there,
saved to scratchpad +616/+192 and loaded with scratchpad addresses.
recomp.py treats `$gp` as a base only for `%gp_rel` relocations, which this
code has none of. `lqc2`/`sqc2` move 128 bits (`M128`,
`ps2_store128`). `qmfc2`/`qmtc2` go through the runtime and copy all 16
bytes. `cfc2`/`ctc2` read and write VI0..15, status, MAC and CLIP from
the state the microprogram left. `vcallmsr` does not occur in the game.
`cfc2` zero-extends rather than sign-extends bit 31. That matters for no
VU0 control register the game reads.

## Aligned quadword locals: the rider shards (2026-10-14, round 6)

`DHD_QUAD_CHECK` now also prints the calling function. In race r8 it logged 64
inline asm programs moving a quadword at an unaligned address. They came from 40
functions (0011FC70, 00129500, 0012D860, 00134FB0, 00217038, the VIF transform
helpers 00221888/00221930/00221F20/00222040, ...), all on host stack locals:
`float buf[4]`, `Vec scale`, `Vector value`, `Contact contacts[8]`. Often the
asm sits in an inlined helper that receives the pointer. On the EE these objects
live in a 16-byte aligned frame. On the i386 host they only get their type's
alignment (4).

General fix, two parts:
- build.py compiles each port source in two steps. The frontend's
  unoptimized LLVM IR has its aggregate stack slots (`alloca` of an array,
  struct, union, class, vector or i128) raised to 16-byte alignment, then that
  IR is optimized and compiled with the same flags. Without the edit, the
  round trip gives the same object code as before (checked on 0011FC70 and
  00221888 with llvm-objdump). This covers every local, whatever path its address
  takes. The two port/patches.py alignment entries from round 5 are therefore
  dropped.
- `ps2asm.c` performs lq/sq/lqc2/sqc2 at `address & ~15`, like the EE and
  recompiled code (recomp.h `M128`), so both sides name the same quadword by
  construction. `DHD_QUAD_CHECK` still reports any access left unaligned. A
  runtime test checks that an lqc2/sqc2 4 bytes into a quadword uses the
  aligned one.

QUAD_CHECK count, race to +50 s: 64 (r8, exact addresses, no alignment), 0 (r9,
aligned locals), 0 (r10, aligned and masked).

Rider check (r10, `race_proof.py --vu0-capture --vu0-capture-count 400`, one
countdown frame): 0x808 calls 155-164 have the ~10-unit camera-relative vector,
e.g. vf4 = (3.00, -1.07, -10.38), |vf4| = 10.85. They and the standard capture
set match PCSX2's VU0 interpreter. The first captured VU1 0590 skin job now has
root translation (-14.56, 16.54, -36.54). Its length, 42.7, is the |vf4| of
0x808 call 185 (42.71): the object's camera-relative position, where r7
had a stale (-0.196, -0.008, -2.394).

Frames (r10, build/port/race-proof/r10-masked): countdown -3.9 s, start
+2.2 s, +11.2 s, +29.2 s, +50.2 s. The riders and bikes at the start gate
are whole and there are no shards at any of the five times. At +50 s the
player's bike lies away from its rider (a crash). Some small blue specks
remain on the start-gate floor.
