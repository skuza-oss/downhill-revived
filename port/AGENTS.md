# port/: Downhill Revived, the PC port

The root `AGENTS.md` rules and current decisions apply (state sync, one delta-time clock, our own
renderer, one code path). What is where: `docs/STATE.md`, "The port".

- `port/` changes apply at build time; `src/` stays matching. A fix to game code is a patch in
  `port/patches*.py` (applied by `port/tools/transform.py`), never an edit of `src/`.
- One code path for every platform: shared code in `port/hle`, `port/host`, `port/host64`,
  `port/gs`, `port/net`, the Rust library in `port/render`. Platform parts are a few lines in
  the host files (finding 099). When something is replaced, delete the old path.
- Build: `python port/tools/build.py [--target T]` (the core `build_wasm.py`, then
  `build_host64.py`); the core module cache skips an unchanged core (`docs/BUILDING.md`).
  Several worktrees on one machine: set `DHD_BUILD_SLOTS` (at most that many builds at once) and
  pass a job count; never end a turn while a build you started is still running.
- Game state must not change by accident. The check is the per-field state trace
  (`port/hle/state_trace.c`, `port/tools/state_trace_run.py`, `state_diff.py`; finding 081):
  boot and race traces against the base build. Deterministic runs (`DHD_FRAME_CLOCK=1`) keep the
  original clock. Picture checks across runs are not a speed check (start spots differ); use the
  trace.
- What the game can detect goes in dhd.log (CPU, GPU, OS, settings, timings; `port/gs/perf_log.c`,
  finding 129), so a report needs no questions.
- Settings: one table in the Rust crate (`port/render/settings/src/table.rs`, finding 101); PC
  SETTINGS reads the generated `port/hle/settings_table.h`. Add a setting there, not in C.
- Players' text: `port/release/`; it must not name another server than `wss://dhd.skuza.app`.
- Tests: `python port/tools/test_runtime.py`, `python port/tools/test_gs_wgpu.py`,
  `python port/net/build_tests.py`; game runs and their harness: `docs/TESTING.md`.

Check before you hand back: `python tools/check.py port` (core + host build and unit tests);
add the state trace when the change can touch game state.
