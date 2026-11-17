"""What the game's menus change with left/right, done by touch (issue #108, finding 268), on a hidden desktop
with touch controls on at a phone's 20:9 (DHD_TOUCH_MOUSE=1, no controller seen) and posted taps:

    python port/tools/touch_left_right_check.py OUT [--exe EXE] [--scenario shop|audio|all]

- shop: a fresh Career (empty memory card in OUT), a menu script to MOUNTAIN CROSS's shop; a tap closes
  its message. A tap on BIKES takes the cursor into the list; a tap on the heading's right half then
  goes to the next page (Triangle, then Right: BIKE UPGRADES), right again (BONUS PURCHASES), left
  (BIKE UPGRADES) and right (BONUS PURCHASES): one page per tap. The legend's SELECT enters the bonus
  items, a tap on "Music 1" takes the cursor there and a second tap on it opens its buy prompt.
- audio: OPTIONS by a menu script; a tap on AUDIO / OTHER opens it; a tap right of Music Volume's row
  raises the volume one step, two taps left of it lower it two steps.

The menus' mouse trace (DHD_MOUSE_TRACE=1) says what each tap did. Pictures in OUT/<scenario>. Exit 0
when every scenario passes."""
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import touch_continue_check as touch  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OPTIONS_NAV = '0:triangle|cross,15:down,15:down,15:cross,99:cross'   # the title, OPTIONS (screens 15, 2)
SHOP_POPUP = 'vtable 08284190'
AUDIO_POPUP = 'vtable 08284fb8'

# Taps as fractions of the 1600x720 window. The shop and the shell are drawn at the shell's scale (the
# 640-wide view in the middle 960 pixels), popups wider (touch_continue_check.popup_tap_fraction).
HEADING_RIGHT, HEADING_LEFT = (0.36, 0.085), (0.22, 0.085)   # view (170, 18) and (21, 18): INVENTORY 13-187
BIKES = (0.30, 0.14)                                         # view (106, 31): the list's first row
SELECT = (0.326, 0.95)                                       # the page's legend: ✕ SELECT
MUSIC_1 = (0.30, 0.514)                                      # view (106, 115): BONUS PURCHASES' fifth line
AUDIO_ROW = (0.64, 0.56)                                     # OPTIONS' AUDIO / OTHER, view (469, 125)


def headings(text):
    return re.findall(r'  heading ([A-Z ]+?): x', text)


def shop_tap(g, at, wait=4):
    mark = len(g.log())
    touch.tap(g, *at)
    time.sleep(wait)
    return g.log()[mark:]


def shop(exe, out):
    g = hidden_game.Game(exe, out, env=touch.env_for(out, dict(DHD_NAV=touch.SHOP_NAV, DHD_TOUCH_WITH_SCRIPT='1')),
                         name='touch-left-right-shop', shot_window=None)
    try:
        g.window()
        g.wait_log(touch.MESSAGE, 240)
        time.sleep(3)
        shop_tap(g, (0.5, 0.45), 5)
        after = shop_tap(g, BIKES)
        if not re.search(r'  list \w+ vtable 08287190 rows 3 cursor 0', after):
            return 'a tap on BIKES did not take the cursor into the list'
        shown = 'INVENTORY'
        expected = [('right', 'BIKE UPGRADES', 'Triangle, then Right'), ('right', 'BONUS PURCHASES', 'Right'),
                    ('left', 'BIKE UPGRADES', 'Left'), ('right', 'BONUS PURCHASES', 'Right')]
        for side, page, press in expected:
            after = shop_tap(g, HEADING_RIGHT if side == 'right' else HEADING_LEFT)
            said = re.findall(r"the shop's heading at \d+,\d+: (.*)", after)
            pages = headings(after)   # from the list the page shows its heading first (Triangle)
            print('shop: heading %s: %s, pages %s' % (side, said, pages), flush=True)
            if said != [press]:
                return 'the heading\'s %s half pressed %r, not %r' % (side, said, press)
            if not pages or pages[-1] != page or len(set(pages) - {shown}) != 1:
                return 'a tap on the heading\'s %s half showed %r, not %s (one page per tap)' % (side, pages, page)
            shown = page
        touch.picture(g, 'bonus-purchases')
        after = shop_tap(g, SELECT)
        if not re.search(r'  list \w+ vtable 08283f50 rows \d+ cursor 0', after):
            return 'SELECT did not enter the bonus items'
        after = shop_tap(g, MUSIC_1, 6)
        cursors = re.findall(r'  list \w+ vtable 08283f50 rows \d+ cursor (\d+)', after)
        if not cursors or cursors[-1] != '4':
            return 'a tap on Music 1 left the cursor on %r' % cursors
        touch.picture(g, 'music-1')
        after = shop_tap(g, MUSIC_1, 5)
        prompt = re.findall(r'mouse trace: popup \w+ vtable (\w+) rows (\d+)', after)
        print('shop: the second tap on Music 1: %s' % prompt, flush=True)
        touch.picture(g, 'buy-prompt')
        if not prompt or prompt[0][0] == '08284190':
            return 'the second tap on Music 1 opened nothing: %r' % prompt
        return None
    finally:
        g.stop()


def audio(exe, out):
    g = hidden_game.Game(exe, out, env=touch.env_for(out, dict(DHD_NAV=OPTIONS_NAV, DHD_TOUCH_WITH_SCRIPT='1')),
                         name='touch-left-right-audio', shot_window=None)
    try:
        g.window()
        g.wait_log('mouse trace: screen 2', 240)
        time.sleep(3)
        after = shop_tap(g, AUDIO_ROW)
        rows = re.search(AUDIO_POPUP + r' rows 5 cursor 0 at (\d+),(\d+) step (\d+)', after)
        music = re.search(r'  row 2 at 0,0: Music Volume:\s+(\d+)', after)
        if not rows or not music:
            return 'AUDIO / OTHER did not open'
        y = int(rows.group(2)) + 2 * int(rows.group(3)) + 4           # Music Volume's line
        level = int(music.group(1))
        # AUDIO / OTHER is drawn at the shell's scale: view x 625 and 40, right and left of its panel
        steps = [('right', 0.2 + 0.6 * 625 / 640, 1), ('left', 0.2 + 0.6 * 40 / 640, -1), ('left', 0.2 + 0.6 * 40 / 640, -1)]
        for side, fx, sign in steps:
            after = shop_tap(g, (fx, y / 224 + 0.005))
            said = re.findall(r'menu mouse: click at .*', after)
            now = re.findall(r'mouse trace: row 2 now: Music Volume:\s+(\d+)', after)
            print('audio: %s: %s, Music Volume %s -> %s' % (side, said, level, now), flush=True)
            if len(said) != 1 or ('right of it: Right' if side == 'right' else 'left of it: Left') not in said[0]:
                return 'a tap %s of Music Volume was taken as %r' % (side, said)
            if not now or not 0 < (int(now[-1]) - level) * sign <= 3:
                return 'Music Volume went %s -> %r on a tap %s of it (one step: 1-3)' % (level, now, side)
            level = int(now[-1])
        touch.picture(g, 'audio')
        return None
    finally:
        g.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--scenario', choices=('shop', 'audio', 'all'), default='all')
    a = ap.parse_args()
    failed = 0
    for name, run in (('shop', shop), ('audio', audio)):
        if a.scenario not in (name, 'all'):
            continue
        problem = run(a.exe.resolve(), a.out.resolve() / name)
        print(('FAIL %s: %s' % (name, problem)) if problem else 'PASS %s' % name, flush=True)
        failed += problem is not None
    return 1 if failed else 0


if __name__ == '__main__':
    sys.stdout.reconfigure(errors='replace')
    sys.exit(main())
