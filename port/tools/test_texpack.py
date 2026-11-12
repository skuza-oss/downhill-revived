"""Texture packs (port/gs/texpack.c, finding 057) through their interface, with synthetic PNGs.

    python port/tools/test_texpack.py [--wic-reference COMMIT]

Builds port/gs/test_texpack.c with texpack.c, the shared host (port/host/host_sdl.c), SDL3 and the
Rust library (its PNG decoder, dhdr_png_*), then:
- names: PCSX2's names from texpack_find for GS textures the harness writes: model names it
  computes from the texels (below a block, CLUTs, TEXA, mip levels) and the block-hashed formats'
  names, which must equal GOLDEN (texpack.c's names before the decoder moved to Rust); the file
  name rules (bit 14, case, sub-folders, other names skipped, duplicates);
- the pack's folder: DHD_TEXTURE_PACK and its two layouts, DHD_DATA_DIR, beside the game, UTF-8;
- load: every PNG kind (8-bit RGBA/RGB, palettes with and without tRNS, grey, grey+alpha, 16-bit,
  low bit depths, interlaced, odd sizes) decoded, with its mip levels, against the pixels this
  script made (a PNG without alpha: 128); damaged and oversized files fail and draw nothing;
- image: texpack_image gives each loaded replacement's first level, and nothing for a damaged one;
- budget: least recently drawn released first, none within 8 fields of use, loaded again;
- stress: two loaders creating while the GS thread draws and evicts, as in a race;
- font icons (port/gs/test_font_icons.c, finding 200): the menus' font copy over a pack's
  replacement of it, with other button icons than the game's, at the pack's scale, the pack's other
  cells kept texel for texel; none with the game's icons in their places.
--wic-reference COMMIT (Windows) also builds the harness with that commit's texpack.c (the WIC
decoder, e.g. 89bee8823) and compares what each gives the renderer, PNG kind by kind.
Synthetic images only: nothing of a game or a real pack. Output under build/port/texpack-test.
"""
import argparse
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build' / 'port' / 'texpack-test'
LLVM = (Path.home() / 'scoop/apps/llvm/current/bin')
TARGET = 'x86_64-pc-windows-msvc'
sys.path.insert(0, str(ROOT / 'port' / 'tools'))

# texpack.c's names for the block-hashed cases (recorded from 89bee8823's texpack.c, the WIC one,
# with this harness: the hashing did not change).
GOLDEN = {'G1': '96c19ddce3ec0bfe-00001980', 'G2': '21c8a9891a3e8e4c-a2b60f39b08686a6-00001553',
          'G3': '3cfedb541bfbeee4-83440bb68de12744-00001994', 'G4': 'ae5e241a1c764e4f-00bf9101',
          'G5': '41cca1a3a8dcf0e2-00001980', 'G6': '7eec0e56ead6877a-2d7c1fc849bc0f39-0000111b'}

# ---- PNG files ----

ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)


def pack_row(row, depth):
    if depth == 8:
        return bytes(s for px in row for s in px)
    if depth == 16:
        return b''.join(struct.pack('>H', s) for px in row for s in px)
    bits, out, n = 0, bytearray(), 0
    for px in row:
        for s in px:
            bits = bits << depth | s
            n += depth
            if n == 8:
                out.append(bits)
                bits = n = 0
    if n:
        out.append(bits << (8 - n))
    return bytes(out)


def png(pixels, color, depth, palette=None, trns=None, interlace=False, filters=(0,)):
    """A PNG of pixels[y][x] (tuples of samples at depth), filter types cycled by row."""
    h, w = len(pixels), len(pixels[0])
    raw = bytearray()

    def rows(sub):
        nonlocal raw
        for y, row in enumerate(sub):
            line = pack_row(row, depth)
            ftype = filters[y % len(filters)]
            bpp = max(1, CHANNELS[color] * depth // 8)
            if ftype == 1:  # Sub
                line = bytes((line[i] - (line[i - bpp] if i >= bpp else 0)) & 255 for i in range(len(line)))
            raw += bytes([ftype]) + line
    if interlace:
        for x0, y0, dx, dy in ADAM7:
            sub = [row[x0::dx] for row in pixels[y0::dy]]
            if sub and sub[0]:
                rows(sub)
    else:
        rows(pixels)
    data = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, color, 0, 0, int(interlace)))
    if palette:
        data += chunk(b'PLTE', bytes(c for rgb in palette for c in rgb))
    if trns is not None:
        data += chunk(b'tRNS', trns)
    return data + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')


def expected_rgba(pixels, color, depth, palette=None, trns=None):
    """What the decoder must give: RGBA8 rows, 16-bit samples cut to their high byte, low bit
    depths of grey scaled to 8 bits, and 128 where the PNG has no alpha."""
    def eight(s):
        return s >> 8 if depth == 16 else s * 255 // ((1 << depth) - 1) if depth < 8 and color == 0 else s
    key = None
    if trns is not None and color in (0, 2):
        key = struct.unpack('>%dH' % (len(trns) // 2), trns)
    out = bytearray()
    for row in pixels:
        for px in row:
            if color == 3:
                r, g, b = palette[px[0]]
                a = (trns[px[0]] if px[0] < len(trns) else 255) if trns is not None else 128
            elif color in (0, 4):
                r = g = b = eight(px[0])
                a = eight(px[1]) if color == 4 else (0 if key and px[0] == key[0] else 255) if key else 128
            else:
                r, g, b = (eight(s) for s in px[:3])
                a = eight(px[3]) if color == 6 else (0 if key and tuple(px) == key else 255) if key else 128
            out += bytes((r, g, b, a))
    return bytes(out)


def mips(base, w, h):
    """texpack.c's levels: 2x2 boxes, rounded, an odd side's last texel kept, down to 1x1."""
    out, level, levels = bytearray(base), base, levels_of(w, h)
    while w > 1 or h > 1:
        nw, nh = max(w // 2, 1), max(h // 2, 1)
        nxt = bytearray(nw * nh * 4)
        for y in range(nh):
            y0, y1 = min(2 * y, h - 1), min(2 * y + 1, h - 1)
            for x in range(nw):
                x0, x1 = min(2 * x, w - 1), min(2 * x + 1, w - 1)
                for c in range(4):
                    s = (level[(y0 * w + x0) * 4 + c] + level[(y0 * w + x1) * 4 + c] +
                         level[(y1 * w + x0) * 4 + c] + level[(y1 * w + x1) * 4 + c] + 2) >> 2
                    nxt[(y * nw + x) * 4 + c] = s
        out += nxt
        level, w, h = bytes(nxt), nw, nh
    return bytes(out), levels


def levels_of(w, h):
    n = 1
    while w > 1 or h > 1:
        w, h, n = max(w // 2, 1), max(h // 2, 1), n + 1
    return n


def kinds(rnd):
    """(name, w, h, color, depth, pixels, palette, trns, interlace, filters) for every PNG kind."""
    def img(w, h, f):
        return [[f(x, y) for x in range(w)] for y in range(h)]
    def r(n):
        return rnd.randrange(n)
    pal = [(r(256), r(256), r(256)) for _ in range(256)]
    trns = bytes(r(256) for _ in range(200))
    yield 'rgba8', 64, 48, 6, 8, img(64, 48, lambda x, y: (r(256), r(256), r(256), r(256))), None, None, False, (0, 1)
    yield 'rgb8', 37, 23, 2, 8, img(37, 23, lambda x, y: (r(256), r(256), r(256))), None, None, False, (0,)
    yield 'rgb8-trns', 16, 16, 2, 8, img(16, 16, lambda x, y: (10, 20, 30) if (x + y) % 3 == 0 else
                                         (r(256), r(256), r(256))), None, struct.pack('>3H', 10, 20, 30), False, (0,)
    yield 'pal8', 32, 32, 3, 8, img(32, 32, lambda x, y: (r(256),)), pal, None, False, (0,)
    yield 'pal8-trns', 32, 32, 3, 8, img(32, 32, lambda x, y: (r(256),)), pal, trns, False, (0,)
    yield 'pal4', 30, 10, 3, 4, img(30, 10, lambda x, y: (r(16),)), pal[:16], None, False, (0,)
    yield 'pal2-trns', 13, 7, 3, 2, img(13, 7, lambda x, y: (r(4),)), pal[:4], bytes((0, 77)), False, (0,)
    yield 'grey8', 16, 16, 0, 8, img(16, 16, lambda x, y: (r(256),)), None, None, False, (0,)
    yield 'grey8-trns', 16, 16, 0, 8, img(16, 16, lambda x, y: (r(4) * 60,)), None, struct.pack('>H', 60), False, (0,)
    yield 'grey4', 16, 8, 0, 4, img(16, 8, lambda x, y: (r(16),)), None, None, False, (0,)
    yield 'grey1', 9, 9, 0, 1, img(9, 9, lambda x, y: (r(2),)), None, None, False, (0,)
    yield 'greya8', 16, 16, 4, 8, img(16, 16, lambda x, y: (r(256), r(256))), None, None, False, (0,)
    yield 'grey16', 16, 8, 0, 16, img(16, 8, lambda x, y: (r(65536),)), None, None, False, (0,)
    yield 'greya16', 8, 8, 4, 16, img(8, 8, lambda x, y: (r(65536), r(65536))), None, None, False, (0,)
    yield 'rgb16', 20, 20, 2, 16, img(20, 20, lambda x, y: (r(65536), r(65536), r(65536))), None, None, False, (0,)
    yield 'rgba16', 20, 20, 6, 16, img(20, 20, lambda x, y: (r(65536), r(65536), r(65536), r(65536))), None, None, \
        False, (0,)
    yield 'rgba8-interlaced', 33, 17, 6, 8, img(33, 17, lambda x, y: (r(256), r(256), r(256), r(256))), None, None, \
        True, (0, 1)
    yield 'pal8-interlaced', 19, 19, 3, 8, img(19, 19, lambda x, y: (r(256),)), pal, None, True, (0,)
    yield 'rgb16-interlaced', 11, 13, 2, 16, img(11, 13, lambda x, y: (r(65536), r(65536), r(65536))), None, None, \
        True, (0,)


# P cases that are damaged or too large: (name, bytes)
def broken(rnd):
    good = png([[(1, 2, 3, 4)] * 8] * 8, 6, 8)
    corrupt = bytearray(good)
    idat = corrupt.index(b'IDAT') + 4
    for i in range(idat, idat + 20):
        corrupt[i] ^= 0x5A
    yield 'corrupt', bytes(corrupt)
    yield 'truncated', good[:len(good) // 2]
    yield 'empty', b''
    yield 'not-png', b'this is no picture\n' * 4
    yield 'too-wide', png([[(0,)] * 4097], 0, 1)


# ---- the harness ----

def build(texpack, exe):
    import rustgs
    import sdl3
    os.environ['PATH'] = str(LLVM) + os.pathsep + os.environ.get('PATH', '')
    sdl_lib, sdl_include = sdl3.build()
    rust = rustgs.build(rustgs.X64)
    # dhd.exe's manifest (the UTF-8 code page), so the environment arrives in UTF-8 as the game's does
    rc = exe.with_suffix('.rc')
    rc.write_text('1 24 "%s"\n' % (ROOT / 'port/res/dhd.manifest').as_posix(), encoding='utf-8')
    subprocess.run([str(LLVM / 'llvm-rc.exe'), '/fo', str(exe.with_suffix('.res')), str(rc)], check=True)
    sources = [exe.with_suffix('.res'), ROOT / 'port/gs/test_texpack.c', texpack, ROOT / 'port/gs/memory.c', ROOT / 'port/host/host_sdl.c',
               ROOT / 'port/host/test_host_end_stub.c', ROOT / 'port/runtime/sha256.c', ROOT / 'port/net/net.c', ROOT / 'port/net/client.c',
               ROOT / 'port/net/json.c', ROOT / 'port/net/hash.c']
    cmd = ['clang', '--target=' + TARGET, '-O1', '-w', '-fms-runtime-lib=static', '-D_CRT_SECURE_NO_WARNINGS',
           '-I' + str(ROOT / 'port/gs'), '-I' + str(ROOT / 'port/include'), '-I' + str(ROOT / 'port/runtime'),
           '-I' + str(ROOT / 'port/host'), '-I' + str(ROOT / 'port/net'), '-I' + str(sdl_include),
           *[str(s) for s in sources], '-o', str(exe), str(sdl_lib), rust['lib'],
           *['-l' + x[:-4] for x in dict.fromkeys(sdl3.SYSTEM_LIBS + ['user32.lib', 'ole32.lib'] +
                                                  [l for l in rust['native_static_libs'] if l.endswith('.lib')])]]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=600)
    if r.returncode:
        raise SystemExit('harness build failed:\n' + r.stdout + r.stderr)


def build_font_icons(exe):
    """port/gs/test_font_icons.c with font_sharp.c and button_icons.c (the icons over a pack's font,
    finding 200); button_icons.inc made as build_host64.py makes it."""
    import rustgs
    import sdl3
    sdl_lib, sdl_include = sdl3.build()
    rust = rustgs.build(rustgs.X64)
    pngs = sorted((ROOT / 'port/res/buttons').glob('*.png'))
    inc = [f'static const unsigned char button_png_{p.stem}[] = {{{",".join(map(str, p.read_bytes()))}}};' for p in pngs]
    inc.append('static const struct { const char *name; const unsigned char *png; unsigned size; } button_pngs[] = {')
    inc += [f'    {{"{p.stem}", button_png_{p.stem}, sizeof button_png_{p.stem}}},' for p in pngs]
    (OUT / 'button_icons.inc').write_text('\n'.join(inc + ['};', '']))
    sources = [ROOT / 'port/gs/test_font_icons.c', ROOT / 'port/gs/font_sharp.c', ROOT / 'port/gs/button_icons.c']
    cmd = ['clang', '--target=' + TARGET, '-O1', '-w', '-fms-runtime-lib=static', '-D_CRT_SECURE_NO_WARNINGS',
           '-I' + str(OUT), '-I' + str(ROOT / 'port/gs'), '-I' + str(ROOT / 'port/include'),
           '-I' + str(ROOT / 'port/runtime'), '-I' + str(sdl_include),
           *[str(s) for s in sources], '-o', str(exe), str(sdl_lib), rust['lib'],
           *['-l' + x[:-4] for x in dict.fromkeys(sdl3.SYSTEM_LIBS + ['user32.lib', 'ole32.lib'] +
                                                  [l for l in rust['native_static_libs'] if l.endswith('.lib')])]]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=600)
    if r.returncode:
        raise SystemExit('font icons harness build failed:\n' + r.stdout + r.stderr)


def run(exe, args, env=None):
    e = dict(os.environ)
    for k in ('DHD_TEXTURE_PACK', 'DHD_DATA_DIR', 'DHD_TEXTURES', 'DHD_TEXTURE_CACHE_MB', 'DHD_TEXTURE_PACK_LOG',
              'TEST_EXE_PATH'):
        e.pop(k, None)
    e.update(env or {})
    r = subprocess.run([str(exe), *args], capture_output=True, text=True, encoding='utf-8', errors='replace',
                       env=e, timeout=600)
    return r.returncode, r.stdout, r.stderr


failures = []


def check(cond, label):
    print(('PASS ' if cond else 'FAIL ') + label)
    if not cond:
        failures.append(label)


def harness_lines(label, code, out, err):
    for line in out.splitlines():
        if line.startswith(('PASS', 'FAIL')):
            print(line)
            if line.startswith('FAIL'):
                failures.append(label + ': ' + line[5:])
    if code or 'RESULT PASS' not in out:
        failures.append('%s: exit %d' % (label, code))
        print(err[-3000:])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--wic-reference', metavar='COMMIT', help="compare against this commit's texpack.c (WIC)")
    a = ap.parse_args()
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir(parents=True)
    os.environ.update(TMP=str(OUT), TEMP=str(OUT))
    exe = OUT / 'test_texpack.exe'
    build(ROOT / 'port/gs/texpack.c', exe)

    # A UTF-8 folder holding PCSX2's layout: <textures>/SCUS-97177/replacements.
    textures = OUT / 'pack Zürich é' / 'textures'
    pack = textures / 'SCUS-97177' / 'replacements'
    pack.mkdir(parents=True)
    (pack / '0-0.png').write_bytes(b'')   # a name to open the pack with; never drawn
    for junk in ('readme.txt', '1a2b-mip1.png', '1a2b-r64x64-00000123.png', '00ff-00000123.png.bak',
                 'zz-00000001.png', '1-2-3-4.png'):
        (pack / junk).write_bytes(b'x')
    (pack / '.hidden').mkdir()
    (pack / '.hidden' / '12-00000001.png').write_bytes(b'')

    # ---- the pack's folder ----
    data = OUT / 'data'
    (data / 'textures' / 'SCUS-97177' / 'replacements').mkdir(parents=True)
    exe_dir = OUT / 'game'
    (exe_dir / 'textures' / 'SCUS-97177' / 'replacements').mkdir(parents=True)
    fake_exe = str(exe_dir / 'dhd.exe')
    for env, want, label in (
            ({'DHD_TEXTURE_PACK': str(textures)}, 1, 'a textures folder (SCUS-97177/replacements below)'),
            ({'DHD_TEXTURE_PACK': str(textures / 'SCUS-97177')}, 1, 'a SCUS-97177 folder (replacements below)'),
            ({'DHD_TEXTURE_PACK': str(pack) + os.sep}, 1, 'the replacements folder, a trailing slash'),
            ({'DHD_TEXTURE_PACK': str(pack).replace('\\', '/')}, 1, 'forward slashes'),
            ({'DHD_TEXTURE_PACK': str(OUT / 'nowhere')}, 0, 'a named folder that is not there'),
            ({'DHD_DATA_DIR': str(data), 'TEST_EXE_PATH': fake_exe}, 1, 'DHD_DATA_DIR/textures/SCUS-97177/replacements'),
            ({'DHD_DATA_DIR': str(OUT / 'empty'), 'TEST_EXE_PATH': fake_exe}, 0,
             'DHD_DATA_DIR without one (beside the game is not used then)'),
            ({'TEST_EXE_PATH': fake_exe}, 1, 'beside the game'),
            ({'TEST_EXE_PATH': str(OUT / 'dhd.exe')}, 0, 'none beside the game')):
        code, out, err = run(exe, ['root'], env)
        check(code == 0 and ('available=%d' % want) in out, 'folder: %s -> %d' % (label, want))

    # ---- names ----
    log = OUT / 'names.csv'
    env = {'DHD_TEXTURE_PACK': str(textures), 'DHD_TEXTURE_PACK_LOG': str(log)}
    code, out, err = run(exe, ['names'], env)
    check(code == 0, 'names: the harness ran (%d)' % code)
    cases = dict(re.findall(r'^case (\d+) (\S+) found=', out, re.M))
    ids = {int(k): v for k, v in cases.items()}
    models = {m[1]: m[2] for m in re.findall(r'^model (\d+) (\S+) (\S+)$', out, re.M)}
    rows = [line.split(',') for line in log.read_text().splitlines()[1:]]
    names = {ids[int(r[0])]: r[4] for r in rows}
    check(len(names) == len(ids), 'names: one logged name per case (%d of %d)' % (len(names), len(ids)))
    m = re.search(r'textures: (\d+) replacements in .* \((\d+) other files skipped, (\d+) duplicates\)', err)
    check(m and m.groups() == ('1', '6', '0'), 'names: index of a pack with 1 name and 6 others skipped (%s)'
          % ((m.groups(),) if m else err[-400:]))
    for k, model in models.items():
        want = model
        if k == 'M6':  # mipmapped: logged with its mip levels; the model is the base level's name
            check(names[k] != model, 'names: %s logged with its mip levels' % k)
            continue
        check(names[k] == want, 'names: %s = the model %s (texpack: %s)' % (k, want, names[k]))
    for k, v in sorted(names.items()):
        if k[0] == 'G':
            check(GOLDEN.get(k) == v, 'names: %s = %s (texpack: %s)' % (k, GOLDEN.get(k), v))
    print('names:', ', '.join('%s %s' % (k, names[k]) for k in sorted(names) if k[0] in 'MG'))

    # ---- the pack: one PNG per case ----
    rnd = random.Random(57)
    expected = {}
    kind_list = list(kinds(rnd))
    broken_list = list(broken(rnd))
    plain = sorted((k for k in names if k[0] == 'P'), key=lambda k: int(k[1:]))
    assert len(plain) >= len(kind_list) + len(broken_list)
    kind_of = {}

    def put(case, data, name=None, folder=pack):
        folder.mkdir(parents=True, exist_ok=True)
        (folder / ((name or names[case]) + '.png')).write_bytes(data)

    def put_kind(case, kind):
        name, w, h, color, depth, pixels, palette, trns, interlace, filters = kind
        put(case, png(pixels, color, depth, palette, trns, interlace, filters))
        base = expected_rgba(pixels, color, depth, palette, trns)
        expected[case] = (w, h) + mips(base, w, h)
        kind_of[case] = name

    for case, kind in zip(plain, kind_list):
        put_kind(case, kind)
    for case, (name, data) in zip(plain[len(kind_list):], broken_list):
        put(case, data)
        kind_of[case] = name
        expected[case] = None
    small = next(k for k in kind_list if k[0] == 'rgba8')
    for case in models:
        if case == 'M4':      # named without TEXA (packs dumped by older PCSX2s)
            tex0, bits = names[case].rsplit('-', 1)
            put(case, png(small[5], 6, 8), '%s-%08x' % (tex0, int(bits, 16) & 0x3FFF))
            expected[case] = (64, 48) + mips(expected_rgba(small[5], 6, 8), 64, 48)
        elif case == 'M6':    # only the base level's name
            put(case, png(small[5], 6, 8), models[case])
            expected[case] = (64, 48) + mips(expected_rgba(small[5], 6, 8), 64, 48)
        else:
            put_kind(case, kind_list[int(case[1:]) % len(kind_list)])
    for case in (k for k in names if k[0] == 'G'):
        kind = kind_list[int(case[1:]) * 3 % len(kind_list)]
        if case == 'G1':      # bit 14 set (early PCSX2 dumps), upper-case hex, .PNG
            tex0, bits = names[case].rsplit('-', 1)
            put_kind(case, kind)
            (pack / (names[case] + '.png')).rename(pack / ('%s-%08X.PNG' % (tex0.upper(), int(bits, 16) | 1 << 14)))
        elif case == 'G2':    # a sub-folder
            put_kind(case, kind)
            (pack / 'sub' / 'deeper').mkdir(parents=True)
            (pack / (names[case] + '.png')).rename(pack / 'sub' / 'deeper' / (names[case] + '.png'))
        else:
            put_kind(case, kind)
    (pack / 'sub' / (names['G3'] + '.png')).write_bytes(png([[(9, 9, 9)]], 2, 8))  # a duplicate name
    bstripe = [[(x, y, (x * y) & 255, 255) for x in range(256)] for y in range(256)]
    for case in (k for k in names if k[0] == 'B'):
        put(case, png(bstripe, 6, 8))
    sblock = [[(x * 2, y * 2, 0) for x in range(128)] for y in range(128)]
    for case in (k for k in names if k[0] == 'S'):
        put(case, png(sblock, 2, 8))

    # ---- load ----
    def load(exe, label):
        dumps = OUT / ('dumps-' + label)
        dumps.mkdir()
        code, out, err = run(exe, ['load', str(dumps)], {'DHD_TEXTURE_PACK': str(textures)})
        status = dict(re.findall(r'^case (\S+) (absent|failed|loaded)', out, re.M))
        got = {}
        for f in dumps.glob('*.bin'):
            raw = f.read_bytes()
            head, body = raw.split(b'\n', 1)
            w, h, levels = map(int, head.split())
            got[f.stem] = (w, h, body, levels)
        return code, status, got, out, err

    code, status, got, out, err = load(exe, 'rust')
    check(code == 0, 'load: the harness ran (%d)' % code)
    harness_lines('load', code, out, err)  # texpack_image against each loaded view
    m = re.search(r'textures: (\d+) replacements in .* \((\d+) other files skipped, (\d+) duplicates\)', err)
    print('load:', m.group(0) if m else err[-500:])
    check(m and m.group(2) == '6' and m.group(3) == '1', 'load: 6 other names skipped, 1 duplicate')
    for case, want in sorted(expected.items()):
        label = '%s (%s)' % (case, kind_of.get(case, 'rgba8'))
        if want is None:
            check(status.get(case) == 'failed' and case not in got, 'load: %s fails and draws nothing' % label)
            continue
        if status.get(case) != 'loaded':
            check(False, 'load: %s loaded (%s)' % (label, status.get(case)))
            continue
        w, h, body, levels = got[case]
        check((w, h, levels) == (want[0], want[1], want[3]) and body == want[2],
              'load: %s %dx%d, %d levels: every pixel and level as made' % (label, w, h, levels))
    for case in (k for k in names if k[0] in 'BS'):
        check(status.get(case) == 'loaded', 'load: %s loaded' % case)
    fails = len(re.findall(r'^textures: can\'t load ', err, re.M))
    check(fails == len(broken_list), "load: one \"can't load\" line per damaged file (%d)" % fails)
    print('\n'.join(l for l in err.splitlines() if 'dhdr_png' in l or "can't load" in l))

    # ---- budget and stress ----
    code, out, err = run(exe, ['budget'], {'DHD_TEXTURE_PACK': str(textures), 'DHD_TEXTURE_CACHE_MB': '1'})
    harness_lines('budget', code, out, err)
    code, out, err = run(exe, ['stress', '3000'], {'DHD_TEXTURE_PACK': str(textures), 'DHD_TEXTURE_CACHE_MB': '1'})
    harness_lines('stress', code, out, err)
    print('\n'.join(l for l in err.splitlines() if l.startswith('textures:')))

    # ---- the button icons over a pack's menu font (font_sharp.c, finding 200) ----
    icons = OUT / 'test_font_icons.exe'
    build_font_icons(icons)
    code, out, err = run(icons, [], {'DHD_FONT': 'sharp'})
    harness_lines('font icons', code, out, err)

    # ---- the WIC decoder, for comparison ----
    if a.wic_reference:
        old = OUT / 'texpack_wic.c'
        text = subprocess.run(['git', 'show', '%s:port/gs/texpack.c' % a.wic_reference], cwd=ROOT,
                              capture_output=True, text=True, encoding='utf-8', check=True).stdout
        if 'texpack_image(' not in text:  # older than finding 200: the harness's load mode asks for it
            text += '\nuint8_t *texpack_image(TexPackEntry *e, unsigned *w, unsigned *h) { (void)e; *w = *h = 0; return NULL; }\n'
        old.write_text(text, encoding='utf-8', newline='\n')
        wic = OUT / 'test_texpack_wic.exe'
        build(old, wic)
        code, out, err = run(wic, ['names'],
                             {'DHD_TEXTURE_PACK': str(textures), 'DHD_TEXTURE_PACK_LOG': str(OUT / 'names-wic.csv')})
        rows = [line.split(',') for line in (OUT / 'names-wic.csv').read_text().splitlines()[1:]]
        old_names = {ids[int(r[0])]: r[4] for r in rows}
        check(old_names == names, 'wic: the WIC build names every case alike (%d names)' % len(names))
        print('GOLDEN = %r' % {k: v for k, v in sorted(names.items()) if k[0] == 'G'})
        code, wstatus, wgot, out, err = load(wic, 'wic')
        print('\n%-18s %-9s %s' % ('PNG kind', 'size', 'WIC against the Rust decoder (base level, RGBA)'))
        for case in sorted(expected, key=lambda c: (c[0], int(c[1:]))):
            if case[0] != 'P':
                continue
            kind = kind_of[case]
            if expected[case] is None:
                print('%-18s %-9s rust: %s, wic: %s' % (kind, '', status.get(case), wstatus.get(case)))
                continue
            w, h = expected[case][:2]
            if case not in wgot:
                print('%-18s %-9s wic: %s' % (kind, '%dx%d' % (w, h), wstatus.get(case)))
                continue
            a_, b_ = got[case][2][:w * h * 4], wgot[case][2][:w * h * 4]
            diff = [0, 0, 0, 0]
            count = [0, 0, 0, 0]
            for i in range(w * h * 4):
                d = abs(a_[i] - b_[i])
                if d:
                    diff[i % 4] = max(diff[i % 4], d)
                    count[i % 4] += 1
            alpha = sorted(set(b_[3::4]))
            note = 'identical' if a_ == b_ else 'max |diff| R %d G %d B %d A %d (texels differing: %s)' % (
                *diff, '/'.join(map(str, count)))
            if a_ != b_ and diff[3]:
                note += '; WIC alpha values %s' % (alpha if len(alpha) < 6 else '%d..%d' % (alpha[0], alpha[-1]))
            print('%-18s %-9s %s' % (kind, '%dx%d' % (w, h), note))
    print('RESULT ' + ('PASS' if not failures else 'FAIL %d' % len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
