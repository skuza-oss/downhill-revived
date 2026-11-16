# 262: three small fixes after 0.8.7: the guard in a shared folder, 258's Result, software GPUs

Status: current

Branch `fix/small-262` from `main` e9bba855 (v0.8.7), started 2026-11-16. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

1. **The pipeline guard and two games in one data folder** (seen by the 0.8.7 release job): a
   second game started from the same folder read the first game's live `pipelines-building.txt`
   as a dead run's, and later could not write it (`Access is denied`). Two such starts in a row
   could refuse a healthy pipeline (`!KEY`) and, for a mesh key, turn Native VU off. Wanted: a
   key in the marker counts only when the run that wrote it is over; a second game in the same
   folder does not write the guard's files; a failed write is logged once and never becomes a
   refusal. Unit tests, and on the PC (hidden) two games from one folder, twice in a row, leave
   no `!KEY` and no `?KEY`.
2. **Finding 258's Result** is filled in from its Progress and the 0.8.7 release job, and
   `port/tools/host64_selftest.py`'s Linux variant passes with the two-part guest layout (or is
   deleted if nothing uses it).
3. **A software adapter** (WARP / Microsoft Basic Render Driver, llvmpipe, SwiftShader): the
   player is told plainly why the game is so slow, in the game and in the launcher when it can
   know; never on a real GPU. A unit test for the decision; a hidden run forced onto WARP shows it.

`check.py quick`, `port`, `docs`; boot and race state traces equal to the base.

## Progress

- [x] 2026-11-16: finding reserved (6e67354f)
- [x] the guard's lock (e51d7a60), unit tests `a_run_that_died_building_is_judged_on_evidence`
  (a second game twice in a row beside a live marker) and `one_game_a_folder_holds_the_guard`
- [x] finding 258's Result, `host64_selftest.py` on Windows and Linux (d58f5949)
- [x] the software adapter's notice in the main menu and the launcher (1ef13a5a)
- [x] checks, state traces, hidden runs (Result)
- [x] the lead's review: a lock that can't be used keeps the guard (ffde7ed4, unit test
  `only_a_held_lock_gives_up_the_guard`); guest.h follows `HOST64_TWO_PARTS` (e4408ed5, 80fa0902);
  a press shorter than a field still counts (35fc0328, `test_keyboard`); the notice logs its
  close (73a17f3d)
- [ ] the lead: the merge

## Decisions

- **Job 1, a lock held for the run's life, not a process id in the file.** At the native
  renderer's create, the game takes `pipelines-guard.lock` beside the list without waiting
  (`list::claim`: std's `File::try_lock`, LockFileEx on Windows, flock on POSIX) and keeps it until
  the process ends; the system frees it at any end, a crash or a kill included. Only the holder
  judges and writes `pipelines-building.txt` and the crash note. A game that finds the lock held
  logs one line, reads the refused keys of `pipelines.txt` (they still hold) and writes neither
  file (`recover_files(.., owner: false, ..)`); so a live marker is never judged, and a marker is
  judged only once the run that wrote it is over. A process id with its start time needs a
  liveness query per platform and is open to id reuse; the lock is one call everywhere.
- **Only a held lock makes a second game.** `File::try_lock`'s `WouldBlock` (another process
  holds it) is the one answer that gives up the guard; any other error (a file system without
  locks, a folder it can't open) keeps it, as 0.8.7 did (one game taken), with one line: `gs
  native: pipelines-guard.lock can't be used (...); this game is taken to be the only one in its
  data folder and keeps the pipeline guard`. Else a PC whose locks fail would lose the guard at
  every start and crash the same pipeline every launch (0.8.6's lock-out).
- **A marker is judged only when it can be removed first.** One that stays (a folder the game
  can't write) would be read again at every start, and a stale one could look like "two runs in a
  row". The start removes the marker before it judges it; when the removal fails the marker is
  not judged and the guard is off for that run (one line). A write that fails during the run is
  logged once, stops the guard's writes, and removes the marker when it can.
- **Job 3, the game's own popup in the main menu**, the disc notice's (finding 208): the game had
  no timed text at the start, and the popup is what the main menu already shows a player once.
  Once a run, before the disc notice, OK or Triangle closes it; not remembered (the next start on
  that adapter says it again). The core asks the host through the existing
  `host_service_gs_scale` with `asked` 2 (no new import, the browser's answer is already 0); the
  host answers 0 on a real GPU, else 1, 2 or 3 for the platform, because the core is one module
  for every platform and the text names the platform's APIs.
- **Software means wgpu's `Cpu` type or Microsoft's 0x1414:0x8C** (WARP, whatever type it reports;
  the same test FXC's choice already made): `device.rs` `software`, `dhdr_perf_read`'s kind 4.
- **The launcher's line** comes from the last run's dhd.log (`gs: software adapter: ...`), the way
  its resolution hint already reads the log: shown on the Play page until a run on a real GPU.
- **A press shorter than a field still counts, for every menu.** The game reads the pad once a
  field (pad.c at VBLANK: `host_pad_device`); on WARP a field lasts about 300 ms (3.3 a second at
  the main menu), so a quick tap of OK fell between two reads and the page never saw it (the
  first hidden WARP run: posted keys held 120 ms were lost at the title and on the notice). The
  latch is in the input layer, so the notice, the disc notice and every other menu gain: the
  keyboard keeps each key pressed since the last read (`keyboard_taps_take`), a controller each
  button held since it (`gamepad_raw_take`, under the pad lock; the CONTROLS page's waits and the
  connected test keep the plain `gamepad_raw`). A tap reads as held for one field and let go on
  the next, the edge a page needs. At 60 fields a second nothing changes but a press shorter
  than 16 ms. Touch needed nothing: a tap is a click (present.c counts presses by event) and the
  menu holds the button it makes for three fields (`menu_page.c` `press`, `PRESS_FIELDS`), and
  OK is the page's row 0 or its legend. A pad button can't be posted to a hidden game (the test
  pads, `DHD_TEST_PAD`, are scripts read at the field, not the SDL path), so its latch is proven
  by reading and its keyboard twin by the unit test and the run.
- **Tests on WARP are not touched**: the GS unit tests (`test_gpu`, `test_depth`) run on WARP but
  never reach the main menu; every hidden game test runs on the default (hardware) adapter.

## Surprises

- **`host64_selftest.py` failed on Windows too**, not only `--linux`: clang-cl does not define
  `__GNUC__`, so wasm-rt.h gives it explicit bounds checks as well, and since finding 258 that
  means iOS's two-part layout, whose POSIX-only placement does not link on Windows. Both variants
  test the 8 GiB layout (their address lists are its), so the layout now follows one switch,
  `HOST64_TWO_PARTS` (memory_layout.h; by default from the memory-check mode, as before), which
  the tool sets to 0. Real builds compile the same code: iOS passes the bounds check, every other
  platform guard pages.
- The first `guest.h` change broke the wasm build (`check.py port` `build_wasm`): the module's
  own units include guest.h from a mirror of `port/include` without `port/host64`; they have no
  guest layout, so guest.h takes the switch only off wasm (80fa0902).
- `tests/abi_layout.rs`'s x86_64 sizes contradict its own formula (216 + 3 pointers is 240, the
  test says 208); this lane changes no ABI struct, and no check runs that test. Not changed here.

## Result

- **Job 1**: one game a data folder owns the pipeline guard's files (`pipelines-guard.lock`, held
  for the run); a second game logs `gs native: another game runs from this data folder
  (pipelines-guard.lock is held); its pipelines-building.txt is not judged and this game writes
  none`, keeps the refusals and writes nothing. On this PC, hidden, two games from one
  `build/host64-exe` (the list removed first), twice in a row: each second game logged that line,
  no `Access is denied`, and afterwards `pipelines.txt` had 85 keys with no `!KEY`, no
  `pipelines-building.txt` was left, so no `?KEY`.
- **Job 2**: finding 258's Result is written. `host64_selftest.py` passes on Windows and, in WSL
  (Ubuntu, x86_64), with `--linux`, both with `--negative-controls`; repaired, not deleted (the
  Windows variant was broken the same way, and the tool is the only test of the 8 GiB layout's
  mirrors and guards with a generated module).
- **Job 3**: on a software adapter the main menu shows, once a run, the popup **GRAPHICS**: "This
  PC's graphics chip does not support Direct3D 12 or Vulkan." / "The game is running without
  graphics acceleration and will be very slow." with OK (macOS and iOS: "This device's graphics
  chip does not support Metal."; Linux and Android: "... does not support Vulkan."). dhd.log:
  `gs: software adapter: the game runs on Microsoft Basic Render Driver without graphics
  acceleration and will be very slow (...)` and `graphics notice: shown (...)`. The launcher's
  Play page shows the Windows sentence (`home.softwareGpu`, in all eight languages) after a run
  whose dhd.log has that line. Hidden on this PC: `DHD_WGPU_ADAPTER=fallback` (WARP) showed the
  notice at the main menu (the title was also dark there, as the player saw); the RTX 3070 run did
  not. That run lost posted keys (an X at the title, then the OK): a field there is about 300 ms,
  the keys were held 120 ms. With the latch, hidden on WARP (3.0-3.1 fields a second at the
  main menu), one X press held 0.12 s once the notice was up closed it: `graphics notice:
  closed` 0.5 s after the press, then the main menu. A pad button takes the same path (by
  reading, above); a touch taps OK as a click.
- **Checked** (again after the review, at 73a17f3d): `check.py port` OK (all 22 steps, `docs` and `quick` in it; `test_keyboard`'s tap test); `rustgs.py test`: the
  pipeline list's tests and `software_adapters`; the launcher's `scale` tests (cargo, target on
  the scratch drive); boot (600 fields) and race (5000) state traces of this branch equal to
  e9bba855's (the 0.8.7 release job's build).
- **Left**: the merge. Not run: the phones and the Mac (not touched); the Mac and iOS builds of
  `memory_layout.h`'s switch (the same preprocessed code for them).
