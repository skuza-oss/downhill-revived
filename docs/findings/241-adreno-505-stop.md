# 241: Adreno 505 device loss after the menus start

Status: current
Checked: 2026-11-14

Branch `fix/adreno-505-stop` from local main 92a7eb4a, started 2026-11-14.

## Goal and acceptance

Investigate issue #91's log and known driver faults. Ship only an evidenced narrow
workaround, or the missing diagnostics and a precise reporter request. No phone,
Mac, push, external post or timing benchmark. Gates: quick, docs, Android native;
port for renderer/host changes, state traces if game state could change.

## Progress

- [x] Read issue/comments and the entire attached current/previous run log.
- [x] Research driver neighbours; add narrow loss-observation diagnostics, no rendering workaround.
- [x] Quick, port and docs gates pass; Android native and focused diagnostic tests pass.
- [x] Quick and docs gates pass. Android native library builds and links all 147
  imports. Focused `rustgs.py test -- --lib loss_context::`: three tests pass,
  including the real nested operation scopes, disabled path, counts and driver selector.

## Decisions

The run is 0.7.16: do not assume the reporter tested the offered 0.8.0 or Native VU off.

## Evidence and limits

GPU Adreno (TM) 505, Qualcomm build 9d44e36 / Ie3bb699d95, dated 09/29/20,
compiler EV031.27.05.02; Android 10, Redmi 8, 3.5 GB total RAM. Both sessions
reach the full-width 3D menus (field 192), compile further draw pipelines and then
report `GPU device was lost (Unknown): Device is lost`. Native VU is on, dual-source
blending is used, Mailbox is at 60 Hz. The failed native present and the game's
intentional device-loss shutdown follow. No Vulkan assertion, OOM, surface-lost
message or freeze-watch termination appears. The later SIGSEGV is in Android's
RenderThread, with GLES/EGL/HWUI stack frames, not a demonstrated Vulkan shader
compiler crash. It may be a consequence of the GPU fault or of showing the exit
dialog; the log does not distinguish those. Do not present either as proven.

The exact draw/dispatch responsible is unknown. The last pipeline creation message
does not establish that its draw caused the asynchronous fault. Total RAM and
texture counts do not prove available memory or exclude a driver allocation failure.
The driver could be at fault, or valid-looking application work could expose another
bug: no reproduction or validation trace proves which.

Lane 239 alone cannot fix this report: **scale=1 and shadows/lens_flare/particles=0
already hold in both failed runs**, at a 640x448 game picture and 1520x720 window.
Reducing from 2x could help a different memory-pressure case, but not these settings.

## Research

- Exact searches for both build hashes and the compiler version found no matching
  wgpu/Qualcomm Vulkan loss remedy. The compiler version occurs on other devices
  and API paths; it is not an adequate workaround selector by itself.
- [PPSSPP 17426](https://github.com/hrydgard/ppsspp/issues/17426) and
  [17434](https://github.com/hrydgard/ppsspp/issues/17434): device loss has several
  causes, including driver/internal allocation failures. A generic device-lost
  return is not a specific diagnosis. Device recreation there is not justification
  to retry our faulting work (finding 171: the exact GPU state answers game reads).
- [PPSSPP 16422](https://github.com/hrydgard/ppsspp/pull/16422), with
  [16015](https://github.com/hrydgard/ppsspp/issues/16015): game-specific old-Adreno
  **GLES** hangs. This game's primary loss is Vulkan; no API fallback added.
- [PPSSPP 11691](https://github.com/hrydgard/ppsspp/pull/11691) and
  [11694](https://github.com/hrydgard/ppsspp/pull/11694): color-mask/stencil defects,
  not this reported loss. Our native depth format is Depth32Float, stencil ignored.
- [wgpu 3345](https://github.com/gfx-rs/wgpu/issues/3345): Android rotation can cause
  suboptimal surface returns; that is not device loss and is not in this log.
- No external code copied; no speculative shader, blending, timer or surface workaround.

## Missing diagnostics added

`port/render/src/loss_context.rs`, enabled on Android only for Adreno 505 with all
four reported driver markers (both hashes, date and compiler). It logs enablement
once. At the first reported device loss it logs the observing CPU operation and
export/thread plus process-wide submit/acquire/present entered/returned counters.
Unlike finding 231's startup breadcrumbs it remains active after three presents.
Other devices get no extra log, and other platforms compile the scopes away.

No per-frame log or file I/O, GPU wait, resource allocation, rendering option or
recovery path is added. Counters are atomic snapshots, not a global ordering or GPU
completion fences. The callback may run asynchronously on an untracked thread; the
log explicitly says so rather than attributing it to the last draw. This narrows
where loss was observed; a fresh on/off comparison is still needed to isolate the
GPU work. The loss callback only reads CPU state and logs, never re-enters wgpu.

## Surprises

Both runs already use 1x with all three Performance Mode effects off. The primary
failure is a reported Vulkan device loss after 20/22 seconds in menus, after 333/352
presents, not a startup stall. One later crash is in Android HWUI's RenderThread,
with libGLESv2_adreno.so / libEGL / libhwui frames, after the device-loss exit path.

## Result

`check.py quick -j 6`, `check.py port -j 6` and `check.py docs` pass; the port gate
includes the desktop build, retail-data scan, renderer WARP tests, settings/launcher
tests, stack analysis and its negative controls. Android native build passes (147
matched imports). Three focused diagnostic unit tests pass. No phone accessed.
This is diagnostic instrumentation, not a proven cure; no same-driver reproduction,
GPU validation trace, Native VU comparison or confirmation of recovery is claimed.
No state trace required for this diagnostic-only change: no game code, shaders, GPU
commands, resource sizes, waits or settings change; desktop production scopes are
no-ops. The only active path is CPU counters on the reported Android driver and a
log at device loss, after the device is unusable. No state-changing workaround shipped.

## Reporter request (draft for the lead, not posted)

Thanks! Both runs in your log lose the Vulkan graphics device after about 20 seconds
in the menus. They already use 1x and Performance Mode, so lowering those cannot fix
this particular stop. The later crash is in Android's graphics driver while closing;
we have not identified the operation that first caused the device loss.

Please try the test build the lead supplies. First leave Native VU on, stay in the
menus for at least one minute (or until it closes), then use the app icon's Share log
shortcut immediately. Next set Settings > Graphics > Native VU off in the launcher,
start again, repeat that same wait and share a separate log. Keep resolution at 1x
and Performance Mode on for both. Tell us whether each run stays open, freezes or
closes, and whether 0.7.14/0.7.15 previously worked. Send both complete files; don't
start extra runs between a failure and sharing. The new `loss context` line remains
active beyond startup and will help us narrow the failure. No adb or computer needed.

Checked:
- 0.8.4 gate on ad4b92f9: `check.py port`, `check.py server` and `check.py docs` OK; boot and race state
  traces equal the reference build.
- Diagnostics only; not run on an Adreno 505.
