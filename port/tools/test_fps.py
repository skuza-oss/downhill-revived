"""Compile/run production clock invariance checks: port/hle/fps.c in the core (wasm32)."""
import testprog

exe = testprog.guest('fps', [testprog.ROOT / 'port/hle/fps.c', testprog.ROOT / 'port/hle/test_fps.c'],
                     memory=0x10010000)
for rate in (30, 45, 60, 61, 75, 90, 100, 120, 144, 165, 240):
    testprog.run(exe, rate)
    testprog.run(exe, rate, 'wall')  # the delta-time clock, paced (finding 139)
testprog.run(exe, 60, 'display')
for rate in (60, 120):
    testprog.run(exe, rate, 'away')  # a phone's app back from the background: no catch-up (finding 227)  # the display rate followed at run time (iOS, finding 224)
