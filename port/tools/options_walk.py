"""Every OPTIONS row opens and closes without a fault (finding 207, public issue #87: POSTERS closed the
game). One game on the hidden desktop (never a visible window, no focus, the untimed game lock), posted
keys, a fresh memory card or one whose save has the Posters and Pickup Editor rows earned, with Unlock
All on and off. For each row the game offers: the row's picture is captured, the game must be alive with
no guest memory fault in its log, and the way back (Triangle, only when the row did not return by itself)
must show the OPTIONS list again with the cursor where it was.

    python port/tools/options_walk.py                       # the four scenarios, this checkout's build
    python port/tools/options_walk.py --scenario on-fresh --rows 5 --scale 4   # POSTERS at 4x
    python port/tools/options_walk.py --exe OTHER.exe --out DIR

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
import hidden_game  # noqa: E402

ROWS = ['REVIVED SETTINGS', 'CONTROLS', 'DIFFICULTY', 'AUDIO / OTHER', 'VIDEO CLIPS', 'POSTERS',
        'PICKUP EDITOR', 'LOAD GAME DATA', 'SAVE GAME DATA', 'LOAD REPLAY']
RETAIL_ROWS = [0, 1, 2, 3, 4, 7, 8, 9]          # POSTERS and PICKUP EDITOR need the shop's flags
PROFILE_FLAGS = {'posters': 0x1219F, 'pickup editor': 0x12198}   # bytes of the 0x12240-byte save file
FAULT = re.compile(r'host64 memory:|fatal|exception|unhandled|panic', re.I)
# the OPTIONS list as a fraction of the picture (x 420-820, y 290-620 of 960x720)
CROP = (0.4375, 0.4028, 0.8542, 0.8611)
SAME = 6.0     # mean grey-level difference of that crop under which two pictures are the same screen


def crop_gray(path):
    from PIL import Image
    im = Image.open(path).convert('L')
    w, h = im.size
    return im.crop((int(CROP[0] * w), int(CROP[1] * h), int(CROP[2] * w), int(CROP[3] * h))).resize((200, 165))


def distance(a, b):
    from PIL import ImageChops, ImageStat
    return ImageStat.Stat(ImageChops.difference(crop_gray(a), crop_gray(b))).mean[0]


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


def walk(exe, out, name, env, card, rows, wanted, scale):
    """Open and close each row in `rows` (the rows this scenario offers, in order) that is in `wanted`."""
    out.mkdir(parents=True, exist_ok=True)
    e = {'DHD_MEMCARD': '1', 'DHD_MEMCARD_DIR': str(card)}
    e.update(env)
    g = hidden_game.Game(exe, out, env=e, name='options-walk', scale=scale)
    problems, labels, at = [], [], 0
    try:
        hidden_game.to_main(g)
        g.keys('down', 'down', 'x')                 # main menu: OPTIONS
        time.sleep(2)
        for row in rows:
            if row not in wanted:
                continue
            g.keys(*['down'] * (rows.index(row) - at))
            at = rows.index(row)
            time.sleep(0.8)
            base = snap(g, f'row{row}-list')
            g.key('x')
            time.sleep(3)
            if not g.alive():                      # a closed game takes no capture
                problems.append(f'{ROWS[row]}: closed the game: ' + ' '.join(
                    l.strip() for l in g.log().splitlines() if FAULT.search(l)))
                break
            opened = snap(g, f'row{row}-open')
            labels += [f'row{row}-list', f'row{row}-open']
            step = f'{ROWS[row]}'
            if not g.alive() or FAULT.search(g.log()):
                fault = next((l for l in g.log().splitlines() if FAULT.search(l)), 'the game exited')
                problems.append(f'{step}: closed the game: {fault.strip()}')
                break
            own = distance(base, opened)
            if own > SAME:                          # a screen of its own: Triangle goes back
                g.key('s')
                time.sleep(2.5)
                if not g.alive() or FAULT.search(g.log()):
                    problems.append(f'{step}: closed the game on the way back')
                    break
                back = snap(g, f'row{row}-back')
                labels.append(f'row{row}-back')
                gap = distance(base, back)
                if gap > SAME:
                    problems.append(f'{step}: the OPTIONS list did not come back (difference {gap})')
                    break
            print(f'  {step}: ok (screen difference {own:.1f})', flush=True)
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
    a = ap.parse_args()
    if 'DHD_ISO' not in os.environ and 'DHD_TEST_ISO' not in os.environ:
        sys.exit('set DHD_TEST_ISO to your Downhill Domination (USA) disc image')
    scenarios = a.scenario or ['off-fresh', 'on-fresh', 'off-earned', 'on-earned']
    wanted = {int(r) for r in a.rows.split(',')} if a.rows else set(range(len(ROWS)))
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
                        card, rows, wanted, a.scale)
        for p in problems:
            print('  FAIL ' + p)
        failed += bool(problems)
    print(f'{len(scenarios) - failed} of {len(scenarios)} scenarios walked every row' + (f'; {failed} FAILED' if failed else ''))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
