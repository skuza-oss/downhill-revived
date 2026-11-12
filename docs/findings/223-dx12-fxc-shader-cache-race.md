# 223: The pipeline worker's crash in wgpu-hal's DX12 `load_shader` (FXC blobs shared across threads)

Status: in progress

Branch `fix/pipeline-worker-crash` from `main` d9714d0a, started 2026-11-12. The 0.8.0 gate saw
the guest game die once in seven two-game online runs, at the start of the shared race, on the
renderer's pipeline worker thread (`dhd-pipelines`, finding 131's background specialisation):
`crash: exception C0000005 ... (reading 00000009)` in `wgpu_hal::dx12::Device::load_shader` <-
`create_render_pipeline` <- wgpu-core <- `dhd_render::native::pipelines::build` <- `Worker::start`.

## Goal and acceptance

Say whose bug it is, since when, and fix it if it can be fixed safely for a release. Done when:

- the crash line is resolved and what was freed or null is named;
- a stress test reproduces the fault on the code before the fix and passes after it
  (`rustgs.py test ... -- --test dx12_shader_cache`);
- `check.py port` and `check.py docs` pass, a boot state trace equals the reference build's, and
  one hidden two-game P2P race on a local server reaches the results with no renderer error line.

Out of scope: upgrading wgpu; reworking the worker.

## Progress

- [x] 2026-11-12: lane branch, finding reserved.
- [ ] Crash resolved, cause found, stress test, vendored wgpu-hal fix, checks.

## Decisions

## Surprises

## Result
