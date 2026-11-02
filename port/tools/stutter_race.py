"""Stutter run: a race the game drives itself, in front on the real desktop, with a player's dhd.ini,
recorded by the game's pacing log and DHD_SPIKES lines and by PresentMon (how Windows showed each
picture: composed by DWM or flipped straight to the screen). PresentMon.exe is found on PATH or
at DHD_PRESENTMON.

    python port/tools/stutter_race.py --exe GAME/dhd.exe --ini GAME/dhd.ini --out OUT --seconds 180
        [--env VAR=VALUE ...] [--menu]

race_compare.py's screen-keyed route reaches the race; then Cross is held (the rider pedals the
whole way; steering does not matter for presentation). --menu stays in the main menu instead
(enough to see the present mode). After --seconds the window is closed (WM_CLOSE) so the logs
flush. The window is kept in front every half second: a game behind another window is composed
by DWM whatever it does. Writes OUT/dhd.log, OUT/pacing-*.csv, OUT/presentmon.csv and prints
the summary (stutter_report.py).
"""
from pathlib import Path
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock
import owned_process
import race_compare as race

PRESENTMON = os.environ.get('DHD_PRESENTMON') or 'PresentMon.exe'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', type=Path, required=True)
    ap.add_argument('--ini', type=Path, required=True, help="the player's dhd.ini (copied; the pacing log goes to OUT)")
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--seconds', type=float, default=180, help='seconds of running before the window is closed')
    ap.add_argument('--env', action='append', default=[], help='VAR=VALUE for the game')
    ap.add_argument('--menu', action='store_true', help='stay in the main menu')
    args = ap.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    # The player's settings, with the logs pointed here.
    lines = [l for l in args.ini.read_text(encoding='utf-8', errors='replace').splitlines()
             if not l.strip().upper().startswith(('DHD_PACING_LOG', 'DHD_SPIKES', 'DHD_PACING_STATS'))]
    lines += [f'DHD_PACING_LOG = {out / "pacing"}', 'DHD_SPIKES = 1', 'DHD_PACING_STATS = 1']
    ini = out / 'dhd.ini'
    ini.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    env.update(DHD_SETTINGS=str(ini), DHD_SKIP_MOVIES='1', DHD_MEMCARD='0',
               DHD_NAV='0:triangle|cross' if args.menu else race.NAV)
    if not args.menu:
        env.update(DHD_RACE_SYNC='1', DHD_RACE_PAD='0:600:cross')
    env.update(dict(item.split('=', 1) for item in args.env))
    for old in out.glob('pacing-*.csv'):
        old.unlink()
    # Every process (without elevation PresentMon cannot name a process that started after it;
    # stutter_report.py keeps the game's rows by game.pid). Stopped through its ETW session (close):
    # a killed PresentMon leaves the session running, and a few of those starve new ones.
    session = f'dhd_stutter_{os.getpid()}'

    def stop_session(_):
        subprocess.run(['logman', 'stop', session, '-ets'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    samples = focused = 0
    # The game closes its window (WM_CLOSE, so the logs flush) when it is stopped: after --seconds,
    # or on an error or Ctrl+C here.
    with game_lock.held('stutter_race', timed=True), \
            open(out / 'dhd.log', 'wb') as log, open(out / 'presentmon.out', 'wb') as monitor_log, \
            owned_process.start([str(args.exe)], cwd=args.exe.parent, env=env, stdout=log, stderr=subprocess.STDOUT,
                                close=owned_process.Owned.close_windows, grace=20) as game, \
            owned_process.start([str(PRESENTMON), '--output_file', str(out / 'presentmon.csv'), '--no_console_stats',
                                 '--session_name', session], stdout=monitor_log, stderr=subprocess.STDOUT,
                                close=stop_session, grace=30):
        (out / 'game.pid').write_text(str(game.pid))
        start = time.monotonic()
        while game.poll() is None and time.monotonic() - start <= args.seconds:
            state = game.focus()
            if state is not None:
                samples += 1
                focused += state
            time.sleep(0.5)
        game.stop()
    print(f'exit {game.returncode}, in front {focused}/{samples} samples')
    subprocess.run([sys.executable, str(Path(__file__).with_name('stutter_report.py')), str(out)])


if __name__ == '__main__':
    main()
