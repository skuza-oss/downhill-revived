"""A race and its replay, headless (finding 141): the state trace's race route (first track, seed
81257, the race-clock pad), its replay started at race second S as REPLAY in the race results
starts it (DHD_REPLAY_TEST, port/hle/replay.c), and every rider's position at each field of race
clock in both (DHD_REPLAY_TRACE). The replay passes when it plays to the end of the recording
(the game's own z check every 60 samples never stops it) and every rider is where it was.

    python port/tools/replay_check.py --fps 45                    # deterministic 45 steps a second
    python port/tools/replay_check.py --fps 144                   # 144 (0.42 fields a step)
    python port/tools/replay_check.py --paced --fps 144 --slow 9,4 # wall clock, uneven frames

--paced runs the delta-time clock on wall time (as players run it), headless; --slow MS,JITTER
makes each frame last MS +- JITTER ms (DHD_SLOW_FRAME, finding 139), so the steps vary. Output:
build/replay-check/<name>.csv (the trace) and .log. Game data stays under build/."""
import argparse
import collections
import csv
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock  # noqa: E402
import owned_process  # noqa: E402
from state_trace_run import DEFAULT_ISO, DEFAULT_SEED, RACE_NAV, RACE_PAD, WINDOWS_EXE  # noqa: E402


def compare(path):
    """Positions by (sample, rider) in the race (pass 0) and the replay (pass 1)."""
    race, replay = {}, {}
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            key = (int(row['sample']), int(row['rider']))
            pos = (float(row['x']), float(row['y']), float(row['z']))
            (replay if row['pass'] == '1' else race)[key] = pos
    worst = collections.defaultdict(float)
    common = 0
    for key, pos in replay.items():
        if key in race:
            common += 1
            d = max(abs(a - b) for a, b in zip(pos, race[key]))
            worst[key[1]] = max(worst[key[1]], d)
    samples = len({k[0] for k in replay})
    return samples, common, dict(worst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fps', type=int, default=60)
    ap.add_argument('--paced', action='store_true', help='wall-clock steps (default: the frame clock)')
    ap.add_argument('--slow', help='DHD_SLOW_FRAME MS,JITTER (with --paced)')
    ap.add_argument('--seconds', type=int, default=60, help='race seconds recorded before the replay starts')
    ap.add_argument('--name', help='output name (default from the options)')
    ap.add_argument('--exe', default=str(ROOT / WINDOWS_EXE))
    ap.add_argument('--iso', default=os.environ.get('DHD_ISO', DEFAULT_ISO))
    ap.add_argument('--timeout', type=int, default=3600)
    args = ap.parse_args()
    name = args.name or ('paced' if args.paced else 'frames') + f'-{args.fps}' + (f'-slow{args.slow}' if args.slow else '')
    out = ROOT / 'build' / 'replay-check'
    out.mkdir(parents=True, exist_ok=True)
    trace, log_path = out / (name + '.csv'), out / (name + '.log')
    if trace.exists():
        trace.unlink()
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    env.update(DHD_ISO=args.iso, DHD_HEADLESS='1', DHD_SKIP_MOVIES='1', DHD_MEMCARD='0', DHD_SND_OFF='1',
               DHD_RENDER='native', DHD_SETTINGS='none', DHD_FPS=str(args.fps), DHD_NAV=RACE_NAV,
               DHD_RACE_SYNC='1', DHD_RACE_PAD=RACE_PAD, DHD_RACE_SEED=str(DEFAULT_SEED),
               DHD_REPLAY_TEST=str(args.seconds), DHD_REPLAY_TRACE=str(trace),
               SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='0')
    if not args.paced:
        env['DHD_FRAME_CLOCK'] = '1'
        if args.fps == 60:
            env['DHD_STEP_CLOCK'] = '1'  # 60 on the delta-time paths (the original's has its own replay)
    if args.slow:
        env['DHD_SLOW_FRAME'] = args.slow
    print(f'{name}: race {args.seconds} s, then its replay; log {log_path}')
    start = time.monotonic()
    result = None
    with game_lock.held('replay_check ' + name), open(log_path, 'wb') as log:
        with owned_process.start([args.exe], cwd=str(out), env=env, stdout=log, stderr=log) as game:
            while time.monotonic() - start < args.timeout and game.poll() is None:
                time.sleep(2)
                text = log_path.read_bytes()
                if b'replay: the end of the recording' in text or b'replay: stopped' in text:
                    time.sleep(1)  # the trace's last rows
                    result = 'end' if b'replay: the end of the recording' in text else 'stopped'
                    break
    for line in log_path.read_bytes().decode(errors='replace').splitlines():
        if line.startswith(('replay', 'fps:')):
            print('  ' + line)
    if not result:
        print(f'no replay end in {time.monotonic() - start:.0f} s (see {log_path})')
        return 1
    samples, common, worst = compare(trace)
    print(f'{result}: {samples} samples replayed, {common} rider positions compared, in {time.monotonic() - start:.0f} s')
    for rider in sorted(worst):
        print(f'  rider {rider}: largest difference {worst[rider]:.9g}')
    exact = result == 'end' and common and not any(worst.values())
    print('PASS: the replay is the race' if exact else 'FAIL')
    return 0 if exact else 1


if __name__ == '__main__':
    sys.exit(main())
