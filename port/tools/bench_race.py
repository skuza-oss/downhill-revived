"""Fair benchmark: a fixed, deterministic menu or race route, no diagnostics, one game at a time.

Every candidate runs race_compare.py's route (screen-keyed navigation, its race pad and seed,
DHD_RACE_SYNC) with default settings, no movies, memory card or sound device. The port times the
field window DHD_BENCH (port/gs/present.c) and exits at its last field. Runs are interleaved
(A B C, A B C, ...) so drift on the machine hits every candidate alike.

    python port/tools/bench_race.py --candidate base=build/a/dhd64.exe         --candidate interp=build/b/dhd64.exe:DHD_INTERP=1 --scene race --scene menu --mode paced

A candidate is LABEL=EXE[:VAR=VALUE[,VAR=VALUE...]]. Scenes: race (fields 6000-7000) and menu
(400-1000); modes: flat (default, DHD_FRAME_CLOCK=1: throughput) and paced (the game's 60 fields/s).
The window is kept in front (Windows slows a background game) and its focused share reported;
--minimized never takes focus, --headless has no window. --sound plays audio and reports its gaps.
Reports medians of fields/s, frame times, thread CPU, displayed rate (DHD_INTERP) and audio
counters; writes OUT/summary.json and one log per run. Game image: DHD_ISO, else state_trace_run's.
"""
from pathlib import Path
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_pcsx2 as compare
import game_lock
import owned_process
import race_compare as race
from state_trace_run import DEFAULT_ISO

LINE = re.compile(rb'bench: fields=(\d+)-(\d+) fields_per_s=([\d.]+) p50_ms=([\d.]+) p99_ms=([\d.]+) '
                  rb'worst1pct_ms=([\d.]+) max_ms=([\d.]+)')
# The pictures shown over the same window (with DHD_INTERP more pictures than fields): displayed
# rate and time between presents.
SHOWN = re.compile(rb'bench interp: displayed_fps=([\d.]+) present_ms p50=([\d.]+) p99=([\d.]+) '
                   rb'worst1pct=([\d.]+) max=([\d.]+)')
# Audio output over the window (--sound; port/host/sound_out_sdl.c): gaps, silence written in
# them, queue.
SOUND = re.compile(rb'bench snd: underruns=(\d+) silence_ms=([\d.]+) dropped_ms=([\d.]+) skipped_ms=([\d.]+) '
                   rb'played_s=([\d.]+) queue_ms p1=(\d+) p50=(\d+) p99=(\d+) rate_ppm=([-+\d.]+) '
                   rb'tick_ms p50=([\d.]+) p99=([\d.]+) max=([\d.]+)')
# CPU time of each named port thread over the window (port/gs/present.c), ms per field.
THREADS = re.compile(rb'bench threads: cpu_ms_per_field((?: \w+=[\d.]+)+) process=([\d.]+) busy_cores=([\d.]+)')
SOUND_KEYS = ('snd_underruns', 'snd_silence_ms', 'snd_dropped_ms', 'snd_skipped_ms', 'snd_played_s', 'snd_queue_p1_ms',
              'snd_queue_p50_ms', 'snd_queue_p99_ms', 'snd_rate_ppm', 'tick_p50_ms', 'tick_p99_ms', 'tick_max_ms')
SHOWN_KEYS = ('displayed_fps', 'present_p50_ms', 'present_p99_ms', 'present_worst1pct_ms', 'present_max_ms')
# Scene: (environment, default field window). The main menu is reached at field ~184.
SCENES = {'race': (dict(DHD_AUTOPAD_FRAMES=compare.frame_script(compare.parse_pad(race.NAVIGATION)),
                        DHD_NAV=race.NAV, DHD_RACE_SYNC='1', DHD_RACE_PAD=race.RACE_PAD, DHD_RACE_SEED='81257'),
                   '6000-7000'),
          'menu': (dict(DHD_NAV='0:triangle|cross'), '400-1000')}
def parse_candidate(text):
    match = re.fullmatch(r'([^=]+)=(.+?)(?::([A-Za-z_]\w*=.*))?', text)
    if not match:
        raise SystemExit(f'bad candidate {text!r}')
    label, exe, env = match.groups()
    extra = dict(item.split('=', 1) for item in env.split(',')) if env else {}
    return label, Path(exe).resolve(), extra


def run_once(exe, extra, scene, mode, args, log_path):
    scene_env, default_window = SCENES[scene]
    env = {k: v for k, v in os.environ.items() if not k.startswith(('DHD_', 'ROUND_'))}
    env.update(DHD_ISO=os.environ.get('DHD_ISO', DEFAULT_ISO), DHD_SKIP_MOVIES='1', DHD_MEMCARD='0', DHD_SND_OFF='1',
               DHD_SETTINGS='none',  # no dhd.ini beside the exe is read (finding 056)
               DHD_BENCH=args.window or default_window, DHD_BENCH_EXIT='1', **scene_env)
    if mode == 'flat':
        env['DHD_FRAME_CLOCK'] = '1'
    if args.sound:  # real audio output, to count its gaps
        del env['DHD_SND_OFF']
    if args.headless:
        env['DHD_HEADLESS'] = '1'
    env.update(extra)
    window = 'none' if args.headless else 'minimized' if args.minimized else 'focus'
    start = time.monotonic()
    samples = focused = 0
    with open(log_path, 'wb') as log, \
            owned_process.start([str(exe)], cwd=exe.parent, env=env, stdout=log, stderr=subprocess.STDOUT,
                                minimized=window == 'minimized',
                                creationflags=getattr(subprocess, 'ABOVE_NORMAL_PRIORITY_CLASS', 0)) as process:
        while process.poll() is None:
            if time.monotonic() - start > args.timeout:
                raise RuntimeError(f'{exe} timed out after {args.timeout} s; see {log_path}')
            if window == 'minimized':
                process.minimize()
            elif window == 'focus':
                state = process.focus()
                if state is not None:
                    samples += 1
                    focused += state
            time.sleep(0.25 if window == 'minimized' else 0.5)
    text = Path(log_path).read_bytes()
    match = LINE.search(text)
    if not match:
        raise RuntimeError(f'{exe} printed no bench line (exit {process.returncode}); see {log_path}')
    fps, p50, p99, worst, peak = (float(match.group(i)) for i in range(3, 8))
    result = dict(scene=scene, mode=mode, window=window, fields_per_s=fps, p50_ms=p50, p99_ms=p99,
                  worst1pct_ms=worst, max_ms=peak, wall_s=round(time.monotonic() - start, 1))
    if window == 'focus':
        result['focused'] = round(focused / samples, 3) if samples else 0.0
    shown = SHOWN.search(text)
    if shown:
        result.update(zip(SHOWN_KEYS, (float(shown.group(i)) for i in range(1, 6))))
    threads = THREADS.search(text)
    if threads:
        cpu = {k.decode(): float(v) for k, v in (item.split(b'=') for item in threads.group(1).split())}
        cpu.update(process=float(threads.group(2)), busy_cores=float(threads.group(3)))
        result['cpu_ms_per_field'] = cpu
    heard = SOUND.search(text)
    if heard:
        result.update(zip(SOUND_KEYS, (float(heard.group(i)) for i in range(1, 13))))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--candidate', action='append', required=True, help='LABEL=EXE[:VAR=VALUE,...]')
    ap.add_argument('--scene', action='append', choices=SCENES, help='race (default), menu')
    ap.add_argument('--mode', action='append', choices=('flat', 'paced'), help='flat (default), paced')
    ap.add_argument('--runs', type=int, default=3)
    ap.add_argument('--window', help='virtual GS fields first-last (default per scene)')
    ap.add_argument('--timeout', type=int, default=1800, help='seconds per run')
    ap.add_argument('--out', type=Path, default=compare.ROOT / 'build/port/bench/latest')
    ap.add_argument('--sound', action='store_true', help='play audio and report its gaps (normally off)')
    shown = ap.add_mutually_exclusive_group()
    shown.add_argument('--minimized', action='store_true', help='keep the window minimised, never take focus')
    shown.add_argument('--headless', action='store_true', help='no window (DHD_HEADLESS=1)')
    args = ap.parse_args()
    candidates = [parse_candidate(c) for c in args.candidate]
    cases = [(scene, mode) for scene in args.scene or ['race'] for mode in args.mode or ['flat']]
    args.out.mkdir(parents=True, exist_ok=True)

    def name(label, scene, mode):  # the summary key: the label alone for a single scene and mode
        return label if len(cases) == 1 else f'{label} {scene} {mode}'
    results = {name(label, *case): [] for case in cases for label, _, _ in candidates}
    with game_lock.held('bench_race', timed=True):  # the PC to itself for every run (game_lock.py)
        run_all(args, candidates, cases, name, results)
    summarize(args, candidates, cases, name, results)


def run_all(args, candidates, cases, name, results):
    for run in range(args.runs):
        for scene, mode in cases:
            for label, exe, extra in candidates:
                key = name(label, scene, mode)
                log = args.out / f'{key.replace(" ", "-")}-{run}.log'
                result = run_once(exe, extra, scene, mode, args, log)
                results[key].append(result)
                print(f'{key} run {run}: {result["fields_per_s"]:.2f} fields/s, p99 {result["p99_ms"]:.2f} ms, '
                      f'worst 1% {result["worst1pct_ms"]:.2f} ms' +
                      (f', focused {result["focused"] * 100:.0f}%' if 'focused' in result else '') +
                      (f', displayed {result["displayed_fps"]:.2f} fps, present p99 {result["present_p99_ms"]:.2f} ms'
                       if 'displayed_fps' in result else '') +
                      (f', audio gaps {result["snd_underruns"]:.0f} ({result["snd_silence_ms"]:.0f} ms silence), '
                       f'queue p1/p50 {result["snd_queue_p1_ms"]:.0f}/{result["snd_queue_p50_ms"]:.0f} ms, '
                       f'game tick p99 {result["tick_p99_ms"]:.1f} max {result["tick_max_ms"]:.1f} ms'
                       if 'snd_underruns' in result else ''), flush=True)


def summarize(args, candidates, cases, name, results):
    summary = {}
    for scene, mode in cases:
        first = None
        for label, exe, extra in candidates:
            key = name(label, scene, mode)
            runs = results[key]
            fps = [r['fields_per_s'] for r in runs]
            s = summary[key] = dict(exe=str(exe), env=extra, scene=scene, mode=mode,
                                    window=args.window or SCENES[scene][1], runs=runs,
                                    median_fields_per_s=statistics.median(fps), spread=max(fps) - min(fps),
                                    median_p99_ms=statistics.median(r['p99_ms'] for r in runs),
                                    median_worst1pct_ms=statistics.median(r['worst1pct_ms'] for r in runs))
            if all('cpu_ms_per_field' in r for r in runs):
                names = sorted(set().union(*(r['cpu_ms_per_field'] for r in runs)))
                s['median_cpu_ms_per_field'] = {
                    n: statistics.median(r['cpu_ms_per_field'].get(n, 0.0) for r in runs) for n in names}
            for keys in (SHOWN_KEYS, SOUND_KEYS):
                if all(keys[0] in r for r in runs):
                    for k in keys:
                        s['median_' + k] = statistics.median(r[k] for r in runs)
            if 'median_snd_underruns' in s:
                s['total_snd_underruns'] = sum(r['snd_underruns'] for r in runs)
            first = first or (label, s['median_fields_per_s'])
            s['gain_vs_first'] = s['median_fields_per_s'] / first[1] - 1
            print(f'{key}: median {s["median_fields_per_s"]:.2f} fields/s (spread {s["spread"]:.2f}), '
                  f'p99 {s["median_p99_ms"]:.2f} ms, worst 1% {s["median_worst1pct_ms"]:.2f} ms, '
                  f'{s["gain_vs_first"] * 100:+.1f}% vs {first[0]}' +
                  (f'; displayed {s["median_displayed_fps"]:.2f} fps, present p50 {s["median_present_p50_ms"]:.2f} / '
                   f'p99 {s["median_present_p99_ms"]:.2f} / worst 1% {s["median_present_worst1pct_ms"]:.2f} ms'
                   if 'median_displayed_fps' in s else '') +
                  (f'; audio gaps {s["total_snd_underruns"]:.0f} in {len(runs)} runs, '
                   f'median silence {s["median_snd_silence_ms"]:.0f} ms, game tick p99 {s["median_tick_p99_ms"]:.1f} ms'
                   if 'total_snd_underruns' in s else ''))
            if 'median_cpu_ms_per_field' in s:
                print('    CPU ms per field: ' + ', '.join('%s %.2f' % kv for kv in s['median_cpu_ms_per_field'].items()))
    (args.out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')


if __name__ == '__main__':
    main()
