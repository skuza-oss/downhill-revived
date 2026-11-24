"""Classic's six shoulder buttons and the layout editor's HIDE (issues #126 and #128, finding 283), on a
hidden desktop with touch controls on at a phone's 20:9 (DHD_TOUCH_MOUSE=1, no controller seen):

    python port/tools/touch_layout_check.py OUT [--exe EXE] [--scenario shoulders|hide|pictures|all]

- shoulders: a single race with Classic pad (state_trace_run's race by a menu script); each of L1, L2,
  L1+L2, R1, R2, R1+R2 is touched at its centre in turn by the race clock (DHD_TOUCH_SCRIPT, where the
  log's "touch: controls" line says it is), and the pad state sent while it is held must carry its
  shoulder buttons and no other; a picture of the race with the six.
- hide: the layout editor alone (the launcher's "Edit the touch layout", DHD_TOUCH_EDITOR=1) with a
  settings file in OUT, by posted taps: HIDE, L3, R1+R2 (both hidden, HIDE still on: a picture with
  their ghosts), DONE; dhd.ini's touch_edits must say both are hidden. Started again, the log must name
  both hidden; HIDE, L3's ghost (shown again), HIDE, DONE: only R1+R2 hidden. A race with that file: a
  touch where R1+R2 is sends nothing, L3 sends L3. The editor again: RESET, DONE: touch_edits empty,
  and the next start hides nothing.
- pictures: the default Classic layout in the editor alone, either hand, on the Mi 11i's shape (its
  cutout band on the right, a 5.2 mm top strip) and on a 20:9 screen with no cutout.

The touch log (DHD_TOUCH_LOG=1) says where each control is and what each touch sent. Pictures in
OUT/<scenario>. Exit 0 when every scenario passes."""
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
L1, R1, L2, R2, L3 = 0x0400, 0x0800, 0x0100, 0x0200, 0x0002
SHOULDERS = [('l1', L1), ('l2', L2), ('l1l2', L1 | L2), ('r1', R1), ('r2', R2), ('r1r2', R1 | R2)]
CONTROLS = re.compile(r'touch: controls(.*)')
SENT = re.compile(r'touch log: sent (\w+) .* race (\d+) ms')


def controls(log):
    """The newest "touch: controls" line: {name: (x mm, y mm) or None when hidden}."""
    found = CONTROLS.findall(log)
    if not found:
        return {}
    out, words = {}, found[-1].split()
    for i in range(0, len(words) - 1, 2):
        out[words[i]] = None if words[i + 1] == 'hidden' else tuple(float(v) for v in words[i + 1].split(','))
    return out


def editor_bar(log):
    m = re.findall(r'touch: editor bar(.*)', log)
    words = m[-1].split() if m else []
    return {words[i]: tuple(float(v) for v in words[i + 1].split(',')) for i in range(0, len(words) - 1, 2)}


def tap_mm(g, x, y, mm=(154.5, 69.5)):
    width, height = g.client()
    g.click(int(x / mm[0] * width), int(y / mm[1] * height))
    time.sleep(0.4)


def touch_edits(ini):
    if not ini.exists():
        return None
    m = re.search(r'^touch_edits[ \t]*=[ \t]*(.*)$', ini.read_text(errors='replace'), re.M)
    return m.group(1).strip().strip('"') if m else ''


def editor(exe, out, name, ini, extra=None):
    env = touch.env_for(out, dict(DHD_TOUCH_EDITOR='1', DHD_SETTINGS=str(ini), **(extra or {})))
    g = hidden_game.Game(exe, out, env=env, name=name, shot_window=None)
    g.window()
    g.wait_log(r'touch: editor bar ', 120)
    time.sleep(1.0)
    return g


def done(g, bar):
    tap_mm(g, *bar['done'])
    end = time.time() + 20
    while time.time() < end and 'is done; the game ends' not in g.log():
        time.sleep(0.25)
    return 'is done; the game ends' in g.log()


def race_sends(exe, out, name, taps, extra=None):
    """A Classic race where each (name, x, y) is held 0.4 s, one a second from 10 s: {name: buttons sent
    while held (or-ed)}; and a picture of the race before the first."""
    script = []
    for i, (_, x, y) in enumerate(taps):
        s = 10.0 + i
        script += ['%.2f:down:%d:%.1f:%.1f' % (s, i + 1, x, y), '%.2f:up:%d:%.1f:%.1f' % (s + 0.4, i + 1, x, y)]
    env = touch.env_for(out, dict(DHD_NAV=pause.NAV, DHD_TOUCH_WITH_SCRIPT='1', DHD_TOUCH_SCRIPT=';'.join(script),
                                  **(extra or {})))
    g = hidden_game.Game(exe, out, env=env, name=name, shot_window=None)
    try:
        g.window()
        g.wait_log(r'touch log: flyover 0', 300)
        time.sleep(4)
        g.print_window('race')
        end = time.time() + 60
        while time.time() < end and not re.search(r'touch log: up %d at ' % len(taps), g.log()):
            time.sleep(0.25)
        time.sleep(1.0)
        log = g.log()
    finally:
        g.stop()
    sends = [(int(b, 16), int(ms)) for b, ms in SENT.findall(log)]
    got = {}
    for i, (label, _, _) in enumerate(taps):
        t0, t1 = 10000 + i * 1000, 10000 + i * 1000 + 400
        got[label] = 0
        for b, ms in sends:
            if t0 <= ms <= t1 + 50:
                got[label] |= b
    return got, log


def shoulders(exe, out):
    first = editor(exe, out, 'touch-layout-find', out / 'find.ini')
    try:
        where = controls(first.log())
    finally:
        first.stop()
    missing = [n for n, _ in SHOULDERS if not where.get(n)]
    if missing:
        return 'the log names no place for %s: %r' % (missing, where)
    taps = [(n, where[n][0], where[n][1]) for n, _ in SHOULDERS]
    got, _ = race_sends(exe, out, 'touch-layout-shoulders', taps)
    print('shoulders: %s' % ', '.join('%s %04x' % (n, got[n]) for n, _ in SHOULDERS), flush=True)
    for n, want in SHOULDERS:
        if got[n] & (L1 | L2 | R1 | R2) != want:
            return '%s sent %04x, not %04x' % (n, got[n] & (L1 | L2 | R1 | R2), want)
    return None


def hide(exe, out):
    ini = out / 'dhd.ini'
    if ini.exists():
        ini.unlink()
    g = editor(exe, out, 'touch-layout-hide', ini)
    try:
        bar, where = editor_bar(g.log()), controls(g.log())
        if 'hide' not in bar or not where.get('l3') or not where.get('r1r2'):
            return 'no HIDE in the editor bar or no L3 / R1+R2: %r %r' % (bar, where)
        tap_mm(g, *bar['hide'])
        tap_mm(g, *where['l3'])
        tap_mm(g, *where['r1r2'])
        time.sleep(1.0)
        g.print_window('hide-mode')
        if not done(g, bar):
            return 'DONE did not end the editor'
    finally:
        g.stop()
    edits = touch_edits(ini)
    print('hidden: %r' % edits, flush=True)
    if not edits or 'classic.l3=0.0,0.0,1.00,hide' not in edits or 'classic.r1r2=0.0,0.0,1.00,hide' not in edits:
        return 'touch_edits after hiding L3 and R1+R2: %r' % edits
    g = editor(exe, out, 'touch-layout-hide', ini)
    try:
        now = controls(g.log())
        if now.get('l3', 0) is not None or now.get('r1r2', 0) is not None:
            return 'started again, L3 and R1+R2 are not hidden: %r' % now
        tap_mm(g, *bar['hide'])
        tap_mm(g, *where['l3'])   # its ghost
        time.sleep(1.0)
        g.print_window('ghost-shown')
        tap_mm(g, *bar['hide'])
        if not done(g, bar):
            return 'DONE did not end the editor (second start)'
    finally:
        g.stop()
    edits = touch_edits(ini)
    print('one shown again: %r' % edits, flush=True)
    if edits != 'classic.layout=2;classic.r1r2=0.0,0.0,1.00,hide':
        return 'touch_edits after showing L3 again: %r' % edits
    got, log = race_sends(exe, out, 'touch-layout-hidden-race', [('r1r2', *where['r1r2']), ('l3', *where['l3'])],
                          dict(DHD_SETTINGS=str(ini)))
    print('race with R1+R2 hidden: r1r2 %04x, l3 %04x' % (got['r1r2'], got['l3']), flush=True)
    if got['r1r2'] & (R1 | R2) or not got['l3'] & L3:
        return 'in the race the hidden R1+R2 sent %04x, L3 %04x' % (got['r1r2'], got['l3'])
    g = editor(exe, out, 'touch-layout-hide', ini)
    try:
        tap_mm(g, *bar['reset'])
        time.sleep(0.5)
        g.print_window('reset')
        if not done(g, bar):
            return 'DONE did not end the editor (after RESET)'
    finally:
        g.stop()
    edits = touch_edits(ini)
    print('after RESET: %r' % edits, flush=True)
    if edits:
        return 'touch_edits after RESET: %r' % edits
    g = editor(exe, out, 'touch-layout-hide', ini)
    try:
        now = controls(g.log())
    finally:
        g.stop()
    if any(v is None for v in now.values()) or not now.get('r1r2'):
        return 'after RESET a control is still hidden: %r' % now
    return None


# (name, screen mm, strips l,t,r,b, cutout x0,x1, window)
SHAPES = [('phone-20x9', '154.5x69.5', None, None, (1600, 720)),
          ('mi11i-cutout', '154.4x69.6', '0,5.2,0,0', '149.3,154.4', (1600, 721))]


def pictures(exe, out):
    problems = []
    for shape, mm, strips, cut, (w, h) in SHAPES:
        for hand in ('right', 'left'):
            extra = dict(DHD_TOUCH_SCREEN_MM=mm, DHD_TOUCH_HAND=hand, DHD_WINDOW_WIDTH=str(w), DHD_WINDOW_HEIGHT=str(h))
            if strips:
                extra['DHD_TOUCH_STRIPS_MM'] = strips
            if cut:
                extra['DHD_TOUCH_CUTOUT_MM'] = cut
            g = editor(exe, out, 'touch-layout-pictures', out / 'pictures.ini', extra)
            try:
                where = controls(g.log())
                path = g.print_window('%s-%s' % (shape, hand))
                print('%s %s: %s' % (shape, hand, path), flush=True)
                if cut:
                    x0, x1 = (float(v) for v in cut.split(','))
                    over = [n for n, p in where.items() if p and x0 - 6 < p[0] < x1 + 6]
                    if over:
                        problems.append('%s %s: centres by the cutout: %r' % (shape, hand, over))
            finally:
                g.stop()
    return '; '.join(problems) or None


SCENARIOS = {'shoulders': shoulders, 'hide': hide, 'pictures': pictures}


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
