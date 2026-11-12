"""The race menu online (finding 215): two games on this PC in one room from the real ONLINE menus,
on hidden desktops, a local server per run; each scenario opens the menu over the running race
with DHD_NET_TEST_MENU (START and the menu's buttons on the wall clock from a race second) and
checks the logs.

    python port/tools/race_menu_check.py OUT --scenario room|end|leave|closed|alone|alone-guest|resume|timeup|timeup-host|finished [--sfu] [--track N]

- resume: the guest opens the menu at 20 s for 2 s, the host at 25 s for 10 s; both RESUME. The
  race never pauses (no HOLD on the other game), both finish, both games list the same results.
- room: the guest's BACK TO THE ROOM (Down, Cross, Down to Yes, Cross) at 20 s: the guest is back
  in the room, the host races on and sees the guest leave.
- end: the host's END THE RACE at 20 s: both games back in the room ("THE HOST ENDED THE RACE" on the
  guest; the host leaves by its own menu at once and shows no reason).
- timeup, timeup-host, finished (finding 211): why the race closed: one rider finishes at 20 s and the
  other never does (the finish window ends it: "TIME UP - DNF" on the one still riding, "RACE OVER" on
  the finisher), or both finish; the logs' "race closed: back to the room (WHY: ...)" line says it.
- leave: the guest's LEAVE THE ROOM (Up to the last row) at 20 s: the guest leaves the room.
- closed: the guest opens the menu at 18 s and leaves it open; the host ends the race at 22 s: the
  guest's menu closes with the race and the guest is back in the room.
- alone: the guest finishes at 12 s (DHD_NET_TEST_FINISH); the host's START at 30 s is the game's
  own pause (no other human races), 5 s, RESUME; the host's race clock stands still meanwhile.
- alone-guest (finding 220): the roles swapped: the host finishes at 12 s, the guest (who draws the
  host's computer riders, still racing) pauses alone at 30 s for 5 s. After the pause the guest's
  race runs 5 s behind the others' for good; the others' snapshots must still land at its "now".

Every scenario fails on a refusal line in either game's log (finding 220: a record or HOLD refused,
a State not on the list). For the alone scenarios the pausing game's live puppet error (its
`puppets off by` lines) is printed before the pause, in the 5 s after it and after that.

The riders finish at 60 s (host) and 70 s (guest) of their own race clocks (DHD_NET_TEST_FINISH;
a scenario may change it). The scripted host starts its first race before the guest is in (as in cross_race.py), so the race
both play is the guest's first. P2P rooms by default (no Realtime credentials); --sfu a private SFU
room (online/server/.dev.vars). Pictures: OUT/<game>/present-NNN.png every --interval seconds at
--scale. --lag: both games trace their puppets and record their riders, and the race both play is
scored as `online_race.py --lag` scores one (puppet_lag.py, OUT/race/A the host, B the guest), with
the seconds after the room ends the race as their own column (finding 216). Untimed game lock
(hidden_game). Needs DHD_TEST_ISO and the x64 build."""
import argparse
import contextlib
import re
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import local_server  # noqa: E402
from cross_race import P2P_HOST_NAV, P2P_GUEST_NAV, PRIVATE_HOST_NAV  # noqa: E402
from online_race import iso_files_sha256  # noqa: E402
from online_riders import PEDAL, rider_env  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
ROOM = r'online menu: room (\w{6,7}) \(host\)'
YES = 'down,cross,w300,down,cross'  # a row's question opens on No
# scenario: (host's DHD_NET_TEST_MENU, guest's, extra host env, extra guest env)
SCENARIOS = {
    'resume': ('25:start,w10000,cross', '20:start,w2000,cross', {}, {}),
    'room': ('', '20:start,w500,' + YES, {}, {}),
    # END THE RACE is the host's third row: Down twice from RESUME
    'end': ('20:start,w500,down,w300,down,cross,w300,down,cross', '', {}, {}),
    'leave': ('', '20:start,w500,up,cross,w300,down,cross', {}, {}),
    'closed': ('22:start,w500,down,w300,down,cross,w300,down,cross', '18:start,w60000,cross', {}, {}),
    'alone': ('30:start,w5000,cross', '', {}, {'DHD_NET_TEST_FINISH': '12'}),
    'alone-guest': ('', '30:start,w5000,cross', {'DHD_NET_TEST_FINISH': '12'}, {}),
    # why the race closed (finding 211): one rider finishes at 20 s, the other never does and is DNF
    # when the room's finish window (60 s) runs out; each screen says why for two seconds
    'timeup': ('', '', {'DHD_NET_TEST_FINISH': '20'}, {'DHD_NET_TEST_FINISH': '99999'}),  # the guest is DNF
    # the host is DNF: the guest finishes at 10 s, so the window ends at about 70 s, before the host's 75 s (its
    # first race, alone, must end too, so it finishes at some time)
    'timeup-host': ('', '', {'DHD_NET_TEST_FINISH': '75'}, {'DHD_NET_TEST_FINISH': '10'}),
    'finished': ('', '', {'DHD_NET_TEST_FINISH': '20'}, {'DHD_NET_TEST_FINISH': '25'}),  # everyone finishes
}
LINES = re.compile(r'online: (race menu: .*|test menu: .*|race closed.*|this PC.s race is over.*|the race is over.*|'
                   r'player \d .* left.*|result \d.*|.* finished in .*|race starts.*)|puppet: .*(hold|HOLD).*|stall: .*')
REFUSED = re.compile(r'record refused|records refused|HOLD refused|HOLDs refused|not on puppet_record')  # finding 220
CLOSED = r'race closed: back to the room \(%s[:)]'  # online.c state_sync: the reason, then what it was worked out from


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


def percentile(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))] if values else float('nan')


def pause_errors(log):
    """The pausing game's live puppet error (units, its once-a-second `puppets off by` line: each rider's
    error found by its last snapshot, finding 202) and snapshot age (ms) in the race both play: before
    the game's own pause, in the 5 s after RESUME (the catch-up) and after that."""
    pressed = log.rfind('test menu: button 0008')  # the race with the pause (a next race may follow)
    race = log[log.rfind('race starts', 0, pressed if pressed >= 0 else len(log)):]
    race = race[:race.find('race starts', 12)] if race.find('race starts', 12) > 0 else race
    opened = re.search(r'test menu: button 0008 at race tick (\d+)', race)
    resumed = re.search(r'test menu: button 4000 at race tick (\d+)', race)
    if not opened or not resumed:
        print('  no pause in the race both play')
        return
    at, back = int(opened.group(1)), int(resumed.group(1))
    phases = {'before': ([], []), 'catch-up (5 s)': ([], []), 'after': ([], [])}
    for m in re.finditer(r'online: race clock (\d+) .*?snapshot age \(ms\):(.*)', race):
        tick = int(m.group(1))
        name = 'before' if tick < at else 'catch-up (5 s)' if tick <= back + 300 else 'after'
        if at <= tick <= back + 1:
            continue  # the pause itself: nothing moves
        for off, age in re.findall(r' \d+:([-\d.]+)/([-\d.]+)', m.group(2)):
            phases[name][0].append(float(off))
            phases[name][1].append(float(age))
    print(f"--- the pausing game's puppets (pause at tick {at}, resume at {back}): error p50/p95 units, age p50/p95 ms")
    for name, (off, age) in phases.items():
        print(f'  {name}: {len(off)} values, error {percentile(off, .5):.2f}/{percentile(off, .95):.2f}, '
              f'age {percentile(age, .5):.0f}/{percentile(age, .95):.0f}')


def checks(scenario, host, guest):
    """What must be in each game's log (of the race both play, from the guest's first countdown)."""
    want = {
        'resume': [(host, r'race menu: open at race tick'), (host, r'race menu: resume'), (guest, r'race menu: open at'),
                   (guest, r'race menu: resume'), (host, r'online: result 1\.|race closed'), (guest, r'online: result 1\.|race closed')],
        'room': [(guest, r'race menu: back to the room'), (guest, r"this PC's race is over; back to the room"),
                 (host, r'player \d \(GUEST\) left')],
        'end': [(host, r'race menu: the host ends the race'), (host, r"back to the room"), (guest, r'race closed: back to the room'),
                (guest, CLOSED % 'THE HOST ENDED THE RACE')],
        'timeup': [(host, CLOSED % 'RACE OVER'), (guest, CLOSED % 'TIME UP - DNF')],
        'timeup-host': [(guest, CLOSED % 'RACE OVER'), (host, CLOSED % 'TIME UP - DNF')],
        'finished': [(host, CLOSED % 'RACE OVER'), (guest, CLOSED % 'RACE OVER')],
        'leave': [(guest, r'race menu: leave the room'), (guest, r'the race is over; back to offline play'),
                  (host, r'player \d \(GUEST\) left')],
        'closed': [(guest, r'race menu: open at'), (host, r'race menu: the host ends the race'),
                   (guest, r'race menu: closed \(the race screen ends\)'), (guest, r'race closed: back to the room')],
        'alone': [(host, r'race menu: alone in the race'), (host, r'test menu: button 4000')],
        # and the guest races on to its finish (70 s) with the host's computer riders after its pause
        'alone-guest': [(guest, r'race menu: alone in the race'), (guest, r'test menu: button 4000'),
                        (guest, r'online: result 1\.|race closed')],
    }[scenario]
    return want


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--scenario', choices=sorted(SCENARIOS), default='resume')
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--sfu', action='store_true', help='a private SFU room instead of P2P')
    ap.add_argument('--track', default='0', help="the room's first track (DHD_NET_TEST_TRACK)")
    ap.add_argument('--no-ai', action='store_true', help="the room's computer riders off")
    ap.add_argument('--link', default='', help='DHD_NET_LINK for both games, e.g. "delay=80"')
    ap.add_argument('--lag', action='store_true', help='trace and record both games and score the race both play (puppet_lag.py)')
    ap.add_argument('--scale', default='1')
    ap.add_argument('--window', default='1280x720')
    ap.add_argument('--interval', default='0', help='seconds between presented pictures (0: none)')
    ap.add_argument('--host-env', action='append', default=[], help='extra NAME=VALUE for the host')
    ap.add_argument('--guest-env', action='append', default=[], help='extra NAME=VALUE for the guest')
    ap.add_argument('--host-click', action='append', default=[],
                    help="S:FX,FY: a posted left click on the host's window S seconds after the race both play starts, "
                         'at FX, FY of its client size (the touch controls with DHD_TOUCH_MOUSE=1)')
    a = ap.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    iso_hash = iso_files_sha256(hidden_game.test_iso(), ROOT / 'build' / 'port' / 'iso-check.txt')
    width, height = a.window.split('x')
    host_menu, guest_menu, host_extra, guest_extra = SCENARIOS[a.scenario]

    def env(name, menu, extra, more):
        e = dict(DHD_NET_ISO_SHA256=iso_hash, DHD_NET_LOG_CODE='1', DHD_NET_SCRIPTED='1', DHD_NET_TEST_READY='1',
                 DHD_NET_TEST_FINISH='60' if name == 'host' else '70', DHD_FPS='60', DHD_MEMCARD='1', DHD_MEMCARD_DIR=str(out / name / 'card'), **rider_env(PEDAL),
                 DHD_NET_NAME=name.upper(), DHD_WINDOW_WIDTH=width, DHD_WINDOW_HEIGHT=height, DHD_SCREENSHOT='<unset>')
        if menu:
            e['DHD_NET_TEST_MENU'] = menu
        if a.link:
            e['DHD_NET_LINK'] = a.link
        if a.lag:  # the hidden game's folder is the exe's: its files by full path
            e.update(DHD_PUPPET_TRACE=str(out / name / 'puppet.txt'), DHD_PUPPET_RECORD=str(out / name / 'own.bin'),
                     DHD_PUPPET_FROM='all')
            for f in ('puppet.txt', 'own.bin'):
                (out / name / f).unlink(missing_ok=True)
        if a.interval != '0':
            e.update(DHD_CAPTURE_PRESENTED=str(out / name / 'present'), DHD_CAPTURE_AT='5', DHD_CAPTURE_INTERVAL=a.interval)
        e.update(extra)
        e.update(dict(x.split('=', 1) for x in more))
        (out / name / 'card').mkdir(parents=True, exist_ok=True)
        for f in (out / name).glob('present-*.png'):
            f.unlink()
        return e

    games = []
    failures = []
    with contextlib.ExitStack(), local_server.running() as url:
        try:
            host_env = env('host', host_menu, dict(DHD_NET_SERVER=url, **({'SDL_MOUSE_AUTO_CAPTURE': '0'} if a.host_click else {}), DHD_AUTOPAD_FRAMES=PRIVATE_HOST_NAV if a.sfu else P2P_HOST_NAV,
                                                   DHD_NET_TEST_TRACK=a.track, **host_extra,
                                                   **({'DHD_NET_NO_AI': '1'} if a.no_ai else {})), a.host_env)
            host = hidden_game.Game(a.exe, out / 'host', env=host_env, name='race-menu-host', scale=a.scale, shot_window=None)
            games.append(host)
            m = wait(host.log, ROOM, 180, games)
            if not m:
                raise RuntimeError('the host never opened a room; see ' + str(out / 'host' / 'game.log'))
            code = m.group(1)
            print('room', code, flush=True)
            guest_env = env('guest', guest_menu, dict(DHD_NET_SERVER=url, DHD_AUTOPAD_FRAMES=P2P_GUEST_NAV,
                                                      DHD_NET_TEST_CODE=code, **guest_extra), a.guest_env)
            guest = hidden_game.Game(a.exe, out / 'guest', env=guest_env, name='race-menu-guest', scale=a.scale, shot_window=None)
            games.append(guest)
            if not wait(guest.log, r'countdown starts', 900, games):
                raise RuntimeError('the guest never reached a countdown')
            print('a race with both started', flush=True)
            for spec in a.host_click:
                when, where = spec.split(':')
                fx, fy = (float(v) for v in where.split(','))

                def click(fx=fx, fy=fy):
                    width, height = host.client()
                    host.click(int(fx * width), int(fy * height))
                    print(f'clicked the host at {fx:.2f}, {fy:.2f}', flush=True)
                threading.Timer(float(when), click).start()
            start = {g: len(g.log()) for g in games}
            for who, pattern in checks(a.scenario, host, guest):
                if not wait(lambda: who.log()[start[who]:], pattern, 420, games):
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
        refused = [line.strip() for line in g.log().splitlines() if REFUSED.search(line)]
        if refused:
            failures.append(f'{g.out.name}: {len(refused)} refusal lines, the first: {refused[0]}')
    if a.scenario.startswith('alone'):
        paused = host if a.scenario == 'alone' else guest
        pause_errors(paused.log())
    if a.lag:
        import puppet_lag
        for g, side in zip(games, 'AB'):
            puppet_lag.last_race(g.out, out / 'race' / side)
        print('--- puppets of the race both play (puppet_lag.py; A the host, B the guest):', flush=True)
        try:
            puppet_lag.report(out / 'race')
            puppet_lag.riders_report(out / 'race')
        except (OSError, ValueError, IndexError) as why:
            failures.append(f'the puppets could not be scored ({why})')
    for f in failures:
        print('FAIL:', f)
    print('PASS' if not failures else 'FAIL')
    return 0 if not failures else 1


if __name__ == '__main__':
    sys.exit(main())
