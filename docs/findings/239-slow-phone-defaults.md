# 239: Slow phones start at suitable graphics settings

Status: in progress

Branch `fix/slow-phone-defaults` from `main` b1d5e515, started 2026-11-14.

## Goal and acceptance

Issues #101 and #95: known minimum and below-minimum Android chips start at 1x
with Performance Mode, only for settings never written by the player. Unknown
hardware, desktop and iOS keep their defaults. No phone use or PC speed benchmarks.
Gate: quick, port, docs, Android native/Java builds and tests, base-equal boot trace.

## Progress

- [x] Read the issue comments and #101's attached log; traced settings and pacing.
- [ ] Implement conservative classes, missing-key defaults and tests.
- [ ] Run gates and document phone checks for the lead.

## Decisions

Use GPU families and SoC identifiers, never phone models. Keep written keys,
including environment aliases, and persist automatic choices as ordinary settings.

## Surprises

No existing below-minimum dialog was found; PLAYING.md contains the advice.

## Result

Pending verification and merge.
