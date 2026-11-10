"""Differential check of the native VU1 kernels on captured run sequences (port/runtime/vu1_seq_diff.c).

Capture first (game data, under build/):
    DHD_VU_SEQ=build/port/vu-seq DHD_VU_SEQ_AT=<game seconds> DHD_VU_SEQ_KICKS=8000 dhd.exe ...
then
    python port/tools/vu1_seq_diff.py build/port/vu-seq [more capture dirs] [--repeat 20]
builds the harness with the game's AOT blocks and native kernels (generated from the local
microcode into OUT, never into tracked files), replays every captured run from its real state on
the interpreter alone (the referee), through the translated blocks, and through the blocks with
the native kernels, and requires every register, the whole data memory and every kicked GIF
packet of the last two to be byte-identical to the referee's. With --repeat it also times the
blocks without and with the kernels per microprogram key (OUT/<capture>.csv). Exit status 1 on
any difference.

--specials SEED puts exponent-255 and denormal bit patterns into lanes of each run's registers and
data memory first (the same for all three executions): the kernels' operand clamps, and every proof
that one is not needed, against the interpreter on values the game rarely holds.

--clean-check builds the kernels with PS2_VU_CLEAN_CHECK: every value one writes as clean (its
operands are clamped where they are written, not where they are read, finding 197) is compared with
its clamp, and any that differs fails the run. Slower: not with --repeat.

--target android-arm64 only builds the harness for a phone (OUT/vu1_seq_diff-android-arm64, the
game build's arm64 flags): the kernels on the phone's own CPU, with its FPCR and NEON paths. Push
it and a capture (the x64 game's serve: ps2_cpu has one layout) to the phone's /data/local/tmp and
run it there as on the PC, `./vu1_seq_diff-android-arm64 CAPTURE [repeat]`; with a repeat it also
counts CPU cycles per key where the kernel lets it (root, or perf_event_paranoid below 2), pinned
to one core with taskset. Delete the capture from the phone afterwards (game data).
"""
import argparse
import csv
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))


def build(out, negative=False, coverage=False, target=None, clean_check=False):
    from vu1_aot import generate
    generate(out / 'vu1_aot.inc')  # also writes vu1_native_clip.inc beside it
    runtime = ROOT / 'port/runtime'
    if negative:
        # Compile a private corrupted implementation; the comparator is unchanged.
        runtime = out / 'negative-runtime'
        runtime.mkdir(exist_ok=True)
        (runtime / 'vu0_interp.c').write_bytes((ROOT / 'port/runtime/vu0_interp.c').read_bytes())
        header = (ROOT / 'port/runtime/vu1_native_geometry.h').read_text()
        # the kernels' MADD (native_madd_c), its result's lowest bit flipped
        needle = '{ return native_result(f, VU_ADD4(native_clean(acc), VU_PRODUCT4(native_clean(a), native_clean(b))), d); }'
        assert header.count(needle) == 1
        header = header.replace(needle, '{ return native_result(f, _mm_xor_ps(VU_ADD4(native_clean(acc), VU_PRODUCT4(native_clean(a), native_clean(b))), _mm_castsi128_ps(_mm_set1_epi32(1))), d); }')
        (runtime / 'vu1_native_geometry.h').write_text(header)
    if target:
        # the phone's CPU, with the game build's arm64 flags (build_host64.py's android-arm64 target)
        import sdl3
        from build_host64 import DEVCACHES, TARGETS
        t = TARGETS[target]
        exe = out / ('vu1_seq_diff-' + target)
        cc = sdl3.android_ndk() / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
        simde = Path(os.environ.get('SIMDE_DIR') or t.simde or str(DEVCACHES / 'simde-src'))
        cmd = [str(cc), *[f for f in t.flags if f != '-fPIC'], '-O3', '-ffp-model=strict', '-ffp-contract=off',
               '-ftrivial-auto-var-init=zero', '-fno-strict-aliasing', '-fwrapv', '-w', '-DPS2_VU_AOT', '-DPS2_VU_AOT_EMBED',
               '-I' + str(out), '-I' + str(ROOT / 'port/runtime'), '-I' + str(ROOT / 'port/include'),
               '-I' + str(ROOT / 'port/host'), '-I' + str(simde)]
        if coverage: cmd += ['-DPS2_VU_COVERAGE']
        if clean_check: cmd += ['-DPS2_VU_CLEAN_CHECK']
        cmd += [str(runtime / 'vu0_interp.c'), str(ROOT / 'port/runtime/vu1_seq_diff.c'), '-o', str(exe), '-lm']
        subprocess.run(cmd, cwd=out, check=True, timeout=1200)
        return exe
    exe = out / 'vu1_seq_diff.exe'
    # x64, with the game build's code generation flags (build_host64.py): the captures come from
    # dhd.exe (x64), whose ps2_cpu holds 64-bit pointers.
    from build_host64 import LLVM, MSVC, SDK, VERSION
    cmd = [str(LLVM / 'clang-cl.exe'), '--target=x86_64-pc-windows-msvc', '/nologo', '/O2', '/clang:-O3',
           '/clang:-march=x86-64-v2', '/clang:-mtune=znver3', '/clang:-ffp-model=strict', '/clang:-ffp-contract=off',
           '/clang:-fno-strict-aliasing', '/clang:-fwrapv', '/MT', '/w', '-DPS2_VU_AOT', '-DPS2_VU_AOT_EMBED',
           '/I' + str(out), '/I' + str(ROOT / 'port/runtime'), '/I' + str(ROOT / 'port/include'),
           '/I' + str(ROOT / 'port/host'), '/imsvc' + str(MSVC / 'include')]
    cmd += ['/imsvc' + str(SDK / 'Include' / VERSION / x) for x in ('ucrt', 'um', 'shared')]
    if coverage: cmd += ['-DPS2_VU_COVERAGE']
    if clean_check: cmd += ['-DPS2_VU_CLEAN_CHECK']
    cmd += [str(runtime / 'vu0_interp.c'), str(ROOT / 'port/runtime/vu1_seq_diff.c'), '/Fe' + str(exe),
            '/link', '/libpath:' + str(MSVC / 'lib/x64'), '/libpath:' + str(SDK / 'Lib' / VERSION / 'ucrt/x64'),
            '/libpath:' + str(SDK / 'Lib' / VERSION / 'um/x64')]
    subprocess.run(cmd, cwd=out, check=True, timeout=1200)
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('captures', nargs='*', type=Path)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/port/vu1-seq-diff')
    ap.add_argument('--repeat', type=int, default=0, help='timed replays of each run per mode')
    ap.add_argument('--max', type=int, default=0, help='runs per capture (default all)')
    ap.add_argument('--negative', action='store_true', help='private clamp-result corruption; comparison must fail')
    ap.add_argument('--sticky-cycle', action='store_true', help='cycle entry sticky Z/S through all four states, identically for every compared mode')
    ap.add_argument('--coverage', action='store_true', help='count AOT and interpreter fallback pairs (diagnostic instrumentation; use without --repeat for normal timings)')
    ap.add_argument('--specials', metavar='SEED', help='special float bit patterns in each run\'s registers and data memory first (DHD_VU_TEST_SPECIALS)')
    ap.add_argument('--clean-check', action='store_true', help='count values the kernels wrote as clean that were not (PS2_VU_CLEAN_CHECK)')
    ap.add_argument('--target', choices=('android-arm64',), help='only build the harness for this target (run it there by hand)')
    args = ap.parse_args()
    out = args.out.resolve()
    if (ROOT / 'build').resolve() not in out.parents and ROOT / 'build' not in out.parents:  # build/ may be a junction
        ap.error('generated code holds game microcode: OUT must stay under build/')
    out.mkdir(parents=True, exist_ok=True)
    exe = build(out, args.negative, args.coverage, args.target, args.clean_check)
    if args.target:
        print('built %s' % exe)
        return
    if not args.captures:
        ap.error('no capture directory given')
    failed = 0
    for capture in args.captures:
        capture = capture.resolve()
        table = out / (capture.name + '.csv')
        with table.open('w') as log:
            env = dict(os.environ)
            if args.sticky_cycle: env['DHD_VU_TEST_STICKY_CYCLE'] = '1'
            if args.specials: env['DHD_VU_TEST_SPECIALS'] = args.specials
            p = subprocess.run([str(exe), str(capture), str(args.repeat)] + ([str(args.max)] if args.max else []),
                               stdout=log, stderr=subprocess.PIPE, text=True, timeout=7200, env=env)
        print('%s: %s' % (capture.name, p.stderr.strip()))
        failed |= p.returncode != 0
        if args.repeat:
            rows = sorted(csv.DictReader(table.open()), key=lambda r: -float(r['exact_us']) * int(r['runs']))
            for r in rows[:12]:
                print('  %s/%s vi13 %s: %5d runs, exact %7.2f us, native %7.2f us, %.2fx' % (
                    r['program'], r['entry'], r['dispatch'], int(r['runs']), float(r['exact_us']),
                    float(r['native_us']), float(r['speedup'])))
    print('VU1 NATIVE DIFF ' + ('FAILED' if failed else 'MATCH'))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
