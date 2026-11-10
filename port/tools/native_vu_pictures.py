"""Native VU's picture check (finding 194; the method of findings 164 and 167): finding 156's route
with GS captures at eleven fields, then two runs compared field by field.

    python port/tools/native_vu_pictures.py capture EXE OUT [--scale 2] [--env NAME=VALUE ...]
    python port/tools/native_vu_pictures.py compare A B [--mae 0.1] [--over 0.1] [--same]

capture: route_profile.py's route (screen-keyed menus, its race pad, seed 81257, the frame clock),
untimed, on a hidden desktop, with the native renderer's specialised pipelines built at first use
(DHD_NATIVE_SPEC=sync) and the lens flare's Z probe answered from the GS stream (DHD_EXACT_PROBE=1),
so every run has the same game state (Native VU then runs in shadow mode: its pictures come from the
mesh items while the VU also runs the batches). The native picture of each field in FIELDS (title
700, 950; shop 3200, 3500; race start 6400, 6600, 6800, 7100; mid-race 9000, 9300, 9600) goes to
OUT/raw/shot-fNNNNNN.bmp; the game is stopped after the last.

compare: per field the mean absolute difference over all channels (0-255), the share of pixels off
by more than 8 and by more than 32 in any channel, and the largest difference. The exit status says
whether every field is within the tolerance (MAE <= --mae and at most --over percent off by more
than 8; the native lane's: 0.1 and 0.1), or with --same identical. Game pictures stay local (never
committed)."""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import route_profile  # noqa: E402

FIELDS = (700, 950, 3200, 3500, 6400, 6600, 6800, 7100, 9000, 9300, 9600)
SCENES = (('title', 450, 1100), ('shop', 2950, 3750), ('start', 6400, 7300), ('mid', 9000, 14000))


def capture(a):
    out = a.out.resolve()
    shots = out / 'raw'
    env = route_profile.route_env(out, a.scale, *map(int, a.size.split('x')))
    env.update(DHD_SCREENSHOT=str(shots / 'shot'), DHD_CAPTURE_FIELDS=','.join(f'{f}-{f}' for f in FIELDS),
               DHD_EXACT_PROBE='1', DHD_NATIVE_SPEC='sync', DHD_BENCH='<unset>', DHD_BENCH_EXIT='<unset>',
               DHD_TIMELINE='<unset>')
    for item in a.env:
        k, _, v = item.partition('=')
        env[k] = v
    game = hidden_game.Game(a.exe.resolve(), out, env=env, name='native_vu_pictures', scale=a.scale,
                            shot_window=None, timed=False)
    last = shots / ('shot-f%06d.bmp' % FIELDS[-1])
    try:
        start = time.time()
        while game.alive() and not last.exists():
            if time.time() - start > a.timeout:
                raise SystemExit('timed out; see %s' % (out / 'game.log'))
            time.sleep(0.5)
        time.sleep(1.0)   # the last file written out
    finally:
        game.stop()
    missing = [f for f in FIELDS if not (shots / ('shot-f%06d.bmp' % f)).exists()]
    print('captured %d of %d fields in %s%s' % (len(FIELDS) - len(missing), len(FIELDS), shots,
                                                 ', missing %s' % missing if missing else ''))
    return 1 if missing else 0


def compare(a):
    import numpy as np
    from PIL import Image
    worst = {}
    bad = 0
    print('field  scene   MAE     >8 %     >32 %    max')
    for f in FIELDS:
        pa, pb = (Path(d) / 'raw' / ('shot-f%06d.bmp' % f) for d in (a.a, a.b))
        if not pa.exists() or not pb.exists():
            print('%5d  missing (%s)' % (f, pa if not pa.exists() else pb))
            bad += 1
            continue
        x = np.asarray(Image.open(pa).convert('RGB'), dtype=np.int16)
        y = np.asarray(Image.open(pb).convert('RGB'), dtype=np.int16)
        if x.shape != y.shape:
            print('%5d  sizes differ: %s, %s' % (f, x.shape, y.shape))
            bad += 1
            continue
        d = np.abs(x - y)
        mae = float(d.mean())
        per = d.max(axis=2)
        over8 = 100.0 * float((per > 8).mean())
        over32 = 100.0 * float((per > 32).mean())
        top = int(d.max())
        scene = next(s for s, lo, hi in SCENES if lo <= f <= hi)
        ok = top == 0 if a.same else mae <= a.mae and over8 <= a.over
        bad += not ok
        print('%5d  %-6s %6.3f  %7.3f  %7.3f  %5d%s' % (f, scene, mae, over8, over32, top, '' if ok else '  OUT'))
        w = worst.get(scene)
        if not w or mae > w[0]:
            worst[scene] = (mae, over8, over32, top, f)
    print('worst field per scene (MAE / >8 %% / >32 %% / max):')
    for scene, (mae, over8, over32, top, f) in worst.items():
        print('  %-6s %d: %.3f / %.3f %% / %.3f %% / %d' % (scene, f, mae, over8, over32, top))
    print('within' if not bad else '%d field(s) out of' % bad,
          'identical' if a.same else 'the tolerance (MAE <= %g, <= %g %% off by more than 8)' % (a.mae, a.over))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('capture')
    c.add_argument('exe', type=Path)
    c.add_argument('out', type=Path)
    c.add_argument('--scale', type=int, default=2)
    c.add_argument('--size', default='1920x1080', help='window size')
    c.add_argument('--env', action='append', default=[], help='NAME=VALUE for the game (DHD_NATIVE_VU1=on)')
    c.add_argument('--timeout', type=float, default=1800)
    k = sub.add_parser('compare')
    k.add_argument('a', type=Path)
    k.add_argument('b', type=Path)
    k.add_argument('--mae', type=float, default=0.1)
    k.add_argument('--over', type=float, default=0.1, help='percent of pixels off by more than 8')
    k.add_argument('--same', action='store_true', help='every field identical')
    a = ap.parse_args()
    sys.exit(capture(a) if a.cmd == 'capture' else compare(a))


if __name__ == '__main__':
    main()
