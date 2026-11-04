# 112: Fewer native render passes

Status: current
Checked: 2026-11-04

Branch `perf/fewer-passes`, based on `5beba4d3e`. Source mirror, builds, caches, APKs, traces and
captures stay on F:; none are committed. No proprietary input was changed.

## Method and implementation

Read findings 108, 109, 110/111 including the merged-build decision. Cross-run screenshots and
pixels are not a correctness gate. Use the required 5,000-field Android versus x64 exact-probe
trace, unchanged three-word mask and all thirteen columns, plus rustgs tests. A single final
screenshot is for the lead's eyes only.

Instrumentation landed before optimization (`3c777ff73`). `DHD_PASS_TRACE=1` logs attachment IDs,
begin/load/store and end reason for one picture in 240. Auxiliary events label surface rectangle
passes, present (clear/store), compute conversion, scanout and Z probe. It is off by default.
`DHD_RSTAT` continues to count **draw render passes only**, so its number must not be called the
total number of GPU render/compute passes. Encoder copies do not themselves create render passes.
GPU timers are off during measurement.

Consecutive draws already reused a pass on identical attachments. The changes:

- Keep an already bound depth attachment across depth-disabled draws to the
  same color target. Both ordinary and alpha-fallback pipelines use Always
  with depth writes disabled; actual depth-tested draws keep their old keys.
- Cache conversions for the current picture, keyed by the write versions of
  all four source surfaces. Every target/depth draw conservatively invalidates
  its surface; transfer commands invalidate their destination. Immutable base
  textures are picture inputs. The existing consecutive-conversion rule stays.
- Before an attachment run, prepare its independent conversions together.
  Stop lookahead at any transfer, color/depth surface change or depth-enable
  change. Exclude conversions reading either run attachment and exclude the
  current consecutive conversion. No draw changes order. No copy moves across
  a draw. Every conversion whose source can change stays at its original site.

The first cache/depth-only diagnostic still had 18 draw passes; grouping the independent conversions
removes six of those boundaries in the sampled race picture. The compute conversion count stays ten:
this batches their position, not their numerical work. Original behavior is retained for feedback
copies, readbacks, dependent conversions and target changes.

## Pass boundaries

Windows x64, Vulkan, seed 81257, 120 field clock uncapped, scale 1, native renderer, overlap and
fold enabled. Diagnostic picture 7200 (race):

| Draw-pass end reason / auxiliary work | Before | After |
|---|---:|---:|
| Color target change | 7 | 7 |
| Conversion interrupts an open draw pass | 10 | 4 |
| End of picture / scanout | 1 | 1 |
| Depth-only change | 0 | 0 |
| Transfer / feedback / clear-only breaks | 0 | 0 |
| **Draw render passes** | **18** | **12** |
| Compute conversions | 10 | 10 |
| Compute scanout | 1 | 1 |
| Folded compute Z probe | 1 | 1 |
| Swapchain render pass, clear/store | 1 | 1 |

The original diagnostic did not label the auxiliary present/probe events; source inspection
establishes those unchanged operations, and the final transcript labels them. Representative picture
totals are not whole-race means. Busy-scene pass counts still vary. Growth and surface-rectangle
events are separately labelled when present, rather than assigned to ordinary draws.

Clear-only passes were not found on this path: present and depth growth already use `LoadOp::Clear`
in the consuming pass. GS clear sprites can be partial, masked or ordered with blends, so they are
not replaced by unconditional clears. No `StoreOp::Discard` was introduced: persistent GS
color/depth surfaces can be read by subsequent draws, conversion, probe or the next picture. There
is no proven dead persistent attachment. Texture feedback requires its ordered copy; no unsafe merge
across that copy was attempted. No stencil attachment exists in these native draw passes. Load/store
for persistent surfaces stays Load/Store.

## Phone protocol

Use copies of finding 111's `phone120.py`, lock and GO-window scripts, from the repo root. No
`/sdcard/PS2` access. Preflight requires zone43 <= 40 C **and** battery <= 33 C.

OFF is the APK built at `581d298f8`: `git diff 581d298f8 5beba4d3e` changes only three documentation
files, so its executable source is exactly main's baseline. ON is freshly linked against the final
Rust library; `verify-apk.log` proves its APK library matches a separate final link byte for byte.
The unchanged portable core and existing C objects are reused. Both arms set `DHD_GPU_OVERLAP=1
DHD_ZREAD_FOLD=1 DHD_FPS=120 DHD_FRAME_CLOCK=1 DHD_VSYNC=0`. Run order: OFF 1x, OFF 2x, ON 1x, ON
2x; cooldown before each. Only one pair per scale. Window is each run's GO+360..GO+3600 (3,241
timeline rows), complete 240-picture timing blocks only. CPU clocks and thermal medians use samples
inside that same GO interval, without integer temperature truncation.

With overlap, Rust present includes finish/submit: report its exclusive part as
acquire+record+present-submit minus arena-write, finish and queue-submit. C `show` is swapchain
presentation and stays separate. Render busy is C frame, excluding handoff wait. Do not add nested
present timing a second time.

## Sources

- Local pinned wgpu 29.0.3, renderer and vendored HAL (`PATCH.md`), read alongside
  findings 108-111. No HAL or shader change.
- [wgpu StoreOp documentation](https://docs.rs/wgpu/latest/wgpu/enum.StoreOp.html):
  discard makes the attachment uninitialized; safe only when not subsequently read.
- [wgpu render-pass implementation](https://github.com/gfx-rs/wgpu/blob/trunk/wgpu-core/src/command/render.rs)
  and [pipeline documentation](https://wgpu.rs/doc/wgpu/api/render_pipeline/struct.RenderPipelineDescriptor.html):
  attachment/pipeline compatibility. No external implementation copied.

## Results

One requested uncapped OFF/ON pair at each scale; all four intervals have 3,241 timeline rows,
GO=6022, and thirteen complete renderer timing blocks. The table reports interval means, with
sampled interval medians for clocks and temperatures. The phone's CPU0 clock differs at 1x and all
clusters differ at 2x: do not infer clock-normalized throughput or sustained performance.

| Run | fields/s | draw passes/frame | render busy ms | CPU0/4/7 GHz | zone43 / battery C |
|---|---:|---:|---:|---|---|

| off1 | 115.1 | 18.15 | 8.497 | 1.094/2.112/2.150 | 62.50/26.0 |
| on1 | 115.2 | 12.15 | 8.376 | 1.805/2.112/2.150 | 68.60/31.5 |
| off2 | 93.9 | 18.15 | 10.555 | 1.094/2.112/2.150 | 66.10/30.0 |
| on2 | 93.0 | 12.15 | 6.134 | 1.805/1.670/1.670 | 65.65/33.0 |

Render-thread parts, milliseconds per frame (exclusive present removes the nested finish/submit;
`show` remains swapchain presentation):

| Run | build | encoder.finish | queue.submit | present exclusive | show | Z read | handoff wait |
|---|---:|---:|---:|---:|---:|---:|---:|
| off1 | 0.632 | 1.872 | 2.686 | 0.110 | 2.456 | 0.529 | 0.084 |
| on1 | 0.683 | 1.850 | 0.872 | 0.108 | 4.151 | 0.495 | 0.201 |
| off2 | 0.736 | 2.195 | 6.548 | 0.100 | 0.705 | 0.063 | 0.040 |
| on2 | 0.996 | 2.377 | 1.576 | 0.134 | 0.728 | 0.056 | 4.549 |

Preflight zone43/battery: off1 30.4/25 C; off2 35.7/28 C; on1 37.0/30 C; on2 waited at 46.6/32 until
39.3/32 C. The baseline and candidate set identical overlap/fold switches. GPU clock was not sampled
by the inherited method; CPU clocks above are observations, not forced frequencies. No clocks,
cooling policy or display settings changed.

1x: pass count falls by about a third, but throughput is essentially flat (115.1 -> 115.2 fields/s).
Busy time is 8.497 -> 8.376 ms, still above the 8.333 ms target. Queue-submit time falls 1.814 ms
while show grows 1.695 ms: submission/presentation pressure moved between buckets. Finish is nearly
flat. Thus the pass reduction is proven; a meaningful 1x speed gain is not.

2x: busy time falls 10.555 -> 6.134 ms, queue-submit 6.548 -> 1.576 ms, but throughput is 93.9 ->
93.0 fields/s. The candidate runs at lower CPU4/7 clocks (1.670 versus 2.112/2.150 GHz). Render
handoff wait grows 0.040 -> 4.549 ms; timeline VU work is 7.80 -> 9.29 ms/field and GS work 5.21 ->
7.05. This run is game/VU limited after the renderer saves work. The single pair cannot isolate how
much of the timing difference is due to pass batching versus clock/thermal changes. No additional
phone runs were used to select a favorable result.

## Verification and evidence

**PASS: final Android versus final x64, 5,000/5,000 fields, all thirteen columns identical:** menu,
phase, clock, ram, scratch, hw, cpu, vu0, gs_stream, gs_regs, gs_vram, sound and code. No --ignore
argument; unchanged three-word mask. `trace-columns.log`, `raw/final-headless.tsv` and
`src/build/state-trace/final-x64.tsv`. Android reports 5,000 rows and canonical ini restored byte
for byte before releasing its lock. This is the required exact-probe gate, not a new claim about
timing-dependent native-probe publication.

- Final x64 exact-probe/headless trace: 5,000 rows, exit 0, seed 81257,
  scale 2, 60 Hz, 4:3, widescreen off, overlap/fold set. `final-x64.tsv`.
- Final rustgs suite: **33 tests PASS**, zero ignored/failed, exit 0
  (`final-tests.log`, `test-exit.txt`). DX12/Vulkan device tests included.
- Windows hidden-desktop final Vulkan and DX12: start, race, exit 0.
  `final-diag-console.log`, `diagnostic-on/game.log`, `dx12-console.log`,
  `dx12-smoke/game.log`. These are smoke checks, not phone speed estimates.
  2x field 10000 (GO+3978, after the timing window). Log confirms the capture
  was written by this run. Rider, terrain, sky and HUD visible. No before/after
  pixel or screenshot comparison was made. The inherited archive directory
  also contained old captures; only this newly logged field was selected.
- APK/library identity: `verify-apk.log`; independently relinking against the
  final Rust archive produces the same stripped library bytes embedded in ON.
- Raw logs, GO timeline/parts, preflight, clocks, restore assertions:
  `raw/{off1,on1,off2,on2}*`, `window-report.txt`, `parts.json` and session logs.
  Every timing session finished its byte-checked canonical ini restore.

APK SHA-256, OFF/ON:

`off 81c3822b01734a8f82f2436a70f94b2764f81e8243245d96b44c2ecbadb1764c`

`on 3d663bb2a4df6914f1a28c8767d6751f47984ac2d9c8bb0b61c3452e89df187e`

## Remaining work

1x needs at least another 0.043 ms off mean render busy, plus enough margin for field-time tails and
the observed 115 fields/s pipeline throughput. The dominant ON bucket is now swapchain show (4.151
ms), followed by finish (1.850 ms); investigate presentation/driver backpressure and compute-pass
recording before making another throughput claim. VU is 7.39 ms/field in this 1x interval, under
budget but without a large thermal margin. At 2x the measured candidate renderer is already below
8.333 ms; VU at 9.29 ms and thermal clock policy prevent 120 in this run. Ten compute conversions,
true color-target switches, dependent conversions and the ordered feedback/copy/probe boundaries
remain. Further changes require the same exactness gate. This work does not promote either existing
switch to a default and does not claim that 120 fields/s has been reached. 