# 097: the lens flare's Z probe caused the race stutter (issue #2)

Status: current
Checked: 2026-11-04

## The stutter

Players saw frame drops and hitches in races (issue #2), on fast PCs too. Automated races at a
player's settings (fps = monitor on a 144 Hz screen, scale 3, vsync 0, frame latency 2) show it in
the game's own present times (`DHD_PACING_LOG`) and in PresentMon:

- 1% low 72 fps, 0.1% low 40 fps, 879 frames over 10 ms in a 200 s race;
- the GPU busy for up to 94 ms in a single picture (`DHD_SPIKES`).

`port/tools/stutter_race.py` runs that race on the real desktop with the game's AI riding: the start
menu by `DHD_NAV`, `DHD_RACE_PAD` holding the accelerate button. It records PresentMon for the
game's process and closes the game after `--seconds`. `port/tools/stutter_report.py` sums up the
folder: frame times, PresentMon's present modes, and the `DHD_SPIKES` lines by kind.

## The cause

The lens flare (func_00228328) reads a 4x4 patch of the Z buffer every field where the sun is.
Scenery nearer than the sun there hides the flare. The PS2 answers that read from GS memory. The
native renderer kept a second, exact GS on the GPU only to answer it: every draw that wrote Z was
drawn a second time into the exact GS. In a race that was 842,000 draws/s, 4,756 compute
dispatches/s, 1,158 4 MB texture snapshots/s, and 608 ms/s spent waiting on readback maps. With the
exact draws switched off (a diagnostic switch, since removed), the 1% low went from 66 to 115 fps.

## The fix

The probe now reads the native renderer's own depth (`port/render/src/native/zread.rs`,
`native_zread_*` in `port/gs/native_core.inc`):

- When the game reads Z, the EE side maps the read to the Z buffer's layout and asks for that
  neighbourhood (`zread_want`, the read's box plus 16 pixels, at most 64 x 64 GS pixels). It
  answers each pixel from the newest finished copy.
- After each picture the render thread runs a small compute pass. It loads the centre texel of
  each GS pixel from the scene's Z surface into a buffer, then maps a copy of that buffer without
  waiting. A depth32float texture can only be copied whole, so a texture copy can't do this.
  The next picture takes the newest finished copy. The answer is one field late: the depth of the
  picture before.
- The Z surface is the one the scene draws: same page, width and layout. A small 64 x 64 Z surface
  starts at the same page.
- The depth is converted with the surface's own Z format. The game reads its Z24 buffer as
  PSMZ32. Only the low 24 bits are written over the CPU image, so the top byte (other data, which
  Z24 draws leave alone) stays as it was.

`DHD_EXACT_PROBE=1` keeps the exact path. The state-trace tools (`state_trace_run.py`,
`test_fps_race.py`) set it, so their game state stays the PS2's. `DHD_EXACT_PROBE=2` uses the exact
answers and compares the native ones with them for each read ("native zread: compare" lines).
`DHD_NATIVE_PROBES` traces reads, copies and takes.

## Checks

- Correctness, `DHD_EXACT_PROBE=2` in a 200 s race: 7,228 reads.
  - 95.9% have the same number of covered pixels (Z written) in both answers, and 97.2% are
    within 2 of 16.
  - Only 12 reads disagree on hidden (12+ of 16 covered) against shown (4 or fewer). Each lasts
    one or two fields, while the sun crosses an edge: the field of lag.
- Visual check: screenshots with the native probe (`DHD_CAPTURE_FIELDS` every 80 fields).
  - Where the native answer says covered, the sun is behind a rock wall, a mountain or a tree,
    and no flare is drawn.
  - Where it says open, the sun and its flare show.
- Speed, the same automated race and settings (RTX 3070, 144 Hz):

  | | before (exact probe) | after (two runs) |
  |---|---|---|
  | 1% low | 72 fps | 119 / 99 fps |
  | 0.1% low | 40 fps | 71 / 57 fps |
  | frames over 10 ms | 879 | 10 / 51 |
  | frames over 20 ms | 21 | 7 / 6 |
  | frame time p99 | 11.3 ms | 7.7 / 8.8 ms |

The second run's extra slow frames came in the 10 s before the start, while shaders and textures
load. The median stays 6.95 ms (144 fps).

`port/tools/zprobe_compare.py` compares the Z reads of two `DHD_GS_READBACK_TRACE` files. Two runs
drift apart in time, though, so the per-read comparison of `DHD_EXACT_PROBE=2` is the one to trust.

## Log writes froze the game (0.6.4)

With `DHD_SPIKES = 1` (about 80 lines a second) a player's race had freezes of 100-520 ms, about 30
s apart. The render thread was stuck in the gap where it writes a log line: `dhd.log` was opened
unbuffered and written by the game's own threads, and a write to the disk sometimes took half a
second. Without the logs the same race had none (1% low 126 fps at 144 Hz).

Now, when dhd.exe opens `dhd.log` itself (started from Explorer or the launcher, or relaunched after
an update), stdout and stderr write into a named pipe and a thread copies it to the file
(`log_start` in `port/hle/host.c`). The pipe's reading end is overlapped: on a synchronous one,
`PeekNamedPipe` waited for the thread's pending read, and the exit hung. The tail reaches the file
at exit (atexit), on a crash or abort (`log_settle`), and in ExitProcess (`log_last_words`, a TLS
callback). A console or a tool's file or pipe stays as it was.

## The mouse at startup crashed the game (issue #11)

Started from Explorer, the game crashed in about half of the starts with "read of uncommitted guest
address 0x4" in `menu_mouse_field`. A mouse move over the window in the first fields reached
`shell_mouse`, whose "the front end runs" test (`fields - shell_seen < 8`) passed before any shell
frame had run, and `func_0023D650` read view 0's object before the game had made it. The test now
also needs a shell frame to have run (`shell_running` in `port/hle/menu_page.c`). 
