# 278: a PowerVR B-Series phone loses the GPU device in the menus

Status: in progress

Branch `fix/powervr-bxm-device-lost` from `main` e272b2a2 (0.8.11 plus finding 276), started
2026-11-22. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #119 (0.8.11, a MediaTek MT6855 phone with a PowerVR B-Series BXM-8-256, driver "PowerVR
B-Series Vulkan Driver 25.1@6715691", Vulkan 1.3.303, Android 16): two runs end in the menus with
`gs: the GPU device was lost (Unknown): Unexpected error variant (driver implementation is at
fault)` and the lost-device box. There is no such phone here: the cause comes from the shared log
(the lead keeps it), the code and what others found.

Acceptance: the place and the cause with the evidence, or what narrows it; a fix in the one path
it belongs to, or the log lines that decide it next time; `DHD_BUILD_SLOTS=4 python tools/check.py
port -j 6` and `check.py docs`; boot and race state traces equal to the base if anything that can
change game state or drawing changed. No phone, no timing benchmark, no visible window.

## Progress

- [x] 2026-11-22: finding reserved.

## Decisions

## Surprises

## Result
