"""Regression for issue #96: shop Back -> rider choice -> ONLINE, hidden, local server.

    python port/tools/online_back_check.py OUT [--exe EXE] [--back esc|backspace|triangle]

Triangle uses the existing frame-script test pad; keyboard runs use posted keys.
No visible window, and the server and game are stopped even on failure.
"""
import argparse
import contextlib
import subprocess
import re
import time
from pathlib import Path

import hidden_game as hg
import local_server
import owned_process
from online_race import iso_files_sha256

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('out', type=Path)
    p.add_argument('--exe', type=Path, default=ROOT / 'build/host64-exe/dhd64.exe')
    p.add_argument('--entry', choices=('quick', 'list', 'public', 'private', 'p2p'), default='private')
    p.add_argument('--back', choices=('esc', 'backspace', 'triangle'), default='esc')
    p.add_argument('--commit', action='store_true', help='check START commits to the room instead')
    p.add_argument('--version', help="the tested game's full room version, needed for quick/list bots")
    a = p.parse_args()
    if a.entry in ('quick', 'list') and not a.version:
        p.error('--version is required for quick/list')
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    pad = out / 'pad.txt'
    pad.write_text('')
    sha = iso_files_sha256(hg.test_iso(), ROOT / 'build/port/iso-check.txt')
    with local_server.running() as url, contextlib.ExitStack() as stack:
        if a.entry in ('quick', 'list'):
            log = stack.enter_context(open(out / 'bot.log', 'w'))
            stack.enter_context(owned_process.start([
                str(ROOT / 'build/port/net/nettool.exe'), '--server', url,
                '--version', a.version, '--iso', sha, '--seconds', '240',
                '--races', '1', '--pick', '0', '--ready-after', '-1', 'host', '--public',
            ], stdout=log, stderr=subprocess.STDOUT))
            until = time.monotonic() + 30
            while 'CODE ' not in (out / 'bot.log').read_text(errors='replace'):
                if time.monotonic() > until:
                    raise RuntimeError('public host did not connect')
                time.sleep(.25)
        env = dict(DHD_NET_SERVER=url, DHD_NET_ISO_SHA256=sha, DHD_NET_NAME='BACK',
                   DHD_ONLINE_MENU_TRACE='1', DHD_FPS='60')
        if a.back == 'triangle':
            env['DHD_AUTOPAD_FRAMES'] = '@' + str(pad)
        g = hg.Game(a.exe.resolve(), out, env=env, name='online-back', interval=.25)
        def tap(key):
            if a.back != 'triangle':
                g.key(key)
                time.sleep(.5)
                return
            button = {'x': 'cross', 's': 'triangle', 'enter': 'start'}.get(key, key)
            field = g.field() + 60
            pad.write_text(f'{field}:12:{button}')
            until = time.monotonic() + 15
            while g.field() < field + 30:
                if not g.alive() or time.monotonic() > until:
                    raise RuntimeError('test pad did not advance')
                time.sleep(.1)
        try:
            g.window()
            until, grey = time.monotonic() + 90, False
            while time.monotonic() < until:
                colour = hg.latest_colour(g)
                if colour is not None and colour < 25:
                    grey = True
                    tap('s')
                    time.sleep(2)
                elif grey and colour is not None and colour >= 25:
                    time.sleep(1.5)
                    tap('x')
                    time.sleep(3)
                    break
                else:
                    time.sleep(.5)
            else:
                raise RuntimeError('main menu not reached')
            for key in ('down', 'down', 'down', 'x'):
                tap(key)
            g.wait_log('online menu: view=0')
            time.sleep(3)
            for key in ('down',) * {'quick': 1, 'list': 2, 'public': 3, 'private': 4, 'p2p': 6}[a.entry] + ('x',):
                tap(key)
            if a.entry == 'list':
                g.wait_log(r'view=1 row=-1[\s\S]*view=1 row=0', 30)
                tap('x')
            g.wait_log('choosing the rider')
            time.sleep(10)
            tap('x')
            g.wait_log('the shop opens on bike')
            time.sleep(12)  # finish the ride and let the shop take input
            g.shot('shop')
            if a.commit:
                tap('enter')
                g.wait_log('the shop closed', 25)
                g.wait_log('online menu: view=4', 25)
                assert 'shop cancelled:' not in g.log(), 'START followed the Back path'
                g.shot('room')
                print('PASS: START commits to the room')
                return
            tap('s' if a.back == 'triangle' else a.back)
            g.wait_log('shop cancelled: choosing the rider', 25)
            g.shot('rider')
            time.sleep(2)
            offset = len(g.log())
            tap('s' if a.back == 'triangle' else a.back)
            until = time.monotonic() + 25
            while not re.search(r'rider choice cancelled: ONLINE[\s\S]*online menu: view=0', g.log()[offset:]):
                if not g.alive() or time.monotonic() > until:
                    raise RuntimeError('Back did not return to ONLINE')
                time.sleep(.25)
            time.sleep(4)
            g.shot('online')
            assert g.log().count('the shop opens on bike') == 1, 'the shop opened again'
            assert 'host64 memory:' not in g.log(), 'guest memory fault'
            print('PASS: shop -> rider choice -> ONLINE (' + a.back + ')')
        finally:
            g.stop()


if __name__ == '__main__':
    main()
