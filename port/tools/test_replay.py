"""Compile/run the race replay checks: port/hle/fps.c's field grid and port/hle/replay.c's recorder in
the core (wasm32), with a stand-in race (port/hle/test_replay.c, finding 142)."""
import testprog

exe = testprog.guest('replay', [testprog.ROOT / 'port/hle/test_replay.c'], memory=0x10010000)
for rate in (30, 45, 60, 90, 144, 165, 240):
    testprog.run(exe, rate)
    testprog.run(exe, rate, 'wall')  # paced: uneven frames (finding 139's delta-time clock)
