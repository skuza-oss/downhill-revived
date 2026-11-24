"""Synthetic native-GS upload/copy ordering checks and blend scenes (no game data), on the Rust
renderer.

    python port/tools/test_native_transfers.py

builds port/gs/test_native_transfers.c for x86_64 over gpu_rust.c + dhd_render.lib
(port/tools/test_gs_wgpu.py's build) and runs it twice: with dual-source blending where the GPU has it
(wgpu) and with DHD_WGPU_DUAL_SOURCE=0 (the two-draw fallback, wgpu-no-dual; it also takes a 2 x 2-page
import atlas, DHD_NATIVE_ATLAS_PAGES=2, finding 281), each dumping its pictures
and the CPU's VRAM after each under build/port/test-native-transfers/dump/<run>/. Each run must pass
its own checks (port/tools/gs_verdict.py, the one pass rule; every run must print "gs: renderer
dhd_render ..." and report the wgpu backend); then every picture is compared pixel by pixel between
the runs (the counts and the largest channel difference are printed) and the CPU's VRAM after every
picture must be the same bytes: the native renderer never writes what the game reads. A picture set
that differs in size or names is a failure too. gs_golden.py compare native-transfers holds the dumps
to the goldens. The runs get the harness environment of test_gs_wgpu.harness_env (only the forwarded
DHD_* variables).

    python port/tools/test_native_transfers.py --presented
    python port/tools/test_native_transfers.py --stress

--presented: the runs set DHD_NATIVE_TEST_PRESENTED=1, so every picture is also presented to a hidden
window and the surface read back must be the picture scaled 4:3, centred, nearest, with black bars
(test_native_transfers.c); each run must say how many pictures it checked (dump-presented/).
--stress: port/gs/test_native_stress.c instead of test_native_transfers.c: six fields queued without
waiting while a thread creates and releases replacements through the ReplaceDevice; every round's
final picture must equal the synchronous run's. Each run must report its rounds and a nonzero count of
replacements, and the runs' pictures and VRAM are compared as above (dump-stress/).
The Direct3D 11 run these were once compared with is gone (finding 088, CL2); its dumps stay in the
goldens, where they are never compared."""
import argparse
import os
import shutil
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gs_verdict  # noqa: E402
import test_gs_wgpu  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/port/test-native-transfers'
# The second run also has an import atlas of 2 x 2 pages (finding 281: what a device short of memory
# falls back to), so a 56-page picture is drawn in 14 passes; its pictures must still be the first's.
RUNS = (('wgpu', {}), ('wgpu-no-dual', {'DHD_WGPU_DUAL_SOURCE': '0', 'DHD_NATIVE_ATLAS_PAGES': '2'}))


def read_dumps(path):
    out = []
    for name_file in sorted(path.glob('*.name')):
        stem = name_file.with_suffix('')
        rgba = stem.with_suffix('.rgba').read_bytes()
        w, h = int.from_bytes(rgba[-8:-4], 'little'), int.from_bytes(rgba[-4:], 'little')
        out.append((name_file.read_text(), w, h, rgba[:-8], stem.with_suffix('.vram').read_bytes()))
    return out


def compare(label, a, b):
    """Pictures and VRAM of two runs: the problems (VRAM or picture-set mismatches, size mismatches);
    RGB differences are reported, not failures."""
    problems = []
    if [x[0] for x in a] != [x[0] for x in b]:
        print('%s: the runs drew different pictures' % label)
        return ['%s: the runs drew different pictures' % label]
    total = 0
    for (name, w, h, pa, va), (_, w2, h2, pb, vb) in zip(a, b):
        if va != vb:
            words = sum(1 for i in range(0, len(va), 4) if va[i:i + 4] != vb[i:i + 4])
            print('%s: %s: VRAM differs in %d words' % (label, name, words))
            problems.append('%s: %s: VRAM differs in %d words' % (label, name, words))
        if (w, h) != (w2, h2) or len(pa) != len(pb):
            print('%s: %s: picture sizes differ (%ux%u vs %ux%u)' % (label, name, w, h, w2, h2))
            problems.append('%s: %s: picture sizes differ' % (label, name))
            total += 1
            continue
        diff = 0
        worst = 0
        for i in range(0, len(pa), 4):
            if pa[i:i + 3] != pb[i:i + 3]:
                diff += 1
                worst = max(worst, max(abs(pa[i + c] - pb[i + c]) for c in range(3)))
        if diff:
            total += 1
            print('%s: %s: %d of %d pixels differ in RGB (largest channel difference %d)' % (label, name, diff, w * h, worst))
    print('%s: %d pictures, %d with RGB differences, %d with VRAM differences'
          % (label, len(a), total, sum(1 for p in problems if 'VRAM differs' in p)))
    return problems


def program_of(mode):
    """(program, dump directory, expected lines) of a mode: None (test_native_transfers.c),
    'presented' (the same, every picture presented) or 'stress' (test_native_stress.c)."""
    if mode == 'stress':
        # every run: six synchronous fields, then the rounds with a nonzero replacement count
        return 'test_native_stress', 'dump-stress', (r'^synchronous: 6 fields, \d+x\d+ final picture$',
                                                     r'^stress: [1-9]\d* rounds, [1-9]\d* replacements created and released$')
    if mode == 'presented':
        return 'test_native_transfers', 'dump-presented', (r'^presented: [1-9]\d* pictures checked against the window$',)
    return 'test_native_transfers', 'dump', ()


def build(mode=None):
    program = program_of(mode)[0]
    return test_gs_wgpu.build(program, [program + '.c', 'gpu_rust.c', 'gs.c', 'memory.c'])


def runs(mode=None):
    """Runs the program of mode (RUNS), judges each run and compares their dumps; the exit code."""
    program, dump_dir, expect = program_of(mode)
    exe = build(mode)
    problems = []
    dumps = {}
    for name, env in RUNS:
        out = OUT / dump_dir / name
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        env = dict(env, DHD_NATIVE_TEST_DUMP=str(out))
        if mode == 'presented':
            env['DHD_NATIVE_TEST_PRESENTED'] = '1'
        code, text = test_gs_wgpu.run(exe, env)
        print('== %s (exit %d)' % (name, code))
        print(text.rstrip())
        problems.extend(gs_verdict.judge(code, text, backend='wgpu', expect=expect,
                                         label='%s %s' % (program[5:], name)))
        dumps[name] = read_dumps(out)
        if not dumps[name]:
            print('%s %s: no pictures dumped' % (program[5:], name))
            problems.append('%s %s: no pictures dumped' % (program[5:], name))
    names = [n for n, _ in RUNS]
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            problems.extend(compare('%s vs %s' % (names[i], names[j]), dumps[names[i]], dumps[names[j]]))
    return gs_verdict.verdict(problems)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--presented', action='store_const', const='presented', dest='mode',
                      help='every picture is also presented, and the surface read back is checked')
    mode.add_argument('--stress', action='store_const', const='stress', dest='mode',
                      help='test_native_stress.c: queued fields while replacements are created and released')
    args = parser.parse_args()
    return runs(args.mode)


if __name__ == '__main__':
    sys.exit(main())
