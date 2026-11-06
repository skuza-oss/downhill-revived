"""One game through finding 156's route on a hidden desktop, with the timeline, optionally sampled at
its four scenes (Windows). The method of findings 156 and 162 in one command:

    python port/tools/route_profile.py EXE OUT [--scale 4] [--profile] [--env NAME=VALUE ...]

The route is race_compare.py's (screen-keyed navigation, its race pad, seed 81257, DHD_RACE_SYNC) with
DHD_FPS=240 DHD_VSYNC=0 DHD_FRAME_CLOCK=1, no movies, memory card, sound or settings file,
DHD_BENCH=300-14000 and DHD_TIMELINE=OUT/timeline.csv. With the frame clock every run draws the same
fields, so the scenes are fixed field windows:

    title 450-1100 (the attract flyover behind the title), shop 2950-3750 (the bike shop),
    start 6400-7300 (GO is field 6321), mid 9000-14000

OUT/scenes.json gets, per scene from the timeline: fields/s, and per field the CPU work of the game,
vu, gs and render threads (CPU minus measured spin), their busy time, the game thread's spin and its
waits, the GPU ms a frame. --profile also runs sample_profile.py --stacks game for a few seconds in
each scene (OUT/<scene>.json; the sampler slows the game, so its shares are scaled by a run without
it), and records which fields each sample window covered. --env adds variables (diagnostics such as
DHD_VU1_PROFILE, DHD_VU1_LAYOUT); a timing run has none. The game takes the PC's timed game lock.
"""
import argparse
import csv
import json
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bench_race  # noqa: E402
import hidden_game  # noqa: E402

SCENES = {'title': (450, 1100), 'shop': (2950, 3750), 'start': (6400, 7300), 'mid': (9000, 14000)}
# Sampling: (scene, log marker, seconds after it, seconds to sample)
SAMPLES = (('title', r'nav step 2 field', 0.4, 2.5), ('shop', r'nav step 6 field', 0.5, 3.0),
           ('start', r'race anchor=g', 0.4, 3.0), ('mid', r'race anchor=g', 7.0, 6.0))


def route_env(out, scale, width, height):
    env = dict(bench_race.SCENES['race'][0])
    env.update(DHD_FPS='240', DHD_VSYNC='0', DHD_FRAME_CLOCK='1', DHD_BENCH='300-14000', DHD_BENCH_EXIT='1',
               DHD_TIMELINE=str(Path(out).resolve() / 'timeline.csv'), DHD_SCALE=str(scale),
               DHD_WINDOW='windowed', DHD_WINDOW_WIDTH=str(width), DHD_WINDOW_HEIGHT=str(height),
               DHD_SCREENSHOT='<unset>', DHD_CAPTURE_AT='<unset>', DHD_CAPTURE_INTERVAL='<unset>',
               DHD_SCREENSHOT_WINDOW='<unset>')
    return env


def summarize(out, log_text):
    """Per scene from the timeline CSV (finding 156's columns)."""
    rows = list(csv.DictReader(open(Path(out) / 'timeline.csv')))
    result = {}
    for scene, (lo, hi) in SCENES.items():
        sel = [r for r in rows if lo <= int(r['field']) <= hi]
        if not sel:
            continue
        n = len(sel)
        s = lambda k: sum(float(r[k]) for r in sel)
        wall = s('wall_ms')
        frames = s('gpu_frames')
        result[scene] = dict(
            fields=n, fps=round(1000 * n / wall, 1), field_ms=round(wall / n, 3),
            game_work=round((s('ee_cpu_ms') - s('ee_spin')) / n, 3),
            game_busy=round((wall - s('ee_wait_vu') - s('ee_wait_gs') - s('ee_ring_full')) / n, 3),
            game_spin=round(s('ee_spin') / n, 3), game_wait_vu=round(s('ee_wait_vu') / n, 3),
            game_wait_gs=round(s('ee_wait_gs') / n, 3),
            vu_work=round((s('vu_cpu_ms') - s('vu_spin')) / n, 3),
            vu_busy=round((wall - s('vu_idle') - s('vu_gs_full')) / n, 3),
            gs_work=round((s('gs_cpu_ms') - s('gs_spin')) / n, 3), gs_busy=round((wall - s('gs_idle')) / n, 3),
            render_cpu=round(s('native_cpu_ms') / n, 3),
            render_busy=round((wall - s('native_wait') - s('present_wait')) / n, 3),
            present=round(s('present_wait') / n, 3),
            gpu_ms=round(s('gpu_span_ms') / frames, 3) if frames else None)
    return result


def field_at(log_text, out, utc):
    """The field the game was drawing at a wall-clock time (timeline rows and the clock line)."""
    m = re.search(r'bench timeline: clock unix_s=([\d.]+) host_s=([\d.]+)', log_text)
    f = re.search(r'bench timeline: clock field=(\d+) host_s=([\d.]+)', log_text)
    if not m or not f:
        return None
    host = float(m.group(2)) + (utc - float(m.group(1)))
    t = float(f.group(2)); field = int(f.group(1))
    for r in csv.DictReader(open(Path(out) / 'timeline.csv')):
        t += float(r['wall_ms']) / 1000
        field = int(r['field'])
        if t >= host:
            return field
    return field


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('exe', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--size', default='1920x1080', help='window size')
    ap.add_argument('--profile', action='store_true', help='sample the game thread (stacks) in each scene')
    ap.add_argument('--interval', type=float, default=1.0, help='sampler ms between ticks')
    ap.add_argument('--env', action='append', default=[], help='NAME=VALUE, added to the route')
    ap.add_argument('--timeout', type=float, default=900)
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    width, height = map(int, a.size.split('x'))
    env = route_env(a.out, a.scale, width, height)
    for item in a.env:
        k, _, v = item.partition('='); env[k] = v
    game = hidden_game.Game(a.exe.resolve(), a.out, env=env, name='route_profile', scale=a.scale,
                            shot_window=None, timed=True)
    windows = {}
    try:
        start = time.time()
        pending = list(SAMPLES) if a.profile else []
        seen = {}
        while game.alive():
            if time.time() - start > a.timeout:
                raise RuntimeError('timed out; see %s' % (a.out / 'game.log'))
            text = game.log() if pending else ''
            for item in list(pending):
                scene, marker, after, seconds = item
                if marker not in seen and re.search(marker, text):
                    seen[marker] = time.time()
                if marker in seen and time.time() >= seen[marker] + after:
                    pending.remove(item)
                    report = a.out / ('%s.json' % scene)
                    subprocess.run([sys.executable, str(Path(__file__).resolve().parent / 'sample_profile.py'),
                                    '--pid', str(game.pid), '--seconds', str(seconds), '--interval', str(a.interval),
                                    '--exe', str(a.exe.resolve()), '--stacks', 'game', '--out', str(report),
                                    '--top', '25'], stdout=open(a.out / ('%s.txt' % scene), 'w'),
                                   stderr=subprocess.STDOUT, check=False)
                    try:
                        windows[scene] = json.loads(report.read_text()).get('utc')
                    except (OSError, ValueError):
                        windows[scene] = None
            time.sleep(0.05)
    finally:
        game.stop()
    log_text = game.log()
    if 'bench timeline: fields' not in log_text:
        raise SystemExit('no timeline report; see %s' % (a.out / 'game.log'))
    result = {'scenes': summarize(a.out, log_text)}
    if windows:
        result['sampled_fields'] = {s: [field_at(log_text, a.out, u[0]), field_at(log_text, a.out, u[1])]
                                    for s, u in windows.items() if u}
    (a.out / 'scenes.json').write_text(json.dumps(result, indent=1))
    print(json.dumps(result, indent=1))


if __name__ == '__main__':
    main()
