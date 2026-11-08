# 105: the exact renderer ran out of GPU memory while minimised

Status: current
Checked: 2026-11-04

## The crash

A minimised start on the exact renderer (`DHD_SETTINGS=none`, so no dhd.ini and the game's own
fallback, or `DHD_RENDER=exact`) died at the main menu after 30-60 s:

```
dhd_render: ERROR wgpu_hal::auxil::dxgi::result: GraphicsCommandList::close failed: Not enough memory resources are available to complete this operation. (0x8007000E)
dhd_render: wgpu error in dhdr_exact_submit: Out of Memory
```

exit code 0xC0000409 (the abort's fail-fast). The same start on the native renderer ran on. RTX
3070, driver 32.0.16.2101, plenty of RAM and VRAM free.

## Not new: 0.6.4 has it

Same start (minimised with `SW_SHOWMINNOACTIVE` and kept minimised, `DHD_SETTINGS=none`,
`DHD_SKIP_MOVIES=1 DHD_MEMCARD=0 DHD_SND_OFF=1`, `DHD_NAV=0:triangle|cross`, the USA disc):

| build | died after | process private bytes before | GPU shared usage before |
|---|---|---|---|
| shipped 0.6.4 `dhd.exe` (zip sha256 1696167…f2d0, as its latest.json) | 46 s | 19.5 GB | 15.1 GB |
| main 4de73d87b (port/render and port/gs as 0.6.4, but present.c's splash) | 39 s | 19.0 GB | 13.9 GB |
| desk/integrate 89bee8823 | 60 s | 19.9 GB | 14.5 GB |

Private bytes grew about 400 MB/s from the menu on, the GPU process's shared (system) memory the
same: as fast as the exact renderer wrote its buffers (`DHD_PROFILE`: 375 MB/s written, 140
submits/s). The renderer code has worked this way since the Rust renderer took over (finding 085),
so no bisect.

## The cause

Nothing held the CPU back to the GPU while the window was minimised. A shown frame waits for the
surface (`get_current_texture`, frame latency 1), so the GPU can never fall more than a frame
behind. Minimised, the client area is 0 x 0, `Surface::acquire` skips the frame at once, and the
exact renderer has no other wait: its only other sync is the readback map (`read_begin`), and the
menu reads nothing (0 reads/s). The GPU (here shared with another game run) fell further behind
every field. Each `queue.write_buffer` keeps its staging memory, and each submit its command list,
until that submission has run on the GPU; wgpu-core frees them in the poll at the next submit only
once the fence has passed. So the backlog of the game's 375 MB/s of writes stayed alive, until D3D12
failed a command list's `Close` with E_OUTOFMEMORY and the uncaptured error handler aborted. Not a
leak of wgpu objects: once the GPU catches up, everything goes.

## The fix

`Exact::frame_end` (`port/render/src/exact/encoder.rs`), called by `present_exact`
(`present/surface.rs`) after the frame's scanout submit, shown or skipped: it waits
(`device.poll(Wait { submission_index })`) for the last submit of the previous frame. At most one
frame of exact work is queued, minimised too. A shown frame has already waited about as long for the
surface, so a shown game doesn't wait longer. In the browser the page's frame loop paces and
`frame_end` waits for nothing.

Out of memory now ends cleanly on Windows (`guard.rs`, `out_of_memory`): the log line, then
`dhd_render: the graphics card ran out of memory; the game ends`, an error box (not with
`DHD_HEADLESS` or a console, so tools and test programs never wait on a box) and exit code 1 instead
of the fail-fast. Validation and internal errors still abort. The box and exit come from raw-dylib
imports (user32, kernel32), so `native-static-libs-x86_64.txt` is unchanged.

## Checks

- Fixed main, the same minimised start, 5 minutes through the menus into a race
  (`race_compare.py`'s route: `DHD_NAV`, `DHD_AUTOPAD_FRAMES`, `DHD_RACE_PAD`,
  `DHD_RACE_SYNC=1`, `DHD_RACE_SEED=81257`): alive at 300 s, field 14,383, racing (phase 3).
  Private bytes 775 MB at 30 s, 837 MB at 300 s; GPU dedicated 177 -> 182 MB, shared 343 MB
  throughout. A 90 s menu-only run: 756 MB private, flat from 10 s to 75 s.
- The native renderer, the same route for 120 s: alive, private 1.14 GB flat from 60 s.
- The out-of-memory path, on a throwaway build without `frame_end` and with the box switched off
  (so no box on a PC in use): died at 59 s with the two log lines and exit code 1, no
  fail-fast. The box itself was not shown in a run.
- `build_host64.py` links; `rustgs.py test` passes; the wasm32 and aarch64-linux-android
  renderer libraries build.
- No benchmarks. Menu fields/s fell from about 35 to 25-27 with the wait in place while another
  game run used the GPU at 99 %: the GPU was the limit, and the old rate only went with a growing
  backlog.
