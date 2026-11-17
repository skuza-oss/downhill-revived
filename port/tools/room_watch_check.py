"""Watching a running room race (finding 269): three games on this PC in one private SFU room from the
real ONLINE menus, on hidden desktops, a local server per run (its Realtime keys from
DHD_SERVER_ENV_FILE or --env-file: online/server/.dev.vars of a checkout that has one).

    python port/tools/room_watch_check.py OUT --scenario watch|watcher-leaves|racer-leaves|none

The host makes the room and races its first race alone; the guest joins during it and readies, so the
second race is the host's and the guest's. The watcher starts once that race's countdown ran and joins
by code; the room shows it the race (it is outside it) and DHD_NET_TEST_WATCH takes WATCH. Each racer
finishes at a set race second (DHD_NET_TEST_FINISH: host 100, guest 110).

- watch: the watcher presses Right, Right, Left (DHD_NET_TEST_MENU) and watches to the end: its camera
  must change rider at each press and when a watched human finishes, it must be back in the room with
  the race's result, and race the third race (its own countdown, a slot of its own).
- watcher-leaves: the watcher's race menu BACK TO THE ROOM at its race second 12: back in the room,
  the racers race on.
- racer-leaves: the guest's BACK TO THE ROOM at its race second 30: the watcher hears it left.
- none: the same two-racer race without a watcher (the base for the racers' results).

Each run prints the racers' results of the second race; the racers' results with a watcher must equal
those of `none` (their finishes are scripted, so equal means the watcher changed nothing). Every
scenario fails on a refusal, `freeze:` or `crash:` line in any log, and on a log line with a network
address or a session id. Pictures of the watcher's view: OUT/watcher/present-NNN.png every --interval
seconds (its window --watcher-window, its scale --watcher-scale). Untimed game lock (hidden_game). Needs DHD_TEST_ISO and the x64 build."""
import argparse
import contextlib
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import local_server  # noqa: E402
from cross_race import P2P_GUEST_NAV, PRIVATE_HOST_NAV  # noqa: E402
from online_race import iso_files_sha256  # noqa: E402
from online_riders import PEDAL, rider_env  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
ROOM = r'online menu: room (\w{6,7}) \(host\)'
YES = 'down,cross,w300,down,cross'  # a row's question opens on No
REFUSED = re.compile(r'record refused|records refused|HOLD refused|HOLDs refused|not on puppet_record|freeze:|crash:')
# a member's address or ids must never reach another's log (AGENTS.md): IPv4/IPv6 literals, SFU ids
LEAK = re.compile(r'\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}:\d+\b|sessionId|installId|\b[0-9a-f]{32}\b')
LINES = re.compile(r'online: (watching.*|race menu: .*|race closed.*|this PC.s race is over.*|player \d .* left.*|result \d.*|'
                   r'.* finished in .*|race starts.*|camera cut.*)|online menu: (WATCH.*|room \w+: watching.*)')


def wait(read, pattern, seconds, games=()):
    until = time.time() + seconds
    while time.time() < until:
        m = re.search(pattern, read())
        if m:
            return m
        for g in games:
            if not g.alive():
                raise RuntimeError(f'a game exited: see {g.out / "game.log"}')
        time.sleep(2)
    return None


def second_race(log):
    """The log of the host's and guest's race (the first one of two racers)."""
    starts = [m.start() for m in re.finditer(r'online: race starts', log)]
    for i, at in enumerate(starts):
        if log.startswith('online: race starts: player ', at) and re.match(r'online: race starts: player \d of 2,', log[at:]):
            return log[at:starts[i + 1] if i + 1 < len(starts) else len(log)]
    return ''


def results(log):
    race = second_race(log)
    return [re.sub(r'^.*online: ', '', x) for x in re.findall(r'online: (?:result \d.*|.* finished in .*)', race)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--scenario', choices=['watch', 'watcher-leaves', 'racer-leaves', 'none'], default='watch')
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--env-file', default=os.environ.get('DHD_SERVER_ENV_FILE'), help="the server's .dev.vars (Realtime keys)")
    ap.add_argument('--track', default='0', help="the room's first track (DHD_NET_TEST_TRACK)")
    ap.add_argument('--scale', default='1')
    ap.add_argument('--interval', default='0', help="seconds between the watcher's presented pictures (0: none)")
    ap.add_argument('--watcher-window', default='1280x720', help="the watcher's window, WxH (its pictures' size)")
    ap.add_argument('--watcher-scale', help="the watcher's resolution scale (default --scale)")
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    iso_hash = iso_files_sha256(hidden_game.test_iso(), ROOT / 'build' / 'port' / 'iso-check.txt')

    def env(name, extra):
        e = dict(DHD_NET_ISO_SHA256=iso_hash, DHD_NET_LOG_CODE='1', DHD_NET_SCRIPTED='1', DHD_NET_TEST_READY='1',
                 DHD_FPS='60', DHD_MEMCARD='1', DHD_MEMCARD_DIR=str(out / name / 'card'), DHD_SND_OFF='1', **rider_env(PEDAL),
                 DHD_NET_NAME=name.upper(), DHD_WINDOW_WIDTH='1280', DHD_WINDOW_HEIGHT='720', DHD_SCREENSHOT='<unset>')
        e.update(extra)
        (out / name / 'card').mkdir(parents=True, exist_ok=True)
        for f in (out / name).glob('present-*.png'):
            f.unlink()
        return e

    games, failures = [], []
    with contextlib.ExitStack(), local_server.running(env_file=a.env_file) as url:
        try:
            host = hidden_game.Game(a.exe, out / 'host', name='watch-host', scale=a.scale, shot_window=None, env=env('host', dict(
                DHD_NET_SERVER=url, DHD_AUTOPAD_FRAMES=PRIVATE_HOST_NAV, DHD_NET_TEST_TRACK=a.track, DHD_NET_TEST_FINISH='100')))
            games.append(host)
            m = wait(host.log, ROOM, 180, games)
            if not m:
                raise RuntimeError('the host never opened a room')
            code = m.group(1)
            print('room', code, flush=True)
            guest_extra = dict(DHD_NET_SERVER=url, DHD_AUTOPAD_FRAMES=P2P_GUEST_NAV, DHD_NET_TEST_CODE=code, DHD_NET_TEST_FINISH='110')
            if a.scenario == 'racer-leaves':
                guest_extra['DHD_NET_TEST_MENU'] = '30:start,w500,' + YES
            guest = hidden_game.Game(a.exe, out / 'guest', name='watch-guest', scale=a.scale, shot_window=None, env=env('guest', guest_extra))
            games.append(guest)
            if not wait(guest.log, r'countdown starts', 900, games):
                raise RuntimeError('the guest never reached a countdown')
            print('the race of host and guest started', flush=True)
            mark = {g: len(g.log()) for g in games}
            watcher = None
            if a.scenario != 'none':
                extra = dict(DHD_NET_SERVER=url, DHD_AUTOPAD_FRAMES=P2P_GUEST_NAV, DHD_NET_TEST_CODE=code, DHD_NET_TEST_WATCH='1',
                             DHD_NET_TEST_FINISH='60')
                extra['DHD_NET_TEST_MENU'] = {'watch': '6:right,w3000,right,w3000,left', 'watcher-leaves': '12:start,w500,down,cross,w300,down,cross'}.get(a.scenario, '')
                if a.interval != '0':
                    extra.update(DHD_CAPTURE_PRESENTED=str(out / 'watcher' / 'present'), DHD_CAPTURE_AT='5', DHD_CAPTURE_INTERVAL=a.interval)
                width, height = a.watcher_window.split('x')
                extra.update(DHD_WINDOW_WIDTH=width, DHD_WINDOW_HEIGHT=height)
                watcher = hidden_game.Game(a.exe, out / 'watcher', name='watch-watcher', scale=a.watcher_scale or a.scale,
                                           shot_window=None, env=env('watcher', extra))
                games.append(watcher)
                mark[watcher] = 0
                if not wait(watcher.log, r'online: watching the room.s race', 600, games):
                    raise RuntimeError('the watcher never watched the race')
                print('the watcher watches', flush=True)
            want = {
                'watch': [(watcher, r'online: watching .*\(slot \d+\)'), (watcher, r'race closed: back to the room \(RACE OVER'),
                          (watcher, r'online: race starts: player \d of 3'), (watcher, r'countdown starts \(waited')],
                'watcher-leaves': [(watcher, r'race menu: back to the room'), (watcher, r"this PC's race is over; back to the room"),
                                   (host, r'online: result 1\.|race closed')],
                'racer-leaves': [(guest, r'race menu: back to the room'), (watcher, r'player 1 \(GUEST\) left'),
                                 (host, r'race closed')],
                'none': [(host, r'race closed'), (guest, r'race closed')],
            }[a.scenario]
            for who, pattern in want:
                if not wait(lambda: who.log()[mark[who]:], pattern, 600, games):
                    failures.append(f'{who.out.name}: no "{pattern}"')
                    break
            time.sleep(8)
        except RuntimeError as why:
            failures.append(str(why))
        finally:
            for g in games:
                g.stop()
    for g in games:
        for line in g.log().splitlines():
            if LINES.search(line):
                print(f'  {g.out.name}: {line.strip()}')
    for g in games:
        log = g.log()
        bad = [line.strip() for line in log.splitlines() if REFUSED.search(line)]
        if bad:
            failures.append(f'{g.out.name}: {len(bad)} refusal/freeze/crash lines, the first: {bad[0]}')
        # its own settings line names the server it was given (a local one): not another member's
        leaks = [line.strip() for line in log.splitlines() if LEAK.search(line) and 'from the environment:' not in line]
        if leaks:
            print(f'  {g.out.name}: lines with an address or id to look at ({len(leaks)}), the first: {leaks[0][:160]}')
    if a.scenario == 'watch' and len(games) == 3:
        watched = re.findall(r'online: watching (.*) \(slot (\d+)\)', games[2].log())
        print('the watcher watched, in order:', ', '.join(f'{n} ({s})' for n, s in watched))
        if len(watched) < 4:
            failures.append(f'the watcher changed rider {len(watched)} times (want: the first, three presses, a finish)')
    for g in games[:2]:
        print(f'--- {g.out.name}: the second race\'s results')
        for r in results(g.log()):
            print('   ', r)
        (out / f'{g.out.name}-results.txt').write_text('\n'.join(results(g.log())) + '\n')
    for f in failures:
        print('FAIL:', f)
    print('PASS' if not failures else 'FAIL')
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
