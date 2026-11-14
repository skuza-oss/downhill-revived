# 245: Bluetooth pad after Android room-code entry

Status: in progress

Branch `fix/pad-after-room-code` from local main bf4f5ab6, started 2026-11-14.

## Goal and acceptance

Issue #80: reason through IME/text-entry and controller ownership, reproduce the
device/input sequence in tests, fix only evidenced defects. Gates: quick, port,
docs, Android native/Java and units; base-equal boot/race traces if HLE changes.
No phone, Mac, push, external post, visible game window or timing benchmark.

## Progress

- [x] Read issue and all five comments: Android, Bluetooth, A/Y survive after
  keyboard entry. No log attachment or controller model/version is provided.
- [ ] Trace SDL/Java/text and slot transitions; regression test and fix; gates.

## Decisions

232 predates this lane but postdates the report. Preserve its intentional-press
claim and filtering rules; do not invent a keyboard blacklist or remap buttons.

## Surprises

Pending code investigation.

## Result

Pending implementation and checks.
