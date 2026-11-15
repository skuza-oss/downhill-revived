"""The title's pulse and the menus' age at every frame rate, update by update (finding 255).

    python port/tools/test_menu_rate.py --out build/menu-rate/a
    python port/tools/test_menu_rate.py --out build/menu-rate/b --configs 30,240,uneven

Each configuration runs the game headless on the deterministic clock (the state trace's recipe)
with the rate watch (port/hle/rate_audit.c, DHD_RATE_WATCH): after every update, the game's field
counter, the step the update ran and a few words. Configurations: 60 (the original arithmetic,
the reference), 60d (the delta-time paths at one field a step, DHD_STEP_CLOCK=1, as paced players
at 60 run), 30 (two-field steps; 20 fps runs the same, the clock's steps stop at two fields), 45,
240, and uneven (DHD_STEP_PATTERN: steps of 0.25 to 2 fields in a repeating, uneven order).

- The title (menu 0): the PRESS X TO START box's edge D_0029E008. Retail moves it 2.4 a field from
  401.6 down to 351.2 and back up, a 42-field triangle; after every update the edge must be that
  triangle's value at the game time reached since the title's first call (within --pulse-error,
  0.1 units, a 24th of a field: at 240 the float sum of 0.6-unit steps drifts about 0.004 units
  per 100 fields, 0.055 over the run; a wrong step or turning point is off by tens of units).
- The menus on the route to a race: every menu block (D_004C6B88 + 0x30 * i, its age at +4;
  func_001F7420 initializes a block whose age is 0, func_001F74F0 counts it). A screen's block
  must have a nonzero age after the screen's first update, as retail's (the update after the
  menu id changed): each further update with age 0 initializes it again (the selection and fades
  reset, a press lost).

Exit status 0 when every configuration passes. The watch files hold game-derived state: --out
must be below build/ (or outside the checkout with --external-out).
"""
import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock  # noqa: E402
import owned_process  # noqa: E402
import state_trace_run  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / 'build' / 'host64-exe' / 'dhd64.exe'
MENU = 0x0029DED8
PULSE = 0x0029E008
BLOCKS = [0x004C6B88 + 0x30 * i for i in range(16)]  # D_004C6B88 .. D_004C6E58
CONFIGS = {
    '60': {'DHD_FPS': '60'},
    '60d': {'DHD_FPS': '60', 'DHD_STEP_CLOCK': '1'},
    '30': {'DHD_FPS': '30'},
    '45': {'DHD_FPS': '45'},
    '240': {'DHD_FPS': '240'},
    'uneven': {'DHD_FPS': '240', 'DHD_STEP_PATTERN': '0.25,0.5,0.25,1.75,0.25,0.5,2,0.25,0.75,0.25,1.25,0.5'},
}
SCENES = {
    'title': ('0:triangle', 1500),            # stays on the title (Triangle cancels the card dialog)
    'menus': (state_trace_run.RACE_NAV, 2600),  # the menus to the race load
}


def run(args, config, scene, out):
    nav, stop = SCENES[scene]
    words = [MENU, PULSE] + [a + k for a in BLOCKS for k in (0, 4)]
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    env.update(DHD_ISO=os.environ.get('DHD_ISO') or state_trace_run.DEFAULT_ISO, DHD_HEADLESS='1',
               DHD_FRAME_CLOCK='1', DHD_SKIP_MOVIES='1', DHD_MEMCARD='0', DHD_SND_OFF='1', DHD_RENDER='native',
               DHD_SETTINGS='none', DHD_NAV=nav, DHD_RATE_WATCH=str(out / f'{scene}-{config}.txt'),
               DHD_RATE_WATCH_WORDS=','.join(f'{w:X}' for w in words), DHD_RATE_WATCH_STOP=str(stop),
               DHD_STATE_STOP='400000')
    env.update(CONFIGS[config])
    with game_lock.held(f'test_menu_rate {scene} {config}'), \
            (out / f'{scene}-{config}.log').open('wb') as log:  # untimed: the virtual clock
        process = owned_process.run([str(Path(args.exe).resolve())], timeout=args.timeout, cwd=str(out), env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
    if process.timed_out:
        raise RuntimeError(f'{scene} {config}: timed out')


def rows(path):
    for line in open(path):
        parts = line.split()
        if len(parts) < 3:
            continue
        values = [int(p, 16) for p in parts[2:]]
        yield int(parts[0]), float(parts[1]), values


def as_float(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def triangle(t):
    """Retail's box edge after t fields of the title: 401.6 down 2.4 a field to 351.2, then up."""
    t %= 42.0
    return 401.6 - 2.4 * t if t <= 21 else 351.2 + 2.4 * (t - 21)


def check_title(path, limit, per_call):
    started, elapsed, worst, samples, low, high = False, 0.0, 0.0, 0, 1e9, -1e9
    for field, step, values in rows(path):
        menu, x = values[0], as_float(values[1])
        if menu != 0:
            continue
        if not started:
            if not 300.0 < x < 450.0:
                continue  # 0 until the title's first call, which sets 401.6 and moves the box at once
            started = True
        elapsed += 1.0 if per_call else step  # the original clock: retail's once a call
        worst = max(worst, abs(x - triangle(elapsed)))
        low, high = min(low, x), max(high, x)
        samples += 1
    ok = samples > 0 and worst <= limit
    return ok, f'{samples} updates, {elapsed:.1f} fields, edge {low:.2f}..{high:.2f}, largest error {worst:.3f}'


def check_menus(path):
    """Per screen opened: the updates from the menu id's change to its block's first nonzero age."""
    waits, changed, menu_before, ages = [], None, None, None
    for n, (field, step, values) in enumerate(rows(path)):
        menu, now = values[0], [values[3 + 2 * i] for i in range(len(BLOCKS))]
        if menu_before is not None and menu != menu_before:
            changed = n
        if ages is not None and changed is not None:
            for i in range(len(BLOCKS)):
                if ages[i] == 0 and now[i] != 0:
                    waits.append(n - changed)
        menu_before, ages = menu, now
    ok = bool(waits) and all(w == 1 for w in waits)
    return ok, (f'{len(waits)} screens: updates until the block is counted {waits} (retail 1; each more '
                f'is one more initialize)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', default=str(EXE))
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--configs', default=','.join(CONFIGS))
    ap.add_argument('--pulse-error', type=float, default=0.1, help='box edge against retail\'s triangle, units')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--timeout', type=int, default=1800)
    ap.add_argument('--external-out', action='store_true', help='allow an output folder outside the checkout')
    args = ap.parse_args()
    out = args.out.resolve()
    if ROOT / 'build' not in out.parents and (not args.external_out or out == ROOT or ROOT in out.parents):
        ap.error('--out must be below build/, or outside the checkout with --external-out')
    if out.exists():
        ap.error(f'{out} exists; every run gets a new folder')
    out.mkdir(parents=True)
    configs = args.configs.split(',')
    for c in configs:
        if c not in CONFIGS:
            ap.error(f'unknown configuration {c}: {", ".join(CONFIGS)}')
    jobs = [(c, s) for c in configs for s in SCENES]
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for future in [pool.submit(run, args, c, s, out) for c, s in jobs]:
            future.result()
    failed = False
    for c in configs:
        ok_t, title = check_title(out / f'title-{c}.txt', args.pulse_error, c == '60')
        ok_m, menus = check_menus(out / f'menus-{c}.txt')
        failed |= not (ok_t and ok_m)
        print(f'{c:7} title {"ok  " if ok_t else "FAIL"} {title}')
        print(f'{"":7} menus {"ok  " if ok_m else "FAIL"} {menus}')
    print('FAIL' if failed else 'PASS')
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
