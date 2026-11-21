"""The game's own pause pages by touch (issue #118, finding 274), on a hidden desktop with touch controls on
at a phone's 20:9 (DHD_TOUCH_MOUSE=1, no controller seen) and posted taps only:

    python port/tools/touch_pause_check.py OUT [--exe EXE] [--scale N]

A menu script (state_trace_run's RACE_NAV, without its Cross in the flight) takes the menus to a single
race; from the race on only taps: the touch Pause button opens the overlay's PAUSED rows, MORE hands the
game's own pause menu to the taps. Then MUSIC PLAYLIST: CUSTOM, a song off; Pause (the menu's Triangle)
back to the pause menu. OPTIONS: a tap right of Music Volume's row raises it a step; Pause back. TRICK
LIST opens; Pause back. Pause on the pause menu itself resumes the race (the overlay takes the touches
again). Then Pause, MORE, RESUME tapped in the game's menu resumes too.

The menus' mouse trace (DHD_MOUSE_TRACE=1) says which popup is up and what each tap did. Pictures in OUT.
Exit 0 when it all passes."""
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import state_trace_run  # noqa: E402
import touch_continue_check as touch  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
# RACE_NAV to the race's load; its last step (Cross in the flight, again every 30 fields) is left out, so
# no scripted press reaches the pause menu; the last step here waits for a screen that never comes
NAV = state_trace_run.RACE_NAV.rsplit(',', 1)[0] + ',99:cross'
SCREEN_MM = (154.5, 69.5)                     # touch_continue_check.env_for's DHD_TOUCH_SCREEN_MM
PAUSE_MENU = '08285040'                       # the game's pause menu (func_0015E130)
EDITOR = '08287a10'                           # MUSIC PLAYLIST > CUSTOM > MODIFY: the playlist editor (D_0043D2A0)
POPUP = re.compile(r'mouse trace: popup (\w+) vtable (\w+) rows (\d+) cursor (-?\d+) at (-?\d+),(-?\d+) '
                   r'step (-?\d+) align (\d)')
ROW = re.compile(r'  row (\d+) at -?\d+,-?\d+(?: \(off\))?: (.*)')
TRACE = ('menu mouse', 'mouse trace', '  row', '  legend at')


def more_row():
    """The PAUSED rows' MORE as a window fraction (touch.c pause_row_box, ROW_MORE: BAIL OUT's right half)."""
    w, h = SCREEN_MM
    rh = max(9.0, h * 0.124)
    top = h * 0.05 + h * 0.08 + 6
    return (w / 2 + w * 0.15) / w, (top + rh * 4.5 + 2) / h


def popups(text):
    """Each popup the trace showed, in order."""
    out = []
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = POPUP.search(line)
        if not m:
            continue
        rows = {}
        for after in lines[i + 1:]:
            r = ROW.match(after)
            if r:
                rows[int(r.group(1))] = r.group(2).strip()
            elif not after.startswith('  '):
                break
        out.append(dict(vtable=m.group(2), count=int(m.group(3)), cursor=int(m.group(4)), x=int(m.group(5)),
                        y=int(m.group(6)), step=int(m.group(7)), left=m.group(8) == '1', rows=rows))
    return out


def view_x(x):
    """A popup's view x as a fraction of this 1600x720 window: over a race the popups are laid out in a
    512-wide view whose middle (256) is the window's, 846 view pixels across the window (measured: 0.5 is
    256, 0.676 is 405)."""
    return 0.5 + (x - 256) / 846


def row_tap(popup, row, x=None):
    """A tap on a popup row's words (centred at its x; left-aligned: 20 right of it), or at view x."""
    if x is None:
        x = popup['x'] + 20 if popup['left'] else popup['x']
    return view_x(x), (popup['y'] + row * popup['step'] + 4) / 224 + 0.005


def legend_tap(text, button):
    """A tap on the middle of a button's word in the last legend the trace measured (finding 266's ranges)."""
    found = re.findall(r'  legend button %04x: glyph (-?\d+)-(-?\d+), word to (-?\d+)' % button, text)
    left, glyph, right = map(int, found[-1])
    return view_x((glyph + right) // 2), 209 / 224


def find_row(popup, word):
    for k, text in popup['rows'].items():
        if word.upper() in text.upper():
            return k
    return None


def traced(log):
    return '\n'.join(line for line in log.splitlines() if line.startswith(TRACE))


class Run:
    def __init__(self, g):
        self.g = g
        self.pause = None

    def tap(self, at, wait=3.0):
        mark = len(self.g.log())
        touch.tap(self.g, *at)
        time.sleep(wait)
        return self.g.log()[mark:]

    def tap_pause(self, wait=3.0):
        return self.tap(self.pause, wait)

    def popup(self, text=None):
        """The last popup the trace showed (in text, else in the whole log)."""
        found = popups(self.g.log() if text is None else text)
        return found[-1] if found else None

    def back_to_menu(self, what):
        """Pause on a page: the menu's Triangle, the game's pause menu again (and no click reached it)."""
        log = self.tap_pause(4)
        if 'menu mouse: click' in log:
            return None, 'the Pause tap on %s also clicked the game\'s menu: %r' % (what, re.findall(r'menu mouse: .*', log))
        menu = self.popup()
        if not menu or menu['vtable'] != PAUSE_MENU:
            return None, 'Pause on %s did not go back to the pause menu (%r)' % (what, menu and menu['vtable'])
        return menu, None


def check(exe, out, scale):
    g = hidden_game.Game(exe, out, env=touch.env_for(out, dict(DHD_NAV=NAV, DHD_TOUCH_WITH_SCRIPT='1')),
                         name='touch-pause', scale=scale, shot_window=None)
    r = Run(g)
    try:
        g.window()
        g.wait_log(r'touch log: flyover 0', 300)   # the flight over the track played out
        spot = re.findall(r'the overlay takes the touches \(Pause at ([\d.]+),([\d.]+) mm\)', g.log())
        if not spot:
            return 'the log does not say where Pause is'
        r.pause = (float(spot[-1][0]) / SCREEN_MM[0], float(spot[-1][1]) / SCREEN_MM[1])
        time.sleep(8)   # past the countdown: the race runs
        r.tap_pause(4)
        menu = r.popup()
        if not menu or menu['vtable'] != PAUSE_MENU:
            return 'the touch Pause button did not open the game\'s pause menu'
        touch.picture(g, 'paused-rows')
        r.g.print_window('paused-rows-window')
        log = r.tap(more_row())
        if "the game's pause menu takes the touches" not in log:
            return 'MORE did not hand the game\'s menu to the taps: %r' % re.findall(r'touch: .*', log)
        print('pause menu rows: %s' % menu['rows'], flush=True)

        # MUSIC PLAYLIST
        log = r.tap(row_tap(menu, find_row(menu, 'MUSIC')), 4)
        page = r.popup(log)
        if not page or page['vtable'] == PAUSE_MENU:
            return 'a tap on MUSIC PLAYLIST opened nothing'
        print('music page %s rows: %s' % (page['vtable'], page['rows']), flush=True)
        touch.picture(g, 'music-playlist')
        problem = music_song_off(r, page)
        if problem:
            return problem
        r.g.print_window('music-window')
        for _ in range(3):   # back page by page to the pause menu
            menu, problem = r.back_to_menu('MUSIC PLAYLIST')
            if menu:
                break
        if problem:
            return problem

        # OPTIONS: Music Volume up by a tap right of its row
        log = r.tap(row_tap(menu, find_row(menu, 'OPTIONS')), 4)
        page = r.popup(log)
        if not page or page['vtable'] == PAUSE_MENU:
            return 'a tap on OPTIONS opened nothing'
        print('options page %s rows: %s' % (page['vtable'], page['rows']), flush=True)
        volume = find_row(page, 'Music Volume')
        if volume is None:
            return 'no Music Volume row on OPTIONS: %r' % page['rows']
        level = int(re.search(r'(\d+)\s*$', page['rows'][volume]).group(1))
        log = r.tap(row_tap(page, volume, 500), 4)   # right of the panel behind the rows: Right
        now = re.findall(r'mouse trace: row %d now: Music Volume:\s+(\d+)' % volume, log)
        print('options: %s; Music Volume %d -> %s' % (re.findall(r'menu mouse: click at .*', log), level, now), flush=True)
        if not now or not 0 < int(now[-1]) - level <= 3:
            return 'Music Volume went %d -> %r on a tap right of it' % (level, now)
        touch.picture(g, 'options-volume')
        menu, problem = r.back_to_menu('OPTIONS')
        if problem:
            return problem

        # TRICK LIST opens and closes
        log = r.tap(row_tap(menu, find_row(menu, 'TRICK')), 4)
        page = r.popup(log)
        print('trick list: %r' % ((page['vtable'], page['rows']) if page else None,), flush=True)
        if not page or page['vtable'] == PAUSE_MENU:
            return 'a tap on TRICK LIST opened nothing'
        touch.picture(g, 'trick-list')
        menu, problem = r.back_to_menu('TRICK LIST')
        if problem:
            return problem

        # Pause on the pause menu: the race goes on
        log = r.tap_pause(4)
        if 'the overlay takes the touches' not in log or 'menu mouse: click' in log:
            return 'Pause on the pause menu did not resume the race: %r' % re.findall(r'touch: .*|menu mouse: .*', log)
        # again, and RESUME tapped in the game's menu
        time.sleep(3)
        r.tap_pause(4)
        menu = r.popup()
        log = r.tap(more_row())
        if "the game's pause menu takes the touches" not in log:
            return 'the second MORE did not hand the menu over'
        log = r.tap(row_tap(menu, find_row(menu, 'RESUME')), 4)
        if 'the overlay takes the touches' not in log:
            return 'RESUME tapped in the game\'s menu did not resume: %r' % re.findall(r'touch: .*|menu mouse: .*', log)
        return None
    finally:
        g.stop()


def music_song_off(r, page):
    """MUSIC PLAYLIST: CUSTOM, then a song off (exploring: every step's trace printed)."""
    log = r.tap(row_tap(page, find_row(page, 'CUSTOM')), 4)
    print('music: CUSTOM:\n%s' % traced(log), flush=True)
    touch.picture(r.g, 'music-custom')
    songs = r.popup(log)
    if not songs:
        return 'CUSTOM opened nothing'
    r.tap(row_tap(songs, 0), 4)   # USER 1: the cursor there
    log = r.tap((view_x(70), 185 / 224), 4)   # the page's own legend line: MODIFY
    print('music: MODIFY:\n%s' % traced(log), flush=True)
    editor = r.popup(log)
    if 'CUSTOM legend: MODIFY' not in log or not editor or editor['vtable'] != EDITOR:
        return 'MODIFY on CUSTOM did not open the playlist editor'
    touch.picture(r.g, 'music-editor')
    # ADD (Circle, the editor's legend): the songs; a tap on the first adds it to USER 1
    log = r.tap(legend_tap(log, 0x2000), 4)
    jukebox = r.popup(log)
    print('music: ADD:\n%s' % traced(log), flush=True)
    if "the legend's 2000" not in log or not jukebox or jukebox['vtable'] == EDITOR:
        return 'ADD on the playlist editor\'s legend opened nothing'
    song = jukebox['rows'].get(0)
    log = r.tap(row_tap(jukebox, 0), 4)
    editor = r.popup(log)
    print('music: a song:\n%s' % traced(log), flush=True)
    if not editor or editor['vtable'] != EDITOR or editor['count'] < 2:
        return 'a tap on the song %r did not add it to the list' % song
    touch.picture(r.g, 'music-song-added')
    # DELETE (R1): the song off the list again (the cursor on it)
    log = r.tap(row_tap(editor, 0), 3)   # the editor: a tap on another row only moves its cursor
    if "the playlist editor's cursor to row 0" not in log or r.popup()['vtable'] != EDITOR:
        return 'a tap on the added song did not just take the cursor there: %r' % re.findall(r'menu mouse: .*', log)
    log = r.tap(legend_tap(r.g.log(), 0x0800), 4)
    editor = r.popup(log)
    print('music: DELETE:\n%s' % traced(log), flush=True)
    if "the legend's 0800" not in log or not editor or editor['count'] != 1:
        return 'DELETE (R1) on the legend did not take the song off: %r' % (editor and editor['rows'])
    touch.picture(r.g, 'music-song-off')
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--scale', type=int, default=2)
    a = ap.parse_args()
    problem = check(a.exe.resolve(), a.out.resolve(), a.scale)
    print(('FAIL: %s' % problem) if problem else 'PASS', flush=True)
    return 1 if problem else 0


if __name__ == '__main__':
    sys.stdout.reconfigure(errors='replace')
    sys.exit(main())
