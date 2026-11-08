# 183: Performance settings: shadows and sun glow, lens flare, particles

Status: in progress

Branch `feat/performance-effects` from `main` 0574b05, started 2026-11-08. This file is the lane's
plan while it runs and its finding when it merges.

## Goal and acceptance

The owner's decisions (2026-11-08): a **Performance** group in REVIVED SETTINGS > Graphics, with a
preset switch **Performance Mode** (turns all three off) and one toggle each:

1. **Shadows and sun glow**: the riders' shadows and the sun's glow chain; the sun disc stays.
2. **Lens flare**: the flare's ghost ring.
3. **Particles**: the particle system's effects (dust, dirt, spray).

Everything on by default (the original look). Not offered: fog, sharp textures, draw distance, a
lower output resolution. The settings are visual only: game state must not change, so they are
allowed online.

The Mali lane (branch `perf/mali-gpu`, its finding not merged yet) measured the effects with renderer heuristics (small surfaces, a blend key, VU program
addresses; a diagnostic patch, never committed). A setting needs an exact identification that works
on every renderer and platform: switch each effect off where the game decides to draw it.

Acceptance:

- each toggle off removes only its effect (PC pictures at a few race fields: on, each off, all off);
- boot and race state traces against the base build: identical with everything on; with
  everything off, identical in the game's state (only the effects' own draw data differs);
- `python tools/check.py port`; the settings tests (`settings/tests/generated.rs`);
- the Mi 11i (throttled `t612`, 4x, uncapped, the Mali lane's route), Performance Mode off against on,
  two interleaved runs each; the whole-race gain compared with the Mali lane's +40 % (all off, 4x).

## Progress

- [x] 2026-11-08: finding reserved

## Decisions

## Surprises

## Result
