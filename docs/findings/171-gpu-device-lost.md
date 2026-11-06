# 171: The picture freezes when the GPU device is lost

Status: in progress

Branch `fix/device-lost` from `main` fce8a1f (0.7.12), started 2026-11-06. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #58 (Windows 11, Intel i5-1035G1 with Intel UHD Graphics, driver 30.0.101.3111 from 2021,
7.7 GB RAM): as soon as the game starts the picture freezes while sound and input go on. dhd.log
(kept by the lead) shows, right after the first present:
`dhd_render: ERROR wgpu_hal::auxil::dxgi::result: Present failed: The GPU device instance has been
suspended. Use GetDeviceRemovedReason ... (0x887A0005)` then
`dhdr_native_present: get_current_texture: validation error`, and the menus at 27 fps with 1 s
stalls. 0x887A0005 is DXGI_ERROR_DEVICE_REMOVED: the driver reset or removed the device (a TDR,
a driver crash, a driver update, a GPU hang); wgpu's device is then lost for good.

Goal: a lost device never leaves a frozen picture. At least: the game notices (wgpu's device lost
callback and the error scopes), logs `GetDeviceRemovedReason` (or the backend's reason) once, and
either recreates the device, surface and every GPU object and carries on (preferred, if the
renderer's state can be rebuilt from the GS memory it keeps), or shows a clear message ("The
graphics driver stopped responding; update it and restart the game") and exits cleanly. Also find
why this laptop loses the device at the first present (a hang in our first frame? TDR from a long
GPU job? the old driver with D3D12?) with evidence from its log and DxDiag (the lead keeps them),
and whether a fallback (Vulkan on Windows, `DHD_WGPU_BACKEND=vulkan`, which exists for development)
would be a sensible automatic retry. One code path; the AGENTS.md decisions stand (no D3D11).

Acceptance: a reproduction of device loss on this PC (e.g. a forced device loss: D3D12's
`ID3D12Device5::RemoveDevice` through wgpu-hal, or `dxcap -forcetdr`-style tooling if available,
on the hidden desktop: never a visible window or focus steal), the game recovering (or exiting with
the message) instead of freezing, the cause for #58 as far as the evidence goes, `check.py port`
passing, and pictures unchanged in normal runs.

## Progress

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
