# 243: Rider tent close on a Helio P35 phone

Status: current
Checked: 2026-11-14

Branch `fix/rider-tent-crash` from local main 65f94597, started 2026-11-14.

## Goal and acceptance

Issue #68: identify the failure from available evidence; no speculative renderer
fix. If evidence is missing, add only decisive diagnostics and a precise reporter
request. Gates: quick, docs, Android native; port for renderer/host changes. No
phone, Mac, visible game window, push, post or timing benchmark.

## Progress

- [x] Read the issue body and every comment: **no attached log exists**. The dhd.log
  field only says RAM 4 / Helio P35. No game version or driver version is provided.
- [x] Trace tent work and diagnostics; add GE8320 runtime pipeline begin/returned
  lines (both ordinary and specialised builds), preserving every pipeline result.
- [x] Three focused trace tests pass: exact GPU family, begin-before-create with
  an error result preserved, and disabled path with no formatting/logging.
- [x] Quick and docs gates pass; Android native build links all 147 imports.
- [x] Port gate passes, including desktop build, retail-data scan, renderer WARP
  tests, settings/launcher tests, stack analysis and its negative controls.

## Decisions

The lead's association with #56/#60 is a hypothesis, not a log from #68. No new
reply confirms either 0.7.14 or 0.7.16 fixed this reporter's phone.

The preceding menus already draw 3D riders: the tent is not necessarily the first
model or Native VU draw. It selects different rider/bike/jersey geometry, textures
and draw states. Native VU's translated lit/skinned batches use the mesh vertex
entry and object-record layout (`port/hle/vu1_mesh.inc`, `native/pipelines.rs`).
The native front end also handles render-target/texture reuse and rider shadows
(`port/gs/native_core.inc`). Nothing in #68 identifies a bad size, index, format,
count or first failing draw; there is no evidenced forced-limit PC reproduction.

Finding 176 establishes one Bike Shop-only blend combination on another GE8320
driver, not this phone: a pipeline/compiler refusal surfaced as OOM. Its kept-colour
draw elimination, pipeline error scopes and soft failure are already present.
Finding 170 handles an earlier startup compiler crash. Findings 152/171 retain native
crashes and report device loss; 231's detailed breadcrumbs stop after three presents.
Finding 241's runtime loss context is deliberately Adreno-specific and remains so.
None of those establishes what killed #68's process without a log.

## Diagnostic change and reading it

`native/pipelines.rs` logs `gs pipeline: begin ID key=KEY: vs=... fs=...` and the
existing draw summary (specialisation, texel scale, blend, colour mask, depth).
`gs pipeline: returned ID key=KEY` means the driver call returned, **not** that its
pipeline or subsequent GPU draw succeeded; existing scoped error lines decide that.
IDs pair concurrent generic/specialised creations. Both paths already share `build`.
Only Android Vulkan names PowerVR Rogue GE8320 / PowerVR GE8320 enable it; no driver
version is guessed. No per-draw/per-frame logging or new GPU operation is added.
The C log bridge drains this prefix into dhd.log, as for startup lines, so an immediate
driver crash cannot strand the preceding line in Android's asynchronous stderr pipe.

An unmatched begin plus a compiler backtrace would identify pipeline creation; a
returned pair plus a scoped refusal identifies its state/error. A later device-loss
line is a different failure. Native crash frames, real resource-allocation errors,
Android exit records and freeze-watch lines remain distinct. An unmatched begin alone
cannot distinguish a slow compiler, a hang, or an external kill: ask what the phone did.
No extra startup record or broad feature workaround is introduced.

## Research

- [Issue 68](https://github.com/skuza-oss/downhill-revived/issues/68): body and all
  three comments checked; no attachment links or inline logs.
- Findings 170/176: existing GE8320 compiler/pipeline fixes and their limits.
- [wgpu 10027](https://github.com/gfx-rs/wgpu/issues/10027): pipeline refusal can
  otherwise appear as later device loss. Our vendored handling already addresses
  pipeline errors; no new mapping is justified by #68.
- [Imagination's GE8320 discussion](https://forums.imgtec.com/t/use-glprogrambinary-in-powervrge8320-always-execute-glslcompiletouniflex-is-slow/4103):
  full compilation versus binary loading can have different costs. It is GLES
  evidence, not proof of this Vulkan crash or a reason to change shaders.
- No external code copied. No GPU limits, shaders or validation checks weakened.

## Surprises

Finding 176 already addresses the known Bike Shop dead-colour blend and refused
pipelines; do not reapply it or infer real OOM from the phone's total RAM.
Lane 239 selects 1x + Performance Mode only for previously unset choices. That can
reduce pixel load or memory pressure, but cannot be claimed to fix a compiler or
native crash, and #68 does not say what settings the failing run used.

## Result

Quick, port and docs gates pass; Android native build passes (147 matched imports).
The three focused trace tests pass. No phone, visible game window or new fault
reproduction was used. No fix for this reporter's crash is claimed without a log.
No game-state trace is needed: GPU descriptors, draws, resources, settings and game
code are unchanged. The new lines only describe cold pipeline creation on GE8320;
other devices/platforms take the original no-log path.

## Draft reply for the lead (not posted)

We don't yet have a log from your phone, so we can't tell whether this is the older
Bike Shop problem or another crash. Please try the build containing this diagnostic
and enter the tent again. If it closes, long-press the app icon and use Share log
immediately; attach the entire text file. If it stays open with a frozen picture,
leave it in front for at least 10 seconds before closing and sharing.
Tell us whether it happens opening the tent, choosing a rider, or cycling bikes,
which rider/bike was selected, and whether sound/input continue. Keep 1x and Performance
Mode on. If it still fails, set Native VU off in launcher Settings > Graphics, repeat
the same action, and send that second complete log separately. Your saves can stay.

Checked:
- 0.8.4 gate on ad4b92f9: `check.py port`, `check.py server` and `check.py docs` OK; boot and race state
  traces equal the reference build.
- Diagnostics only; not run on a Helio P35 phone.
