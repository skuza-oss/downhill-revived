# 281: an iPhone with 3 GB closes "out of memory" from the native renderer (0.8.12)

Status: in progress

Branch `fix/ios-gpu-memory` from `main` 6937e805 (0.8.12), started 2026-11-24. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

A player's iPhone XR (iPhone11,8, A12, 2.8 GB, iOS 18.7.10), 0.8.12 sideloaded, Metal, native
renderer, Native VU1 on, resolution 2x (picture 1280x896, surface 1792x828). The guest memory
reserves in two parts (findings 258, 267). Two runs, both closed by iOS with no crash report:

- the first after the disc import: menus, an online room and its rider/bike shop, then sixteen
  `dhdr_texture_create: native texture: Out of Memory`, `gs: the GPU device was lost (Unknown):
  Out of memory` and `wgpu error in dhdr_native_picture_indexed: Out of Memory` (about 30 s);
- the next one: 71 pipelines of the first run's list built ahead in 0.6 s, then at field 148 (the
  title's first 3D picture), 2 s after the start, `gs native: out of memory (vertices)`: a CPU
  `realloc` of the native renderer's vertex array (`port/gs/native_core.inc` native_reserve)
  returned NULL.

Done means: the cause found with evidence; the fix in the one path every platform runs; at an
out-of-memory in the renderer one log line says what was asked, the renderer's own totals and
on Apple the process's footprint and free address space; proof by a test that fails before and
passes after (or the nearest honest thing), the iOS Simulator to a race, `check.py port`,
`check.py docs`, boot and race state traces on the PC equal to the base. Out of scope: the
extended-virtual-addressing and increased-memory-limit entitlements (a paid account's).

## Progress

- [x] 2026-11-24: the renderer's totals (textures, surfaces, arenas) and the out-of-memory line
  (`port/render/src/memory.rs`, 302048fe)
- [x] the cause, measured on the Mac (macOS build: the same Metal backend; the Simulator hands
  Metal to another process, so its address space shows no GPU memory)
- [x] the fix: upscaled IMPORT pages drawn in one pass from an atlas (4476145d); a byte budget
  for decoded textures (5eae87e6)
- [x] checks: `check.py port`, `check.py docs`, `test_native_transfers.py` (a new patterned
  picture), boot and race state traces equal to the base, the iOS Simulator to a race

## Decisions

- **One line at an out-of-memory, the same on every platform.** `memory: out of memory in WHAT
  (N bytes asked); renderer: textures N (X MiB), surfaces N (X MiB), arenas N (X MiB); process:
  ...`. The process part is Apple's only: `task_info(TASK_VM_INFO)` footprint,
  `os_proc_available_memory` (iOS), and the free address space between `min_address` and
  `max_address` (`mach_vm_region` walk: free bytes, ranges, the largest). Logged at a failed
  `dhdr_texture_create`, a device lost for memory, any wgpu error saying "out of memory" and C's
  native arrays (`native_oom_size`, through `dhdr_memory_report`); the first 3 and every 100th. A
  line `memory: the renderer holds X MiB: ...` at each new 64 MiB, so a log shows the growth before
  a failure.
- **The import batch (the cause, below).** A run of upscaled IMPORT pages into one surface goes
  into a 1024 x 1024 atlas (256 pages of 64 x 64, by buffer copies) and is drawn in one render pass
  with one bind group, a draw per page, in the same order. render.rs draws the waiting pages before
  any other command and at the picture's end. Same blit shader, same constants except the source's
  offset (an integer added before the floor: the same texel), so the same pixels. ALPHA_UPLOAD keeps
  the 64 x 64 import texture.
- **A decoded-texture budget.** The cache had 4096 slots and no byte cap, freeing a slot only when
  a new key collided with it: on the Simulator's race it held 2339 textures, 219 MiB. Now C counts
  each texture's bytes and, past the budget, frees the oldest no queued field can use (`used + 8 <
  field`, the eviction rule it already had) down to 3/4 of it. The budget is the device's memory /
  16 (SDL_GetSystemRAM) within [128, 512] MiB: a 3 GB iPhone about 179 MiB, a 4 GB phone 256, a PC
  with 8 GB or more 512, which the PC route never reaches (192 MiB at its busiest), so PCs draw as
  before. A failed texture create frees half and tries once more. `DHD_NATIVE_TEXTURE_MB` sets the
  budget for tests. The browser takes 256 MiB.

## Surprises

- The brief named `arenas.rs:91` for `gs native: out of memory (vertices)`: that line is C's, the
  `realloc` of the recorded frame's vertex array (`native_core.inc` native_reserve), a CPU
  allocation; it failed because the address space was gone, not because the arrays were large
  (a few MiB).
- **The cause (high confidence, measured on an M1 Mac, macOS build, native, Native VU1 on, scale 2,
  title, no pipeline list):** at about 1-2 s the process's GPU memory (`vmmap`'s
  IOAccelerator (graphics)) jumped to 307.5 MiB in 3750 regions, footprint 412-480 MB; the dirty
  memory went by 3 s, but the 307.5 MiB of address space stayed for the whole run. The title's
  first two pictures import 71 and 70 pages (DHD_NATIVE_TRACE, fields 29-30); at scale 2 each page
  was a buffer copy into the 64 x 64 import texture plus its own bind group and render pass, about
  27 regions a page. At scale 1 (a plain copy per page) the same run had 23 regions, 9 MiB; scales 2
  and 3, Native VU1 off and the original font all had the burst. With the batch: **24.7 MiB in 150
  regions, footprint 124 MB at the same moment** (`macearly.sh`, the same script and settings). On
  a 3 GB iPhone (about 3.4 GiB of address space, the guest's 1184 MiB in it, finding 258) 300 MiB
  kept for nothing is what turned the next `realloc` (run 2, field 148, 2 s in) into NULL.
- The textures were the other half: unbounded, so a longer session (run 1: menus, a room, its
  shop) fills the address space with textures until `dhdr_texture_create` fails and the device is
  lost. The PC route: 64 MiB in the shop, 192 MiB at the race start (816 textures, 155 MiB); the
  Simulator's race before the budget 2354 textures, 219 MiB and growing. With the budget of a 3 GB
  iPhone (`DHD_NATIVE_TEXTURE_MB=179`, the Simulator's own memory being the Mac's 8 GB): one trim at
  field 2984 freed 343 textures (44.9 MiB), the renderer never passed 192 MiB, footprint at most
  158.7 MB, the race started (GO at field 3212) and ran.
- The first Simulator app of this lane kept an older `libdhd_ios.a`: `build_ios.py` reuses the core
  library unless `--rebuild-core` is given, so a renderer change needs it.
- Tried and dropped: bounding the exact renderer's encoders in flight (8) changed nothing in the
  burst (the native renderer was drawing). The 129-pipeline list built ahead adds about 48 MiB of
  address space and 80 MB of footprint at 1 s: real, but not what failed; kept.
- `tests/abi_layout.rs` asserted `DhdrCommand` at 208 bytes on x86_64 while the line above it says
  216 + 3 pointers (240) since 6d9c4423: the test failed on `main` (`check.py` does not run it).
  Fixed to 240.
- Six of this lane's macOS test games stayed alive after their script's kill (its ssh ended first);
  found by their folder, killed by PID; `macearly.sh` now kills its game on any exit.

## Result

Two changes in the one shared path (Rust renderer and `native_core.inc`), and the log line:

- upscaled imports in one pass from an atlas: the scale-2 burst of GPU address space on Metal
  307.5 MiB / 3750 regions -> 24.7 MiB / 150 regions (macOS, title, the same moment);
- decoded textures within a budget from the device's memory (179 MiB on a 3 GB iPhone; PCs keep
  their behaviour: the budget of 512 MiB is above anything the route holds);
- `memory:` lines: growth each 64 MiB, and at a failure what was asked, the renderer's three parts
  and on Apple the footprint, what iOS still allows and the free address space.

Proof: the patterned 2x upload in `test_native_transfers.c` (every pixel its own colour; a wrong
atlas offset fails it with 450560 wrong pixels, the batch passes with 0); the test's other checks
pass and its two runs (with and without dual-source blending) still agree on all 38 pictures; `check.py port` and `docs` OK; boot and race state traces
equal to the base (6937e805: boot 600 and race 5000 fields, every row); the iOS Simulator app
(built with `--rebuild-core`) to a race at the 3 GB budget. Not proved: an iPhone XR. There is
none here; the next player log says (the `memory:` lines) whether the address space still runs
out, and where.

What a player can set in 0.8.12 until then, by the evidence: Resolution 1x (no upscaled imports:
no burst, and a quarter of the surfaces); restart the game before a long online session (the
textures only grow); the renderer Exact is not a way out (a larger steady footprint, about 390 MB).
