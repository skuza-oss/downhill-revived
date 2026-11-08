# 085: The Rust (wgpu) GS renderer: spike report

Status: current
Supersedes: parts of 084
Checked: 2026-11-04

2026-10-25, branch `render/rust` (spike S, combined from the parallel parts S-native, S-web and
S-present, then S5 and S6). The plan and its task list are kept outside the repository. Its section
2 defines the spike, and this report answers it. Everything here is synthetic: test programs and
hand-built data, no game data. The one exception is the boot state trace of S1, which runs the game
as the repo's harness always does.

- Only x64 Windows and the browser move to Rust (then; since finding 099 every target draws
  with it).
- The i686 build stays frozen on today's renderer (D2 = option (c)); it was retired soon after
  (finding 088). S2, the i686 parts of S1 and every i686 check were skipped on purpose.
- `gpu_exact.inc` and `native_core.inc` stay shared C. Only the GPU code moves to Rust.

**Verdict: pass for x64 and the browser.** No stop criterion was hit. The one item left open in the
first version of this report (the name of wasm-bindgen's glue import module, item 4 below) is
settled in the build: the locked layout checker passes on the real browser build.

## Where things are

| Part | Files | Checks |
|---|---|---|
| S0: pins | `port/render/{Cargo.toml, Cargo.lock, rust-toolchain.toml}` | `cargo tree -i` |
| S1/S3: native library, link, MXCSR | `port/render/spike/native/` (`src/lib.rs`, `spike_native.c`, `build.py`, `link_dhd64.py`, `gpu_rust_spike.c`, `run_minimised.py`, `fold_probe.wgsl`) | `spike_native ops/s3/panic/validation/reenter`, the dhd64 link and exe checks |
| S4: browser chain | `port/render/spike/chain/` (README has the S4 answers) | `node port/render/spike/chain/run.mjs` |
| S4 items 5-7: present, upload, sizes | `port/render/spike/present/` (`results/*.json`) | `run.mjs` (three runs recorded) |
| S5: parity smoke | `port/render/spike/parity/` (`s5_parity.py`, `s5_record.inc`, `s5_replay.c`) | `python port/render/spike/parity/s5_parity.py` |
| S6: API reference | `port/render/examples/reference.rs` | `cargo run --features native --example reference` |

Scratch output stays outside the repo:

## Pass rules (plan section 2), x64 and browser

**1. Link, CRT, imports, paths (x64; S-native).** The scratch library goes into the real `dhd64.exe`
in place of `wgpu_native.lib`. `link_dhd64.py` derives a builder from `build_host64.py`; the file
itself is unchanged, since its `--gs-rust` is task A1c.
- **Link.** lld-link without `/force` gives an empty link log, 136 generated imports and no stubs.
  The i686 census (23 undefined names) does not apply: i686 is not linked with Rust.
- **`package_release.exe_problems(exe, set(), libraries=[])`.** It reports the same 2
  `SDL_OWN_PATHS` strings as the unchanged D3D11-only `dhd64.exe`, and `package_x64_test` filters
  those. There is nothing else and no exemption for the library. `game_data_runs` with no library
  gives 0. All of this was re-checked for this report with this branch's tools.
- **Imports.** Every import matches `package_x64_test.SYSTEM_DLL`. The new ones are `OLEAUT32`,
  `ntdll` and `api-ms-win-core-synch-l1-2-0`. `d3d12` and `dxgi` load at run time.
- **`native-static-libs` (x64).** `kernel32 ntdll userenv ws2_32 dbghelp /defaultlib:libcmt`
  (`spike/native/native-static-libs-x86_64.txt`). This is rustgs's allow-list (A1a).
  - **Caveat for F0b/A1a:** `include_str!` embeds the WGSL bytes as checked out. `.gitattributes`
    has no rule for `*.wgsl`, so `text=auto` gives CRLF on this machine. A tree with other line
    endings changes the bytes.
  - **Fix:** add `*.wgsl text eol=lf`, or have rustgs refuse CR in an embedded shader. The C path
    is not affected: `wgpu.shader_header` reads the file as text.
- **Boot state trace (minimised window).**
  - The library starts its device on the EE thread and an exact round trip passes. The stub then
    lets the game fall back to D3D11.
  - `trace_columns` against the D3D11-only exe with `--ignore cpu`: every column is identical,
    headless too.
- **Planted faults.** A panic and an uncaptured validation error are logged through
  `dhd_host_log`, in `dhd.log` too, and then end with `0xC0000409`. A second entry between
  `read_begin` and `read_finish` aborts and names the export. All three were re-run here:
  `0xC0000409` each time.

**2. Pins (S0).**
- Resolved for x64: `naga`, `wgpu-core`, `wgpu-hal` and `wgpu-types` are all **29.0.3**
  (`cargo tree --features native --target x86_64-pc-windows-msvc -i …`, re-run here).
- Every crate that wgpu-native v29.0.1.1's Cargo.lock and ours share is at wgpu-native's version,
  with three exceptions:
  - the wasm-bindgen family (0.2.129; js-sys and web-sys 0.3.106; wasm-bindgen-futures 0.4.79);
  - syn 3 (used only by the wasm-bindgen macro);
  - rustc-hash (wgpu-native's graph has it twice).
- `wgpu` itself is 29.0.3. 29.0.4 exists, but 29.0.3 is the release of the same family.
- The wasm32 graph (`--features web`) contains no wgpu-core, wgpu-hal or naga.
- S-present's scratch crate pinned `wgpu =29.0.4`. Its timings stand, but the renderer uses 29.0.3
  as locked. The chain crate (S-web) is on 29.0.3 and passes the same present check.
- **D5:** the pins equal wgpu-native's, so the native-picture criterion is **0 differing pixels
  against the C wgpu-native goldens**. S5 below already shows exact VRAM equality on the same
  inputs.

**3. MXCSR (S3): a red/green probe exists.**
- `fold_probe.wgsl` is built inside the export, with the caller at `0xFFC0` (round toward zero,
  FTZ, DAZ).
- **Without the guard (`no-fp-guard` build):** 4 of 5 compared words differ from IEEE.
  - naga itself folds the literals `0.1`, `1.0/3.0` and `4294967295.0` one ulp low under RTZ.
    Under DXC: `3DCCCCCC`, `3EAAAAAA`, `4F7FFFFF`; IEEE gives `3DCCCCCD`, `3EAAAAAB`, `4F800000`.
  - FXC adds a second ulp on `0.1 + uniform`.
- **With the guard:** 0 of 5 differ. The caller's MXCSR comes back unchanged after every call.
- `raster.wgsl`'s `depth_probe`: 65,536 cases, half with Z32 ≥ 0xFFFFFF00, all equal both ways.
  Its literals happen to be exact.
- This is the 244df66db class, reproduced through naga rather than through FXC alone. The guard is
  required, not just defensive.
- **Exact ops** (`spike_native ops`: pattern, `local_copy`, submit, READBACK/STAGED read): 0 bytes
  differ.
  - Configurations: DX12 FXC, DX12 DXC, Vulkan, WARP FXC and WARP DXC, each also with `0xFFC0`
    around every call (`--ee-mxcsr`).
  - Hardware: RTX 3070, driver 617.42.

**4. Browser (S4; S-web chain, S-present).** Re-run here on the merged branch, with the glue
0. The layout checks pass on the real build, and all five sessions (`main`, `main-prewarm`,
`owned`, `oom`, `lose`) pass every check:
- **Byte-exact twice.** Equal with changed data, over all 512 pages, and from a second promising
  root (the fiber root, as `worker.js activate()` reaches it).
- **Suspension.** The JS heartbeat runs while a read is suspended. Rust is off the stack: the read
  is begin, then the reactor's own Suspending `web_gpu_await()`, then finish.
- **Memory.**
  - Sentinels are intact in every session.
  - The Rust window is all zero before Rust loads.
  - Rust uses the core's memory object (`rx.memory === core.memory`): no clone and no snapshot
    copy.
  - No shared flag and no atomics.
- **No JS frames.** The reactor calls the raw Rust exports wasm-to-wasm.
- **Failure paths.** A map error, a validation error and device loss each return 0 with no hung
  promise. A competing entry is deferred. `Owned<T>` aborts a second entry and names both exports.
  The bounded allocator traps at `0x69000000` without touching a sentinel.
- **Upload.** Direct `write_buffer` is within 2x of gpu.js's staging copy everywhere:

  | Size | Rust `write_buffer` | gpu.js staging |
  |---|---|---|
  | 8 KiB | 2105 MB/s | 2065 MB/s |
  | 4 MiB | 4706 MB/s | 4369 MB/s |
  | 16 MiB | 1027 MB/s | 935 MB/s |

- **Present.** One present per paced field: 60 of 60 drawn, median 0.3 ms.
  - S-present checked every composited screenshot pixel for pixel (0 differing, 1280x960 from
    640x448). The presented texture with COPY_SRC is exact too. Present CPU is about 0.1 ms, the
    same as gpu.js's `show()`.
- **Imports: only `wbg`.** The Rust module's imports are the memory plus 287 glue functions.
  wasm-bindgen 0.2.129 `--target web` puts all of them, memory included, under the module
  `./dhd_render_bg.js` (the name follows `--out-name`), not `wbg`. The locked P0-6 checker
  (`check_web_layout.py`) wants `wbg` (rules M1 and R1), and it stays as it is.
  - **Decision (option b): the build renames the module to `wbg` after wasm-bindgen.** The name is
    only the key of the import object the glue builds. `build.py`'s `name_glue_module` rewrites
    the import section of `dhd_render_bg.wasm` and the one key `"./dhd_render_bg.js": import0` in
    `dhd_render.js`. It stops the build if the module imports from anything but that one module,
    or if the key is not there exactly once.
  - The browser loads the renamed pair, and every session passes.
  - B1: `port/web/build_renderer.py` has to do the same rename, with the same two refusals,
    before the manifest hashes.

**5. Parity smoke (S5): VRAM equal.** `python port/render/spike/parity/s5_parity.py` (this branch,
render/rust merged) does the following:
- builds `test_gpu` exactly as `test_gs_wgpu.py` does (x64, wgpu-native, clang-cl `/MT`,
  `DHD_GS_TEST_BUILD`), but with a scratch copy of `gpu_wgpu.c` that includes `s5_record.inc`.
  `port/gs` is not changed;
- runs it on the C path with `DHD_GS_BACKEND=wgpu`. It records the device ops up to test_gpu's
  first checkpoint ("basics") as a version-1 `.optrace`, in the `port/render/OPTRACE.md` format;
- replays the trace on the spike's Rust library.

Results on RTX 3070, DX12, FXC, `raster_flags` 7:
- The trace has 137 writes, 57 copies, 35 batches, 33 submits and 33 reads.
- Every READ is equal: 15,874 pages. The last READ is the whole checkpoint VRAM, and it also
  equals the `DHD_GS_VRAM_LOG` "basics" record.
- **Negative control:** the same trace with one checkpoint word changed (fnv recomputed) fails at
  that word.
- The replay checks the trace's integrity first (END, counts, FNV-1a), as OPTRACE.md requires.
  `s5_replay.c` is a working prototype of the R1a replayer.

## Open questions (plan section 8) answered

1. **Settings.** wgpu-native v29.0.1.1's effective settings are written out in
   `spike/native/src/lib.rs` and `examples/reference.rs`, for `device.rs`. The source of
   wgpu-native was read (`src/conv.rs` map_instance_descriptor and map_device_descriptor; `src/lib.rs`
   create_shader_module_impl and wgpuDeviceCreateComputePipeline):
   - InstanceFlags empty and no `with_env()` (DEBUG | VALIDATION only for `DHD_WGPU_DEBUG`);
   - FXC, or `DynamicDxc { dxc_path }`; other DX12 options default; no memory budget;
   - the adapter's own limits; dual-source blending when offered; trace off;
   - checked shader modules (`ShaderRuntimeChecks::checked()`, the default);
   - `zero_initialize_workgroup_memory = false`; override constants by name as f64.

   `wgpu` 29.0.3 resolves to core, hal and naga 29.0.3.
2. **i686 DX12 render pipelines:** not run (D2 = c).
3. **MXCSR reproducer:** yes. naga folds constants under RTZ (pass rule 3).
4. **Split readback from a non-root promising root:** works, with Rust off the stack.
5. **Memory and link flags.**
   - The memory import module is `./<out-name>_bg.js`, the same as every glue import. The build
     renames it to `wbg` (item 4 above).
   - `--export-memory` is accepted.
   - `--no-stack-first`, `--global-base=0x60000000` and `-z stack-size=1048576` are honoured:
     data at `0x60000000`, a 1 MiB stack above it, `__heap_base` about `0x6010F460`.
   - rustc 1.97 no longer exports `__heap_base` and `__data_end`. They need
     `--export=__heap_base --export=__data_end` plus wasm-bindgen `--keep-lld-exports` (B1).
   - There is no start section. `__wbindgen_start` is a plain export that the glue calls.
6. **Upload:** direct `write_buffer` is on par with gpu.js. The old 40 MB/s writeBuffer problem
   does not show in Chrome 140. `write_buffer_with` is no escape on the webgpu backend: its staging
   is a wasm Vec. A mapped staging buffer is slower for small writes. **B5 is not needed.**
7. **First-dispatch stall.**
   - `create_compute_pipeline(main)` returns in under 1 ms.
   - The first `main` dispatch stalls 4.5-7 s cold under FXC (6.8 s in this run).
   - A prewarm dispatch in `dhdr_web_init`, awaited through `on_submitted_work_done`, moves the
     stall into init. The first read in `promising(main)` then takes 15-53 ms. **Prewarm: yes.**
8. **`native-static-libs`:** as in pass rule 1 (x64). The raw-dylib imports
   (`api-ms-win-core-synch`, `ntdll`) resolve under lld-link without stubs.
9. **Sizes.** Chain module (the spike subset): `_bg.wasm` 390 KB raw, 108 KB gzip; glue 64 KB raw,
   12 KB gzip. S-present: 328/90 KB, or 169/65 KB after wasm-opt.

Readback in the browser costs about 2.9-3.0 ms per read at every size, for gpu.js and for Rust
alike. mapAsync's turnaround sets that cost, not the copy.

## Proposed G1 decisions

2. **Upload:** direct `Queue::write_buffer`. **Prewarm** in `dhdr_web_init`.
3. **D5:** 0 differing pixels against the C wgpu-native goldens (the pins match).
4. **DX12 compiler:** DXC when `dxcompiler.dll` ≥ 1.8.2502 sits beside the exe, as on the C path;
   FXC otherwise. Both pass the ops and S3. **Memory window:** global base `0x60000000`, initial
   `0x69000000`, 1 MiB stack.
5. **Header and tool notes before G2:**
   - `dhdr_exact_read_begin` must not copy. The spike's version copies the wanted pages from VRAM
     itself. That is right only for READBACK reads right after their copies, and wrong for STAGED
     reads, whose copies were made earlier (`stage_pages`). The ABI text (begin maps; C copies
     through `op_copy`) is correct as written. S5 refuses STAGED reads for this reason.
   - The browser glue module rename (pass rule 4): `build_renderer.py` (B1) does what the spike's
     `build.py` does.
   - `*.wgsl` line endings (pass rule 1).
   - The `--export` / `--keep-lld-exports` flags (question 5).
   - `DhdrDeviceDesc.abi` is 0 in the spike and becomes `DHDR_ABI_VERSION` (1).

## Reference module (S6)

`port/render/examples/reference.rs` is the compiled crib of the wgpu 29.0.3 API, in the shapes the
renderer modules use:
- the MXCSR guard;
- the device with wgpu-native's settings;
- raster.wgsl's three pipelines inside a Validation error scope, with the explicit `main` layout
  (binding 0 a dynamic uniform, min 16) and the override constants;
- a planted invalid module caught by a scope;
- dynamic-offset dispatches in a `ComputePass<'static>` (`forget_lifetime`) kept open across
  calls;
- a copy that ends the pass;
- `map_async` plus `poll(PollType::wait_indefinitely())`;
- a surface on a never-shown Win32 window: Immediate > Mailbox > Fifo, frame latency 1, Opaque,
  COPY_SRC when offered, every `CurrentSurfaceTexture` case, a clear pass, present.

It passes on RTX 3070 DX12, WARP and Vulkan (no surface section on Vulkan). Without the `native`
feature it builds to a stub that prints a note.

```
cd port/render
cargo run --locked --offline --release --features native --example reference [-- --warp | --vulkan]
```

## Sources

- The wgpu, wgpu-types and naga 29.0.3 sources and docs (cargo registry, MIT/Apache-2.0).
- wasm-bindgen 0.2.129 (CLI output, and its source for the `jspi` attribute).
- Finding 084 (the C wgpu renderer, the W5 batching, §10.5.4 i686 DX12, §10.5.6 the WARP
  early-depth fault).

## Gate G1: decisions (2026-10-25)

The gate re-ran the spike's checks on `render/rust` at `5458dc789` (all merged tasks so far), on RTX
3070 driver 617.42, Chromium 140 (Playwright `chromium-1187`):

| Check | Result |
|---|---|
| `cargo tree -i` naga / wgpu-core / wgpu-hal / wgpu-types, `--features native`, x64 | all 29.0.3; the wasm32 `--features web` graph has none of them |
| `spike_native ops`: DX12 FXC, DX12 DXC, Vulkan, WARP; `--ee-mxcsr` | 0 bytes differ in every run (READBACK and STAGED) |
| `spike_native s3`, FXC and DXC v1.9.2609 | guarded 0 of 5 differ; `no-fp-guard` 4 of 5 differ (exit 1); depth probe 0 of 65,536 |
| planted panic, validation error, re-entry | `0xC0000409` each, message logged first |
| `s5_parity.py` | 33 reads, 15,874 pages equal; the one-word negative fails |
| `spike/chain/run.mjs` | exit 0; 64 of 64 checks; `check_web_layout` OK on the real build, stack-first negative red |
| spike `dhd64.exe` (rust, rust2) | pe-sha equal (`baeffbad…`); link log empty, no `/force`; imports all `SYSTEM_DLL`; `game_data_runs` 0 |
| boot state trace, minimised, 600 fields | D3D11-only exe vs Rust-linked exe: every column identical (cpu included), Rust device started and its exact round trip passed |

That is not a leak: `FORBIDDEN` adds the checkout's parent folder without its drive (`\Temp`), and
that matches naga's remapped panic location `…\src\front\wgsl\lower\template_list.rs`. The check
stays as it is.

**Decisions.**
3. **Upload: direct `Queue::write_buffer`.** It is on par with gpu.js's staging copy at 8 KiB,
   4 MiB and 16 MiB, and `write_buffer_with` is no faster on the webgpu backend. **B5 is not run.**
   **Prewarm: yes**, a `main` dispatch in `dhdr_web_init` awaited through `on_submitted_work_done`
   (the cold first dispatch costs 4-7 s otherwise; 4.6 s in the gate's run, 31 ms after prewarm).
4. **Native-picture criterion (D5): 0 differing pixels against the C wgpu-native goldens.** The
   pins equal wgpu-native v29.0.1.1's. VRAM and every EE-consumed readback are exact. No
   case-by-case exception is accepted later; `gs_golden.py` encodes this rule.
5. **DX12 compiler:** DXC when `dxcompiler.dll` ≥ 1.8.2502 sits beside the exe, FXC otherwise,
   as on the C path. Exact referees run both (`DHD_WGPU_COMPILER=fxc` and DXC).
   **Browser memory window:** core memory initial `0x70E00000`, max 2 GiB; graphics reactor
   `--initial-memory=0x50000000`; Rust `--global-base=0x60000000 --no-stack-first -z
   stack-size=1048576 --initial-memory=0x69000000`. The bounded allocator ends at `0x69000000`.
6. **MXCSR guard: required on every export.** naga itself folds float literals one ulp low under
   the EE thread's `0xFFC0`. `fold_probe.wgsl` becomes `guard.rs`'s referee (F0b): red on the
   `no-fp-guard` build, green guarded, under FXC and DXC.
7. **Pins** stay as locked: wgpu, wgpu-core, wgpu-hal, wgpu-types, naga, wgpu-naga-bridge
   `=29.0.3`; windows 0.62.2, windows-link 0.2.1; rustc 1.97.1; wasm-bindgen 0.2.129.

**For F0a (before the G2 freeze) and later tasks.**
- `dhdr_exact_read_begin` only maps; it never copies. C issues the copies through `op_copy` (the
  ABI text is right; the spike's begin copied, which is wrong for STAGED reads).
- `DhdrDeviceDesc.abi` carries `DHDR_ABI_VERSION` (1); the spike passed 0.
- B1/B2: `build_renderer.py` renames wasm-bindgen's glue import module to `wbg` (import section
  and the one import-object key) with the spike's two refusals, before the manifest hashes. The
  Rust link adds `--export=__heap_base --export=__data_end --export=__stack_pointer`, and
  wasm-bindgen runs with `--keep-lld-exports`.
- `*.wgsl` is now `text eol=lf` in `.gitattributes` (this gate), so `include_str!` embeds the same
  bytes in every checkout. A1a's rustgs may still refuse a CR in an embedded shader as a second
  guard.

- `gs_verdict.py` counts `failures=N` again. The earlier fix that stopped reading
  `full_vram_words=N failures=0` as N failures also stopped counting test_hw_renderer's
  `failures=N`; its nonzero exit still failed the run, and now the count does too.
- `harness_negative.py`: a green `failures=0` and a red `failures=2` case, and the
  native-transfers baseline is required (P0-7 has merged; it was reported, not required, before).

## Gate G0: phase-0 referees locked (2026-10-25)

Gate G0 covers P0-2, P0-3, P0-4 and P0-5 and locks every phase-0 referee (plan sections 4.2 and 7).
Everything below ran on `render/rust` (x64, RTX 3070, driver 32.0.16.1742, WARP 10.0.26100.9278)
after the changes listed further down. Scope as decided at G1: x64 and the browser. The i686 runs
are only those that show the frozen build still works.

**Referees run (all green).**

| Referee | Result |
|---|---|
| P0-2: `gs_golden_selftest.py` (new, below) | 68 cases pass; five planted `gs_golden.py` bugs each turn it red |
| P0-2: `gs_golden.py record --out build/rr/g0-run2` + `compare all` | 690 items of 690 identical (test-gpu, test-gpu-ee, replay, replay-native, replay-native-interp, native-transfers, -presented, -stress, web) |
| P0-3: `test_gs_wgpu.py --impl c --only gpu --skip-hardware`, x86_64 and i686 | VERDICT PASS on both (WARP, d3d11 vs wgpu word-identical) |
| P0-3: `replay_gs.py g+0 --backend both --arch x86_64` + `compare replay` | bit-identical; 8 of 8 identical |
| P0-3: `test_native_transfers.py --backend both` + `compare native-transfers` | 33 pictures, 0 VRAM differences; 297 of 297 identical |
| P0-3: `web_gs_replay.mjs g+0` + `compare web` | 1 of 1 identical |
| P0-3: `gpu_js_web.c` at most 150 lines, front end only in `gpu_exact.inc` | 82 lines; `gpu_wgpu.c` keeps only device ops, host hooks and its own exports |
| P0-5: `test_native_transfers.py` (i686 D3D11) | 17 of 17 checked scenes PASS (the 13 old ones plus P0-7's 4 no-draw ones) |
| P0-5: `test_native_frontend.py` | MATCH; its negative control fails as required |
| P0-5: `replay_gs.py g+3600 --backend wgpu --native --interp --arch x86_64` + `compare replay-native-interp` | 4 of 4 identical; the run reports interpolated frames |
| P0-5: wasm32 `-fsyntax-only -DNATIVE_INLINE` | clean (wasi-sdk 34, target `wasm32-wasip1`); without `NATIVE_INLINE` it errors as it should |
| P0-5: no `<process.h>`, `<intrin.h>` or direct Win32 call in `native_core.inc` | none |
| F0c: `gen/gen.py --check` | both tables regenerate byte-identical (after the re-pin below) |
| P0-6: `check_web_layout.py --selftest` | 6 fixtures, each with its verdict |

- `port/tools/gs_golden_selftest.py` (G0's file). It drives `gs_golden.py`'s command line on a
  synthetic golden (no game data) and on copies of `$GOLD`'s test-gpu, replay g+0 and web groups.
  - Must fail: a VRAM word in each file kind, a pixel (PPM, RGBA), an alpha-only byte, a picture
    header, a hash line, missing, extra and truncated files, CR-only in a binary, a missing group,
    a wrong meta count, a zero-item source in every layout.
  - Must refuse: the run's or the golden's adapter, device, driver or WARP version, a changed,
    missing or extra golden file, no `meta.json`, another format.
  - Must pass: unchanged copies, and CR-only differences in text files.
- `gen/gen.py`: the verbatim span of `native_keys.inc` is pinned to `gpu_wgpu_native.inc`
  lines 281-449. R2's recorder had moved it down 55 lines, so F0c's referee was red. The check
  itself is unchanged.
- `replay_gs.py --interp`: P0-5's and JN's referees set `DHD_INTERP=1` in the shell, but the
  harness forwards no `DHD_INTERP`, so they replayed without interpolation. `--interp` now sets it
  and requires the run's "interpolated frames" line. `DHD_INTERP` in the caller's environment
  without `--interp` exits 1.
- `replay_gs.py`, `test_gs_wgpu.py` (`test_gpu`) and `web_gs_replay.mjs` remove their earlier
  outputs before they run. `gs_golden.py compare` reads whatever a prefix or directory holds, so
  a stale picture, VRAM log or hash file could stand in for one the run never wrote. A planted
  stale hardware log now makes `compare test-gpu` fail after `--skip-hardware`.

**Decisions.**
   - `r48-plain` fails on that same exe at g+0 to g+16 in both arms. At g+0, 2,613 of the 2,910
     differing words are i686 code addresses in r48 and wasm table indices in x64 (finding
     081/082). The other 297 are float and zero words that are not explained. r48 is an i686
     0.2.9 recording, so it is not the x64 reference.
   - The boot baseline is `baseline-gates/x64-boot/pre.tsv`.
   - Both folders and `verdict.json` are now read-only and backed up to
     `D:/Backups/dhd-gs-golden-20261025/baseline-gates-x64.zip`.
2. **The optrace goldens stand as recorded.** Their manifest names commit `5836dcdaa` (a P0-4
   branch). `render/rust` now records the same records byte for byte.
3. **`native_transfers-exact.optrace` holds no device op** (DESTROY and END only). Under
   `DHD_RENDER=native` the exact instance of test_native_transfers makes no op, because no Z
   probe happens in those scenes. That is the C path's real behaviour, but A3 gets nothing from
   this trace. A3's exact coverage is the two test_gpu traces (and g+0 and g+3600 at A4).
4. **The interp golden is a smoke check, not a picture oracle.** `replay-native-interp` has the
   same bytes as `replay-native`. With two fields there is never an older field to mix with, so
   the picture read back is the newest field's. The check proves that the interpolation thread
   runs through the replay without a deadlock or crash, not that its frames are right. Frames
   between fields depend on wall-clock time, so no exact golden exists for them. JN keeps the
   same check, now with `--interp`.
6. **`test_native_frontend.py` calls clang-cl directly.** Run it from a `vcvarsall x86` shell
   with LLVM on PATH. Unchanged, since it is locked and works as is.
7. **Notes for later tasks.**
   - A browser native mode must also declare `gs_frame_latency_wait` and `gs_native_view`
     (`gpu_win32.h` does that on Windows).

## Gate J1 (G3): exact parity on x64 (2026-10-25)

Gate J1 covers A1a, A2, A3, A4, HK, C1, A1c, A5, A7, R1a, P0-3 and P0-4. Every run below is on
`render/rust` at `792d312ff` (N9 and D1 merged) plus this gate's harness change. Hardware: RTX 3070,
driver 32.0.16.1742, WARP 10.0.26100.9278. Renderer library: `dhd_render.lib` release, feature
test-hooks, sha256 `25f3eebf…`.

for `rust`, so the C1, A5, A7 and J1 referees could not run as written (A7 reported it).
`test_gs_wgpu.py` and `replay_gs.py` now build `--impl rust`:
- `gpu_rust.c` in place of `gpu_wgpu.c`, which includes `gpu_rust_test.c` in the test build;
- `port/render/include` in place of `webgpu.h`;
- `dhd_render.lib` from rustgs (release, test-hooks), rustc's native-static-libs, and `user32.lib`
  for test_present's window (wgpu-native's library list used to bring it in);
- x86_64 only: `--impl rust --arch i686` exits;
- a never-both check on every link list.

The pass rule, the environment rules and the comparisons are unchanged. The `--impl c` build
commands are byte-identical to before (test_gpu, test_present and the replay on both arches).
`harness_negative.py` gives 26 checks, 0 wrong.

**Referees run (all green).**

| Task | Referee | Result |
|---|---|---|
| J1, C1, A5 | `test_gs_wgpu.py --impl rust --arch x86_64 --only gpu --only depth --only present`, `DHD_TEST_EXPECT_SURFACE=1`, FXC | test_gpu word-identical to D3D11 on warp, warp-early0, hardware and hardware-early0; test_depth 262,144 cases × 5 sets on WARP and hardware with `exact_vs_cpu=0` and `d3d11_vs_cpu=0`, depth probe exact; test_present 0 pixels differ on DX12 WARP, DX12 hardware and Vulkan hardware (18 checks) |
| C1 | the same `--only gpu` with `DHD_TEST_EE_MXCSR=1`, FXC | identical on all four device sets |
| C1, A5 | DXC (`dxcompiler.dll` v1.9.2609 beside the test exes), hardware: test_gpu with EE MXCSR, test_present, test_depth | identical; 0 pixels (12 checks); exact |
| P0-2 goldens | `gs_golden.py compare test-gpu` and `test-gpu-ee` on the Rust VRAM logs | 4 of 4 identical, each |
| A7 | `replay_gs.py` g+0, g+2, g+12, g+16, g+3600, g+4500 `--backend both --impl rust --arch x86_64` | all six bit-identical (pictures, per-field VRAM, last VRAM) |
| A7 | `gs_golden.py compare replay` g+0 and g+3600 | 8 of 8 identical, each |
| A3, A4 | `optrace_replay` on `$GOLD/optrace`: test_gpu-warp-early0 (fallback), test_gpu-hw, native_transfers-exact, g+0, g+3600 | every READ and CAPTURE equal (159 reads, 2,354 pages per test_gpu trace; g+0 1 read, 118 pages, 2 captures; g+3600 1 read, 116 pages, 2 captures; native_transfers-exact has no op, as G0 noted) |
| A3, A4 | the same with `--dxc` on hardware (test_gpu-hw, g+0, g+3600) | equal |
| A2 | `rustgs.py test -- --test device_smoke exact` | 7 of 7 (WARP and hardware, FXC and DXC, Vulkan, no dual source) |
| R1a | `optrace_replay --selftest` | PASS |
| A1a | `rustgs_selftest.py` | all checks pass (no-fp-guard refused, cache hit, equal lib from a second tree, planted include and naga drift refused) |
| A1c | refusals | `DHD_GS_IMPL=rust` refused by `link.py` and `build_clean.py` (D2 = c); `--wgpu --gs-rust` refused |

P0-3 and P0-4 were run at G0. Since then no C-path source has changed (`gpu_wgpu.c`,
`gpu_exact.inc`, `gpu.c`, `gs.c`, `replay.c`, `test_gpu_stubs.c`). `link.py` and `build_clean.py`
only gained A1c's refusals and its census guard (0 duplicates in the recent i686 links). The optrace
referees above re-prove P0-4's traces against the Rust ops.

**Reviews.**
- **Ordering (`exact/encoder.rs`, `readback.rs`, `display.rs`).**
  - `copy` and `record` end the open pass before they record (`Encoder::encoder` drops the pass);
    `submit` ends it before `finish`.
  - `read_begin` submits, then maps; it never copies (G1's note).
  - The batch header's word 3 stays 0, and batch records are 256 bytes apart (`BATCH_STRIDE`,
    shared `gpu_exact.inc`). Display rows are padded to 256 bytes.
- **HK.** `presented.rs` copies the frame through `COPY_SRC` before present, with 256-byte rows;
  `depth_probe.rs` dispatches raster.wgsl's `depth_probe` entry point.
- **Stray files.** None: no build output, traces, pictures or game data among the 115 files added
  since the plan's base.

**Open items (not regressions, not fixed here).**
- **`optrace_replay --selftest --gpu`** (an extra mode; R1a's referee is `--selftest`, which
  passes) aborts on its own synthetic trace. That trace writes 3 bytes at an unaligned offset,
  which the model accepts and wgpu refuses (`COPY_BUFFER_ALIGNMENT`); C never issues such a write.
  The fix is to make that synthetic write 4-byte aligned in `optrace_replay.rs` (locked; R1a's
  line). Not changed here.
- **The `--fake` dhd64 link pulls almost nothing from `dhd_render.lib`.** `/opt:ref` keeps
  `dhdr_version` only. Full symbol resolution of `gpu_rust.c` (with `gpu_rust_native.inc`)
  against the library is proven by the test programs, which link it whole. The real game link
  is D2's (gate G6).

**Verdict: G3 passes for x64.** Exact parity holds against in-process D3D11 and against the C
goldens, with FXC and DXC and under the EE thread's MXCSR. i686 is out of scope (D2 = c), and its
build is unchanged when `DHD_GS_IMPL` is not `rust`.

## Gate B4 (G5): browser parity (2026-10-25)

Gate B4 covers B1, B2 and B3, and does B4's own task, the `worker.js` hookup. Every run below is on
`render/rust` at `cf6a28a6d` (J1 merged) plus this gate's changes, in headless Chromium on the RTX
3070 (driver 32.0.16.1742): Playwright `chromium-1187` for the replays, the installed Chrome for
`check_browser`.

- `port/web/www/worker.js` (B4's file). The worker picks the provider from the reactor's imports
  (`dhd_render` imported: Rust; otherwise gpu.js, unchanged). The Rust path follows plan 3.3:
  1. compile both modules, instantiate the core (it defines the memory);
  2. check that the Rust window `[0x60000000, 0x69000000)` is all zero;
  3. `import('./build/renderer/dhd_render.js')`, then `R.default({module_or_path, memory})`, which
     must return the core's memory;
  4. `await R.dhdr_web_init(canvas, 7)`: device, canvas surface, the exact pipelines for raster
     key 7 (the key `gpu_rust_web.c` passes to `dhdr_exact_create`), and the prewarm dispatch;
  5. instantiate the reactor with `dhd_render` = the Rust module's own wasm exports (each checked
     to be a wasm function, so no JS frame sits between the modules) and
     `env.web_gpu_await = new WebAssembly.Suspending(() => R.dhdr_web_pending())`;
  6. `_initialize`, the thunks, `web_gpu_enable`, `promising(main)`.

  At vsync, `web_display` is followed by `web_present(canvas.width, canvas.height)` when a picture
  is displayed. Both run in IEEE mode, as `web_display` did before. Rust's console lines (its wbg
  log bridge) also go to the page's log. The status message names the renderer.
- `port/web/run_headless.mjs`: stubs for the Rust reactor's `dhd_render` imports and
  `env.web_gpu_await`. Each throws if called, like the existing `web_gpu_*` stubs: Node never
  enables the GPU. Without them a `--renderer rust` `graphics.wasm` cannot be instantiated in Node,
  so B2's referee could not run. The file is not locked and no check changed.
- `port/web/build.py` and `build_graphics.py`: `--renderer rust` is now the default, as the plan
  says (sections 3.3 and 6: "js until B4"). `--renderer js` stays selectable until X3.

**Referees run (all green).**

| Task | Referee | Result |
|---|---|---|
| B2 | `build.py --renderer rust` | exit 0. The core input is P0-8's `build/wasm/dhd_core.wasm` (sha256 `9eef6cbb…`, the same bytes as S-native's independent build; since then the core's sources changed only by `link.py`'s refusals). Renderer module 346,789 bytes (125,207 gzip), glue 71,323 (13,623 gzip). |
| B1, B2 | `check_web_layout.py build/web`, with and without `--require-renderer`; `--selftest` | OK (core, graphics, renderer); selftest: 6 fixtures, each with its verdict |
| B2 | `check_rust_lib.py build/web/renderer/dhd_render_bg.wasm` | ok |
| B2 | `run_headless.mjs --fields 300 --trace-off` | 300 of 300 fields, exit 0 |
| B3, B4 | `web_gs_replay.mjs --renderer rust` on g+0, g+2, g+12, g+16, g+3600, g+4500 | every replay exits 0 with its last field presented. g+0: `gs_golden.py compare web` 1 of 1 identical. g+0 and g+3600: hashes equal the C goldens' `replay/<cap>/wgpu/vram.txt` and `d3d11/vram.txt`. g+2, g+12, g+16 and g+4500 have no golden: their hashes equal J1's in-process D3D11 and Rust runs (J1's D3D11 g+0 and g+3600 files equal the goldens). The six captures give six different hash pairs. |
| B4 | the g+0 replay's canvas screenshot, Rust against gpu.js (`--renderer js`, same tree) | 0 differing pixels; the VRAM hashes are equal too |
| B4 | `check_browser.mjs --iso <USA iso> --field 600` | Rust: field 638 in 26 s, 260 readbacks, 609 frames shown, the title screen in the screenshot. The same tree with only `graphics.wasm` swapped for the js build: field 641, 260 readbacks. Draw counts are equal at equal fields (e.g. 17,732,682 at field 513 in both). |

That is within the noise, so no slowdown is shown. `dhdr_web_init` took 116 ms (device), 818 ms
(pipelines) and 10 ms (prewarm) in the game. In the replays, each with a fresh profile, the
pipelines took 4.0-5.5 s and the prewarm 12-24 ms.

**Not run, and why.**
- **B1's `spike/chain/run.mjs` "on the real crate"** cannot run as written. The chain's stub reactor
  and worker call the spike crate's test exports (`dhdr_spike_*`), which the real crate does not
  have, and its `build.py` builds without `--features web`. The real crate is covered on the main
  path by the layout checks, the six replays and `check_browser` above: shared memory, raw
  exports, the split read from the game's fiber roots, and the prewarm.
- **The failure sessions on the real crate were not run:** forced map error, validation error,
  device loss, a second entry between begin and finish, and allocator exhaustion. B1 ported those
  paths from the spike, which passed them at G1.

**Reviews.**
- **Locked files.** Since J1, the B tasks changed only `web_gs_replay.mjs` (B3's line). It adds a
  Rust page in the plan's order. Its failure handling is unchanged: no result means exit 1, and
  earlier outputs are removed first. The comparison stays in `gs_golden.py`, which is unchanged.
- **Stray files.** None. Since G0 the B tasks added only `port/web/build_renderer.py`,
  `port/web/gpu_rust_web.c` and crate sources: no build output, captures or game data.
- **`gpu_rust_web.c`:**
  - a read that cannot begin, or whose map failed, traps (as `gpu_wgpu.c` exits natively);
  - `web_present` submits the scanout before it presents;
  - a failed present (-1) stops presentation, as `present.c` does.

**Open items (not regressions).**
- The real-crate failure sessions above.
- `dhdr_adapter_name()` is empty in Chrome, so the log says "WebGPU", where gpu.js says
  "nvidia ampere" (vendor and architecture). This is cosmetic.

**Verdict: G5 passes.** The browser's Rust renderer gives the native goldens' VRAM on all six
captures. The three modules' layout holds, and the game reaches field 600 in Chrome with readbacks
and a picture. Rust is now the browser build's default renderer.

## Gate JN (G4): native parity on x64 (2026-10-25)

Gate JN covers N1-N9, R2 and R1b. Scope as decided at G1: x64 and the browser, so the plan's i686
Vulkan replay is not run. The i686 D3D11 transfers run is run, because it shows that the frozen i686
build still works on the shared `native_core.inc`. Every run below is on `render/rust` at
`cf6a28a6d` (J1 merged) plus this gate's commits. Hardware: RTX 3070, driver 32.0.16.1742. Renderer
library: `dhd_render.lib` release, feature test-hooks, sha256 `25f3eebf…`, the same library J1
tested (no Rust source has changed since).

**The `.ntrace` goldens did not exist.** R2's done criterion was
`$GOLD/ntrace/{transfers-dual, transfers-nodual, g+3600}.ntrace`. R2 had only staged the two
referees had no input. The gate recorded all three on the C path, using `gpu_wgpu.c` with R2's
`DHD_NATIVE_RTRACE` recorder in the harness environment on x86_64:
- `transfers-dual` and `transfers-nodual` come from test_native_transfers' wgpu and wgpu-no-dual
  runs (DX12, FXC). `g+3600` comes from the x86_64 replay of `gs-g+3600.gs` with `--backend wgpu
  --native` (Vulkan), the same configuration as the replay-native golden.
- Each trace was recorded twice, and the two recordings are byte-identical. The two transfer
  traces are also byte-identical to the ones R2 staged.
- With recording on, the runs still equal the existing goldens: `compare native-transfers` gives
  297 of 297 identical, and `compare replay-native` on the replay gives 4 of 4.
- The traces are installed read-only in `$GOLD/ntrace`, with `manifest.json` (size, SHA-256,
  commit, how each was run). The folder is backed up to
  `D:/Backups/dhd-gs-golden-20261025/10de-2484-32.0.16.1742-dx12-ntrace.zip`.
- The header's `recorder_sha` is zeros, because the harness build passes no
  `DHD_NATIVE_RTRACE_SHA`. The optrace goldens have the same zeros.

**Referees run (all green).**

| Task | Referee | Result |
|---|---|---|
| R1b | `native_replay --selftest` | 30 cases pass |
| R1b (gate) | GPU negative control: g+3600 with one display byte, then one surface byte, flipped (FNV recomputed so the trace stays valid) | each fails (exit 1), naming picture 0, the pixel, both values and the last command that wrote there |
| N1 | `rustgs.py test -- --test native_keys`; `--test device_smoke native` | 6 of 6; 2 of 2 (WARP and hardware) |
| N2 | `--test native_arenas` | 1 of 1 |
| N3 | `--test native_textures` | 4 of 4 (handle references, decoded round trip, replacement mips, threads) |
| N4 | `native_replay transfers-dual.ntrace --only no-draw` | 7 pictures equal (3,211,264 display pixels, 42 surfaces) |
| N5 | `native_replay transfers-dual.ntrace --only convert-scenes` | 5 pictures equal (2,293,760 pixels, 15 surfaces) |
| N6 | `native_replay transfers-dual.ntrace`; `transfers-nodual.ntrace --no-dual` | 33 of 33 equal in each (15,138,816 pixels, 217 surfaces) |
| JN | `native_replay g+3600.ntrace` (Vulkan) | 2 of 2 equal (1,835,008 pixels, 8 surfaces) |
| N8 | `test_native_transfers.py --backend both --impl rust` + `gs_golden.py compare native-transfers` | 33 pictures, 0 RGB and 0 VRAM differences in all three pairs; 297 of 297 identical |
| N7 | the same with `--presented` + `compare native-transfers-presented` | 33 pictures checked against the window in each WebGPU run; 297 of 297 identical |
| N7 | `test_gs_wgpu.py --impl rust --arch x86_64 --only present`, `DHD_TEST_EXPECT_SURFACE=1` | 0 pixels differ in all 18 checks |
| N9 | `test_native_transfers.py --backend both --impl rust --stress` + `compare native-transfers-stress` | 8 rounds per run, 201, 252 and 254 replacements created and released; 7 pictures identical; 63 of 63 identical |
| R2 | recording on: test_native_transfers' runs and the g+3600 replay, each recorded twice | outputs equal the goldens (above); traces byte-identical |
| JN | `replay_gs.py gs-g+3600.gs build/rr/jn --backend both --native --impl rust --arch x86_64` | D3D11 vs Rust (Vulkan): 0 of 917,504 pixels differ in both pictures; VRAM per field and last VRAM bit-identical |
| JN | the same with `--backend wgpu` + `compare replay-native`, and with `--interp` + `compare replay-native-interp` | 4 of 4 identical, each; the interp run reports its interpolated frames |
| JN | `test_native_transfers.py` (the frozen i686 D3D11 build) | 17 of 17 checked scenes pass |

N4's, N5's and N6's referees could not have passed when those tasks merged: render.rs called
placeholder methods until N9 connected it to surfaces.rs, and no `.ntrace` existed. They pass now,
on the merged tree.

**Beyond the referees: the game's own configuration.** The x64 game draws natively on DX12, and it
ships `dxcompiler.dll`. The goldens, by contrast, are FXC, and the replay-native ones are Vulkan.
So the gate also compared the C path with the Rust path directly, in the same harness environment
(scratch drivers, not committed):
- test_native_transfers (wgpu and wgpu-no-dual), DX12, with `dxcompiler.dll` v1.9.2609 beside both
  exes: C and Rust give 33 of 33 pictures with identical RGBA and VRAM.
- The g+3600 native replay on DX12 with FXC, and again with DXC: C and Rust give identical
  pictures (both PPMs), per-field VRAM hashes and last VRAM.

**Reviews.**
- **Files.** Each task changed only its own files. N9 fixed two bugs in files it does not own,
  render.rs (N6's) and `gpu_rust_native.inc` (N8's). Neither file is locked, and N9 reported both
  fixes. No placeholder is left in the crate: no `log::todo` or `log::unimplemented` is called
  outside log.rs. No stray file: no trace, picture or build output is tracked.
- **`gpu_rust_native.inc` against `gpu_wgpu_native.inc`.** The eight contract functions do the
  same steps in the same order: the texture and sharp-font views, the replacement view, the
  surface scale and growth, the window scale, the present rectangle and cut, pacing outside
  DHD_INTERP, the read-back size check, and the movie at 4:3. Two differences, both harmless:
  - A surface grown twice before one picture gets its oldest contents copied in once. The texture
    from the first growth holds only those contents plus zeros, so the result is the same.
  - A failed present (-1) stops presentation, as the ABI says. The C path skips a failed acquire
    silently.
- **R1b (`native_replay.rs`).**
  - It compares every result counter, every display byte (alpha included) and every surface
    byte, with no tolerance.
  - It refuses a trace of another version, adapter, driver, API, compiler or fallback adapter.
    It also refuses a DUAL trace on a device without dual-source blending.
  - On Vulkan the trace stores driver version 0 and the replayer reads 0, so the driver check
    applies on DX12 only. NTRACE.md allows this ("or 0"), and `gs_golden.py` pins the driver for
    the replay-native compare.
  - Re-locked (below).

- `$GOLD/ntrace` recorded and installed, as described above.
- `test_native_transfers.py`:
  - `--impl rust` with the default `--backend d3d11` (the i686 build, which has no Rust
    renderer) now exits with the arch refusal. Before, it built the i686 program and then failed
    on the missing renderer line.
  - The docstring no longer says that rust "errors until the Rust build exists".
  - The `--impl c` commands are unchanged, and `harness_negative.py` gives 26 checks, 0 wrong.

**Open items (not regressions).**
- **No run draws a movie, a texture-pack replacement or the sharp font.**
  - No capture or synthetic scene has `movie_on`.
  - The stress test creates and releases replacements from a second thread, but never draws them,
    because the test programs stub `texpack_open` and `font_sharp_view`.

**Verdict: G4 passes for x64.** The native renderer's pictures, counters and surfaces equal the C
wgpu-native path's with 0 differing pixels (D5). This holds on the synthetic transfers (dual and no
dual, presented, stress), on g+3600 (Vulkan from the trace and the harness; DX12 with FXC and DXC
directly against C) and under DHD_INTERP. VRAM stays exact throughout. The frozen i686 D3D11 build
still passes its 17 scenes.

## Gate D2 (G6): game integration on x64 (2026-10-25)

Gate D2 covers D1 (package refusals) and the game integration itself. Every run below is on
`render/rust` at `fca6bbb2b` (JN and B4 merged). Hardware: RTX 3070, driver 617.42 (32.0.16.1742).

**The exes.** Both are built from this tree, from P0-8's wasm core module (sha256 `9eef6cbb…`, the
same module B4 used; no core source has changed since P0-8 built it) and P0-8's generated VU1
headers, copied into the gate tree's ignored `build/`. The ELF was passed with `--elf`, not copied.
- Rust: `package_x64_test.py --wasm … --impl rust --elf <ELF>`, which runs `build_host64.py
  --gs-rust` (rustgs release, no test hooks; `dhd_render.lib` sha256 `4ee2123f…`). `dhd64.exe`
  sha256 `534f1d86…`, 45,812,736 bytes.
- C, for the comparisons: `build_host64.py --wgpu` from the same tree (sha256 `920a8a1c…`). The
  game-state gate also compares with P0-8's frozen pre-change C exe (`b084262c…`).

**Referees run (all green).**

| Task | Referee | Result |
|---|---|---|
| D2 | census of the x64 game link | `lld-link` without `/force` (and with `/errorlimit:0`) exits 0 with an empty link log: 0 undefined and 0 duplicate symbols. `link.rsp` names `dhd_render.lib` once and no `wgpu_native.lib` (the never-both assert ran). The map holds all 33 non-test, non-web `dhdr_*` exports, so the real game link pulls in the whole renderer (J1's `--fake` link kept only `dhdr_version`). The core module's census: strict link ok, 136 imports, all `host_api`. |
| D1 | `package_x64_test.py --impl rust --elf <ELF>` | exit 0: no problem from `exe_problems`, no game-data run, imports only Windows DLLs (no `d3d12.dll` and no `opengl32.dll`: wgpu loads D3D12 itself) |
| D1 | the same checks with no exemption at all | `exe_problems(exe, set(), [])`: 0 problems (the C exe has 343 without wgpu-native's path exemption). `game_data_runs(exe, elf, [])`: 0 runs. The Rust exe needs neither exemption D1 allows. |
| D1 | `package_release.gs_libraries` | `('c', 'i686')` gives exactly the old `[wgpu.library('i686')]` for both lists, so the i686 release check is unchanged. `rust` on i686, `rust` without a library and an unknown impl are refused. |
| D2 | `state_trace_run.py boot --name rr-boot --exe <rust exe>` + `trace_columns.py baseline-gates/x64-boot/pre.tsv rr-boot.tsv --ignore cpu` | 600 rows; every column identical, `cpu` included (also without `--ignore`). The run logs the `gs: renderer dhd_render …` line. |
| D2 | `gate_native_renderer.py --exe <rust exe> --out build/rr/d2 --reference baseline-gates/x64-reference`, run through P0-8's `run_on_desktop.py` (a separate desktop that is never shown, so the window presents) | NATIVE RENDERER GAME-STATE GATE MATCH: EE MATCH at all 6 moments in both arms and against the reference; READBACK MATCH, 1,132 records through field 9,053; 9,003 native frames, 8,973 presented. FXC (no `dxcompiler.dll` beside the gate's copy). |
| D2 | the gate's race pictures (exact and native, at GO+0, 2, 12, 16, 3600 and 4500) against P0-8's run of the frozen C exe | 12 of 12 pixel-identical (exact 512x448; native 1024x896 at the default scale 2) |
| D2 | minimised `dhd64.exe` runs to field 900 (`SW_SHOWMINNOACTIVE`, the boot recipe's virtual clock, the race menu route), screenshots at 11 fields from 60 to 899, Rust and C from this tree, with `dxcompiler.dll` beside both (DXC) | without movies, exact: 11 of 11 pixel-identical. Native with `DHD_TEXTURE_PACK` (the PCSX2 pack, 6,181 replacements) and the default sharp font: 11 of 11 pixel-identical. |

**Movies, the texture pack and the sharp font (JN's open item).** These runs are the first that
draw them on the Rust path.
- **Movies.** With movies on, both renderers play the startup movies in exact and native mode with
  no error. Movie frames are timed by the wall clock, so they are not repeatable: C against C
  differs in 2 or 3 of 11 frames per pair, always on movie frames. Apart from the cold texture-pack
  frame below, Rust against C differs only on such movie frames (1 of 11 exact, 3 of 11 native).
  The other frames, movie frames among them, are pixel-identical.
- **Texture pack.** With `DHD_PROFILE=1`, 27,040 draw states used a replacement in the Rust run.
  C's last profile line, printed on a wall-clock timer, gives 27,781. The pictures are identical.
  One run, the first with a cold disk cache, drew the original memory-card background at field 60;
  two repeats and three C runs drew the replacement. `texpack.c` loads a replacement on loader
  threads the first time it is drawn and draws the original until then, so a cold cache can delay
  the replacement by a few fields.
- **Sharp font.** `DHD_FONT=original` changes only the text ("RIDER SELECTED", 15,362 pixels at
  field 420), so the default run does draw the sharp font. That default run equals C's.

**Speed (information only: the PC was shared).** The gate's unpaced race averaged 71.3 fields/s
exact and 119.9 native on Rust. P0-8's C run of the same gate averaged 68.4 and 109.3.

**Reviews.**
- **D1's diff removes no check.**
  - `package_release.main` passes the same wgpu-native library as before
    (`gs_libraries('c', 'i686')`).
  - `package_x64_test.py` narrows `SYSTEM_DLL`: `opengl32.dll` is now allowed only for impl c.
  - The Rust library counts only for the game-data scan, never as a path exemption, and an
    unremapped one is refused (the negative control above).
  - `link.rsp` must agree with `--impl`.
- **Stray files.** None. Since the plan's base the tree adds the same 115 files as at J1: no build
  output, captures, pictures or game data.
- **i686.**
  - No i686-compiled C has changed since G0. Only `gpu_rust.c`, `gpu_rust_native.inc` and
    `gpu_rust_test.c` changed, and they are x64 only. G0 built and ran the i686 test programs.
  - A1c's census guard now stops `link.py` on duplicates. P0-8's i686 game link had 0 duplicates
    and the 23 baseline undefined symbols.
  - A full i686 game link was not run: it is out of scope.

**Open items (not regressions).**

**Verdict: G6 passes for x64.**
- The game links with the Rust renderer, with no undefined or duplicate symbols and only Windows
  imports.
- It passes the package checks with no exemption, and an unremapped library is refused.
- Boot game state is identical to the pre-change exe's.
- The race game-state gate matches the x64 reference in both arms.
- Every picture is pixel-identical to the C renderer's: the race gate's, and the minimised menu
  runs' (exact; native with the texture pack and the sharp font; FXC and DXC).
