"""Flying scenery at the same speed in game time at every frame rate (finding 193).

    python port/tools/test_scenery_rate.py --exe build/host64-exe/dhd64.exe --out build/scenery-rate/a
    python port/tools/test_scenery_rate.py --row 3 --rates 60,240 --out build/scenery-rate/red   # Red Pass FR
    python port/tools/test_scenery_rate.py --right 11 --out build/scenery-rate/r11              # every track unlocked

Runs the same race at each rate (the state trace's deterministic headless recipe: virtual clock,
pinned seed, Cross held from GO) with DHD_SCENERY_TRACE, which writes every flying scenery object
once per race tick: the helicopters, the birds and bats of a flock, the swooping attacker
(port/hle/scenery_trace.c). From each run it takes, per kind of object:

- the cruise speed: the 90th percentile of the distance an object covers in a second of race
  clock, over the seconds in which it moved;
- the distance flown per object and second, over all the run's objects and seconds;
- the closest any of them came to a rider.

A run at a higher rate passes when each kind's cruise speed is within --tolerance of the 60 run's
(the objects follow their own paths, so their speed is the measure that does not depend on the
riders, whose race differs a little between rates; finding 139). Before finding 193 the
helicopters flew at 240 about four times as fast as at 60. The positions against race time of the
first --align seconds are reported too (median and largest distance between the two runs at the
same tick, per kind): before any rider-dependent choice they follow the same path.

Exit status 0 when every compared rate passes. The traces hold game-derived state: --out must be
below build/ (or outside the checkout with --external-out).
"""
import argparse
import csv
import json
import math
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock  # noqa: E402
import owned_process  # noqa: E402
import state_trace_run  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'build' / 'host64-exe' / 'dhd64.exe'
FULL_PEDAL = '2:1200:cross'


def race_nav(row, right):
    """The menu route to the race: the track chooser moved right so often (state_trace_run)."""
    steps = right if right is not None else row
    return state_trace_run.track_nav(steps) if steps else state_trace_run.RACE_NAV


def run(args, rate, out):
    out.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    env.update(DHD_ISO=os.environ.get('DHD_ISO') or state_trace_run.DEFAULT_ISO, DHD_HEADLESS='1',
               DHD_FRAME_CLOCK='1', DHD_SKIP_MOVIES='1', DHD_MEMCARD='0', DHD_SND_OFF='1', DHD_RENDER='native',
               DHD_SETTINGS='none', DHD_FPS=str(rate), DHD_NAV=race_nav(args.row, args.right), DHD_RACE_SYNC='1',
               DHD_RACE_PAD=FULL_PEDAL, DHD_RACE_SEED=str(args.seed),
               DHD_FPS_TRACE=str(out / 'riders.csv'), DHD_FPS_STOP=str(args.ticks),
               DHD_SCENERY_TRACE=str(out / 'scenery.csv'), DHD_STATE_STOP='200000')
    if args.right is not None:
        env['DHD_UNLOCK_ALL'] = '1'
    start = time.monotonic()
    with game_lock.held(f'test_scenery_rate {out.name}'), (out / 'game.log').open('wb') as log:  # untimed
        process = owned_process.run([str(Path(args.exe).resolve())], timeout=args.timeout, cwd=str(out), env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
    if process.timed_out:
        raise RuntimeError(f'{rate}: no race tick {args.ticks} in {args.timeout} s ({out / "game.log"})')
    if not (out / 'scenery.csv').exists():
        raise RuntimeError(f'{rate}: no scenery trace ({out / "game.log"})')
    return time.monotonic() - start


def read(path):
    """The track ids seen, and {(kind, index): {tick: (x, y, z, state, nearest_distance)}}."""
    objects, tracks = {}, set()
    with path.open() as f:
        for row in csv.DictReader(f):
            tracks.add(int(row['track']))
            if row['kind'] == 'race':
                continue
            key = (row['kind'], int(row['index']))
            objects.setdefault(key, {})[int(row['tick'])] = (
                float(row['x']), float(row['y']), float(row['z']), int(row['state']), float(row['nearest_distance']))
    return tracks, objects


def speeds(objects):
    """Per kind: the distance each object covered in each whole second of race clock it was traced in."""
    per_kind = {}
    for (kind, _), ticks in objects.items():
        for second in range(min(ticks) // 60, max(ticks) // 60 + 1):
            span = [ticks[t] for t in range(second * 60, second * 60 + 61) if t in ticks]
            if len(span) < 61:
                continue
            distance = sum(math.dist(a[:3], b[:3]) for a, b in zip(span, span[1:]))
            if distance < 1e4:  # a respawn moves a body anywhere: not flight
                per_kind.setdefault(kind, []).append(distance)
    return per_kind


def summary(objects):
    result = {}
    for kind, seconds in speeds(objects).items():
        moving = sorted(s for s in seconds if s > 1.0)
        closest = min(v[4] for (k, _), ticks in objects.items() if k == kind for v in ticks.values())
        result[kind] = dict(objects=sum(1 for k, _ in objects if k == kind), seconds=len(seconds),
                            moving_seconds=len(moving),
                            cruise=moving[int(0.9 * (len(moving) - 1))] if moving else 0.0,
                            mean=statistics.fmean(seconds) if seconds else 0.0, closest_rider=closest)
    return result


def align(low, high, seconds):
    """Per kind: distance between the two runs' positions of the same object at the same tick."""
    result = {}
    for key, ticks in low.items():
        other = high.get(key)
        if not other:
            continue
        gaps = [math.dist(ticks[t][:3], other[t][:3]) for t in sorted(ticks) if t in other and t <= seconds * 60]
        if gaps:
            result.setdefault(key[0], []).extend(gaps)
    return {kind: dict(ticks=len(g), median=statistics.median(g), largest=max(g)) for kind, g in result.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', default=str(EXE))
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--rates', default='60,144,240', help='the first is the reference (60)')
    ap.add_argument('--row', type=int, default=0,
                    help='the track chooser moved right this often, fresh save (state_trace_run.UNLOCKED_TRACKS)')
    ap.add_argument('--right', type=int, help='the same with every track unlocked (DHD_UNLOCK_ALL=1): 3 is '
                                              'Red Pass FR, the trace names the track')
    ap.add_argument('--ticks', type=int, default=3600, help='race ticks (1/60 s) from GO')
    ap.add_argument('--seed', type=int, default=state_trace_run.DEFAULT_SEED)
    ap.add_argument('--tolerance', type=float, default=0.15, help='cruise speed: high/60 within 1 +- this')
    ap.add_argument('--align', type=int, default=20, help='seconds of positions compared at the same tick')
    ap.add_argument('--timeout', type=int, default=1800)
    ap.add_argument('--external-out', action='store_true', help='allow an output folder outside the checkout')
    args = ap.parse_args()
    out = args.out.resolve()
    if ROOT / 'build' not in out.parents and (not args.external_out or out == ROOT or ROOT in out.parents):
        ap.error('--out must be below build/, or outside the checkout with --external-out')
    if out.exists():
        ap.error(f'{out} exists; every run gets a new folder')
    rates = [int(r) for r in args.rates.split(',')]
    traces = {}
    for rate in rates:
        seconds = run(args, rate, out / str(rate))
        tracks, traces[rate] = read(out / str(rate) / 'scenery.csv')
        print(f'{rate}: track {sorted(tracks)}, {len(traces[rate])} objects traced through tick {args.ticks} '
              f'({seconds:.0f} s)')
    reference = summary(traces[rates[0]])
    report = dict(rates=rates, row=args.row, right=args.right, track=sorted(tracks), ticks=args.ticks, seed=args.seed,
                  tolerance=args.tolerance, runs={}, verdict='PASS')
    if not reference:
        report['verdict'] = 'NONE'
        print('no flying scenery object moved in the reference run')
    for rate in rates:
        result = summary(traces[rate])
        entry = report['runs'][rate] = dict(kinds=result)
        if rate != rates[0]:
            entry['aligned'] = align(traces[rates[0]], traces[rate], args.align)
        for kind, r in sorted(result.items()):
            ref = reference.get(kind)
            ratio = r['cruise'] / ref['cruise'] if ref and ref['cruise'] else None
            ok = ratio is None or rate == rates[0] or abs(ratio - 1) <= args.tolerance
            r['ratio'] = ratio
            r['pass'] = ok
            if not ok:
                report['verdict'] = 'FAIL'
            shown = f'x{ratio:.3f}' if ratio is not None else '-'
            line = (f'  {rate:3} {kind:8} {r["objects"]} objects, {r["moving_seconds"]}/{r["seconds"]} s moving: '
                    f'cruise {r["cruise"]:.1f}/s ({shown} of {rates[0]}), mean {r["mean"]:.1f}/s, '
                    f'closest to a rider {r["closest_rider"]:.1f}')
            if rate != rates[0] and kind in entry['aligned']:
                a = entry['aligned'][kind]
                line += f'; first {args.align} s against {rates[0]}: median {a["median"]:.1f}, largest {a["largest"]:.1f}'
            print(line + ('' if ok else '  FAIL'))
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report['verdict'])
    return report['verdict'] != 'PASS'


if __name__ == '__main__':
    sys.exit(main())
