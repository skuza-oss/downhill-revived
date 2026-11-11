# 201: PowerVR GE8322 (driver 1.18) closes at the first picture's submit

Status: in progress

Branch `fix/powervr-ge8322-submit` from `main` f0971070 (0.7.16), started 2026-11-11. This file
is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

Issue #82: an itel A70 (itel A665L, Android 13, Unisoc T603 / SC9863A, PowerVR Rogue GE8322,
driver "PowerVR Rogue Vulkan Driver 1.18@6267915", 3.8 GB RAM) shows a black screen and closes
right after the game starts: `crash: SIGSEGV at 0000000000000000 in thread native`, a null pointer
dereference in the driver (`vulkan.sp9863a.so`) under `vulkan::driver::QueueSubmit`.

Goal: name the crash's place (our function and what it submits) and its cause with the evidence;
fix it with one code path where possible; if the cause cannot be pinned, log what pins it from the
next shared log. A release-signed test APK for the reporter (not published by the lane).

There is no PowerVR phone here; the cause comes from the log, the release's symbols, the code and
what others found.

## Progress

- [x] 2026-11-11: finding reserved.

## Decisions

## Surprises

## Result
