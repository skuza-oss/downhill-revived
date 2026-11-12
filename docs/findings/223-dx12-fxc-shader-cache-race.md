# 223: The pipeline worker's crash in wgpu-hal's DX12 `load_shader` (FXC blobs shared across threads)

Status: in progress

Branch `fix/pipeline-worker-crash` from `main` d9714d0a, started 2026-11-12. The 0.8.0 gate saw
the guest game die once in seven two-game online runs, at the start of the shared race, on the
renderer's pipeline worker thread (`dhd-pipelines`, finding 131's background specialisation):
`crash: exception C0000005 ... (reading 00000009)` in `wgpu_hal::dx12::Device::load_shader` <-
`create_render_pipeline` <- wgpu-core <- `dhd_render::native::pipelines::build` <- `Worker::start`.

In short: wgpu-hal's bug (gfx-rs/wgpu#10547), in every release since the worker shipped in 0.7.7,
reached only where the renderer compiles with FXC. Fixed here by backporting upstream's fix into
the vendored wgpu-hal.

## Goal and acceptance

Say whose bug it is, since when, and fix it if it can be fixed safely for a release. Done when:

- the crash line is resolved and what was freed or null is named;
- a stress test reproduces the fault on the code before the fix and passes after it
  (`rustgs.py test ... -- --test dx12_shader_cache`);
- `check.py port` and `check.py docs` pass, a boot state trace equals the reference build's, and
  one hidden two-game P2P race on a local server reaches the results with no renderer error line.

Out of scope: upgrading wgpu; reworking the worker.

## Progress

- [x] 2026-11-12: lane branch, finding reserved.
- [x] 2026-11-12: crash resolved, cause found and reproduced (sections 1-3).
- [x] 2026-11-12: upstream's fix backported into `port/render/vendor/wgpu-hal` and the stress test
  `port/render/tests/dx12_shader_cache.rs` added (section 4).
- [x] 2026-11-12: checks (Result): `check.py port`, the renderer's cargo tests, boot and race state
  traces, a hidden two-game P2P race to the results.

## 1. What the crash was

The crashed build was the gate's (main-merge at 65542300, its `dhd64.map`). The map's
`load_shader` and every return address on the logged stack line up with one image base (64 KiB
aligned), giving the faulting instruction at `load_shader`+0x74d: `call [rax+8]` with `rax = 1`,
which is the AddRef of a COM object (vtable slot 1) whose vtable pointer read 1, hence
"reading 00000009". The instructions before it are the cache hit of `load_shader`
(wgpu-hal 29.0.3 `dx12/device.rs`):

    if let Some(value) = shader_cache.entries.get_mut(&key) {
        value.last_used = nr_of_shaders_compiled;
        return Ok(value.shader.clone());   // <- here
    }

(a SwissTable probe with 0x58-byte entries, two string compares for the HLSL source and entry
point, two byte compares for stage and shader model, the `last_used` store, then the enum's niche
decoded as tag 1 = `CompiledShader::Fxc(ID3DBlob)` and its clone). So the cached FXC blob had been
freed while the cache still held it, and its memory reused: a first word of 1 is what a fresh
Rust `Arc` (strong count 1) or similar small allocation puts there. Nothing was null; nothing the
main thread owns was involved.

The gate build had no `dxcompiler.dll` beside `dhd64.exe`, so `port/gs/gpu_rust.c` chose FXC.

## 2. Why the blob was freed

wgpu-hal's DX12 device keeps a shader cache (`ShaderCache`, under a mutex) and gives every
pipeline a clone of the cached `CompiledShader`. With FXC that clone is an AddRef on
d3dcompiler_47's `ID3DBlob` (under the lock); the pipeline's copy is released when
`create_render_pipeline` is done (outside the lock). d3dcompiler_47's blob counts references
without atomics. Measured with a small program on this PC (2026-11-12, d3dcompiler_47 of Windows 11
25H2): two threads each cloning and dropping one blob 10 million times, with 100 000 extra
references held, ended at 95 069 / 70 617 instead of 100 001 (a `D3DCreateBlob` blob / a
`D3DCompile` output); four threads lost 3-11 %; one run lost all 100 000 margin references and
crashed. The same program on DXC's blobs (`IDxcUtils::CreateBlob`, `IDxcCompiler3` output,
dxcompiler v1.9.2609) kept the count exactly: DXC's reference count is atomic.

The renderer makes pipelines on two threads at once by design (finding 131): the worker builds a
draw's specialised pipeline while the render thread builds generic ones. Every native pipeline
shares the same vertex stage, so both threads hit the same cached vertex blob; a race start (new
draw states) is when both build the most. One lost AddRef is enough: the count reaches zero while
the cache holds the blob, it is freed, the heap reuses it, and the next cache hit (here on the
worker) calls AddRef through garbage.

## 3. Whose, since when, who is exposed

- **wgpu's.** Upstream issue gfx-rs/wgpu#10547 describes exactly this (FXC blobs shared by the
  DX12 shader cache across threads; use-after-free and heap corruption). Upstream PR #10805
  ("Fix FXC shader cache use-after-free across threads, fixes #10547", merged into trunk
  2026-11-11) makes `CompiledShader` an owned `Arc<[u8]>` copy of the bytecode. It is in no
  release yet; wgpu-hal 29.0.4 (the newest 29.x) has no DX12 change. Sources: the issue and PR
  pages on GitHub, and a diff of the 29.0.3 and 29.0.4 crates.
- **Not ours.** `pipelines.rs`'s worker owns what it uses (`&'static Gpu`, its own `Sources`), and
  wgpu-core allows `create_render_pipeline` from several threads; the cache's map itself is under
  its mutex. No lifetime or ordering mistake on our side was needed to reach it.
- **Not new.** The worker shipped in 0.7.7 (fb12874d, 2026-11-01); 0.7.7 through 0.7.16 have the
  same exposure. `git diff v0.7.16 main` changes the worker by 11 lines, none of them relevant.
- **Not the environment.** No out-of-memory or device-lost line in the log; the fault reproduces on
  an idle PC with one process (section 4).
- **Who can hit it:** Windows on DX12 with FXC: builds without `dxcompiler.dll` next to the exe
  (development builds such as the gate's), `DHD_WGPU_COMPILER=fxc`, and the WARP adapter, which
  `device.rs` `software_fxc` always compiles with FXC, release zips included. Release zips on a
  hardware GPU use DXC (`package_release.py` ships `dxcompiler.dll`) and are not exposed; Vulkan,
  Metal, Android and the browser are not either. No player crash log with this chain is in the
  repository's findings or notes.

## 4. The stress test, the fix and its cost

`port/render/tests/dx12_shader_cache.rs` makes render pipelines from one WGSL module (one vertex
and one fragment entry point; two target formats, so distinct pipeline states that share both
cached shaders) on N threads for S seconds, beside a thread that keeps freeing and taking small
heap blocks (as the game's own allocations do, so a freed blob is soon someone else's memory).
`DHDR_STRESS_SECS` (default 3) and `DHDR_STRESS_THREADS` (default 8). One test for FXC, one for
DXC, on the hardware adapter.

Before the fix (wgpu-hal as shipped), RTX 3070, driver 32.0.16.2114:

| threads x seconds | runs | pipelines per run | result |
|---|---|---|---|
| 8 x 60 | 2 | 2 739 776, 2 613 136 | passed, but see below |
| 8 x 20 | 1 | 860 160 | the two shared cached blobs' counts afterwards 6045 and 7303 (should be 1) |
| 2 x 30 | 4 | about 190 000 | 3 crashed: `STATUS_HEAP_CORRUPTION`, `STATUS_ACCESS_VIOLATION`, and `STATUS_STACK_BUFFER_OVERRUN` after D3D12 refused the garbage bytecode (`Internal error ... Unspecified error (0x80004005)`); the 4th ended with counts 47 and 27 |

With eight threads the releases mostly race each other (lost decrements: the count drifts up, a
leak); with two threads, as in the game, an AddRef racing a Release is as likely to be lost, and
one lost AddRef frees the blob. (The counts came from a temporary diagnostic in the vendored hal
that read each cached blob's count after the threads stopped; it is not committed.)

The fix is upstream's PR #10805 applied to the vendored wgpu-hal 29.0.3
(`port/render/vendor/wgpu-hal/PATCH.md`, last section): `dx12/mod.rs` `CompiledShader` is an
`Arc<[u8]>`; `shader_compilation.rs` copies FXC's and DXC's output into it and releases the blob on
the thread that compiled it; `device.rs` takes DXIL passthrough bytes the same way. The cache now
shares only an atomically counted `Arc`. Cost: one copy of a shader's bytecode (a few KiB) when it
is compiled; a cache hit is an atomic increment instead of a COM AddRef. Nothing on the render
thread waits for the worker; pipelines are still made on both threads at once.

After the fix, same PC and test (with the heap churn thread):

| threads x seconds | runs | pipelines per run | result |
|---|---|---|---|
| 2 x 30, FXC | 9 | 185 712 - 203 120 (about 1.74 million in all) | all passed |
| 8 x 60, FXC | 1 | 1 316 432 | passed |
| 8 x 60, DXC | 1 | 1 336 768 | passed |

Before the fix 3 of 4 two-thread runs crashed; nine clean runs in a row after it would happen by
chance with probability about 0.25^9 if the fault were still there.

## Decisions

- Backport upstream's fix into the vendored wgpu-hal rather than serialise pipeline creation in
  our renderer: a mutex around `create_render_pipeline` would make the render thread wait for the
  worker's FXC compiles (tens of ms), which finding 131's worker exists to avoid; the backport is
  three small hunks, identical in substance to upstream's merged fix, so a later wgpu upgrade
  simply drops it. No wgpu upgrade in this lane (the brief).
- DXC's output is copied too (as upstream does), although DXC's blob is atomic: one representation,
  no blob kept in the cache whatever its threading rules.
- The stress test runs 3 s by default inside `check.py port`; it is a guard against a later wgpu
  upgrade losing the fix, not a proof by itself (the proof runs are the longer ones above).

## Surprises

- Eight threads did not crash in 6.2 million pipelines; two threads crashed in 3 of 4 runs of
  about 190 000. The direction of the count's drift depends on which operations race (section 4).
- The gate's crashed object read 1 where its vtable should be: the freed blob's memory had already
  been reused, which is why the fault is on the cache hit and not on the release that freed it.

## Result

The 0.8.0 gate's crash is wgpu-hal's (gfx-rs/wgpu#10547), present since 0.7.7 and fixed on this
branch by upstream's fix backported into `port/render/vendor/wgpu-hal`; players' release builds on
a hardware GPU (DXC) were never exposed, WARP and FXC builds were.

Checks on the fix commit, this PC (RTX 3070, Windows 11 25H2):

- `python tools/check.py port -j 6`: OK (in a fresh worktree after one core build with
  `DHD_MODULE_CACHE=0`). It does not run the renderer's cargo integration tests, so
  `rustgs.py test --target x86_64-pc-windows-msvc` ran them too: every test passed, the new
  `dx12_shader_cache` included (3 s default run).
- State traces against the reference build of main 524948ba (format 2): `boot` all 600 rows match,
  `race` all 5000 rows match (state_diff.py PASS). The change is in the renderer only.
- `race_menu_check.py OUT --scenario finished --link delay=80` (two hidden games, P2P room, local
  server, the development build compiling with FXC as the gate's did): PASS, both games list the
  same results; no renderer error line in either log.

Not checked: WARP (the other FXC case) beyond the code path being the same; Vulkan, Metal and
Android are untouched by the change (DX12 backend only). When wgpu is upgraded to a release that
contains PR #10805, drop the DX12 section of `PATCH.md` with the hunks; the stress test stays.
