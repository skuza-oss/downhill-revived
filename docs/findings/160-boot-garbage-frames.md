# 160: Garbage blocks on screen in the first seconds after start

Status: in progress

Branch `fix/boot-artifacts` from `main` 113f76f, started 2026-11-06. This file is the lane's plan
while it runs and its finding when it merges.

## Goal and acceptance

For the first 1-3 seconds after the game starts, players see rectangular blocks of other pictures:
pieces of the loading card and the logo over old picture data, then rock textures under the title's
PRESS X TO START (two players' screenshots, kept by the lead in `F:\Temp\boot-artifacts\`; one
from an Android phone, the owner sees the same on Windows; issue #47's tester reports it too). The
blocks look like GS memory pages that hold other data (textures uploaded at boot) shown as
picture.

Goal: what a PS2 shows in those seconds is what the port shows (PCSX2 is the referee: replay or
record the same GS stream before blaming the renderer, finding 025), on every platform and both
renderers if both show it. Candidates to rule in or out with evidence: the display switched off or
faded on the PS2 (PMODE's circuit enables and ALP, DISPFB/DISPLAY, SMODE) and the port's scanout
showing the framebuffer anyway; the native renderer's surfaces or page tracking showing pages it
never drew; a frame presented before the game's first real one; the widescreen/4:3 card code
(finding 140) in those frames.

Acceptance: the cause named with evidence (frame dumps of the first seconds from the port and
PCSX2's picture or GS-stream replay for the same fields), the fix in the common code path, the
first seconds clean on Windows with RENDER=native and RENDER=exact (frame dumps or the harness's
captures on the hidden desktop), `test_gs_wgpu` unchanged, `check.py port` passing, and boot and
race state traces equal to the base build.

## Progress

- [ ] 2026-11-06: finding reserved

## Decisions

## Surprises

## Result
