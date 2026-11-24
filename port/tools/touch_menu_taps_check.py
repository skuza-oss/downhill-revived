"""Taps on the game's value page and its one-row pickers (issues #125 and #118, finding 282), on a hidden
desktop with touch controls on at a phone's 20:9 (DHD_TOUCH_MOUSE=1, no controller seen) and posted taps:

    python port/tools/touch_menu_taps_check.py OUT [--exe EXE] [--scenario audio|pause|all]

- audio: the front end's OPTIONS > AUDIO / OTHER by a tap; a tap on each row's words: a switch (Vibration,
  Stereo Sound) changes, a volume is chosen and keeps its value, and the page stays open after every
  tap. Then the legend's SELECT keeps the values and closes it.
- pause: a single race (touch_pause_check's menu script), Pause, MORE; OPTIONS: each row as above; the
  legend's BACK. MUSIC PLAYLIST: its list chosen by the picker's arrows and words (CUSTOM), USER 1,
  MODIFY, then three songs added from ADD TRACK, a one-row picker: the first song shown (top), the
  song after five taps right of the line and one left of it (middle), and the last (bottom, by taps
  right of the line). Each time the song the picker showed must be the one the
  editor gets, and the tapped song alone.

The menus' mouse trace (DHD_MOUSE_TRACE=1) says which popup is up, the row a picker shows and what each
tap did. Pictures in OUT/<scenario>. Exit 0 when every scenario passes."""
import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import touch_continue_check as touch  # noqa: E402
import touch_left_right_check as left_right  # noqa: E402
import touch_pause_check as pause  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
AUDIO = '08284fb8'                            # OPTIONS > AUDIO / OTHER (D_00284FB8)
SHOWS = re.compile(r'  picker shows row (\d+): (.*)')


def shell_x(x):
    """A front-end popup's view x as a window fraction: drawn at the shell's scale (the 640-wide view in
    the middle 960 of 1600 pixels; touch_left_right_check's AUDIO / OTHER)."""
    return 0.2 + 0.6 * x / 640


def row_at(page, row, to_x):
    x = page['x'] + 20 if page['left'] else page['x']
    return to_x(x), (page['y'] + row * page['step'] + 4) / 224 + 0.005


def value_rows(r, page, to_x, label):
    """A tap on each row of AUDIO / OTHER: a switch changes, a number is chosen only, the page stays."""
    rows = dict(page['rows'])
    for row in sorted(rows):
        log = r.tap(row_at(page, row, to_x), 3)
        said = re.findall(r'menu mouse: .*', log)
        now = [t.strip() for t in re.findall(r'mouse trace: row %d now: (.*)' % row, log)]
        last = r.popup()
        print('%s: row %d %r: %s; now %r' % (label, row, rows[row], said, now), flush=True)
        if 'mouse trace: popup closed' in log or not last or last['vtable'] != AUDIO:
            return None, 'a tap on %r closed AUDIO / OTHER' % rows[row]
        if last['cursor'] != row:
            return None, 'a tap on %r left the cursor on row %d' % (rows[row], last['cursor'])
        number = re.search(r'\d\s*$', rows[row])
        if number and now:
            return None, 'a tap on the volume %r changed it to %r' % (rows[row], now)
        if not number and (not now or now[-1].split()[-1] == rows[row].split()[-1]):
            return None, 'a tap on the switch %r did not change it (%r)' % (rows[row], now)
        if now:
            rows[row] = now[-1]
    touch.picture(r.g, label + '-rows')
    return rows, None


def audio(exe, out):
    g = hidden_game.Game(exe, out, env=touch.env_for(out, dict(DHD_NAV=left_right.OPTIONS_NAV, DHD_TOUCH_WITH_SCRIPT='1')),
                         name='touch-menu-taps-audio', shot_window=None)
    r = pause.Run(g)
    try:
        g.window()
        g.wait_log('mouse trace: screen 2', 240)
        time.sleep(3)
        log = r.tap(left_right.AUDIO_ROW, 4)
        page = r.popup(log)
        if not page or page['vtable'] != AUDIO:
            return 'AUDIO / OTHER did not open'
        rows, problem = value_rows(r, page, shell_x, 'audio')
        if problem:
            return problem
        legend = re.findall(r'  legend button 4000: glyph (-?\d+)-(-?\d+), word to (-?\d+)', g.log())
        glyph, right = int(legend[-1][1]), int(legend[-1][2])
        log = r.tap((shell_x((glyph + right) // 2), 209 / 224), 4)
        print('audio: SELECT: %s' % re.findall(r'menu mouse: .*|mouse trace: popup closed', log), flush=True)
        if 'mouse trace: popup closed' not in log:
            return "the legend's SELECT did not close AUDIO / OTHER"
        log = r.tap(left_right.AUDIO_ROW, 4)   # again: SELECT kept what the taps changed
        again = r.popup(log)
        print('audio: opened again: %r' % (again and again['rows']), flush=True)
        if not again or again['vtable'] != AUDIO or again['rows'] != rows:
            return 'AUDIO / OTHER opened again with %r, not %r' % (again and again['rows'], rows)
        return None
    finally:
        g.stop()


def shown(text):
    """The row the last picker trace showed: (row, text)."""
    found = SHOWS.findall(text)
    return (int(found[-1][0]), found[-1][1].strip()) if found else (None, None)


def picker_tap(r, page, where, wait=2.0):
    """A tap on a picker's line: its words, or near the 512-wide view's left or right edge (a long song's
    words reach 150 pixels from the middle)."""
    x = page['x'] + 20 if page['left'] else page['x']
    x = {'words': x, 'left': 24, 'right': 488}[where]
    return r.tap((pause.view_x(x), (page['y'] + 4) / 224 + 0.005), wait)


def add_song(r, editor_log, moves, label):
    """ADD on the editor's legend, the picker's moves (moves(its row count): taps left / right of its
    line), a tap on its words: the song shown must be the one the editor gets."""
    before = r.popup(editor_log)
    log = r.tap(pause.legend_tap(r.g.log(), 0x2000), 4)
    picker = r.popup(log)
    if not picker or picker['vtable'] == pause.EDITOR or not picker.get('picker'):
        return None, 'ADD did not open the song picker: %r' % (picker and picker['vtable'])
    row, song = shown(log)
    for move in moves(picker['count']):
        log = picker_tap(r, picker, move)
        said = re.findall(r'menu mouse: .*', log)
        now = shown(log)
        if not said or ('Left' if move == 'left' else 'Right') not in said[0] or now[0] is None or now[0] == row:
            return None, 'a tap %s of the picker\'s line: %r, shown %r (was row %r)' % (move, said, now, row)
        row, song = now
    touch.picture(r.g, 'song-%s-shown' % label)
    log = picker_tap(r, picker, 'words', 4)
    editor = r.popup(log)
    said = re.findall(r'menu mouse: .*', log)
    print('songs: %s: shown row %d %r; %s; the editor: %r' % (label, row, song, said, editor and editor['rows']), flush=True)
    if not editor or editor['vtable'] != pause.EDITOR:
        return None, 'a tap on the shown song %r did not go back to the editor' % song
    # the editor's own last row ("○ to Add Track" while empty, then "End of List") is no song
    added = [t for t in editor['rows'].values() if t not in before['rows'].values()
             and not re.search(r'end of list|add track', t, re.I)]
    if editor['count'] != before['count'] + 1 or added != [song]:
        return None, 'a tap on %r added %r (the editor: %r)' % (song, added, editor['rows'])
    touch.picture(r.g, 'song-%s-added' % label)
    return log, None


def songs(r, menu):
    log = r.tap(pause.row_tap(menu, pause.find_row(menu, 'MUSIC')), 4)
    page = r.popup(log)
    if not page or page['vtable'] == pause.PAUSE_MENU:
        return 'a tap on MUSIC PLAYLIST opened nothing'
    print('music page %s%s rows: %s; shows %r' % (page['vtable'], ' (a picker)' if page.get('picker') else '',
                                                   page['rows'], shown(log)), flush=True)
    if page.get('picker'):   # to CUSTOM by the arrows, then its words
        for _ in range(len(page['rows']) + 1):
            if 'CUSTOM' in (shown(r.g.log())[1] or '').upper():
                break
            picker_tap(r, page, 'right')
        if 'CUSTOM' not in (shown(r.g.log())[1] or '').upper():
            return 'the MUSIC PLAYLIST picker never showed CUSTOM'
        log = picker_tap(r, page, 'words', 4)
    else:
        log = r.tap(pause.row_tap(page, pause.find_row(page, 'CUSTOM')), 4)
    custom = r.popup(log)
    if not custom or custom['vtable'] != '08287a98':
        return 'CUSTOM did not open: %r' % (custom and custom['vtable'])
    r.tap(pause.row_tap(custom, 0), 3)   # USER 1
    log = r.tap((pause.view_x(40), 185 / 224), 4)   # its own legend line: MODIFY
    editor = r.popup(log)
    if not editor or editor['vtable'] != pause.EDITOR:
        return 'MODIFY on CUSTOM did not open the playlist editor'
    for label, moves in (('top', lambda n: []), ('middle', lambda n: ['right'] * 5 + ['left']),
                         ('bottom', lambda n: ['right'] * (n - 1))):
        log, problem = add_song(r, log, moves, label)
        if problem:
            return problem
    return None


def pause_run(exe, out):
    g = hidden_game.Game(exe, out, env=touch.env_for(out, dict(DHD_NAV=pause.NAV, DHD_TOUCH_WITH_SCRIPT='1')),
                         name='touch-menu-taps-pause', scale=2, shot_window=None)
    r = pause.Run(g)
    try:
        g.window()
        g.wait_log(r'touch log: flyover 0', 300)
        spot = re.findall(r'the overlay takes the touches \(Pause at ([\d.]+),([\d.]+) mm\)', g.log())
        if not spot:
            return 'the log does not say where Pause is'
        r.pause = (float(spot[-1][0]) / pause.SCREEN_MM[0], float(spot[-1][1]) / pause.SCREEN_MM[1])
        time.sleep(8)
        r.tap_pause(4)
        menu = r.popup()
        if not menu or menu['vtable'] != pause.PAUSE_MENU:
            return "the touch Pause button did not open the game's pause menu"
        log = r.tap(pause.more_row())
        if "the game's pause menu takes the touches" not in log:
            return "MORE did not hand the game's menu to the taps"
        log = r.tap(pause.row_tap(menu, pause.find_row(menu, 'OPTIONS')), 4)
        page = r.popup(log)
        if not page or page['vtable'] != AUDIO:
            return 'OPTIONS in the pause menu is not AUDIO / OTHER: %r' % (page and page['vtable'])
        print('pause options rows: %s' % page['rows'], flush=True)
        _, problem = value_rows(r, page, pause.view_x, 'pause-options')
        if problem:
            return problem
        log = r.tap(pause.legend_tap(g.log(), 0x1000), 4)   # the legend's BACK
        if not r.popup() or r.popup()['vtable'] != pause.PAUSE_MENU:
            return "the legend's BACK did not go back to the pause menu"
        return songs(r, menu)
    finally:
        g.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out', type=Path)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--scenario', choices=('audio', 'pause', 'all'), default='all')
    a = ap.parse_args()
    failed = 0
    for name, run in (('audio', audio), ('pause', pause_run)):
        if a.scenario not in (name, 'all'):
            continue
        problem = run(a.exe.resolve(), a.out.resolve() / name)
        print(('FAIL %s: %s' % (name, problem)) if problem else 'PASS %s' % name, flush=True)
        failed += problem is not None
    return 1 if failed else 0


if __name__ == '__main__':
    sys.stdout.reconfigure(errors='replace')
    sys.exit(main())
