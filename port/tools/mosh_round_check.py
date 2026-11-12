"""The arena's scored round online (finding 204): two games on this PC in one room, from the real
ONLINE menus, on hidden desktops (hidden_game.py), and nettool bots up to the arena's limit if
asked. The host makes a P2P room on the Mosh Pit arena (DHD_NET_TEST_TRACK=23), the guest joins by
its code, both ready; the round plays its two minutes by the game's Mosh Bowl rules; then the
round's table, COMBAT RESULTS and back to the room. A local server per run (local_server.py, P2P
rooms: no Realtime credentials needed). The riders follow DHD_RACE_PAD scripts with
DHD_RACE_SYNC=1 (online_riders.py; without the switch they took no input, finding 206).

    python port/tools/mosh_round_check.py OUT                  # --scenario hit: a human scores on a human
    python port/tools/mosh_round_check.py OUT --scenario ride [--bots 4] [--then same|next] [--no-ai]
    python port/tools/mosh_round_check.py OUT --offline        # the game's own Mosh Bowl, no server

--scenario hit (the default): no computer riders; both riders hold the brake (Triangle) all round,
so they stay side by side at the gates, and the guest punches and kicks every 0.6 s from 5.2 s
after GO (an attack hits nothing before race tick 301, func_0012E230). Besides the checks below it
passes when the guest has points and the host's rider hits taken, the guest first, and each
rider's final is the same on both games (its owner's "(sent)" line, the other's "(from player N)"
line) and in the table.
--scenario ride: both games pedal all round with a punch or a kick every 2.5 s, the room's
computer riders on unless --no-ai.

The scripted host starts its first race before the guest is in (as in cross_race.py), so with
--then same (the default) its first round may be its own and the next one has both. It passes when
both games have the table of the round both played and the tables are equal, and the room's
results of that race (the room page's log line, on each game) list the players in the table's
order with the same points and places. --then next: the race after the host's own round is the
next track (Single Event), a check that the room goes back to it. Pictures:
OUT/<game>/present-NNN.png every --interval seconds, rendered at --scale (default 4) in a --window
sized window; untimed game lock, two places.
--offline: one game through the game's own menus (SINGLE PLAYER, the Mosh Bowl row of the
single-player modes, its timed row) for one round with the ride scenario's pad, pictures as above;
it passes when the game runs its time with no online line in its log (the pictures show the
round's end and COMBAT RESULTS). The offline mode must stay the game's own: every online hook
returns at once offline.

Needs DHD_TEST_ISO (or DHD_ISO) and the x64 build (build/host64-exe/dhd64.exe)."""
import argparse
import contextlib
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import local_server  # noqa: E402
import owned_process  # noqa: E402
from cross_race import P2P_HOST_NAV, P2P_GUEST_NAV  # noqa: E402
from online_race import iso_files_sha256  # noqa: E402
from online_riders import BRAKE, PEDAL, attacks, pad, rider_env  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
# ride: a punch (square) or a kick (circle) every 2.5 s through the round: pressed every half second
# the rider hardly pedals and lands nothing (finding 204's first runs: 0 points, 1 mph up the wall)
RIDE = pad(PEDAL, attacks(2, 2.5, 52))
# hit: both brake at the gates, side by side; the guest attacks from 5.2 s (race tick 312, past 301)
SCENARIOS = {'hit': (BRAKE, pad(BRAKE, attacks(5.2, 0.6, 192))), 'ride': (RIDE, RIDE)}
FINAL = re.compile(r"online: round: (.+?)'s final score (\d+), (\d+) hits taken \((sent|from player \d+)\)")
ROOM = r'online menu: room (\w{7}) \(host\)'
TABLE = re.compile(r'online: round: the table[^:]*\(([\d.]+) s after time up\):(.*)')
ROOM_RESULTS = re.compile(r"online menu: the room's results of race (\d+) \((.+?)\):( .*)")
# Offline: the title, SINGLE PLAYER (15), the rider (3), NEW (16), the single-player modes (1): its
# fourth row is the Mosh Bowl (func_001ED900 row 3), whose page (21) has the last-rider-standing row
# first and the timed row second (func_001EF5D0); then the shop (18) and the flight over the arena.
OFFLINE_NAV = ('0:triangle|cross,15:cross,3:cross,16:cross,1:down,1+20:down,1+40:down,1+70:cross,21:down,21+30:cross,'
               '18@4C7F38=0:start,18@4C7F38=1:cross,9/5:cross')


class Stop(Exception):
    pass


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


def table_entries(text):
    """' 1. Name 300 (2) 2. ...' -> [(name, points, hits)]"""
    return [(m.group(1), int(m.group(2)), int(m.group(3)))
            for m in re.finditer(r' \d+\. (.+?) (\d+) \((\d+)\)(?= \d+\. |$)', text)]


def room_entries(text):
    """' 1. Name 300 PTS; 2. ...' -> [(place, name, points or None)]"""
    out = []
    for part in text.split(';'):
        m = re.match(r'\s*(\d+)\. (.+?) (\d+) PTS\s*$', part)
        if m:
            out.append((int(m.group(1)), m.group(2), int(m.group(3))))
        elif part.strip():
            m = re.match(r'\s*(\d+)\. (.+?) (.*)$', part)
            if m:
                out.append((int(m.group(1)), m.group(2), None))
    return out


def finals(log):
    """The finals of the last round with a table in a game's log: {name: (points, hits taken, 'sent' or
    'from player N')}."""
    starts = 'online: race starts: player '
    table = log.rfind('online: round: the table')
    start, end = log.rfind(starts, 0, max(table, 0)), log.find(starts, max(table, 0))
    return {m.group(1): (int(m.group(2)), int(m.group(3)), m.group(4))
            for m in FINAL.finditer(log, max(start, 0), end if end >= 0 else len(log))}


def hit_failures(games, table):
    """--scenario hit: the guest scored on the host's rider, and both games say the same."""
    failures = []
    rows = {n: (p, h) for n, p, h in table}
    if not rows.get('GUEST', (0, 0))[0]:
        failures.append(f"the guest scored nothing on the host's rider: {table}")
    if not rows.get('HOST', (0, 0))[1]:
        failures.append(f"the host's rider took no hit: {table}")
    if table and table[0][0] != 'GUEST':
        failures.append(f'the guest is not first: {table}')
    seen = {g.out.name: finals(g.log()) for g in games}
    for name, own in seen.items():
        print(f'{name}: the finals of its last round: {own}')
    for player, owner, other in (('HOST', 'host', 'guest'), ('GUEST', 'guest', 'host')):
        mine, theirs = seen.get(owner, {}).get(player), seen.get(other, {}).get(player)
        if not mine or mine[2] != 'sent':
            failures.append(f'{owner}: no final of its own rider ({player}) sent')
        elif not theirs or theirs[2] == 'sent' or theirs[:2] != mine[:2]:
            failures.append(f"{other}: {player}'s final {theirs} is not its owner's {mine}")
        elif mine[:2] != rows.get(player):
            failures.append(f"{player}'s final {mine[:2]} is not the table's {rows.get(player)}")
    return failures


def offline(a):
    out = a.out.resolve()
    (out / 'offline').mkdir(parents=True, exist_ok=True)
    width, height = a.window.split('x')
    for f in (out / 'offline').glob('present-*.png'):
        f.unlink()
    # Mosh Bowl's row is locked on a fresh save (func_001ED900: D_004225A0 +0x1219B): Unlock All
    env = dict(DHD_UNLOCK_ALL='1', DHD_NAV=OFFLINE_NAV, **rider_env(a.pad or RIDE), DHD_FPS='60',
               DHD_WINDOW_WIDTH=width, DHD_WINDOW_HEIGHT=height, DHD_SCREENSHOT='<unset>')
    if a.interval != '0':
        env.update(DHD_CAPTURE_PRESENTED=str(out / 'offline' / 'present'), DHD_CAPTURE_AT='5', DHD_CAPTURE_INTERVAL=a.interval)
    g = hidden_game.Game(a.exe, out / 'offline', env=env, name='mosh-offline', scale=a.scale, shot_window=None)
    try:
        # the menus and the flight (about a minute), the round's two minutes, its end and results:
        # the pictures show them (nothing in the log marks the offline results)
        until = time.time() + 60 * min(a.minutes, 6)
        while time.time() < until and g.alive():
            time.sleep(2)
        ran = g.alive()
    finally:
        g.stop()
    online_lines = [line for line in g.log().splitlines() if line.startswith('online:')]
    print('offline: the game ran its time' if ran else 'offline: the game exited early; see', out / 'offline' / 'game.log')
    print('offline: online lines in its log:', len(online_lines), '; pictures:', out / 'offline')
    return 0 if ran and not online_lines else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--nettool', type=Path, default=ROOT / 'build' / 'port' / 'net' / 'nettool.exe')
    ap.add_argument('--scenario', choices=sorted(SCENARIOS), default='hit',
                    help='hit: a human scores on a human, no computer riders (default); ride: both pedal and attack')
    ap.add_argument('--bots', type=int, default=0, help='nettool players besides the two games (the arena takes 6)')
    ap.add_argument('--then', choices=('same', 'next', 'random'), default='same', help="the room's Then rule")
    ap.add_argument('--no-ai', action='store_true', help="the room's computer riders off (always so with --scenario hit)")
    ap.add_argument('--scale', default='4')
    ap.add_argument('--window', default='1280x960')
    ap.add_argument('--interval', default='5', help='seconds between presented pictures (0: none)')
    ap.add_argument('--minutes', type=float, default=6, help='--offline: how long the game runs')
    ap.add_argument('--offline', action='store_true', help="the game's own Mosh Bowl, one game, no server")
    ap.add_argument('--link', default='', help='DHD_NET_LINK for both games (port/hle/puppet_link.h), e.g. "delay=80,jitter=15"')
    ap.add_argument('--pad', help="both games' race pad (DHD_RACE_PAD) instead of the scenario's")
    ap.add_argument('--host-pad', help="the host's race pad instead of the scenario's")
    ap.add_argument('--guest-pad', help="the guest's race pad instead of the scenario's")
    ap.add_argument('--host-env', action='append', default=[], help='extra NAME=VALUE for the host, e.g. DHD_NET_TEST_MENU=...')
    ap.add_argument('--guest-env', action='append', default=[], help='extra NAME=VALUE for the guest')
    a = ap.parse_args()
    if a.offline:
        return offline(a)
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    iso_hash = iso_files_sha256(hidden_game.test_iso(), ROOT / 'build' / 'port' / 'iso-check.txt')
    width, height = a.window.split('x')
    hit = a.scenario == 'hit'
    pads = dict(zip(('host', 'guest'), SCENARIOS[a.scenario]))
    pads.update(host=a.host_pad or a.pad or pads['host'], guest=a.guest_pad or a.pad or pads['guest'])

    def env(name, extra):
        e = dict(DHD_NET_ISO_SHA256=iso_hash, DHD_NET_LOG_CODE='1', DHD_NET_SCRIPTED='1', DHD_NET_TEST_READY='1',
                 DHD_FPS='60', DHD_ONLINE_MENU_TRACE='1', DHD_MEMCARD='1', DHD_MEMCARD_DIR=str(out / name / 'card'),
                 **rider_env(pads[name]), DHD_NET_NAME=name.upper(), DHD_WINDOW_WIDTH=width, DHD_WINDOW_HEIGHT=height,
                 DHD_SCREENSHOT='<unset>')
        if a.link:  # the STATE each game receives through a test link (finding 202)
            e['DHD_NET_LINK'] = a.link
        if a.interval != '0':
            e.update(DHD_CAPTURE_PRESENTED=str(out / name / 'present'), DHD_CAPTURE_AT='5', DHD_CAPTURE_INTERVAL=a.interval)
        e.update(extra)
        e.update(dict(x.split('=', 1) for x in (a.host_env if name == 'host' else a.guest_env)))
        (out / name / 'card').mkdir(parents=True, exist_ok=True)
        for f in (out / name).glob('present-*.png'):
            f.unlink()
        return e

    games, bots = [], []
    ok = False
    with contextlib.ExitStack() as stack, local_server.running() as url:
        try:
            host_env = env('host', dict(DHD_NET_SERVER=url, DHD_AUTOPAD_FRAMES=P2P_HOST_NAV, DHD_NET_TEST_TRACK='23',
                                        DHD_NET_TEST_THEN=a.then, **({'DHD_NET_NO_AI': '1'} if a.no_ai or hit else {})))
            host = hidden_game.Game(a.exe, out / 'host', env=host_env, name='mosh-host', scale=a.scale, shot_window=None)
            games.append(host)
            m = wait(host.log, ROOM, 180, games)
            if not m:
                print('the host never opened a room; see', out / 'host' / 'game.log')
                return 1
            code = m.group(1)
            print('room', code, flush=True)
            version = wait(host.log, r'version (dhd-port [\w.-]+(?: \w+)?)', 30, games)
            for k in range(a.bots):
                d = out / f'bot{k}'
                d.mkdir(exist_ok=True)
                log = stack.enter_context(open(d / 'stdout.log', 'w'))
                bots.append(stack.enter_context(owned_process.start(
                    [str(a.nettool), '--server', url, '--version', version.group(1), '--iso', iso_hash, '--name', f'Bot{k}',
                     '--players', str(2 + a.bots), '--pick', str(k * 2 + 1), '--races', '3', '--ready-after', '1',
                     '--echo-ai', '--finish-after', '122', '--score', str(100 * (k + 1)), 'join', code],
                    stdout=log, stderr=subprocess.STDOUT)))
            guest_env = env('guest', dict(DHD_NET_SERVER=url, DHD_AUTOPAD_FRAMES=P2P_GUEST_NAV, DHD_NET_TEST_CODE=code))
            guest = hidden_game.Game(a.exe, out / 'guest', env=guest_env, name='mosh-guest', scale=a.scale, shot_window=None)
            games.append(guest)
            # The scripted host starts before the guest is in (cross_race.py says the same), so its
            # first round can be its own; the guest's first round is the one both play.
            race = None
            if not wait(guest.log, r'countdown starts', 900, games):
                raise Stop('the guest never reached a countdown')
            print('a round with both started', flush=True)
            if not wait(guest.log, r'round: the table', 300, games):
                raise Stop("the guest never had the round's table")
            # its round's results: logged after its table (a guest that joined between races logs
            # the room's earlier results as it comes in)
            seen = guest.log().index('round: the table')
            race = wait(lambda: guest.log()[seen:], r"the room's results of race (\d+) \((?i:mosh)", 120, games)
            if not race:
                raise Stop("the guest never got back to the room with the round's results")
            if not wait(host.log, rf"the room's results of race {race.group(1)} ", 120, games):
                raise Stop("the host never got back to the room with the round's results")
            ok = True
        except Stop as why:
            print(why, flush=True)
        finally:
            time.sleep(5)
            for g in games:
                g.stop()
            for b in bots:
                b.stop()
    # the evidence: each game's table of the round both played (the last one each), the room's
    # results of that race on each, the round's lines
    tables, rooms = {}, {}
    shared = int(race.group(1)) if race else None
    for g in games:
        name = g.out.name
        log = g.log()
        found = list(TABLE.finditer(log))
        tables[name] = table_entries(found[-1].group(2)) if found else []
        rooms[name] = {int(m.group(1)): (m.group(2), room_entries(m.group(3))) for m in ROOM_RESULTS.finditer(log)}
        print(f'{name}: the round\'s table, {found[-1].group(1) if found else "-"} s after its time up: {tables[name]}')
        for n, (track, entries) in sorted(rooms[name].items()):
            print(f'{name}: the room\'s results of race {n} ({track}): {entries}')
        for line in log.splitlines():
            if re.search(r"round: |arena's round|back to the room|race starts|race closed|race menu|freeze:|host64 memory", line):
                print(f'  {name}: {line.strip()}')
    for b in range(a.bots):
        text = (out / f'bot{b}' / 'stdout.log').read_text(errors='replace')
        print(f'bot{b}:', ' | '.join(line for line in text.splitlines() if re.match(r'(ROUND OVER|PHASE|RESULT|FAILED)', line)))
    failures = []
    if not ok:
        failures.append('the run did not finish')
    if len({tuple(t) for t in tables.values()}) != 1 or not tables.get('host'):
        failures.append("the games' tables differ or are missing")
    elif shared is not None:
        table = tables['host']
        for name in rooms:
            if shared not in rooms[name]:
                failures.append(f'{name}: no room results for race {shared}')
                continue
            entries = rooms[name][shared][1]
            names = {n for _, n, _ in entries}
            got = [(n, p, place) for place, n, p in entries if p is not None]
            want = [(n, p, i + 1) for i, (n, p, _) in enumerate(table) if n in names]
            if [(n, p) for n, p, _ in got] != [(n, p) for n, p, _ in want]:
                failures.append(f"{name}: the room's order {got} is not the table's {want}")
            elif got != want:
                failures.append(f"{name}: the room's places {got} are not the table's {want}")
    if hit and ok:
        failures += hit_failures(games, tables.get('host', []))
    for f in failures:
        print('FAIL:', f)
    print('PASS' if not failures else 'FAIL')
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
