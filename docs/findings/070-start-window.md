# 070: the start window and the shader cache

Status: history

0.2.x (October 2026), 32-bit Windows build. Both parts are gone: the start
window was removed on 2026-10-27, when the launcher (`port/launcher`, finding 093) became the
start screen on every platform, and the shader cache went with the D3D11 renderer (finding 088).
The Rust renderer compiles its pipelines through wgpu (DX12 shaders with the pinned DXC, finding
084 §14) and keeps no pipeline cache (`port/render/src/exact/pipelines.rs`).

## What it measured (why the window existed)
Players saw nothing for many seconds after a double-click and thought the game was stuck. Start
steps on the development PC, idle:
- update check (then in the game, `auto_update`): up to 6 s;
- disc check, relaunch, game data: about 0.3 s;
- graphics (`gs_gpu_init`): 8 s, 20-25 s with a cold driver cache, because every start compiled
  raster.hlsl's 15 shaders with `D3DCompile`. A bytecode cache keyed by the source, entry point,
  target, defines, flags and the system `d3dcompiler_47.dll` cut it to 0.05 s from the second
  start.

## Lesson kept
Show something within a second of a start and say what the slow step is. The launcher does that
now; if shader compile time ever grows again, measure it with `port/tools/shader_compile_time.py`.

Full text: `git show 860ab6b:docs/findings/070-start-window.md`
