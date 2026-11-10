# 194: Native VU (experimental), a Graphics setting on every platform

Status: in progress

Branch `feat/native-vu-option` from `main` cdee456, started 2026-11-10. This file is the lane's
plan while it runs and its finding when it merges. Someone who has only this file and the git log
must be able to pick the lane up.

## Goal and acceptance

The owner's decision (2026-11-10): ship the native VU1 path (VU1 microprograms run as GPU shaders
instead of being emulated; findings 162, 164, 167, 168, 172, the local branch `native/vu-gs`) as an
experimental option on every platform, **off by default**.

- Bring the native work onto this branch from today's `main`: merge `native/vu-gs` (26 commits on
  `main` 9ecb8e1), keeping what finding 172 kept and leaving out what it dropped. `native/vu-gs`
  itself is not rewritten.
- One setting: **Native VU (experimental)** in REVIVED SETTINGS > Graphics, next to the Performance
  group; a dhd.ini key, default off, in the settings crate, the game's page and the launcher's
  settings, the way finding 183 added its settings. `DHD_NATIVE_VU1` stays as its override (its
  per-program values for testing).
- Safety: a native pipeline or shader a driver refuses (PowerVR, Mali, Intel, WebGPU limits) sends
  that draw path back to the emulated one for the session, with one dhd.log line saying why; the
  game never closes over it. Finding 188's Intel dual-source rule holds in the native path.
- Platforms: Windows fully; macOS on the owner's Mac (build, a harness run with the option on and
  off); Linux build; Android release-signed APK, the phone run with the option on and off; the
  browser built, the option working or hidden there with the reason.

Acceptance:

- Option off: byte-for-byte today's `main`: boot and race state traces equal to `main`'s, the
  picture unchanged.
- Option on: the game's state unchanged (state traces), pictures within finding 164's tolerance
  (per field MAE <= 0.1, at most 0.1 % of pixels off by more than 8) against the emulated path.
- Timing: Windows RTX at 8x, off against on; the Mi 11i on the t612 profile at 2x, off against on
  (the recipes of findings 146-149 and 172).
- `python tools/check.py port` passes; the release APK builds.

## Progress

- [x] 2026-11-10: finding reserved (`36be34c`)
- [x] 2026-11-10: `native/vu-gs` merged (`32df819`): four conflicts, all beside each other (main's
  pass trace fields and the mesh group flag in `render.rs`, the raster scanout stage and the mesh
  vertex stage in `native_spirv.rs`, two docs tables); the native findings keep their numbers (main
  has no 162, 164, 167, 168 or 172). Builds on Windows; core id unchanged by the merge (the VU1
  code is the host's, `vif.c`).
- [x] the setting, live, and the fallback (below)
- [ ] pictures: main against off (identical), off against on (finding 164's tolerance), 2x and 4x
- [ ] state traces: boot and race, off against main; on (forced skip and shadow) against off
- [ ] Windows RTX 8x timing, off against on
- [ ] macOS: build and a harness run, on and off
- [ ] Linux build
- [ ] Android: release APK, the Mi 11i (t612, 2x) off against on
- [ ] the browser: build, the row hidden
- [ ] `check.py port`

## What came over from `native/vu-gs`, and what did not

All 26 commits, by a merge (the branch itself unchanged): finding 162's tools (`route_profile.py`,
`gs_census.py`, `sample_profile.py --stacks`, `DHD_VU1_LAYOUT`), the twelve translated batch loops
(findings 164, 167: 0170, 1708, 08B0, 0798, 18B0, 1110, 13A0, 1510, 3140, 0AA0, 0C30, 2DE8),
finding 168's object records in the vertex buffer and `DHD_RSTAT`'s counters, and finding 172's one
kept step, the draw constants read by instance index (which also changes the GS path's draws: their
constants are one array bound per 256 draws instead of a dynamic offset per draw; the pictures are
identical, finding 172). What finding 172 dropped was never on the branch (kept outside the
repository): the shared pipeline layout, texture arrays, lazy constant reads, generic pipelines.

## The setting

- **dhd.ini `native_vu = on|off`** (`port/render/settings/src/table.rs`, a switch, default and
  built-in `off`), read into `DHD_NATIVE_VU1` as every setting is; `DHD_NATIVE_VU1` set outside the
  game wins (the page shows the row locked, "Set by DHD_NATIVE_VU1 outside the game"). Besides `on`
  and `off` it keeps the native lane's test values: a list of loops by address or group (`prelit`,
  `lit`, `shadow`, `all`), then `:skip`, `:shadow`, `:gpuclip` (`gs_mesh_parse`, `port/gs/gs.c`).
- **REVIVED SETTINGS > Graphics**: "Native VU (Experimental)", the row right above the Performance
  group, hint "VU1 programs run on the GPU: faster, but new" ("Only with the native renderer" with
  `renderer = exact`). On Android's SETTINGS too (`DHD_PC_SETTINGS_ROWS`). The launcher's Graphics
  page has the switch with the same words.
- **Live.** The page sets the variable and calls `gs_present_options_apply` (the host, the game
  thread), which reads it into `gs_mesh_setting` (an atomic word: the loops and flags); the VU
  thread reads that word before every VU1 run (`mesh_enabled`, `port/hle/vu1_mesh.inc`), so a change
  takes effect at the next batch. The VU thread reads no environment: `host_settings_load` reads the
  word first, before the game runs. Turning it at any batch is safe: a skipped batch leaves the VU
  as its program would (finding 164), and translated batches have always mixed with batches run on
  the VU (the near-bound ones, about 15 % of 0170's). One dhd.log line at each change ("native VU1:
  loops ... drawn by the native renderer, the VU skips them", "native VU1: off; the VU runs every
  program"); the `settings:` line names it as every setting.

## The fallback

- **The renderer decides whether it can draw meshes** (`port/render/src/native/init.rs`): the mesh
  layouts are made only where the device has storage buffers for the vertex stage
  (`DownlevelFlags::VERTEX_STORAGE`, at least one storage buffer a stage), inside a validation error
  scope; a refused mesh draw pipeline (`render.rs`'s `set_pipeline`, which already logs and skips a
  refused pipeline, finding 176) and a vertex buffer larger than one storage binding also refuse.
  Each writes one dhd.log line, "gs native: Native VU can't run on this GPU (why); the VU runs
  every program", and sets a flag for the session, `dhdr_native_meshes()` (`dhd_render_ext.h`).
- **The game goes on emulating.** C sets `gs_mesh_mode` from it at the renderer's start
  (`native_install`) and clears it after any picture where the flag says no
  (`gpu_rust_shared.inc`); the VU thread sees the cleared mode at its next batch and runs every
  program itself ("native VU1: Native VU needs the native renderer with indexed fields (or the GPU
  refused its draws); the VU runs every program"). A mesh draw already recorded when the refusal
  came is skipped (its pipeline failed), so at most a picture or two lack those objects.
- **Test:** `DHD_TEST_NO_MESHES=N` refuses at the renderer's N-th picture (0: at its start).
- **The browser** never makes the mesh layouts (no line: by design) and its VU is built without the
  translation; the row is not offered there (`port/web/www/settings.js`).
- **Intel's dual-source rule (finding 188)**: mesh draws choose their blend variant in the same
  `draw_passes` as every draw, so without dual-source blending (Intel, or `DHD_WGPU_DUAL_SOURCE=0`)
  a mesh draw blending on As takes the two-draw form like any other; the mesh entry points of the
  dual-source module exist only when that module is made.

## Decisions

- **Both paths are kept while native is experimental** (the owner, 2026-11-10): the emulated VU1
  path stays beside the native one, an explicit exception to the single-implementation rule (root
  AGENTS.md, decision 099), as for the P2P experiment (180). The plan: once native is proven on
  every platform (pictures, state, speed, drivers), it becomes the default and the emulated draw
  path for the translated loops is deleted.
- **Merged, not rebuilt**: the conflicts were few and side by side, so the branch's history stays
  as it was measured.
- **The row above the Performance group, not under a heading of its own**: an "Experimental"
  heading made the Graphics tab eleven rows, and the last ran into the "L1 / R1 Tabs" legend.
- **Live, not at the next start**: switching at a batch is safe (above), and it costs the VU thread
  two atomic loads a VU1 run.
- **The vertex buffer's storage use** is added where meshes are possible, whether the option is on
  or not (the buffer is made before the option can change); the off path's draws do not read it.

## Surprises

- `vu1_mesh.inc`'s browser stub had no `MESH_ON`, which `vif.c` uses: the browser build never
  compiled on `native/vu-gs`. Added.
- `DHD_PASS_TRACE` measured a mesh draw's bounds from its vertices, which are object-space: such a
  draw's bounds are now its scissor.

## Result
