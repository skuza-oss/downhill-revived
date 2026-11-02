"""Does the race drift with the frame rate? (finding 139)

    python port/tools/rate_drift.py --exe build/host64-exe/dhd64.exe --out build/rate-drift/NAME
    python port/tools/rate_drift.py --exe ... --out ... --rates 30,45,60d,90,120,144 --ticks 30000

Runs one fixed-seed race (race_compare.py's menu route, seed 81257, Cross held: full pedal) on the
deterministic stepped clock (DHD_FRAME_CLOCK=1) at each rate: DHD_FPS=N, every step 60/N fields,
the steps a slow or fast PC takes on the delta-time clock, but repeatable. "60" is the original
arithmetic (the exact path); "60d" is the delta-time clock's code paths at one field a step
(DHD_STEP_CLOCK=1), the reference the other rates are compared with (--reference). The game runs
headless (no window) under the game lock (untimed), recording every rider once per race tick
(DHD_FPS_TRACE); a run ends when the race does (its clock stops after the finishes) or at --ticks.

Reported per rate: the player's official finish time and its difference from the reference, the
AI finish times (mean and largest difference, same slots), the crashes in the first 10000 ticks
(the player's, and the share of rider-ticks spent crashed, all ten riders), and at fixed
race-clock moments the player's distance from the reference's position, both checkpoints and both
speeds. The traces
hold game-derived state: they stay below build/ (or outside the checkout, --external-out). Needs
DHD_ISO or DHD_TEST_ISO.
"""
import argparse
import csv
import json
import math
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock  # noqa: E402
import race_compare as rc  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MOMENTS = (600, 1800, 3600, 7200, 14400, 21600)  # race ticks: 10 s, 30 s, 1, 2, 4, 6 min


def run(exe, out, rate, ticks, timeout):
    step_clock = rate.endswith('d')
    fps = rate.rstrip('d')
    env = {'DHD_FPS': fps, 'DHD_RENDER': 'native', 'DHD_EXACT_PROBE': '1', 'DHD_HEADLESS': '1',
           'DHD_FPS_TRACE': str(out / 'trace.csv'), 'DHD_FPS_STOP': str(ticks)}
    if step_clock:
        env['DHD_STEP_CLOCK'] = '1'
    settings = argparse.Namespace(exe=exe.resolve(), nav=rc.NAV, race_pad='0:900:cross', window=0,
                                  race_seed=rc.RETAIL_RACE_SEED, dump=False, no_gsdump=True, port_env=env)
    start = time.monotonic()
    with game_lock.held('rate_drift ' + rate):
        process, digest = rc.launch_port(settings, out, rc.clock_events(settings.race_pad), [])
        try:
            while process.poll() is None:
                if time.monotonic() - start > timeout:
                    raise TimeoutError(f'{rate}: the race did not end in {timeout} s')
                time.sleep(1)
        finally:
            process.stop()
    (out / 'run.json').write_text(json.dumps(dict(rate=rate, ticks=ticks, seed=rc.RETAIL_RACE_SEED, exe_sha256=digest,
                                                  wall_seconds=round(time.monotonic() - start, 1)), indent=2) + '\n')


def load(out):
    """{slot: [row, ...]} by tick, and each slot's official finish in ticks (None: not finished)."""
    rows, finish = {}, {}
    with (out / 'trace.csv').open() as f:
        for row in csv.DictReader(f):
            slot = int(row['slot'])
            rows.setdefault(slot, []).append(row)
            if row['finished'] != '0' and finish.get(slot) is None:
                finish[slot] = int(row['finish_ticks'])
    return rows, finish


def at(rows, tick):
    """The first row at or after `tick` (a 30 step covers two ticks), None past the end."""
    for row in rows:
        if int(row['tick']) >= tick:
            return row
    return None


def summarize(out, rates, reference):
    data = {r: load(out / r) for r in rates if (out / r / 'trace.csv').exists()}
    ref_rows, ref_finish = data[reference]
    result = {}
    for rate, (rows, finish) in data.items():
        player = finish.get(0)
        ai = {s: t for s, t in finish.items() if s and t is not None}
        same = [s for s in ai if ref_finish.get(s) is not None]
        early = [r for slot in rows for r in rows[slot] if int(r['tick']) <= 10000]
        crashes, previous = 0, '0'
        for r in rows[0]:
            if int(r['tick']) > 10000:
                break
            crashes += r['crash'] == '1' and previous == '0'
            previous = r['crash']
        entry = dict(player_crashes=crashes, crashed_share=sum(r['crash'] == '1' for r in early) / max(1, len(early)),
                     player_finish_s=player / 60 if player else None,
                     player_delta_pct=(player - ref_finish[0]) / ref_finish[0] * 100
                     if player and ref_finish.get(0) else None,
                     ai_finished=len(ai), ai_mean_s=sum(ai.values()) / len(ai) / 60 if ai else None,
                     ai_mean_delta_pct=(sum(ai[s] for s in same) / sum(ref_finish[s] for s in same) - 1) * 100
                     if same else None,
                     ai_largest_delta_s=max((abs(ai[s] - ref_finish[s]) / 60 for s in same), default=None),
                     moments={})
        for tick in MOMENTS:
            a, b = at(rows[0], tick), at(ref_rows[0], tick)
            if not a or not b:
                continue
            distance = math.dist([float(a[k]) for k in 'xyz'], [float(b[k]) for k in 'xyz'])
            entry['moments'][tick] = dict(distance=round(distance, 2), checkpoint=int(a['checkpoint']),
                                          reference_checkpoint=int(b['checkpoint']), speed=round(float(a['speed']), 2),
                                          reference_speed=round(float(b['speed']), 2))
        result[rate] = entry
    return result


def text(summary, reference):
    lines = [f'reference {reference}; finish times are the official counter / 60']
    for rate, e in summary.items():
        p = f"{e['player_finish_s']:.3f} s" if e['player_finish_s'] else 'no finish'
        d = f" ({e['player_delta_pct']:+.2f}%)" if e['player_delta_pct'] is not None else ''
        ai = (f"AI {e['ai_finished']} finished, mean {e['ai_mean_s']:.2f} s ({e['ai_mean_delta_pct']:+.2f}%), "
              f"largest slot difference {e['ai_largest_delta_s']:.2f} s") if e['ai_mean_s'] and e['ai_mean_delta_pct'] is not None else \
            f"AI {e['ai_finished']} finished"
        moments = ', '.join(f"{t // 60} s: {m['distance']:.1f} away, cp {m['checkpoint']}/{m['reference_checkpoint']}"
                            for t, m in e['moments'].items())
        lines.append(f"{rate:>4}: player {p}{d}; {ai}; crashes {e['player_crashes']} (crashed {e['crashed_share'] * 100:.0f}%); "
                     f'{moments}')
    return '\n'.join(lines) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--rates', default='30,45,60,60d,90,120,144')
    ap.add_argument('--reference', default='60d')
    ap.add_argument('--ticks', type=int, default=30000, help='race ticks (1/60 s) at most')
    ap.add_argument('--timeout', type=int, default=7200, help='seconds per run')
    ap.add_argument('--summary-only', action='store_true', help='summarize existing runs in --out')
    ap.add_argument('--external-out', action='store_true', help='allow an output folder outside the checkout')
    args = ap.parse_args()
    out = args.out.resolve()
    build = (ROOT / 'build').resolve()  # build/ may be a junction to another drive (worktree_setup.py)
    if build not in out.parents and (not args.external_out or out == ROOT or ROOT in out.parents):
        ap.error('--out must be below build/, or outside the checkout with --external-out')
    rates = args.rates.split(',')
    if args.reference not in rates:
        ap.error('--reference must be one of --rates')
    for rate in rates:
        if not 30 <= int(rate.rstrip('d')) <= 240:
            ap.error(f'rate {rate}: 30..240')
    if not args.summary_only:
        if not os.environ.get('DHD_ISO'):
            if not os.environ.get('DHD_TEST_ISO'):
                ap.error('set DHD_ISO or DHD_TEST_ISO to the game image')
            os.environ['DHD_ISO'] = os.environ['DHD_TEST_ISO']
        for rate in rates:
            folder = out / rate
            if (folder / 'run.json').exists():
                print(f'{rate}: already run')
                continue
            folder.mkdir(parents=True, exist_ok=True)
            print(f'{rate}: running', flush=True)
            run(args.exe, folder, rate, args.ticks, args.timeout)
    summary = summarize(out, rates, args.reference)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    report = text(summary, args.reference)
    (out / 'summary.txt').write_text(report)
    print(report, end='')
    return 0


if __name__ == '__main__':
    sys.exit(main())
