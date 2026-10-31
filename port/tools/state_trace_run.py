"""Run one build headless with the deterministic recipe and write its per-field state trace
(port/hle/state_trace.c; compare with port/tools/state_diff.py).

    python port/tools/state_trace_run.py boot --name boot-a            # menus, 600 fields
    python port/tools/state_trace_run.py race --name race-a            # first track, 5000 fields
    python port/tools/state_trace_run.py race --name race-b --seed 81258   (positive control)
    python port/tools/state_trace_run.py race --name race-c --race-pad "1:5:cross"  (another pad)
    python port/tools/state_trace_run.py race3 --name race3-a          # another track, see SCENARIOS
    python port/tools/state_trace_run.py split --name split-a          # 2-player split-screen race
    python port/tools/state_trace_run.py memcard --name memcard-a      # save, then load, on a card

Output: build/state-trace/<name>.tsv (and <name>-f<field>-*.bin with --dump). Game data: stays
under build/. The game runs headless (DHD_HEADLESS=1, no window) and ends itself at the last
field (DHD_STATE_STOP); if it hasn't ended after --timeout seconds it is stopped, with whatever it
started (owned_process), and the run fails.

The recipe (build/audit/h4-state-trace.md, section 7): the virtual clock (DHD_FRAME_CLOCK=1),
no movies, no memory card, no sound device, native renderer, default settings only
(DHD_SETTINGS=none), a pinned race seed, and field- or race-clock-keyed pad scripts. The race
scenario needs DHD_RACE_SYNC=1 for its race-clock pad (the same route bench_race.py uses); that starts a 30 s heartbeat thread that only reads and prints."""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import owned_process  # noqa: E402

# the test tools' disc image: DHD_TEST_ISO (the game itself never reads it; DHD_ISO would override
# dhd.ini in every game started from that environment, the player's own included)
DEFAULT_ISO = os.environ.get('DHD_TEST_ISO', '')
WINDOWS_EXE = 'build/host64-exe/dhd64.exe'  # build_host64.py's default output
LINUX_EXE = 'build/host64-linux/dhd64'
LINUX_ISO = os.environ.get('DHD_LINUX_ISO', '')  # the disc image as the Linux side sees it
# The title press, then the main menu: the standard boot route (race_compare.py's NAV, first step).
BOOT_NAV = '0:triangle|cross'
# race_compare.py's NAV (menu route to the race load), with the same screens; see its comments.
RACE_NAV = ('0:triangle|cross,15:cross,3:cross,16:cross,1:cross,11:cross,'
            '18@4C7F38=0:start,18@4C7F38=1:cross,9/5:cross')
# Race-clock pad (60 per second after GO): accelerate (Cross) held in 5 s stretches, left and
# right steering taps alternating, and two trick buttons (Circle, Square) tapped in each stretch.
# The trick buttons' effect is not checked; they only add input.
RACE_PAD = ','.join(
    f'{2 + 5 * k}:5:cross,{3 + 5 * k}:1.5:{"left" if k % 2 else "right"},'
    f'{4 + 5 * k}:0.4:circle,{5 + 5 * k}:0.4:square' for k in range(12))
DEFAULT_FIELDS = {'boot': 600, 'race': 5000}
DEFAULT_SEED = 81257

# Scenarios beyond boot and race. Their menu routes are DHD_NAV scripts (menu_nav.h), the same
# steps as race_compare.py and the former visual_sweep.py (finding 063).
# On a fresh save the front-end track selector lists six tracks, in this order (EE track IDs;
# visual_sweep: Italy FR/MX/TD, Utah FR/MX/TD). The other tracks stay locked without the
# game's unlock code, which also turns on other cheats, so they have no scenario here.
UNLOCKED_TRACKS = (0, 6, 24, 1, 7, 13)
# A race is pedalled from the countdown to the finish: Cross held for 1200 race-clock seconds,
# no steering (the pad of visual_sweep's alpine-finish, which finishes track 0).
FULL_PEDAL = '2:1200:cross'
# BOOT's title press, then the 2-player route of visual_sweep's split-screen: multiplayer
# (menu 10), two players (menu 16/24), the combined rider page (34), then the race load (9/5).
SPLIT_NAV = ('0:triangle|cross,15:down,15:cross,10:cross,16:cross,24:cross,11:cross,'
             '34+240:cross,9/5:cross')
# Options (menu 2): the SAVE row is 5 downs in, LOAD 4 (visual_sweep's save and load routes).
OPTIONS_NAV = '0:triangle|cross,15:down,15:down,15:cross'


def track_nav(index):
    """RACE_NAV with the track selector moved down `index` unlocked rows first."""
    prefix, suffix = RACE_NAV.split('18@4C7F38=1:cross', 1)
    return prefix + '18@4C7F38=1+240:down,' * index + '18@4C7F38=1+240:cross' + suffix


# name -> (DHD_NAV script, extra env, default fields, what it covers).
SCENARIOS = {
    # Races: GO is at field ~3650; at 14000 fields the rider is still racing (race clock ~172 s),
    # so the default does not reach the finish.
    **{f'race{i + 1}': (track_nav(i), {'DHD_RACE_PAD': FULL_PEDAL}, 14000,
                       f'track ID {UNLOCKED_TRACKS[i]} (selector row {i}), full-pedal race')
       for i in range(1, len(UNLOCKED_TRACKS))},
    'split': (SPLIT_NAV, {'DHD_PADS': '2', 'DHD_RACE_PAD': FULL_PEDAL,
                          'DHD_RACE_PAD1': FULL_PEDAL}, 14000, '2-player split-screen race, full pedal'),
    # save (the cursor returns to its row, SAVE), then up one row to LOAD, and the confirmations
    'memcard': (OPTIONS_NAV + ',2:down,2:down,2:down,2:down,2:down,2:cross,2:up,2:cross,13:cross',
                {'DHD_MEMCARD': '1'}, 3000, 'save game data to an empty card, then load it'),
}


def build_env(args, out_prefix, sha, fields):
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    env.update(DHD_ISO=args.iso, DHD_HEADLESS='1', DHD_FRAME_CLOCK='1', DHD_SKIP_MOVIES='1',
               DHD_MEMCARD='0', DHD_SND_OFF='1', DHD_RENDER='native', DHD_EXACT_PROBE='1', DHD_SETTINGS='none',
               DHD_STATE_TRACE=str(out_prefix), DHD_STATE_STOP=str(fields), DHD_STATE_EXE=sha)
    if args.scenario == 'boot':
        env['DHD_NAV'] = BOOT_NAV
    elif args.scenario == 'race':
        env.update(DHD_NAV=RACE_NAV, DHD_RACE_SYNC='1', DHD_RACE_PAD=args.race_pad or RACE_PAD,
                   DHD_RACE_SEED=str(args.seed))
    else:
        nav, extra, _, _ = SCENARIOS[args.scenario]
        env.update(DHD_NAV=nav, **extra)
        if 'DHD_RACE_PAD' in extra:  # a race: the race-clock pad, pinned seed (as race above)
            env.update(DHD_RACE_SYNC='1', DHD_RACE_SEED=str(args.seed))
            if args.race_pad:
                env['DHD_RACE_PAD'] = args.race_pad
        if extra.get('DHD_MEMCARD') == '1':  # a card in a folder of this run's own (main empties it)
            env['DHD_MEMCARD_DIR'] = str(out_prefix) + '-card'
    if args.mask:
        env['DHD_STATE_MASK'] = str(Path(args.mask).resolve())
    if args.codeptrs:
        env['DHD_STATE_CODEPTRS'] = str(Path(args.codeptrs).resolve())
    if args.dump:
        env['DHD_STATE_DUMP'] = args.dump
    if args.env:
        for item in args.env:
            key, _, value = item.partition('=')
            env[key] = value
    return {k: v for k, v in env.items() if v != ''}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('scenario', choices=('boot', 'race') + tuple(SCENARIOS))
    ap.add_argument('--name', required=True, help='trace name (build/state-trace/NAME.tsv)')
    ap.add_argument('--exe', default=str(ROOT / WINDOWS_EXE))
    ap.add_argument('--iso', default=os.environ.get('DHD_ISO', DEFAULT_ISO))
    ap.add_argument('--fields', type=int, help='fields to run (default: boot 600, race 5000)')
    ap.add_argument('--seed', type=int, default=DEFAULT_SEED, help='DHD_RACE_SEED (race)')
    ap.add_argument('--race-pad', help='DHD_RACE_PAD script (race), race-clock seconds after GO')
    ap.add_argument('--mask', default=str(Path(__file__).resolve().parent / 'state_trace_mask.txt'),
                    help='DHD_STATE_MASK file: EE word addresses to zero (default: state_trace_mask.txt)')
    ap.add_argument('--codeptrs', default=str(Path(__file__).resolve().parent / 'state_trace_codeptrs.txt'),
                    help='DHD_STATE_CODEPTRS file: EE words that hold code addresses, hashed by identity '
                         '(default: state_trace_codeptrs.txt; "" for none)')
    ap.add_argument('--dump', help='DHD_STATE_DUMP fields, e.g. 300,4000')
    ap.add_argument('--env', action='append', default=[], help='extra NAME=VALUE for the game')
    ap.add_argument('--timeout', type=int, default=3600, help='seconds before the run is stopped')
    ap.add_argument('--linux', action='store_true',
                    help='Linux headless run: default --exe to build/host64-linux/dhd64 and --iso to the '
                         'WSL disc path (the recipe itself does not change)')
    args = ap.parse_args()
    if args.linux:
        if args.exe == str(ROOT / WINDOWS_EXE):
            args.exe = str(ROOT / LINUX_EXE)
        if args.iso == os.environ.get('DHD_ISO', DEFAULT_ISO):
            args.iso = LINUX_ISO

    exe = Path(args.exe).resolve()
    if not exe.exists():
        ap.error(f'no executable at {exe}')
    fields = args.fields or DEFAULT_FIELDS.get(args.scenario) or SCENARIOS[args.scenario][2]
    out_dir = ROOT / 'build' / 'state-trace'
    out_dir.mkdir(parents=True, exist_ok=True)
    out_prefix = (out_dir / args.name).resolve()
    tsv = Path(str(out_prefix) + '.tsv')
    for stale in [tsv] + list(out_dir.glob(args.name + '-f*')):
        if stale.exists():
            stale.unlink()
    card = Path(str(out_prefix) + '-card')  # a run starts from an empty memory card
    if args.scenario in SCENARIOS and SCENARIOS[args.scenario][1].get('DHD_MEMCARD') == '1' and card.exists():
        shutil.rmtree(card)
    sha = hashlib.sha256(exe.read_bytes()).hexdigest()
    env = build_env(args, out_prefix, sha, fields)
    log_path = out_dir / (args.name + '.log')
    print(f'{args.scenario}: {fields} fields, exe {exe} (sha256 {sha[:16]}...), log {log_path}')
    start = time.monotonic()
    with open(log_path, 'wb') as log:
        process = owned_process.run([str(exe)], timeout=args.timeout, cwd=str(out_dir), env=env, stdout=log,
                                    stderr=subprocess.STDOUT)
    status = 'timeout' if process.timed_out else 'ok'
    rows = 0
    if tsv.exists():
        rows = max(0, sum(1 for line in open(tsv, 'rb') if line[:1].isdigit()))
    masked = 0
    for line in open(log_path, 'rb'):
        if b'state-trace:' in line and b'masked' in line:
            masked = int(line.split(b' EE words')[0].split(b':')[-1])
    print(f'{status}: {rows} rows written in {time.monotonic() - start:.0f} s; exit '
          f'{process.returncode}; masked words {masked}; trace {tsv}')
    if status != 'ok' or rows != fields:
        print(f'run incomplete: expected {fields} rows, got {rows} (see {log_path})')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
