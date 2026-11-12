"""Every OPTIONS row opens and closes without a fault (finding 207, public issue #87: POSTERS closed the
game), and each row is shown to have done what it should (finding 219). One game on the hidden desktop
(never a visible window, no focus, the untimed game lock), posted keys, a fresh memory card or one whose
save has the Posters and Pickup Editor rows earned, with Unlock All on and off. For each row the game
offers: the row's list picture is captured, Cross is pressed, and then

* every row opens something: a page of its own, or a confirm box (LOAD GAME DATA, SAVE GAME DATA and
  LOAD REPLAY ask or say so before they act, and Triangle cancels, so nothing is loaded or saved: no
  row acts in place). The picture must differ from the list by more than SAME, be the same again a
  second later (settled: a screen, not a transition), and differ from every other row's picture (a
  dropped Down would show a neighbour's page again); Triangle must bring the list back. A picture
  that did not change is a failure ("did not open"), never a pass: a dropped key, a greyed row and a
  wrong row all look like that, and POSTERS (the crash this tool exists for) would pass unvisited;
* before Cross, the list picture must differ from the previous row's (the cursor moved);
* the game must be alive with no guest memory fault in its log after every step.

A key posted to a hidden desktop is sometimes dropped under load: when a row shows no sign that its Cross
arrived, the walk waits a moment (a slow page) and presses once more, and says so in its output ("retry").
A second miss is a failure.

    python port/tools/options_walk.py                       # the four scenarios, this checkout's build
    python port/tools/options_walk.py --scenario on-fresh --rows 5 --scale 4   # POSTERS at 4x
    python port/tools/options_walk.py --exe OTHER.exe --out DIR
    python port/tools/options_walk.py --scenario on-fresh --withhold-cross 5   # test switch: must FAIL

Scenarios: off-fresh (the retail rows: POSTERS and PICKUP EDITOR greyed, so 8 rows), on-fresh (Unlock
All: 10 rows), off-earned and on-earned (a card made by SAVE GAME DATA in the first run, then
the save's shop flags +0x12198 (pickup editor) and +0x1219F (posters) set; 10 rows). Pictures, the
game's log and a contact sheet per scenario go to OUT (default build/options-walk). About a minute and
a half a scenario. Row numbers: 0 REVIVED SETTINGS, 1 CONTROLS, 2 DIFFICULTY, 3 AUDIO / OTHER,
4 VIDEO CLIPS, 5 POSTERS, 6 PICKUP EDITOR, 7 LOAD GAME DATA, 8 SAVE GAME DATA, 9 LOAD REPLAY."""
import argparse
import os
import re
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

ROWS = ['REVIVED SETTINGS', 'CONTROLS', 'DIFFICULTY', 'AUDIO / OTHER', 'VIDEO CLIPS', 'POSTERS',
        'PICKUP EDITOR', 'LOAD GAME DATA', 'SAVE GAME DATA', 'LOAD REPLAY']
RETAIL_ROWS = [0, 1, 2, 3, 4, 7, 8, 9]          # POSTERS and PICKUP EDITOR need the shop's flags
PROFILE_FLAGS = {'posters': 0x1219F, 'pickup editor': 0x12198}   # bytes of the 0x12240-byte save file
FAULT = re.compile(r'host64 memory:|fatal|exception|unhandled|panic', re.I)
# the OPTIONS list as a fraction of the picture (x 420-820, y 290-620 of 960x720)
CROP = (0.4375, 0.4028, 0.8542, 0.8611)
SAME = 6.0     # mean grey-level difference of that crop under which two pictures are the same screen

# What Cross shows on each row (for the messages). A row that acted in place, with no picture of its
# own, would need its own side-effect check here; none does today (finding 219).
PAGES = {
    0: 'the REVIVED SETTINGS tabs', 1: 'the CONTROLS page', 2: 'the DIFFICULTY choice', 3: 'the AUDIO / OTHER page',
    4: 'the video clip list', 5: 'the poster listing or "Poster data not found"', 6: 'the pickup editor switch',
    7: 'the "Load Game Data?" box', 8: 'the "Overwrite Game Data?" box', 9: 'the "Replay Data Not Found" box',
}
LIST_MOVED = 0.002   # fraction of pixels (grey 480x360, 40 levels) that differ between two list pictures after a Down
JITTER = 0.0002      # extra tolerance between two pictures of the same page (animated pages wobble by their own measure)


def crop_gray(path):
    from PIL import Image
    im = Image.open(path).convert('L')
    w, h = im.size
    return im.crop((int(CROP[0] * w), int(CROP[1] * h), int(CROP[2] * w), int(CROP[3] * h))).resize((200, 165))


def distance(a, b):
    from PIL import ImageChops, ImageStat
    return ImageStat.Stat(ImageChops.difference(crop_gray(a), crop_gray(b))).mean[0]


def pixels_differing(a, b, level=40):
    """The fraction of the whole picture's pixels (grey, 480x360) that differ by more than `level`: small
    text changes show in it where the mean over the list crop does not."""
    from PIL import Image, ImageChops
    ia = Image.open(a).convert('L').resize((480, 360))
    ib = Image.open(b).convert('L').resize((480, 360))
    return sum(ImageChops.difference(ia, ib).histogram()[level + 1:]) / (480 * 360)


def snap(g, label):
    """The newest capture as OUT/LABEL.png; the game writes captures as it goes, so one caught half
    written is taken again."""
    from PIL import Image
    for _ in range(5):
        path = g.shot(label, settle=1.2)
        try:
            if path and path.suffix == '.png':
                Image.open(path).load()
                return path
        except OSError:
            pass
        time.sleep(1)
    raise RuntimeError(f'no readable capture for {label}')


def contact_sheet(out, labels):
    from PIL import Image, ImageDraw
    w, h, cols = 320, 240, 4
    sheet = Image.new('RGB', (cols * w, ((len(labels) + cols - 1) // cols) * (h + 16)))
    draw = ImageDraw.Draw(sheet)
    for i, label in enumerate(labels):
        x, y = (i % cols) * w, (i // cols) * (h + 16)
        try:
            sheet.paste(Image.open(out / (label + '.png')).convert('RGB').resize((w, h)), (x, y + 16))
        except OSError:
            pass
        draw.text((x + 4, y + 2), label, fill=(255, 255, 0))
    sheet.save(out / 'sheet.png')


def make_earned_card(exe, out, scale):
    """A card with a save that owns POSTERS and PICKUP EDITOR: SAVE GAME DATA once on an empty card,
    then the two shop flags set in the file (the save is the profile itself, flags at their offsets)."""
    import hidden_game
    card = out / 'card-earned'
    shutil.rmtree(card, ignore_errors=True)
    card.mkdir(parents=True)
    g = hidden_game.Game(exe, out / 'make-card', env={'DHD_MEMCARD': '1', 'DHD_MEMCARD_DIR': str(card)},
                         name='options-walk-card', scale=scale)
    try:
        hidden_game.to_main(g)
        g.keys('down', 'down', 'x')                 # OPTIONS
        time.sleep(1.5)
        g.keys(*['down'] * RETAIL_ROWS.index(8))    # SAVE GAME DATA among the retail rows
        g.key('x')
        time.sleep(4)
    finally:
        g.stop()
    saves = list(card.glob('card1/*/BASCUS-97177'))
    if len(saves) != 1:
        raise RuntimeError(f'SAVE GAME DATA left no save in {card}: {saves}')
    data = bytearray(saves[0].read_bytes())
    for offset in PROFILE_FLAGS.values():
        data[offset] = 1
    saves[0].write_bytes(data)
    return card


def fault_line(g):
    return next((l.strip() for l in g.log().splitlines() if FAULT.search(l)), 'the game exited')


def visit(g, row, base, labels, withheld, notes):
    """Cross on `row` (the cursor is on it; `base` is its list picture) and the evidence that it did what
    the row does. Returns (problems, (the row's page picture, its wobble) or None, stop): stop when the game is gone or
    the list did not come back, so the next row would not be where the walk thinks it is."""
    step = ROWS[row]
    opened, own = None, 0.0
    for attempt in (1, 2):
        suffix = '' if attempt == 1 else '-retry'
        if row in withheld:
            print(f'    (test switch: no Cross on {step})', flush=True)
        else:
            g.key('x')
        time.sleep(3)
        if not g.alive() or FAULT.search(g.log()):                 # a closed game takes no capture
            return [f'{step}: closed the game: {fault_line(g)}'], None, True
        opened = snap(g, f'row{row}-open{suffix}')
        labels.append(f'row{row}-open{suffix}')
        own = distance(base, opened)
        if own > SAME:
            break
        time.sleep(2.5)                                             # a slow page: look again before pressing again
        late = snap(g, f'row{row}-late{suffix}')
        labels.append(f'row{row}-late{suffix}')
        if distance(base, late) > SAME:
            opened, own = late, distance(base, late)
            notes.append(f'{step}: opened late (after the first capture)')
            break
        if attempt == 2:
            why = 'Cross was withheld (test switch)' if row in withheld else 'the row did nothing'
            return [f'{step}: did not open: the picture stayed the OPTIONS list (difference {own:.1f}, '
                    f'at most {SAME}) after Cross, twice, so {PAGES[row]} never showed ({why}: a dropped key, '
                    f'a greyed row or the wrong row)'], None, False
        notes.append(f'{step}: retry: the picture had not changed 5 s after Cross, pressed it once more')
        print(f'    retry: {step}: no change after Cross, pressing it once more', flush=True)
    # evidence 2: the page is a screen, not a transition
    time.sleep(1.0)
    again = snap(g, f'row{row}-open2')
    labels.append(f'row{row}-open2')
    wobble = distance(opened, again)
    problems = []
    if wobble > SAME:
        problems.append(f'{step}: the picture kept changing (difference {wobble:.1f} between two captures '
                        f'a second apart): not a settled page')
    # the way back
    g.key('s')
    time.sleep(2.5)
    if not g.alive() or FAULT.search(g.log()):
        return problems + [f'{step}: closed the game on the way back: {fault_line(g)}'], None, True
    back = snap(g, f'row{row}-back')
    labels.append(f'row{row}-back')
    gap = distance(base, back)
    if gap > SAME:
        return problems + [f'{step}: the OPTIONS list did not come back (difference {gap:.1f})'], None, True
    print(f'  {step}: ok (page differs from the list by {own:.1f}, stable to {wobble:.1f}, list back to {gap:.1f})',
          flush=True)
    return problems, (opened, pixels_differing(opened, again)), False


def walk(exe, out, name, env, card, rows, wanted, scale, withheld=()):
    """Open and close each row in `rows` (the rows this scenario offers, in order) that is in `wanted`."""
    import hidden_game
    out.mkdir(parents=True, exist_ok=True)
    e = {'DHD_MEMCARD': '1', 'DHD_MEMCARD_DIR': str(card)}
    e.update(env)
    g = hidden_game.Game(exe, out, env=e, name='options-walk', scale=scale)
    problems, labels, notes, pages, at, prev = [], [], [], {}, 0, None
    try:
        hidden_game.to_main(g)
        g.keys('down', 'down', 'x')                 # main menu: OPTIONS
        time.sleep(2)
        for row in rows:
            if row not in wanted:
                continue
            g.keys(*['down'] * (rows.index(row) - at))
            at_before, at = at, rows.index(row)
            time.sleep(0.8)
            base = snap(g, f'row{row}-list')
            labels.append(f'row{row}-list')
            if prev is not None and at != at_before and pixels_differing(prev, base) <= LIST_MOVED:
                problems.append(f"{ROWS[row]}: the list picture is the previous row's: the cursor did not move "
                                f"(a dropped Down), so Cross would act on the wrong row")
                break
            prev = base
            found, page, stop = visit(g, row, base, labels, set(withheld), notes)
            problems += found
            if page is not None and not found:
                pages[row] = page
            if stop:
                break
        # a wrong row (a dropped Down) shows another row's page: no two pages may look alike
        rows_seen = sorted(pages)
        for i, a in enumerate(rows_seen):
            for b in rows_seen[i + 1:]:
                d = pixels_differing(pages[a][0], pages[b][0])
                if d <= pages[a][1] + pages[b][1] + JITTER:
                    problems.append(f'{ROWS[a]} and {ROWS[b]} show the same picture ({d:.5f} of the pixels '
                                    f'differ): one of the Cross presses opened the wrong row')
        for n in notes:
            print('  note: ' + n, flush=True)
        print('  log: ' + ('no fault' if not FAULT.search(g.log()) else 'FAULT'), flush=True)
    finally:
        try:
            contact_sheet(out, labels)
        finally:
            g.stop()
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'options-walk')
    ap.add_argument('--scenario', action='append', choices=['off-fresh', 'on-fresh', 'off-earned', 'on-earned'])
    ap.add_argument('--rows', help='comma-separated row numbers (default: every row the scenario offers)')
    ap.add_argument('--scale', type=int, default=1, help='DHD_SCALE: the internal resolution (4 for the pictures)')
    ap.add_argument('--withhold-cross', help='TEST SWITCH: comma-separated row numbers whose Cross press is not '
                    'sent; the walk must then FAIL on those rows (the proof that "ok" needs evidence)')
    a = ap.parse_args()
    if 'DHD_ISO' not in os.environ and 'DHD_TEST_ISO' not in os.environ:
        sys.exit('set DHD_TEST_ISO to your Downhill Domination (USA) disc image')
    scenarios = a.scenario or ['off-fresh', 'on-fresh', 'off-earned', 'on-earned']
    wanted = {int(r) for r in a.rows.split(',')} if a.rows else set(range(len(ROWS)))
    withheld = {int(r) for r in a.withhold_cross.split(',')} if a.withhold_cross else set()
    earned = None
    failed = 0
    for scenario in scenarios:
        unlock, kind = scenario.split('-')
        if kind == 'earned' and earned is None:
            earned = make_earned_card(a.exe, a.out, a.scale)
        card = a.out / f'card-{scenario}'
        shutil.rmtree(card, ignore_errors=True)
        if kind == 'earned':
            shutil.copytree(earned, card)
        else:
            card.mkdir(parents=True)
        rows = RETAIL_ROWS if scenario == 'off-fresh' else list(range(len(ROWS)))
        print(f'{scenario}: Unlock All {unlock}, {kind} card, rows {[r for r in rows if r in wanted]}', flush=True)
        problems = walk(a.exe, a.out / scenario, scenario, {'DHD_UNLOCK_ALL': '1'} if unlock == 'on' else {},
                        card, rows, wanted, a.scale, withheld)
        for p in problems:
            print('  FAIL ' + p)
        failed += bool(problems)
    print(f'{len(scenarios) - failed} of {len(scenarios)} scenarios walked every row' + (f'; {failed} FAILED' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
