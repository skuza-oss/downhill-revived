#!/usr/bin/env python3
"""The evidence rules of port/tools/options_walk.py (finding 219) on a scripted stand-in for the game:
no game, no disc, no window. The stand-in draws an OPTIONS list (a highlight bar at the cursor) and a
picture per row for what Cross opens, and can drop, ignore or break key presses on purpose.

The old walk printed `ok` for every row whose picture did not change; each scenario below that the old
walk passed wrongly must now FAIL, and the normal one must pass. Needs Pillow (skipped without it)."""
import os, shutil, sys, tempfile, time
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

FAILS = []


def expect(name, got, want):
    if got != want:
        FAILS.append(name)
        print('FAIL %s: %r, expected %r' % (name, got, want))
    else:
        print('ok   %s' % name)


def make_fake(Image, ImageDraw):
    class Fake:
        """One scripted game. drop_cross: {row: how many Cross presses are lost}; drop_down: {row: Down
        presses lost on the way to it}; same_page: {row: row whose picture it shows}; animate: rows whose
        page keeps changing; stuck: rows where Triangle does nothing."""

        def __init__(self, exe, out, env=None, name='', scale=1, **kw):
            self.out = Path(out)
            self.out.mkdir(parents=True, exist_ok=True)
            self.mode, self.cursor, self.n, self.alive_ = 'start', 0, 0, True
            self.dropped_cross, self.dropped_down = {}, {}
            Fake.last = self

        # the game's API as options_walk uses it
        def alive(self):
            return self.alive_

        def log(self):
            return ''

        def stop(self):
            pass

        def key(self, name, hold=0.12):
            s = Fake.script
            if self.mode == 'start' and name == 'x':
                self.mode, self.cursor = 'list', 0
            elif self.mode == 'start':
                pass                                   # the main menu's own Downs
            elif self.mode == 'list' and name == 'down':
                lost = s.get('drop_down', {}).get(self.cursor + 1, 0)
                if self.dropped_down.get(self.cursor + 1, 0) < lost:
                    self.dropped_down[self.cursor + 1] = self.dropped_down.get(self.cursor + 1, 0) + 1
                else:
                    self.cursor += 1
            elif self.mode == 'list' and name == 'x':
                lost = s.get('drop_cross', {}).get(self.cursor, 0)
                if self.dropped_cross.get(self.cursor, 0) < lost:
                    self.dropped_cross[self.cursor] = self.dropped_cross.get(self.cursor, 0) + 1
                else:
                    self.mode = 'page'
            elif self.mode == 'page' and name == 's' and self.cursor not in s.get('stuck', ()):
                self.mode = 'list'

        def keys(self, *names, gap=0.35):
            for n in names:
                self.key(n)

        def shot(self, label, settle=1.2):
            self.n += 1
            im = Image.new('RGB', (960, 720), (40, 30, 20))
            d = ImageDraw.Draw(im)
            if self.mode == 'list':
                d.rectangle([430, 300 + self.cursor * 30, 810, 322 + self.cursor * 30], fill=(250, 250, 90))
            else:
                row = Fake.script.get('same_page', {}).get(self.cursor, self.cursor)
                d.rectangle([40 + row * 20, 40, 900, 700 - row * 15], fill=(10, 10, 160))
                d.rectangle([450, 320 + row * 8, 700, 340 + row * 8], fill=(250, 250, 250))
                if self.cursor in Fake.script.get('animate', ()):
                    d.rectangle([420 + (self.n % 5) * 70, 300, 480 + (self.n % 5) * 70, 600], fill=(255, 0, 0))
            path = self.out / (label + '.png')
            im.save(path)
            return path

    Fake.script = {}
    return Fake


def run_walk(options_walk, Fake, script, rows=None, withheld=()):
    Fake.script = script
    out = Path(tempfile.mkdtemp(prefix='options-walk-test-'))
    try:
        card = out / 'card'
        card.mkdir()
        rows = rows or list(range(10))
        problems = options_walk.walk('x.exe', out / 'run', 'test', {}, card, rows, set(rows), 1, withheld)
        return problems
    finally:
        shutil.rmtree(out, ignore_errors=True)


def main():
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print('SKIP test_options_walk: no Pillow')
        return
    import types
    fake_module = types.ModuleType('hidden_game')
    Fake = make_fake(Image, ImageDraw)
    fake_module.Game = Fake
    fake_module.to_main = lambda g, timeout=90: None
    sys.modules['hidden_game'] = fake_module
    import options_walk
    time.sleep = lambda s: None                      # the waits are the game's, not the logic's
    import io, contextlib

    def quiet(script, **kw):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            problems = run_walk(options_walk, Fake, script, **kw)
        return problems, out.getvalue()

    # 1. as it should be: every row opens, settles, comes back
    problems, out = quiet({})
    expect('normal: no problem', problems, [])
    expect('normal: ten rows ok', out.count(': ok (page differs'), 10)

    # 2. the old false pass: the Cross of POSTERS never arrives (a dropped key, a greyed row)
    problems, out = quiet({'drop_cross': {5: 99}})
    expect('POSTERS never opens: a failure', len(problems), 1)
    expect('  that names the row and the reason', 'POSTERS: did not open' in problems[0], True)
    problems, out = quiet({}, withheld={5})
    expect('Cross withheld (the test switch): a failure', ['POSTERS: did not open' in p for p in problems], [True])
    expect('  and the walk said so', 'test switch: no Cross on POSTERS' in out, True)

    # 3. one dropped key is retried, says so, and passes
    problems, out = quiet({'drop_cross': {3: 1}})
    expect('one dropped Cross: retried and passed', problems, [])
    expect('  and the output says retry', 'retry: AUDIO / OTHER' in out, True)
    problems, out = quiet({'drop_cross': {3: 2}})
    expect('two dropped Cross presses: a failure', ['AUDIO / OTHER: did not open' in p for p in problems], [True])

    # 4. a dropped Down: the cursor stays, Cross would open the previous row again
    problems, out = quiet({'drop_down': {2: 1}})
    expect('a dropped Down: a failure', len(problems), 1)
    expect('  caught as a cursor that did not move', 'cursor did not move' in problems[0], True)

    # 5. two rows showing one picture (a wrong row opened)
    problems, out = quiet({'same_page': {4: 3}})
    expect('two rows, one picture: a failure', ['VIDEO CLIPS' in p or 'AUDIO / OTHER' in p for p in problems], [True])
    expect('  says the same picture', 'show the same picture' in problems[0], True)

    # 6. a page that never settles
    problems, out = quiet({'animate': {6}})
    expect('an unsettled page: a failure', any('kept changing' in p for p in problems), True)

    # 7. Triangle does not bring the list back
    problems, out = quiet({'stuck': {2}})
    expect('no way back: a failure that stops the walk', ['did not come back' in p for p in problems], [True])
    expect('  and later rows are not walked', out.count(': ok (page differs'), 2)

    print('test_options_walk: %s' % ('FAIL (%d)' % len(FAILS) if FAILS else 'OK'))
    sys.exit(1 if FAILS else 0)


if __name__ == '__main__':
    main()
