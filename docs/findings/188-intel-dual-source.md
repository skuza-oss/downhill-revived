# 188: dual-source blending off on Intel GPUs

Status: in progress

Branch `fix/intel-dual-source` from `main` ac62c78d (0.7.14), started 2026-11-09. This file is the
lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #58 (Windows 11, Intel i5-1035G1: Ice Lake, Intel UHD Graphics G1, Gen11; driver
31.0.101.2145): a few seconds after the start the picture stops while sound and input go on. On
D3D12 the driver removes the device (`Present failed: The GPU device instance has been suspended`,
0x887A0005; finding 171 read the player's DxDiag: one `VIDEO_ENGINE_TIMEOUT_DETECTED` TDR per
start). The player's matched A/B on Vulkan (native renderer, borderless, 2x, V-Sync on, only
`DHD_WGPU_DUAL_SOURCE` changed): dual-source on froze after about 8 s
(`get_current_texture: validation error`, `presenting failed; no more pictures are shown`);
`DHD_WGPU_DUAL_SOURCE=0` ran menus and a race without a stop, on Vulkan and on D3D12.

Goal: the game turns dual-source blending off by itself on the affected Intel GPUs, decided once
from the adapter's information in one place for every platform, with one dhd.log line saying so.
`DHD_WGPU_DUAL_SOURCE=0` stays the manual override, `=1` forces it on.

Acceptance: a unit test of the rule; `python tools/check.py port` passes; a boot state trace equal
to the base build's (the native renderer only shows pictures; nothing the game reads changes).
Out of scope: making dual-source work on Intel, a device recreated after a loss (finding 171's
decision stands).

## Progress

- [x] 2026-11-09: finding reserved
- [ ] the rule, the log line and the override in `port/render/src/device.rs`
- [ ] checks: unit test, `check.py port`, boot state trace

## Decisions

## Surprises

## Result
