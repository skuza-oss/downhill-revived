# 052 — Frame pacing and input latency (round 38)

Status: partly superseded by 139 (the fixed 59.94 Hz grid at 60; D3D11 present details)
Checked: 2026-11-04

Still true: just-in-time frames (the EE waits until `next_due - work`, then re-reads the pad;
`DHD_LATENCY=0` gives the old order, `port/hle/kernel.c`) and the precise wait
(`host_wait_wall`). Changed: the grid is the selected rate's, and every frame steps by the real
time since the last (the delta-time clock, finding 139), not a fixed 59.94 Hz field; the DXGI
flip-model, waitable-object and `ALLOW_TEARING` details describe the retired D3D11 present
(finding 088) — the Rust renderer presents through wgpu, `DHD_TEARING` still reads there
(`port/gs/gpu_rust.c`); the precise wait is one shared `host_wait_wall` (`port/host/host_sdl.c`:
a sleep to 1 ms before the deadline, a high-resolution timer on Windows, nanosleep on POSIX, then
a pause-instruction spin; finding 099).

Goal: a flat 16.68 ms present line on a 144 Hz G-SYNC monitor with the game at 59.94 pictures
a second, and the youngest pad state the game can use, without changing the simulation.

## Where the pacing was

- `port/hle/kernel.c` `vsync()` is the emulated VBLANK. It ends one frame and starts the next:
  pace → graphics_wait → gs_vblank → pad_vsync → widescreen → sound_tick → VBLANK_START
  handlers → graphics_wait → gs_vsync (picture handed to the presenter) → VBLANK_END handlers.
- `pace()` waited for the next 59.94 Hz tick *before* the VBLANK, so the pad was read just after
  the wait and the frame built from it was shown one field later. Measured pad-to-present age:
  17.1–17.3 ms in a race.
- The EE runs on a virtual frame clock in paced mode, so where the wall wait sits does not change
  the simulation (gate below).
- `host_wait_wall` (port/hle/host.c) slept with `Sleep` and then spun; the presenter
  (port/gs/present.c, port/gs/gpu_native.inc) presented with sync interval 1 and no latency limit.

## What changed

Just-in-time frames (kernel.c, default when paced; `DHD_LATENCY=0` gives the old order):
- The picture is handed to the presenter at once; the presenter holds it to a fixed 59.94 Hz
  due grid (`pacing_present`), so the line is set by one wait on one thread.
- After the VBLANK the EE waits until `next_due − work`, where work is the largest of the last
  300 frame times (frames longer than a field are left out unless more than 1 in 10 are) plus
  1.5 ms. Then `pad_refresh()` (port/hle/pad.c) re-reads the host pad into the newest pad frame
  (buttons, sticks, pressure only; scripted input modes are left alone).
- A picture that is still late moves the grid: the presenter keeps at least one field after the
  last picture and publishes when it showed it; the EE starts its next frame from there. No short
  frame follows a long one.
- More than one frame behind: the grid re-anchors to now.

Precise wait (host.c `host_wait_wall`): a per-thread `CreateWaitableTimerExW(...,
CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` to 1 ms before the deadline (fallback `timeBeginPeriod(1)`
and `Sleep`), then a `YieldProcessor` spin on QueryPerformanceCounter. The timer woke up to
0.85 ms late here, hence the 1 ms spin. Deadlines are absolute (`next = prev + period`).

Presentation then (present.c, gpu_native.inc; D3D11, retired):
- FLIP_DISCARD with `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`, plus `ALLOW_TEARING`
  when `IDXGIFactory5::CheckFeatureSupport` says yes (`DHD_TEARING=0` turns it off).
- `IDXGISwapChain2::SetMaximumFrameLatency(1)` and the waitable object, waited on before drawing
  (fallback `IDXGIDevice1::SetMaximumFrameLatency(1)`). ResizeBuffers repeats the flags.
- `Present(0, DXGI_PRESENT_ALLOW_TEARING)` (not in exclusive full screen): G-SYNC shows each
  picture when presented, our limiter does the pacing.
- `DHD_INTERP` keeps its own path; `DHD_PACING_STATS=1` prints pad-to-present age every 600
  pictures; `DHD_PACING_LOG=prefix` writes per-frame CSVs.

## Measured

PresentMon 2.6.0 on the presenting child process, 20 s of a scripted race, borderless 1280x960,
native renderer, scale 4. Present intervals (ms):

| build | mean | sd | min | max | mode |
|---|---|---|---|---|---|
| before 39063ee3 (b3) | 16.683 | 0.143 | 15.71 | 17.67 | Independent Flip |
| before (b1, b2) | 16.683 | 0.121–0.130 | 15.57–15.97 | 17.36–17.71 | Composed Flip |
| after cb8e185b (f1) | 16.684 | 0.028 | 16.55 | 16.91 | Composed Flip |
| after b00c9174 (f3) | 16.687 | 0.080 | 16.41 | 19.18 (one frame) | Independent Flip, tearing |
| new exe, old order (o1) | 16.683 | 0.087 | 16.26 | 17.23 | Composed Flip |

Before, long and short intervals came in pairs (17.7 then 15.6); after, a late picture is one
longer interval with no short one after it. Pad-to-present age (CPU side): 17.1–17.3 ms before;
14–16.6 ms in the race and 12.7–14.2 ms late in the race after. The gain is small in a heavy
race because frame work peaks near 15 ms (mean ~9.7, p99 13.7). Windowed (not borderless) is
composed at the desktop rate and judders (display-change sd 3.6 ms); borderless is the mode to
use with G-SYNC.

## The queue before the presenter (2026-10-28, `desk/integrate3`)

With deferred vsync (finding 090 HO1) up to two fields wait for the GS, and the hitch at the start
(a 115 ms gap before the second picture) left three pictures queued before the presenter. The rule
above followed the presenter only when it was one picture behind, and the presenter shows one late
picture a period after the last, so the queue never emptied: each picture was presented 52 ms
after its due, the work samples were all longer than a field (work = a whole field, no
just-in-time start), and pad to present was 69-83 ms (`DHD_PACING_STATS`; round 2's build the same).
A Mac worker measured 45-73 ms from a press to the menu reacting.

`host_pacing_wait` now also follows a presenter two or more pictures behind, while it is presenting
(a minimised window presents nothing): the picture handed now is shown `behind` periods after the
presenter's last one, and the grid starts there, once, so the queue empties. Measured on Windows
(RTX 3070, 143 Hz desktop, windowed 896x672, native 2x, V-Sync; the title screen; a probe that is
not committed: a key down from SDL's thread every 0.7 s at a random phase, the first pad read that
holds it, the present of the picture whose pad read did):

| build | key to pad read | pad read to present | key to present |
|---|---|---|---|
| before | 7.5 ms | 82.6 ms (82.3-82.9) | 90.1 ms |
| before, `DHD_LATENCY=0` | 8.1 ms | 17.4 ms | 25.6 ms |
| before, `DHD_VSYNC=0` | 10.0 ms | 12.9 ms | 22.9 ms |
| after | 8.3 ms | 3.0 ms (2.4-4.6) | 11.4 ms |

The present spacing is unchanged (16.68 ms, sd 0.01 before and after, no gap off by 2 ms in 2,500
pictures); pictures are presented at their due (median lateness 52.1 -> 0.0 ms) and the work
estimate is real again (2.6 ms on the title). In a race (DHD_NAV, no `pad_refresh`) pad to present
is 13-16 ms. The game's own frames between reading a press and drawing its effect come on top,
the same on every platform, and so does the display (DWM composes a windowed game).

## Gate

`port/tools/gate_native_renderer.py` on b00c9174 (and on cb8e185b before it): both arms match
each other at every EE moment (+0, +2, +12, +16, +3600, +4500) and in all readbacks (1,139
records on b00c9174; 1,197 on cb8e185b, byte-identical to the pre-pacing round 37 gate). The
comparison to the canonical reference fails on 65 static/menu words, as the round 37 gates did
before this work (explained in finding 050); against round 37's dumps only 3 words differ
(0x4F4234/0x4F4254/0x4F4324, from the ONLINE menu commits, constant at every moment). The pacing
does not change EE state.

## Sources

- RetroArch "frame delay" (start emulation late in the frame to shorten input age).
- NVIDIA Reflex (just-in-time frame start, render queue of 0–1).
- Microsoft docs: DXGI flip model, `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`,
  `IDXGISwapChain2::SetMaximumFrameLatency`, variable refresh rate / `DXGI_PRESENT_ALLOW_TEARING`,
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`.
- PresentMon (MIT, used as a tool only, not committed) for present intervals and present mode.
No code was copied.
