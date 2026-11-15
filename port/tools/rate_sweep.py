"""Speed sweep: the same scenes at 60 and at a high rate, every EE word's travel per field compared
(finding 255).

    python port/tools/rate_sweep.py --out build/rate-sweep/a                       # every scene
    python port/tools/rate_sweep.py --out build/rate-sweep/t --scenes title,menu3
    python port/tools/rate_sweep.py --out build/rate-sweep/r --scenes track0,track7 --rate 240
    python port/tools/rate_sweep.py --list                                         # the scenes

Each scene runs twice, at 60 and at --rate (240), headless on the deterministic clock (the state
trace's recipe: virtual clock, no movies, no sound, no memory card, default settings, pinned race
seed, the same field-keyed menu route and race-clock pad), with the rate audit
(port/hle/rate_audit.c, DHD_RATE_AUDIT): from the moment the scene shows (its menu id, race phase)
plus a skip, over the same number of fields at both rates, every word of EE RAM sums how far it
moved (travel) from one field's end to the next (DHD_RATE_AUDIT_FIELDS: the high rate is read at
the same instants as 60, not at each of its updates). A word that moves by game time travels as far per field at both rates; a word
stepped once per update without the frame's time travels rate/60 times as far at the high rate
(4 at 240), and a steering loop whose result depends on the step size lands anywhere else.

Unlike rate_audit.py's compare (which keeps words moving one way), oscillators stay in: a pulse,
a blink, a bounce or a wobble has travel but no net, and is exactly what this sweep is for. The
report lists every word whose travel per field differs by more than --tolerance (3 %), with the
nearest data symbol below it (the D_ names of src/), grouped by symbol, largest difference first.
Words the game counts each update with no game-time meaning (frame and packet counters, buffer
pointers) show too, at rate/60: read each against its code before calling it a bug.

Scenes (--list): the boot title (menu 0), each menu on the route to a race (the route stops
before the step that leaves it), every track's first minute of race (every track unlocked, the
chooser moved right N times, Cross held), the race's results (DHD_TEST_FINISH: the player's rider
finishes at race second 20) and a replay (DHD_REPLAY_TEST: the replay starts at race second 20;
its 60 run takes the delta-time paths at one field a step, DHD_STEP_CLOCK=1, as replays exist
only there). Race scenes start once a race's loop runs (DHD_RATE_AUDIT_RACE): the phase and race clock words
hold stale values before the first race.

The audits hold game-derived state: --out must be below build/ (or outside the checkout with
--external-out). Exit status 0 when every scene ran and nothing differs (or --report-only).
"""
import argparse
import bisect
import concurrent.futures
import json
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock  # noqa: E402
import owned_process  # noqa: E402
import rate_audit  # noqa: E402
import state_trace_run  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'build' / 'host64-exe' / 'dhd64.exe'
TRACKS = 26  # track chooser positions with every track unlocked (finding 193: 0..25)
# Words the game counts once per picture or per update by design, with no game-time meaning
# (finding 255 read each): listed apart, not a difference.
BOOKKEEPING = {
    'D_0029E10C': 'picture counter (001B4DD0: its parity picks the double buffer)',
    'D_0029E110': 'picture double-buffer flag (001B4DD0)',
    'D_0029E160': 'animation clock, 1/1024 field on the delta-time clock (finding 090)',
    'D_0029E218': 'texture cache use stamp (00218C28)',
    'D_0029E224': 'texture cache bank flip (00219EC8)',
    'D_00625190': 'texture cache slots: use stamps (00218C28)',
    'D_004C8940': 'SIF RPC client, calls per picture (00201180)',
    'D_00660380': 'pad read buffers, read each update (0021F400)',
    'D_0027ECC0': 'GS frame descriptors, set per picture (0023C8D8)',
    'D_00528380': 'GS frame descriptors, set per picture (0023D098)',
    'D_0029E284': 'particle packet buffer flag (0021D250)',
    'D_0029E3E0': "the original clock's timer count (00238C00; the port's clock replaces it)",
    'D_0029E3E8': "the original clock's timer count (00238C00)",
    'D_0029E3EC': "the original clock's timer count (00238C00)",
    'D_00513B00': 'draw list double buffer (001517D0)',
    'D_00660200': 'DMA packet buffer, uncached (0021F0A0)',
    'D_0062F980': 'DMA packet buffer',
    'D_0062E850': 'texture cache (00218C28)',
    'D_0062E890': 'texture cache (00218C28)',
    'D_00279450': 'SIF RPC buffers (00201180)',
    'D_00279468': 'SIF RPC buffers (00201180)',
}


def route_until(index):
    """RACE_NAV's first `index` steps: the game stays on the screen the next step waits for."""
    steps = state_trace_run.RACE_NAV.split(',')
    return ','.join(steps[:index]), int(re.match(r'\d+', steps[index])[0])


def scenes():
    """name -> dict(nav, menu, phase, skip, fields, env, what). skip and fields count fields."""
    # the title is menu 0 behind the memory card dialog, which Triangle cancels (menu_nav.h)
    result = {'title': dict(nav='0:triangle', menu=0, phase=None, skip=300, fields=900, env={},
                            what='the boot title, PRESS X TO START')}
    steps = state_trace_run.RACE_NAV.split(',')
    seen = set()
    for i in range(1, len(steps) - 1):
        nav, menu = route_until(i)
        if menu in seen or menu == 9:
            continue
        seen.add(menu)
        result[f'menu{menu}'] = dict(nav=nav, menu=menu, phase=None, skip=60, fields=600, env={},
                                     what=f'menu {menu} on the race route, idle')
    race_env = dict(DHD_RACE_SYNC='1', DHD_RACE_PAD=state_trace_run.FULL_PEDAL)
    for n in range(TRACKS):
        result[f'track{n}'] = dict(nav=state_trace_run.track_nav(n), menu=9, phase='2', race=1, skip=10, fields=3600,
                                   env=dict(race_env, DHD_UNLOCK_ALL='1'),
                                   what=f'race, the chooser moved right {n} (every track unlocked), first minute')
    # from the load to GO the race runs on the field grid (finding 142): one field a step at every
    # rate, so the window counts the same updates (grid)
    result['countdown'] = dict(nav=state_trace_run.RACE_NAV, menu=9, phase='1', skip=0, fields=150, env=race_env, grid=True,
                               what='Mt. Midoule FR, the countdown before GO')
    # after the finish the race stays in phase 3 with its clock stopped near the finish
    result['results'] = dict(nav=state_trace_run.RACE_NAV, menu=9, phase='3', race=1, skip=60, fields=900,
                             env=dict(race_env, DHD_TEST_FINISH='20'),
                             what='Mt. Midoule FR, the results after a finish at race second 20')
    # from 5 s into the replay (race=2: the clock plays a replay's steps); replays exist on the
    # delta-time clock only, so its 60 run takes those paths at one field a step
    result['replay'] = dict(nav=state_trace_run.RACE_NAV, menu=9, phase=None, race=2, skip=300, fields=900,
                            env=dict(race_env, DHD_REPLAY_TEST='20'), env60=dict(DHD_STEP_CLOCK='1'),
                            what='Mt. Midoule FR, a replay of the first 20 s, from 5 s into it')
    return result


def run_one(args, scene, spec, rate, out):
    out.mkdir(parents=True, exist_ok=True)
    audit = out / f'{scene}-{rate}.txt'
    scale = 1 if spec.get('grid') else rate / 60
    env = rate_audit.game_env(rate, audit, round(spec['skip'] * scale), round(spec['fields'] * scale), 'boot',
                              args.seed, spec['menu'], spec['phase'])
    env['DHD_NAV'] = spec['nav']
    env['DHD_RATE_AUDIT_FIELDS'] = '1'  # both rates sampled at the same field ends
    if 'DHD_RACE_SYNC' in spec['env']:
        env['DHD_RACE_SEED'] = str(args.seed)
    env.update(spec['env'])
    if rate == 60:
        env.update(spec.get('env60', {}))
    if spec.get('race'):
        env['DHD_RATE_AUDIT_RACE'] = str(spec['race'])
    for item in args.env or ():
        key, _, value = item.partition('=')
        env[key] = value
    start = time.monotonic()
    with game_lock.held(f'rate_sweep {scene} {rate}'), (out / f'{scene}-{rate}.log').open('wb') as log:  # untimed
        process = owned_process.run([str(Path(args.exe).resolve())], timeout=args.timeout, cwd=str(out), env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
    if process.timed_out or not audit.exists():
        raise RuntimeError(f'{scene} at {rate}: no audit ({out / f"{scene}-{rate}.log"})')
    return time.monotonic() - start


def symbols():
    """Sorted data symbol addresses and names from src/ (D_XXXXXXXX)."""
    names = set()
    for path in (ROOT / 'src').rglob('*.c*'):
        names.update(re.findall(r'\bD_([0-9A-F]{8})\b', path.read_text(errors='replace')))
    addresses = sorted(int(n, 16) for n in names)
    return addresses


def symbol_of(addresses, addr):
    i = bisect.bisect_right(addresses, addr) - 1
    if i < 0:
        return f'0x{addr:08X}'
    base = addresses[i]
    return f'D_{base:08X}' + (f'+0x{addr - base:X}' if addr != base else '')


def compare(low_path, high_path, args, addresses):
    h60, r60 = rate_audit.read_audit(low_path)
    hhi, rhi = rate_audit.read_audit(high_path)
    f60 = int(h60['field1']) - int(h60['field0'])
    fhi = int(hhi['field1']) - int(hhi['field0'])
    n60 = int(h60.get('samples', 0))
    rows = []
    for addr, (travel, kind, value, changes, net) in r60.items():
        if travel / max(f60, 1) < args.min_travel or (changes or 0) < args.min_changes * n60:
            continue
        high = rhi.get(addr, (0.0,))[0]
        ratio = (high / max(fhi, 1)) / (travel / f60)
        if abs(ratio - 1) > args.tolerance:
            symbol = symbol_of(addresses, addr)
            rows.append(dict(addr=f'0x{addr:08X}', symbol=symbol, kind=kind, ratio=ratio,
                             per_field_60=travel / f60, per_field_high=high / max(fhi, 1), changes_60=changes,
                             changes_high=rhi.get(addr, (0, '', '', 0))[3], value_60=value))
    rows.sort(key=lambda r: -abs(r['ratio'] - 1))
    known = [r for r in rows if r['symbol'].split('+')[0] in BOOKKEEPING]
    rows = [r for r in rows if r['symbol'].split('+')[0] not in BOOKKEEPING]
    return dict(bookkeeping=sorted({r['symbol'].split('+')[0] for r in known}),fields_60=f60, fields_high=fhi, samples_60=n60, samples_high=int(hhi.get('samples', 0)),
                gate_fail=(h60.get('gate_fail'), hhi.get('gate_fail')), menus=(h60.get('menu1'), hhi.get('menu1')),
                phases=(h60.get('phase1'), hhi.get('phase1')), words=rows)


def grouped(words):
    """Words per symbol: the symbol's largest difference, its word count and the words."""
    groups = {}
    for w in words:
        groups.setdefault(w['symbol'].split('+')[0], []).append(w)
    return sorted(groups.items(), key=lambda kv: -max(abs(w['ratio'] - 1) for w in kv[1]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', default=str(EXE))
    ap.add_argument('--out', type=Path)
    ap.add_argument('--scenes', help='comma-separated scene names (default every scene)')
    ap.add_argument('--rate', type=int, default=240)
    ap.add_argument('--seed', type=int, default=state_trace_run.DEFAULT_SEED)
    ap.add_argument('--tolerance', type=float, default=0.03, help='travel per field: high/60 within 1 +- this')
    ap.add_argument('--min-travel', type=float, default=0.01, help='least travel per field at 60')
    ap.add_argument('--min-changes', type=float, default=0.25,
                    help='least fraction of the 60 run\'s updates at which the word changed')
    ap.add_argument('--top', type=int, default=12, help='symbols shown per scene')
    ap.add_argument('--jobs', type=int, default=2, help='games at once')
    ap.add_argument('--env', action='append', help='KEY=VALUE for every run')
    ap.add_argument('--timeout', type=int, default=3600)
    ap.add_argument('--compare-only', action='store_true', help='compare the audits already in --out')
    ap.add_argument('--report-only', action='store_true', help='exit 0 whatever differs')
    ap.add_argument('--external-out', action='store_true', help='allow an output folder outside the checkout')
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()
    table = scenes()
    if args.list:
        for name, spec in table.items():
            print(f'{name:10} {spec["what"]}')
        return 0
    if not args.out:
        ap.error('--out is required')
    out = args.out.resolve()
    if ROOT / 'build' not in out.parents and (not args.external_out or out == ROOT or ROOT in out.parents):
        ap.error('--out must be below build/, or outside the checkout with --external-out')
    chosen = args.scenes.split(',') if args.scenes else list(table)
    for name in chosen:
        if name not in table:
            ap.error(f'unknown scene {name} (--list)')
    if not args.compare_only:
        jobs = [(name, rate) for name in chosen for rate in (60, args.rate)
                if not (out / f'{name}-{rate}.txt').exists()]
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            futures = {pool.submit(run_one, args, name, table[name], rate, out): (name, rate) for name, rate in jobs}
            for future in concurrent.futures.as_completed(futures):
                name, rate = futures[future]
                try:
                    print(f'{name} at {rate}: {future.result():.0f} s', flush=True)
                except RuntimeError as error:
                    print(f'FAILED {error}', flush=True)
    addresses = symbols()
    report, failed, differs = {}, [], False
    for name in chosen:
        low, high = out / f'{name}-60.txt', out / f'{name}-{args.rate}.txt'
        if not low.exists() or not high.exists():
            failed.append(name)
            continue
        result = report[name] = compare(low, high, args, addresses)
        differs |= bool(result['words'])
        print(f'\n{name}: {table[name]["what"]}; fields {result["fields_60"]} / {result["fields_high"]}, '
              f'end menu {result["menus"]} phase {result["phases"]}, '
              f'{len(result["words"])} words outside +-{args.tolerance:.0%}')
        if result['bookkeeping']:
            print('  bookkeeping (not counted): ' + ', '.join(result['bookkeeping']))
        for symbol, words in grouped(result['words'])[:args.top]:
            words.sort(key=lambda w: -abs(w['ratio'] - 1))
            w = words[0]
            print(f'  {symbol:12} {len(words):4} words, largest x{w["ratio"]:.3f} at {w["symbol"]} ({w["kind"]}, '
                  f'{w["per_field_60"]:.4g}/field at 60, {w["per_field_high"]:.4g} at {args.rate})')
    (out / 'report.json').write_text(json.dumps(dict(rate=args.rate, tolerance=args.tolerance, scenes=report,
                                                     failed=failed), indent=1) + '\n')
    if failed:
        print('no audit for: ' + ', '.join(failed))
    return 0 if args.report_only else int(bool(failed) or differs)


if __name__ == '__main__':
    sys.exit(main())
