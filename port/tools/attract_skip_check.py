"""The title's attract demo with touch controls on, at a phone's 20:9 (finding 218): SKIP must show over
the demo and a tap on it must end it. Finding 214's unit test set the demo's flag by hand; the real
game never set it (port_in_race leaves the demo out), so SKIP never showed on a phone.

    python port/tools/attract_skip_check.py OUT [--exe EXE] [--scale 4]

One game on a hidden desktop (untimed game lock) with DHD_TOUCH_MOUSE=1, a 1600x720 window and a
phone's size in mm. DHD_TEST_ATTRACT_AFTER=5 starts the demo after 5 s on the title instead of 60.
The log's `touch log: attract 1` / `attract 0` lines say when the demo runs; pictures (presented, the
overlay as a player sees it) in OUT: demo-skip.png (SKIP over the demo), after-skip.png. Exit 0 when
the demo ended by the tap. The key that leaves the memory card screen hides the overlay (a pad was
used), so the first tap only brings SKIP back; the second ends the demo."""
import argparse
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SKIP = ((154.5 - 8 - 15) / 154.5, 0.86)   # SKIP's centre as a fraction of the window: 23 mm in from the right edge of the 154.5 mm screen (touch.c skip_box, finding 226)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--scale', default='4')
    a = ap.parse_args()
    out = a.out.resolve()
    pres = out / 'pres'
    for d in (out / 'card', pres):
        d.mkdir(parents=True, exist_ok=True)
    for f in pres.glob('*.png'):
        f.unlink()
    nopad = dict(SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT='0xFFFF/0xFFFF')   # this PC's pad unseen, as a phone
    env = dict(DHD_LAUNCHER='1', DHD_WINDOW_WIDTH='1600', DHD_WINDOW_HEIGHT='720', DHD_WIDESCREEN='1', DHD_MEMCARD='1',
               DHD_MEMCARD_DIR=str(out / 'card'), SDL_MOUSE_AUTO_CAPTURE='0', DHD_TOUCH_LOG='1', DHD_TOUCH_MOUSE='1',
               DHD_TOUCH_SCREEN_MM='154.5x69.5', DHD_TEST_ATTRACT_AFTER='5', DHD_SCREENSHOT='<unset>',
               DHD_CAPTURE_PRESENTED=str(pres / 'p'), DHD_CAPTURE_AT='0', DHD_CAPTURE_INTERVAL='1', **nopad)

    def latest():
        files = sorted(pres.glob('p-*.png'), key=lambda f: f.stat().st_mtime)
        return files[-2] if len(files) > 1 else None   # the newest may still be written

    def saturation():
        from PIL import Image, ImageStat
        f = latest()
        if not f:
            return None
        try:
            m = ImageStat.Stat(Image.open(f).convert('HSV').resize((64, 48))).mean
            return m[1] if m[2] > 8 else None
        except Exception:
            return None

    def shot(label):
        time.sleep(2.5)
        f = latest()
        if f:
            shutil.copy(f, out / (label + '.png'))

    g = hidden_game.Game(a.exe, out, env=env, name='attract-skip', scale=a.scale, shot_window=None)
    ended = False
    try:
        g.window()
        start, grey = time.time(), False
        while time.time() - start < 120 and 'touch log: attract 1' not in g.log():
            s = saturation()
            if s is not None and s < 25 and not grey:   # the memory card screen: Triangle
                grey = True
                time.sleep(1.0)
                g.key('s')
            time.sleep(0.5)
        g.wait_log(r'touch log: attract 1', 120)
        width, height = g.client()
        x, y = int(SKIP[0] * width), int(SKIP[1] * height)
        for k in range(3):
            g.click(x, y)
            time.sleep(3)
            if 'touch log: attract 0' in g.log():
                break
            if k == 0:
                shot('demo-skip')
        shot('after-skip')
        ended = 'touch log: attract 0' in g.log()
    finally:
        g.stop()
    print('PASS: SKIP ended the attract demo' if ended else 'FAIL: the attract demo did not end by SKIP', flush=True)
    return 0 if ended else 1


if __name__ == '__main__':
    sys.exit(main())
