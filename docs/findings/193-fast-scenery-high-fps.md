# 193: helicopters, birds and other scenery at the right speed at every frame rate

Status: in progress

Branch `fix/fast-scenery-high-fps` from `main` 9609ea6f, started 2026-11-09. This file is the
lane's plan while it runs and its finding when it merges.

## The report

Players on Discord, 0.7.14: at 120, 144 and 240 fps the helicopters on Mt. Midoule fly far too
fast, and the birds on other tracks too. On some tracks (Red Pass was named) the helicopter moves
so fast and so far off its course that it hits the rider and kills it: it changes the race, not
only the picture. At 60 everything is right. An earlier attempt did not fix it.

## Goal and acceptance

Find the root cause and fix it in one place, so every such object (helicopters, birds, planes,
any scripted or path-following scenery or actor, every track) moves at the same speed in game time
at every frame rate. Per mechanism, not per object, unless the evidence shows truly separate
mechanisms.

Done when:

- A rate test shows the helicopter's (and a bird's) position against race time equal between 60
  and 240 (and 144) within a tolerance, and fails on the base build.
- `python tools/check.py port` passes.
- Boot and race state traces at 60 (`state_trace_run.py boot|race`) equal main's
  (`state_diff.py` PASS): a deterministic run at 60 keeps the original arithmetic.
- `port/tools/test_fps_race.py` passes.

Out of scope: anything that is right at 60; the physics equivalence limits of finding 139.

## Progress

- [x] 2026-11-09: lane started, finding reserved.
- [ ] Reproduce at 60 and 240 (rate_audit race window, Mt. Midoule FR and Red Pass).
- [ ] Find the helicopter object, its update and what it advances per update.
- [ ] Find every other object on the same mechanism (all tracks).
- [ ] Explain the Red Pass kill.
- [ ] Fix through the delta-time clock (`port/patches_fps.py`).
- [ ] Rate test, before/after numbers.
- [ ] Checks.

## Decisions

## Surprises

## Result
