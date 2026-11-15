"""The online room's pad reader takes the left stick as the game's menus do (issue #103, finding
250): port/hle/menu_pads.c with the game's own stick test (src/unsorted/002209A8.c) in the core
(wasm32), and no screen of online_menu.c reads the button word by itself."""
import re

import testprog

ROOT = testprog.ROOT
source = (ROOT / 'port/hle/online_menu.c').read_text(encoding='utf-8')
direct = [n for n, line in enumerate(source.splitlines(), 1) if re.search(r'\bfunc_00220230\s*\(', line)]
assert not direct, 'online_menu.c reads the pads without the stick (use menu_pads_held) at lines %s' % direct

out = testprog.out_dir('menu-pads')
stick = testprog.game_unit('unsorted/002209A8.c', out)
exe = testprog.guest('menu-pads', [ROOT / 'port/hle/menu_pads.c', ROOT / 'port/hle/test_menu_pads.c', stick], out=out)
testprog.run(exe)
