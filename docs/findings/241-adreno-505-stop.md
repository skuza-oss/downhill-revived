# 241: Adreno 505 device loss after the menus start

Status: in progress

Branch `fix/adreno-505-stop` from local main 92a7eb4a, started 2026-11-14.

## Goal and acceptance

Investigate issue #91's log and known driver faults. Ship only an evidenced narrow
workaround, or the missing diagnostics and a precise reporter request. No phone,
Mac, push, external post or timing benchmark. Gates: quick, docs, Android native;
port for renderer/host changes, state traces if game state could change.

## Progress

- [x] Read issue/comments and the entire attached current/previous run log.
- [ ] Research the driver, implement the smallest justified change, run gates.

## Decisions

The run is 0.7.16: do not assume the reporter tested the offered 0.8.0 or Native VU off.

## Surprises

Both runs already use 1x with all three Performance Mode effects off. The primary
failure is a reported Vulkan device loss after 20/22 seconds in menus, after 333/352
presents, not a startup stall. One later crash is in Android HWUI's RenderThread,
with libGLESv2_adreno.so / libEGL / libhwui frames, after the device-loss exit path.

## Result

Pending research and checks.
