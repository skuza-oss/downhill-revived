"""Compile the production input cache and reader; require one edge per press."""
import testprog

out = testprog.out_dir('input-edges')
units = [testprog.game_unit(rel, out) for rel in ('unsorted/001909A8.c', 'unsorted/00220640.c', 'unsorted/0017EB00.c')]
exe = testprog.guest('input-edges', [testprog.ROOT / 'port/hle/pad.c', testprog.ROOT / 'port/hle/test_input_edges.c', *units],
                     out=out, flags=['-ffunction-sections'])
for rate in (30, 45, 60, 61, 75, 90, 100, 120, 144, 165, 240):
    testprog.run(exe, rate)
    testprog.run(exe, rate, 'variable')  # paced steps at every rate (finding 139)
