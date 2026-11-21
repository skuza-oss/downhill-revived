# 273: the phone launcher on old WebViews, and a clear message on a too-old iPad/iPhone

Status: in progress

Branch `fix/launcher-old-webview` from `main` bbe4acc5 (0.8.10), started 2026-11-21. This file is
the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

- Issue #114 (Android 10, Snapdragon 665): the launcher shows its page with the desktop's window
  buttons and nothing reacts to a tap: the page's script did not start. Find the constructs that
  need a newer Chromium than the player's WebView, lower the page's minimum, and show a plain
  "update Android System WebView" message (with the Play Store button) when the WebView is still
  too old. The WebView's version goes in the logs.
- Issue #113 (iPad Pro, A10X, iPadOS 17): the app dies when the core's code runs. Check the chip
  before any code built for the A12 floor (finding 267) can run and show one plain page instead.

Done: `check.py quick` and `check.py docs`, the launcher's tests, the debug APK, the iOS app build
and `ipa_audit.py`. No game state changes (no traces).

## Progress

- [ ] the launcher's page runs on Chromium 80; the test that refuses newer syntax and APIs
- [ ] the ES5 safety net (boot.js) and the WebView's version in the logs
- [ ] the iOS chip check

## Decisions

## Surprises

## Result
