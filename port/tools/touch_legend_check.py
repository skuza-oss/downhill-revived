"""The game's legend bar by taps and the touch camera button (issues #120 and #121, finding 277), on a hidden
desktop with touch controls on at a phone's 20:9 (DHD_TOUCH_MOUSE=1, no controller seen):

    python port/tools/touch_legend_check.py OUT [--exe EXE] [--scenario menus|pause|camera|all]

- menus: a menu script to the main menu only, then taps only, each on a word of the legend bar at the
  bottom (aimed from the bar's ranges in the mouse trace): NEXT moves the cursor down, PREV up, SELECT
  opens the rider screen; there NEXT and PREV change the rider, INFO opens the rider's page (its bar
  changes) and BACK there is the main menu (the game's own: a pad's Triangle does the same).
- pause: a menu script to a single race (touch_pause_check's), then taps: Pause, MORE; the pause pages
  draw no legend bar (none may be said to show), their legend is the popup's footer: OPTIONS by its row,
  the footer's BACK there is the pause menu again, its SELECT (on RESUME) resumes the race.
- camera: a single race; four taps on the camera button, each must change the view (the window's
  picture after a tap differs from the one before it far more than two pictures with no tap between),
  and of two quick taps the second waits for the game's bail window (SELECT twice), so it steps too.

Each step is confirmed by the menus' mouse trace (DHD_MOUSE_TRACE=1: the cursor, the rider, the
popup) or the touch log. Pictures in OUT/<scenario>. Exit 0 when every scenario passes."""
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import touch_continue_check as touch  # noqa: E402
import touch_pause_check as pause  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
MAIN_NAV = '0:triangle|cross,99:cross'   # the title (and any card notice) to the main menu; then nothing
UP, RIGHT, DOWN, LEFT, TRIANGLE, CROSS, SQUARE = 0x10, 0x20, 0x40, 0x80, 0x1000, 0x4000, 0x8000
BAR = re.compile(r'mouse trace: legend bar (\d+) at -?\d+,-?\d+ \(view \d+x\d+, row ([\d.]+)\)')
BUTTON = re.compile(r'  bar button (\w+): taps -?\d+--?\d+, window ([\d.]+)-([\d.]+)')


def bar(text):
    """The legend bar shown now, by the trace: (index, {button: (x fraction, y fraction)}); (None, {}) when
    none is (the last bar line is followed by "legend bar gone")."""
    lines = text.splitlines()
    for i in range(len(lines) - 1, -1, -1):
        if 'mouse trace: legend bar gone' in lines[i]:
            return None, {}
        m = BAR.search(lines[i])
        if not m:
            continue
        spots = {}
        for after in lines[i + 1:]:
            b = BUTTON.match(after)
            if not b:
                break
            spots[int(b.group(1), 16)] = ((float(b.group(2)) + float(b.group(3))) / 2, float(m.group(2)))
        return int(m.group(1)), spots
    return None, {}


class Run:
    def __init__(self, g):
        self.g = g

    def wait(self, pattern, after, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, self.g.log()[after:])
            if m:
                return m
            time.sleep(0.25)
        return None

    def tap_word(self, button, expect, what):
        """A tap on the bar's word for button; expect (a regex) must follow in the log. None, or the problem."""
        index, spots = bar(self.g.log())
        if button not in spots:
            return '%s: the legend bar %s has no button %04x (%r)' % (what, index, button, sorted(spots))
        mark = len(self.g.log())
        touch.tap(self.g, *spots[button])
        found = self.wait(expect, mark)
        said = re.findall(r'menu mouse: click at .*', self.g.log()[mark:])
        print('%s: bar %s, tap %04x at %.3f,%.3f: %s -> %s' % (what, index, button, spots[button][0], spots[button][1],
                                                                said, found.group(0) if found else None), flush=True)
        if not said or 'the legend bar' not in said[0]:
            return '%s: the tap was not taken as the legend bar\'s word: %r' % (what, said)
        return None if found else '%s: nothing followed the tap (wanted %r)' % (what, expect)


def menus(exe, out):
    g = hidden_game.Game(exe, out, env=touch.env_for(out, dict(DHD_NAV=MAIN_NAV, DHD_TOUCH_WITH_SCRIPT='1')),
                         name='touch-legend-menus', shot_window=None)
    r = Run(g)
    try:
        g.window()
        g.wait_log(r'mouse trace: screen 15 cursor 0', 240)
        g.wait_log(r'mouse trace: legend bar 0 ', 30)
        time.sleep(2)
        touch.picture(g, 'main-menu')
        steps = [(DOWN, r'screen 15 cursor 1', 'main menu NEXT'), (UP, r'screen 15 cursor 0', 'main menu PREV'),
                 (CROSS, r'screen 3 rider \d+ info 0', 'main menu SELECT')]
        for button, expect, what in steps:
            problem = r.tap_word(button, expect, what)
            if problem:
                return problem
            time.sleep(1.5)
        g.wait_log(r'mouse trace: legend bar 1 ', 30)
        time.sleep(2)
        touch.picture(g, 'rider-screen')
        rider = int(re.findall(r'screen 3 rider (\d+) info 0', g.log())[-1])
        others = r'screen 3 rider (?!%d )\d+ info 0' % rider
        for button, expect, what in [(RIGHT, others, 'rider NEXT'), (LEFT, r'screen 3 rider %d info 0' % rider, 'rider PREV'),
                                     (SQUARE, r'screen 3 rider %d info 1' % rider, 'rider INFO')]:
            problem = r.tap_word(button, expect, what)
            if problem:
                return problem
            time.sleep(2)
        g.wait_log(r'mouse trace: legend bar 2 ', 30)
        touch.picture(g, 'rider-info')
        time.sleep(1)   # BACK on the rider's page is the rider screen's own BACK: the main menu (as a pad's Triangle)
        return r.tap_word(TRIANGLE, r'mouse trace: screen 15 cursor', 'rider page BACK')
    finally:
        g.stop()


def pause_menu(exe, out):
    g = hidden_game.Game(exe, out, env=touch.env_for(out, dict(DHD_NAV=pause.NAV, DHD_TOUCH_WITH_SCRIPT='1')),
                         name='touch-legend-pause', scale=2, shot_window=None)
    r = Run(g)
    p = pause.Run(g)
    try:
        g.window()
        g.wait_log(r'touch log: flyover 0', 300)
        spot = re.findall(r'the overlay takes the touches \(Pause at ([\d.]+),([\d.]+) mm\)', g.log())
        if not spot:
            return 'the log does not say where Pause is'
        p.pause = (float(spot[-1][0]) / pause.SCREEN_MM[0], float(spot[-1][1]) / pause.SCREEN_MM[1])
        time.sleep(8)
        p.tap_pause(4)
        menu = p.popup()
        if not menu or menu['vtable'] != pause.PAUSE_MENU:
            return 'the touch Pause button did not open the game\'s pause menu'
        log = p.tap(pause.more_row())
        if "the game's pause menu takes the touches" not in log:
            return 'MORE did not hand the game\'s menu to the taps'
        # the pause pages draw no legend bar: their legend is the popup's footer ("△ BACK ✕ SELECT"),
        # whose words take taps since finding 274 (menu_page.c legend_tap); a bar left from the menus must not
        index, spots = bar(g.log())
        print('pause menu: rows %s, legend bar %s' % (menu['rows'], index), flush=True)
        if index is not None:
            return 'a legend bar is said to show over the pause menu (%s)' % index
        touch.picture(g, 'pause-menu')
        log = p.tap(pause.row_tap(menu, pause.find_row(menu, 'OPTIONS')), 4)
        page = p.popup(log)
        if not page or page['vtable'] == pause.PAUSE_MENU:
            return 'a tap on OPTIONS opened nothing'
        touch.picture(g, 'options')
        for button, expect, what in [(TRIANGLE, r'vtable %s rows' % pause.PAUSE_MENU, 'OPTIONS footer BACK'),
                                     (CROSS, r'the overlay takes the touches', 'pause menu footer SELECT (RESUME)')]:
            mark = len(g.log())
            log = p.tap(pause.legend_tap(g.log(), button), 4)
            found = r.wait(expect, mark)
            print('%s: %s -> %s' % (what, re.findall(r'menu mouse: .*', log), found.group(0) if found else None), flush=True)
            if "the legend's %04x" % button not in log or not found:
                return '%s: the footer word did not press its button' % what
        return None
    finally:
        g.stop()


CAMERA_TAPS = (12.0, 15.0, 18.0, 21.0)


def camera(exe, out):
    """Four camera taps (race seconds), pictures before and after each, and a pair with no tap between; two
    quick taps last (the second waits for the bail window: no bail)."""
    script = ['9.9:down:9:90:40', '10:up:9:90:40']
    for s in CAMERA_TAPS + (26.0, 26.25):
        script += ['%s:down:1:CX:CY' % s, '%s:up:1:CX:CY' % (s + 0.15)]
    first = hidden_game.Game(exe, out, env=touch.env_for(out, dict(DHD_NAV=pause.NAV, DHD_TOUCH_WITH_SCRIPT='1')),
                             name='touch-legend-camera', shot_window=None)
    try:   # where the camera button is on this screen (the same in the run below)
        first.window()
        first.wait_log(r'touch: the camera button at ([\d.]+),([\d.]+) mm', 300)
        cam = re.findall(r'touch: the camera button at ([\d.]+),([\d.]+) mm', first.log())[-1]
    finally:
        first.stop()
    text = ';'.join(script).replace('CX', cam[0]).replace('CY', cam[1])
    g = hidden_game.Game(exe, out, env=touch.env_for(out, dict(DHD_NAV=pause.NAV, DHD_TOUCH_WITH_SCRIPT='1',
                                                              DHD_TOUCH_SCRIPT=text)),
                         name='touch-legend-camera', shot_window=None)
    caps = [9.0, 10.5] + [s + d for s in CAMERA_TAPS for d in (-0.6, 1.6)]
    try:
        g.window()
        g.wait_log(r'touch log: flyover 0', 300)
        end, base = time.time() + 120, None
        while time.time() < end and base is None:
            m = re.findall(r'down 9 at .* race (\d+) ms', g.log())
            if m:
                base = time.time() - int(m[-1]) / 1000.0
            time.sleep(0.02)
        if base is None:
            return 'the race clock never reached the first scripted touch'
        pictures = {}
        for c in sorted(set(caps)):
            while time.time() - base < c:
                time.sleep(0.01)
            pictures[c] = g.print_window('camera-%04.1f' % c)
        while time.time() - base < 29:
            time.sleep(0.2)
        log = g.log()
    finally:
        g.stop()
    from PIL import Image, ImageChops, ImageStat

    def differ(a, b):
        x, y = (Image.open(pictures[k]).convert('L').resize((320, 144)) for k in (a, b))
        return sum(ImageStat.Stat(ImageChops.difference(x, y)).mean)

    still = differ(9.0, 10.5)
    steps = [differ(s - 0.6, s + 1.6) for s in CAMERA_TAPS]
    notes = re.findall(r'touch: camera: .*', log)
    print('camera: no tap %.1f, each tap %s; notes %s' % (still, ['%.1f' % d for d in steps], notes), flush=True)
    if len(notes) != len(CAMERA_TAPS) + 2:
        return 'the camera taps did not all step the view: %r' % notes
    if min(steps) < max(3 * still, 4.0):
        return 'a camera tap left the picture as it was (%.1f vs %.1f with no tap)' % (min(steps), still)
    sends = re.findall(r'touch log: sent (\w+) .* race (\d+) ms', log)
    quick = [(b, int(ms)) for b, ms in sends if int(ms) >= 26000 and int(b, 16) & 1]
    if len(quick) < 2 or not any(ms - quick[0][1] >= 450 for _, ms in quick):
        return 'the second quick camera tap did not wait for the bail window: %r' % quick
    return None


SCENARIOS = {'menus': menus, 'pause': pause_menu, 'camera': camera}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--scenario', choices=list(SCENARIOS) + ['all'], default='all')
    a = ap.parse_args()
    failed = 0
    for name in (SCENARIOS if a.scenario == 'all' else [a.scenario]):
        out = (a.out / name).resolve()
        out.mkdir(parents=True, exist_ok=True)
        problem = SCENARIOS[name](a.exe.resolve(), out)
        print('%s: %s' % (name, ('FAIL: %s' % problem) if problem else 'PASS'), flush=True)
        failed += problem is not None
    return 1 if failed else 0


if __name__ == '__main__':
    sys.stdout.reconfigure(errors='replace')
    sys.exit(main())
