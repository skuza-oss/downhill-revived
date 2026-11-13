# 224: The native iOS port on main

Status: in progress

Branch `feat/ios-port` from `main` b9da043f (v0.8.0), started 2026-11-13. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The owner's decision (2026-11-13): the native iOS port by Gustavo Ribeiro (GitHub `ribeirogustav`,
his fork `downhill-revived-ios`, pull requests #72, #73, #74) comes into main, and iOS becomes a
platform we build and release (an unsigned, experimental IPA, like the Mac, Linux and Android
builds). His SwiftUI launcher and his touch controls come in as they are (iOS only): an exception
to the one-code-path decision, recorded in `AGENTS.md`.

Done when: his shared-code changes are on the branch as reviewed commits in his name (each kept,
changed or left out, with the reason here); `port/ios` is on the branch in his name, with our
commits on top for what 0.8.0 needs; the iOS Simulator build and the unsigned device IPA build on
the Mac from this branch; the Simulator app starts and its logs show how far the game gets;
online on iOS is what Android offers (P2P rooms only); the docs (`AGENTS.md`, `port/ios/AGENTS.md`,
`docs/BUILDING.md`, `docs/PLAYING.md`, `docs/STATE.md`, `port/release/`) say how iOS is built,
installed and what is experimental; on Windows `check.py port` and `check.py docs` pass and the
seven state traces equal the reference; the Mac desktop build still builds and starts.

Out of scope: an on-screen run (a second worker with the device tools), a real iPhone (needs an
Apple ID the lane does not have), changes to his launcher's or touch controls' design.

## Progress

- [x] 2026-11-13: finding reserved

## Decisions

## Surprises

## Result
