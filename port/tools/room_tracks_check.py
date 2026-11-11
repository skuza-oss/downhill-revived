"""Every track an online room offers loads and races (finding 203): for each, the state trace's
race recipe (state_trace_run.py race: headless, the virtual clock, untimed game lock) with
DHD_RACE_SETUP=<track>, which writes the track into the setup block on the load screen as the room
does online, in the mode online races use (Single Event). The run must reach the last field with
the race running (phase 3, the race clock moving) and no guest memory fault. An online race loads
its track by the same code (the loading bar, the level, the riders), so a track whose load breaks
breaks here first, with no server and no other player: issue #84's MOSH PIT crash did.

    python port/tools/room_tracks_check.py                  # the room's 25 tracks (all but 21, DUAL)
    python port/tools/room_tracks_check.py --tracks 23,17   # some of them
    python port/tools/room_tracks_check.py --exe OTHER.exe  # another build

About two minutes a track (4000 fields at about 30 a second). Traces and logs go to build/state-trace/room-track-NN.*."""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = Path(__file__).resolve().parent / 'state_trace_run.py'
# The room's tracks (port/hle/online_menu.c track_at): every game track but 21, the career's Dual
# Slalom, which has no Single Event entry (finding 166).
ROOM_TRACKS = [t for t in range(26) if t != 21]


def check(track, exe, fields, timeout):
    name = f'room-track-{track:02d}'
    cmd = [sys.executable, str(TOOL), 'race', '--name', name, '--fields', str(fields), '--timeout', str(timeout),
           '--env', f'DHD_RACE_SETUP={track}']
    if exe:
        cmd += ['--exe', str(exe)]
    run = subprocess.run(cmd, capture_output=True, text=True)
    out_dir = ROOT / 'build' / 'state-trace'
    log = (out_dir / f'{name}.log').read_text(errors='replace') if (out_dir / f'{name}.log').exists() else ''
    if 'host64 memory' in log:
        return 'guest memory fault: ' + next(l for l in log.splitlines() if 'host64 memory' in l)
    if f'track {track} (' not in log:
        return 'the setup never took the track (no "race setup" line)'
    if run.returncode:
        return 'run incomplete: ' + run.stdout.strip().splitlines()[-1]
    rows = [line.split('\t') for line in (out_dir / f'{name}.tsv').read_text().splitlines() if line[:1].isdigit()]
    last, before = rows[-1], rows[-61]
    if last[2] != '3' or int(last[3]) <= int(before[3]):
        return f'not racing at the last field (phase {last[2]}, race clock {before[3]} -> {last[3]})'
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tracks', help='comma-separated game track ids (default: the room\'s 25)')
    ap.add_argument('--exe', type=Path, help='the game (default: state_trace_run.py\'s, this checkout\'s build)')
    ap.add_argument('--fields', type=int, default=4000, help='fields per run (GO comes near field 1700-3700, by the level)')
    ap.add_argument('--timeout', type=int, default=900, help='seconds per run')
    a = ap.parse_args()
    tracks = [int(t) for t in a.tracks.split(',')] if a.tracks else ROOM_TRACKS
    failed = 0
    for track in tracks:
        problem = check(track, a.exe, a.fields, a.timeout)
        print(f'track {track:2d}: {"FAIL: " + problem if problem else "ok"}', flush=True)
        failed += problem is not None
    print(f'{len(tracks) - failed} of {len(tracks)} tracks load and race' + (f'; {failed} FAILED' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
