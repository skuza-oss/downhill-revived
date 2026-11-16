"""The game's one-button messages answered by touch (issue #105, finding 266), on a hidden desktop with
touch controls on at a phone's 20:9 (DHD_TOUCH_MOUSE=1, no controller seen) and posted taps:

    python port/tools/touch_continue_check.py OUT [--exe EXE] [--scenario shop|card|all]

- shop: a fresh Career (empty memory card in OUT), a menu script (DHD_NAV) to CAREER > SPECIALIZED
  CAREER > MOUNTAIN CROSS; the shop opens with "MOUNTAIN CROSS / ENTRY BIKES ACQUIRED / PRESS X TO
  CONTINUE". One tap in the middle must close it, and the shop behind it must take nothing from
  the same tap (no other popup, its list not moved).
- card: no memory card (DHD_MEMCARD=0): the boot stops at "memory card not found" (✕ RETRY,
  △ CANCEL), four times, one tap each: on CANCEL's left edge, its glyph, the word's middle and its
  right edge, aimed from the legend's ranges in the trace (RETRY's range must end before CANCEL's).
  Each must press Triangle and close the notice; after the middle one the title's PRESS ✕ TO START
  must still wait (one tap answers one box), and a second tap must open the main menu.

The menus' mouse trace (DHD_MOUSE_TRACE=1) says which popup is up. Pictures in OUT/<scenario>.
Exit 0 when every scenario passes."""
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
# The title, then SINGLE PLAYER, the rider, START NEW GAME, CAREER, SPECIALIZED CAREER, MOUNTAIN
# CROSS (the shell's screens 15, 3, 16, 1, 12, 17; menu_nav.h). The last step waits for a screen
# that never comes, so the script presses nothing more.
SHOP_NAV = '0:triangle|cross,15:cross,3:cross,16:cross,1:down,1:cross,12:down,12:cross,17:down,17:cross,99:cross'
MESSAGE = 'vtable 08283c20 rows 4'          # the shop's message popup (D_003638A0)
CARD = 'vtable 08286a28 rows 7'             # "memory card ... not found" (✕ RETRY, △ CANCEL)


def env_for(out, extra):
    (out / 'card').mkdir(parents=True, exist_ok=True)
    env = dict(DHD_MOUSE_TRACE='1', DHD_MEMCARD='1', DHD_MEMCARD_DIR=str(out / 'card'), DHD_WINDOW_WIDTH='1600',
               DHD_WINDOW_HEIGHT='720', DHD_WIDESCREEN='1', SDL_MOUSE_AUTO_CAPTURE='0', DHD_TOUCH_LOG='1',
               DHD_TOUCH_MOUSE='1', DHD_TOUCH_SCREEN_MM='154.5x69.5',
               SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT='0xFFFF/0xFFFF')
    env.update(extra)
    return env


def tap(g, fx, fy):
    """A tap at a fraction of the window; the menus' 4:3 picture is its middle 960 of 1600."""
    width, height = g.client()
    g.click(int(fx * width), int(fy * height))


def picture(g, label):
    """The newest finished capture as OUT/LABEL.png."""
    import os
    from PIL import Image
    time.sleep(1.0)
    files = sorted(g.shots.glob('shot-*.bmp'), key=os.path.getmtime)
    if len(files) > 1:
        Image.open(files[-2]).save(g.out / (label + '.png'))


def since(g, mark):
    return g.log()[mark:]


def shop(exe, out):
    g = hidden_game.Game(exe, out, env=env_for(out, dict(DHD_NAV=SHOP_NAV, DHD_TOUCH_WITH_SCRIPT='1')),
                         name='touch-continue-shop', shot_window=None)
    try:
        g.window()
        g.wait_log(MESSAGE, 240)
        time.sleep(3)
        picture(g, 'message')
        if 'mouse trace: popup' in g.log().split(MESSAGE)[-1]:
            return 'the message closed by itself'
        mark = len(g.log())
        tap(g, 0.5, 0.45)
        time.sleep(6)
        picture(g, 'after-tap')
        after = since(g, mark)
        taps = re.findall(r'menu mouse: click at .*', after)
        popups = re.findall(r'mouse trace: popup \w+ vtable (\w+)', after)
        print('shop: taps', taps, 'popups after', popups, flush=True)
        if len(taps) != 1 or 'a message' not in taps[0] or 'the message takes Cross' not in after:
            return 'the tap was not taken as the message\'s Cross: %r' % taps
        if not popups or set(popups) != {'08284190'}:
            return 'after the tap: %r (expected the shop alone)' % popups
        # the shop builds its list on BIKES, then puts it on its INVENTORY header (-1) by itself
        cursors = re.findall(r'  list \w+ vtable \w+ rows \d+ cursor (-?\d+)', after)
        if not cursors or cursors[-1] != '-1':
            return 'the shop\'s list is not on its header (%r): the tap reached the shop too' % cursors
        return None
    finally:
        g.stop()


# A popup's view x from a tap's place across this 1600x720 window: popups in widescreen are drawn
# wider than the 4:3 shell (measured: 0.5 of the width is x 320, 0.6125 is 519, 0.5678 is 439).
POPUP_X_PER_WIDTH = 1760


def popup_tap_fraction(x):
    return 0.5 + (x - 320) / POPUP_X_PER_WIDTH


# CANCEL's places a tap must take as Triangle, from its range in the trace (glyph left-right, word
# right end): 2 pixels inside its left and right edges, the glyph's middle, the word's middle.
CANCEL_SPOTS = {
    'left-edge': lambda left, glyph, right: left + 2,
    'glyph': lambda left, glyph, right: (left + glyph) // 2,
    'middle': lambda left, glyph, right: (glyph + right) // 2,
    'right-edge': lambda left, glyph, right: right - 2,
}


def card(exe, out, spot='middle', title=True):
    g = hidden_game.Game(exe, out, env=env_for(out, dict(DHD_MEMCARD='0')), name='touch-continue-card', shot_window=None)
    try:
        g.window()
        g.wait_log(CARD, 120)
        time.sleep(3)
        picture(g, 'card-notice')
        notice = g.log().split(CARD)[-1]
        cancel = re.search(r'legend button 1000: glyph (-?\d+)-(-?\d+), word to (-?\d+), taps (-?\d+)-(-?\d+)', notice)
        retry = re.search(r'legend button 4000: glyph (-?\d+)-(-?\d+), word to (-?\d+), taps (-?\d+)-(-?\d+)', notice)
        if not cancel or not retry:
            return 'the trace gave no RETRY and CANCEL ranges'
        left, glyph, right, tap_from, tap_to = map(int, cancel.groups())
        if int(retry.group(5)) > tap_from:
            return 'RETRY\'s range reaches into CANCEL\'s: %s %s' % (retry.groups(), cancel.groups())
        x = CANCEL_SPOTS[spot](left, glyph, right)
        print('card: CANCEL glyph %d-%d, word to %d; tap %s at x %d' % (left, glyph, right, spot, x), flush=True)
        mark = len(g.log())
        tap(g, popup_tap_fraction(x), 0.94)   # the legend's row (y 210 of 224)
        time.sleep(5)
        after = since(g, mark)
        print('card: after the tap', re.findall(r'menu mouse: .*|mouse trace: .*', after), flush=True)
        landed = re.search(r'menu mouse: click at (-?\d+),', after)
        if not landed or not left <= int(landed.group(1)) < right:
            return 'the tap landed at %s, off CANCEL (%d-%d): the window-to-popup scale moved' % (
                landed.group(1) if landed else None, left, right)
        if 'the message takes Triangle' not in after:
            return 'the tap on CANCEL (%s) was not Triangle' % spot
        if 'popup closed' not in after:
            return 'the notice did not close'
        if not title:
            return None
        picture(g, 'title')
        if 'mouse trace: screen 15' in after:
            return 'the same tap also passed the title'
        mark = len(g.log())
        tap(g, 0.5, 0.5)
        time.sleep(5)
        if 'mouse trace: screen 15' not in since(g, mark):
            return 'a tap on the title did not open the main menu'
        picture(g, 'main-menu')
        return None
    finally:
        g.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--scenario', choices=('shop', 'card', 'all'), default='all')
    a = ap.parse_args()
    failed = 0
    runs = [('shop', lambda out: shop(a.exe.resolve(), out))]
    runs += [('card-' + spot, lambda out, spot=spot: card(a.exe.resolve(), out, spot, title=spot == 'middle'))
             for spot in CANCEL_SPOTS]
    for name, run in runs:
        if a.scenario not in (name.split('-')[0], 'all'):
            continue
        problem = run(a.out.resolve() / name)
        print(('FAIL %s: %s' % (name, problem)) if problem else 'PASS %s' % name, flush=True)
        failed += problem is not None
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
