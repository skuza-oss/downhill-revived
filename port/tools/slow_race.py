"""A slow PC on a fast one: does the game keep real time? (finding 139)

    python port/tools/slow_race.py --exe build/host64-exe/dhd64.exe --out build/slow-race/after \
        --fps 60,120 --slow 22.2,4 15.2,5 11.8,3

For every --fps and every --slow (DHD_SLOW_FRAME=MS,JITTER: each frame lasts at least MS ms, give or
take JITTER, busy on the EE thread), one game on the hidden desktop (hidden_game.py, a timed run:
it has the PC to itself) goes through the menus to a race (race_compare.py's menu route, seed
81257, Cross held) and rides it paced, as players run it (threaded graphics, scale 1), with the
sound device open at zero gain (DHD_SND_MUTE=1) so its queue is measured in silence. It reads
dhd.log's "clock:" lines (fps.c: the race clock against wall time, its slowest and fastest second,
the steps, the sound rendered against wall time; one per 30 s of race), the sound device's
per-second "snd host:" lines (queue, the game speed it measured, time-stretching, underruns) and
dhd.log's perf: race lines (frames shown). Writes OUT/<fps>-<slow>/game.log and OUT/summary.txt.
"""
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import race_compare as rc  # noqa: E402

CLOCK = re.compile(r'clock: race (\d+) s game x([\d.]+) \(seconds x([\d.]+)-x([\d.]+)\), '
                   r'steps ([\d.]+)-([\d.]+) fields, sound x([\d.]+)')
SND = re.compile(r'snd host: queue (\d+)-(\d+) ms, rate ([+-]\d+) ppm, game speed ([\d.]+), tempo ([\d.]+), '
                 r'stretched ([\d.]+) s, underruns (\d+), silence ([\d.]+) ms, dropped (\d+), skipped (\d+)')
PERF = re.compile(r'perf: race 30 s frames=\d+ fps=([\d.]+)/\d+ low1%=([\d.]+) .* max=([\d.]+)ms')


def ride(exe, out, fps, slow, lines, timeout):
    env = {'DHD_FPS': str(fps), 'DHD_SLOW_FRAME': slow, 'DHD_NAV': rc.NAV, 'DHD_RACE_SYNC': '1',
           'DHD_RACE_PAD': '0:900:cross', 'DHD_RACE_SEED': str(rc.RETAIL_RACE_SEED), 'DHD_SCREENSHOT': '<unset>',
           'DHD_SND_OFF': '<unset>', 'DHD_SND_MUTE': '1', 'DHD_SND_HOST_TRACE': '1'}
    g = hidden_game.Game(exe, out, env=env, name='slow-race', timed=True)
    try:
        start = time.time()
        while len(CLOCK.findall(g.log())) < lines:
            if not g.alive():
                raise RuntimeError(f'the game exited (see {out / "game.log"})')
            if time.time() - start > timeout:
                raise TimeoutError(f'{lines} clock: lines not reached in {timeout} s')
            time.sleep(2)
    finally:
        g.stop()
    return g.log()


def summarize(log, skip_snd):
    clock = [tuple(float(x) for x in m) for m in CLOCK.findall(log)]
    snd = [m for m in SND.findall(log)]
    race_snd = snd[-int(sum(c[0] for c in clock)):] if clock else []  # the device lines of the race
    race_snd = race_snd[skip_snd:]
    wall = sum(c[0] for c in clock)
    game = sum(c[0] * c[1] for c in clock) / wall
    sound = sum(c[0] * c[6] for c in clock) / wall
    slowest, fastest = min(c[2] for c in clock), max(c[3] for c in clock)
    least, most = min(c[4] for c in clock), max(c[5] for c in clock)
    underruns = int(race_snd[-1][6]) - int(race_snd[0][6]) if len(race_snd) > 1 else 0
    stretched = float(race_snd[-1][5]) - float(race_snd[0][5]) if len(race_snd) > 1 else 0
    skipped = int(race_snd[-1][9]) - int(race_snd[0][9]) if len(race_snd) > 1 else 0
    queue = (min(int(s[0]) for s in race_snd), max(int(s[1]) for s in race_snd)) if race_snd else (0, 0)
    perf = [tuple(float(x) for x in m) for m in PERF.findall(log)]
    shown = (f'fps {sum(p[0] for p in perf) / len(perf):.1f} (low1% {min(p[1] for p in perf):.1f}, '
             f'longest {max(p[2] for p in perf):.0f} ms), ') if perf else ''
    return (shown + f'game x{game:.3f} (seconds x{slowest:.3f}-x{fastest:.3f}), steps {least:.2f}-{most:.2f} fields, '
            f'sound x{sound:.3f}; device: queue {queue[0]}-{queue[1]} ms, underruns {underruns}, '
            f'stretched {stretched:.1f} s, skipped {skipped / 48:.0f} ms over {wall:.0f} s of race')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--fps', default='60,120')
    ap.add_argument('--slow', nargs='+', default=['22.2,4', '15.2,5', '11.8,3'])
    ap.add_argument('--lines', type=int, default=2, help='clock: lines (30 s of race each) per run')
    ap.add_argument('--timeout', type=int, default=600)
    args = ap.parse_args()
    report = []
    for fps in args.fps.split(','):
        for slow in args.slow:
            folder = args.out / f'{fps}-{slow.replace(",", "_")}'
            log_file = folder / 'game.log'
            if not (log_file.exists() and len(CLOCK.findall(log_file.read_text(errors='replace'))) >= args.lines):
                print(f'FPS={fps} DHD_SLOW_FRAME={slow}: riding', flush=True)
                ride(args.exe.resolve(), folder, fps, slow, args.lines, args.timeout)
            line = f'FPS={fps:>3} slow {slow:>7}: ' + summarize(log_file.read_text(errors='replace'), 2)
            print(line, flush=True)
            report.append(line)
    (args.out / 'summary.txt').write_text('\n'.join(report) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
