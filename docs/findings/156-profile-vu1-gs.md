# 156: Where a frame's time goes, before replacing the VU1 and GS emulation

Status: in progress

Branch `perf/profile-vu1-gs` from `main` 085117f, started 2026-11-05. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

The owner's direction (2026-11-05): keep the decompiled game code, and consider replacing what is
still emulated for graphics (VU1 microprograms, VIF/DMA packets, the GS command stream and its
renderer front end) with native rendering. Before any design, measure where a frame's time goes
today, per thread and per stage, so the gain of a native path is known.

Measure the current main (no speed changes in this lane) on:
- **Windows** (this PC: the CPU and GPU named in dhd.log),
- **macOS** (the M1 MacBook Air build host),
- **Android, the Mi 11i** (later, after the next mobile release; the lead says when).

Per machine, at least: the attract/title, a menu with a 3D scene (the bike shop), the race start
(the slowest interval on phones, findings 147-148) and mid-race, uncapped
(`DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1`), at the scales players use there (the owner,
2026-11-05: 1x and 2x neither are played on desktops nor stress them):
- **Windows and macOS**: 4x, 8x and `DHD_SCALE=window` (the picture at the window's own
  resolution), in windows of the screen sizes players use: Windows 2560x1440 and 3840x2160 (on the
  hidden desktop, if the harness allows those sizes), macOS the built-in display's native size;
- **the phone**: 2x (the phone default) and 4x. For each: frames per second and the time per frame
of each thread and stage: EE game code, VU0, VU1 (native kernels / translated / interpreted),
VIF/DMA, the GS front end (exact or native), the renderer's CPU side (wgpu encoding, uploads,
conversions, readbacks such as the Z read), and the GPU. Use what exists first (dhd.log `perf:`
lines, finding 129; the Tracy/simpleperf/Instruments routes in docs/TESTING.md); add only
instrumentation that is cheap and worth keeping.

Acceptance: a table per machine in this finding, the method to repeat it, and a ranked list of
what a native VU1+GS path would remove (ms per frame and which thread), versus what stays (game
code, audio, presentation). No game-state change.

## Progress

- [x] 2026-11-05: finding reserved
- [x] 2026-11-05: scales changed by the owner: desktops 4x, 8x, window (2560x1440, 3840x2160; the
  Mac's own display); the phone 2x and 4x
- [ ] Windows tables
- [ ] macOS tables
- [ ] phone recipe (the Mi 11i is measured later, after the next mobile release)

## Decisions

## Surprises

## Result
