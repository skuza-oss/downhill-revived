"""READY by touch on the race room page (finding 218): a phone could not ready in a P2P room. A local
room server, a scripted host game that makes a P2P room and readies, and a guest game with touch
controls on at a phone's 20:9 shape that joins by code, goes through the shop and taps READY. The
guest must log `READY on by tap` and `the room has this player ready`, and the race must start.

    python port/tools/room_ready_touch_check.py OUT [--exe EXE] [--scale 2] [--race]

Both games on hidden desktops (one untimed game lock), `DHD_NET_CREATE`-free: the menus' own way,
driven by DHD_AUTOPAD_FRAMES (cross_race.py's P2P navs) with DHD_NET_SCRIPTED's code and shop
switches; the guest's touches go through DHD_TOUCH_WITH_SCRIPT. Pictures of the guest's room page
before and after the tap in OUT/guest (`room-before.png`, `room-after.png`, the presented picture).
`--race` waits for the race's results too. Exit 0 when READY took and the race started."""
import argparse
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock  # noqa: E402
import hidden_game  # noqa: E402
import local_server  # noqa: E402
from cross_race import P2P_GUEST_NAV, P2P_HOST_NAV  # noqa: E402
from online_riders import PEDAL, rider_env  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
READY = (0.435, 0.778)   # the room bar's READY cell, a fraction of a 20:9 window (the phone's own picture)
# cross_race's guest nav ends with a Cross that lands in the room on READY: without it only the tap readies
GUEST_NAV = P2P_GUEST_NAV.replace(',2930:8:cross', '')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--scale', default='2')
    ap.add_argument('--race', action='store_true', help='also wait for the race and its results')
    a = ap.parse_args()
    out = a.out.resolve()
    for d in ('hcard', 'gcard', 'gpres'):
        (out / d).mkdir(parents=True, exist_ok=True)
    for f in (out / 'gpres').glob('*.png'):
        f.unlink()
    common = dict(DHD_LAUNCHER='1', DHD_NET_SCRIPTED='1', DHD_NET_LOG_CODE='1', DHD_FPS='60', DHD_MEMCARD='1',
                  DHD_ONLINE_MENU_TRACE='1', DHD_AUTO_UPDATE='off', DHD_NET_TEST_FINISH='25', **rider_env(PEDAL))
    host_env = dict(common, DHD_AUTOPAD_FRAMES=P2P_HOST_NAV, DHD_NET_TEST_READY='1', DHD_NET_NAME='PCHOST',
                    DHD_MEMCARD_DIR=str(out / 'hcard'))
    ok = False
    with local_server.running() as url, game_lock.held('room-ready-touch'):
        host = hidden_game.Game(a.exe, out / 'host', env=dict(host_env, DHD_NET_SERVER=url), name='room-host', interval=5)
        guest = None
        try:
            host.wait_log(r'online menu: room (\w{7}) \(host\)', 150)
            code = re.search(r'online menu: room (\w{7}) \(host\)', host.log()).group(1)
            genv = dict(common, DHD_NET_SERVER=url, DHD_AUTOPAD_FRAMES=GUEST_NAV, DHD_NET_TEST_CODE=code,
                        DHD_NET_NAME='RIDER', DHD_MEMCARD_DIR=str(out / 'gcard'), DHD_WINDOW_WIDTH='1600',
                        DHD_WINDOW_HEIGHT='720', DHD_WIDESCREEN='1', SDL_MOUSE_AUTO_CAPTURE='0', DHD_TOUCH_LOG='1',
                        DHD_TOUCH_MOUSE='1', DHD_TOUCH_SCREEN_MM='154.5x69.5', DHD_TOUCH_WITH_SCRIPT='1',
                        SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT='0xFFFF/0xFFFF', DHD_SCREENSHOT='<unset>',
                        DHD_CAPTURE_PRESENTED=str(out / 'gpres' / 'p'), DHD_CAPTURE_AT='0', DHD_CAPTURE_INTERVAL='1')
            guest = hidden_game.Game(a.exe, out / 'guest', env=genv, name='room-guest', scale=a.scale, shot_window=None)

            def shot(label):
                time.sleep(2.0)
                files = sorted((out / 'gpres').glob('p-*.png'), key=lambda f: f.stat().st_mtime)
                if len(files) > 1:
                    shutil.copy(files[-2], out / 'guest' / (label + '.png'))

            guest.window()
            guest.wait_log(r'P2P room: the address notice shown', 240)
            time.sleep(5)
            shot('room-before')
            width, height = guest.client()
            x, y = int(READY[0] * width), int(READY[1] * height)
            for _ in range(3):   # a posted tap can lose its up (TESTING.md); READY toggles, so tap once more only if it didn't take
                guest.click(x, y)
                time.sleep(3)
                if 'online menu: READY on by tap' in guest.log():
                    break
            shot('room-after')
            took = 'online menu: READY on by tap' in guest.log()
            guest.wait_log(r'online menu: room \w+: the race starts', 60)
            ok = took and 'the room has this player ready' in guest.log()
            if a.race:
                guest.wait_log(r"the room's results of race|back to the room after", 300)
                shot('results')
        finally:
            if guest:
                guest.stop()
            host.stop()
    print('PASS: READY by tap took and the race started' if ok else 'FAIL: READY by tap did not take', flush=True)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
