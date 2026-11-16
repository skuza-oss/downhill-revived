# 260: the Redmi 9 dies at the title in 0.8.6: the Mali-G52 compiler and the mesh vertex stage

Status: in progress

Branch `fix/mali-mesh-shader-260` from `main` 2760f372 (0.8.6 plus findings 257-259), started
2026-11-16. This file is the lane's plan while it runs and its finding when it merges.

## Goal and acceptance

0.8.6 with its default settings (Native VU on) dies at the title on the Redmi 9 (Mali-G52 MC2,
driver r26p0), at every start: the driver's shader compiler (`cmpbe_v2_compile_multiple_shaders`)
crashes while the first mesh pipeline, `E0180033E26BC000`, is built (finding 259). The next start
builds it again and dies again.

1. Find the shader construct the compiler dies on and write the same maths in a form it compiles,
   one code path for every device.
2. A start that died inside a pipeline build must not walk into the same death at the next start;
   the log and the trouble report still say what died.
3. On the phone: the fixed debuggable build with the default settings reaches the title, the menus
   and a race, 5 starts of 5; race screenshots with Native VU on; one timing run at 2x with Native
   VU on and one off (finding 259's route); a test build with the bad shader dies once and the
   next start gets through.
4. `check.py quick` and `port`; boot and race state traces equal to the base; the renderer's
   picture tests.

Out of scope: speed work, other phones (the Mi 11i and the Mac are not touched).

## Progress

- [ ] 2026-11-16: finding reserved

## Decisions

## Surprises

## Result

To be filled at the merge.
