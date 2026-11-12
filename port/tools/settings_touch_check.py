"""OPTIONS > SETTINGS with touch controls on, at a phone's 20:9 (finding 211): the page's legend must say
what a touch player can do (tap a tab's name), not the keyboard's A / D, and a tap on a tab's name
must change the tab.

    python port/tools/settings_touch_check.py OUT [--exe EXE] [--scale 2] [--keyboard]

One game on a hidden desktop (untimed game lock) with DHD_TOUCH_MOUSE=1 (the mouse is a finger; the
host sets DHD_TOUCH=mouse like a phone's android), a 1600x720 window (20:9) and a phone's size in mm.
Pictures in OUT: settings.png (the page as opened), tapped.png (after a posted tap on the Graphics tab's
name), read by eye. The log's `touch:` lines show the tap (DHD_TOUCH_LOG=1).
`--exe` another build (a release's dhd.exe needs DHD_LAUNCHER=1, set here); `--keyboard` runs without
touch controls (the keyboard's legend must stay)."""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
# the Graphics tab's name on the page, as a fraction of the picture (a 20:9 screen: widescreen's
# whole width; measured from the phone's own 2400x1080 capture of the page)
GRAPHICS = (0.404, 0.302)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--scale', default='2')
    ap.add_argument('--keyboard', action='store_true', help='no touch controls: the keyboard legend stays')
    a = ap.parse_args()
    # no controller is seen (a phone has none; this PC's own pad would put the pad's L1 / R1 in the legend)
    nopad = dict(SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT='0xFFFF/0xFFFF')  # SDL ignores every device but this one
    env = dict(DHD_LAUNCHER='1', DHD_WINDOW_WIDTH='1600', DHD_WINDOW_HEIGHT='720', DHD_MEMCARD='1', DHD_WIDESCREEN='1',
               DHD_MEMCARD_DIR=str(a.out.resolve() / 'card'), SDL_MOUSE_AUTO_CAPTURE='0', DHD_TOUCH_LOG='1', **nopad)
    if not a.keyboard:
        env.update(DHD_TOUCH_MOUSE='1', DHD_TOUCH_SCREEN_MM='154.5x69.5')
    (a.out / 'card').mkdir(parents=True, exist_ok=True)
    g = hidden_game.Game(a.exe, a.out, env=env, name='settings-touch', scale=a.scale, shot_window=None)
    try:
        hidden_game.to_main(g)
        g.keys('down', 'down', 'x')   # main menu: OPTIONS
        time.sleep(2)
        g.key('x')                    # its first row: SETTINGS (REVIVED SETTINGS on a PC)
        time.sleep(2)
        g.shot('settings', settle=1.5)
        width, height = g.client()
        x, y = int(GRAPHICS[0] * width), int(GRAPHICS[1] * height)
        for _ in range(2):            # a posted tap can miss its `up` (TESTING.md): a second tap on the
            g.click(x, y)             # same tab changes nothing
            time.sleep(1.0)
        g.shot('tapped', settle=1.5)
        print('tapped the Graphics tab at', x, y, 'of', width, 'x', height, flush=True)
    finally:
        g.stop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
