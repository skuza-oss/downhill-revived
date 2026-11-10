"""What a GS command stream uses: a census of PCSX2-format GS dumps (the game's DHD_GS_CAPTURE files, or
PCSX2's own uncompressed dumps of the retail game), for planning a native renderer (finding 162).

    python port/tools/gs_census.py CAPTURE.gs [...] [--group NAME=FIRST-LAST ...] [--log game.log]
                                   [--json OUT.json]

Each dump is parsed as the GS would: GIF tags of PATH1/2/3 (PACKED, REGLIST, IMAGE), the registers they
write, vertex kicks and the primitives they make. Counted per field, by group of dumps (a group is a
field range; a dump's first field comes from the game log's "compare: dump" lines when --log is
given, else from its file name order):
- primitives and vertices by type and attributes (IIP, TME, FGE, ABE, AA1, FST), and by path;
- texture formats (PSM, CLUT format and storage mode, TFX, TCC), filters and mipmaps, clamp modes;
- alpha blend equations, alpha/destination-alpha/depth tests, Z formats and writes, frame masks,
  PABE, FBA, COLCLAMP, DTHE, fog;
- render targets (FRAME and ZBUF): how many, their formats, target switches per field;
- textures read from pages the GS itself drew into (render-to-texture, feedback), and from Z;
- image transfers: host to local, local to local, local to host (readbacks), by format and size;
- the display circuit (PMODE, DISPFB, DISPLAY) at each vsync.
Only reads the files given; prints a text report (and --json the numbers).
"""
import argparse
import collections
import json
import re
import struct
import sys
from pathlib import Path

PRIMS = ('point', 'line', 'linestrip', 'triangle', 'tristrip', 'trifan', 'sprite', 'prim7')
PSM = {0x00: 'CT32', 0x01: 'CT24', 0x02: 'CT16', 0x0A: 'CT16S', 0x13: 'T8', 0x14: 'T4', 0x1B: 'T8H',
       0x24: 'T4HL', 0x2C: 'T4HH', 0x30: 'Z32', 0x31: 'Z24', 0x32: 'Z16', 0x3A: 'Z16S'}
TFX = ('MODULATE', 'DECAL', 'HIGHLIGHT', 'HIGHLIGHT2')
ATST = ('NEVER', 'ALWAYS', 'LESS', 'LEQUAL', 'EQUAL', 'GEQUAL', 'GREATER', 'NOTEQUAL')
AFAIL = ('KEEP', 'FB_ONLY', 'ZB_ONLY', 'RGB_ONLY')
ZTST = ('NEVER', 'ALWAYS', 'GEQUAL', 'GREATER')
WM = ('REPEAT', 'CLAMP', 'REGION_CLAMP', 'REGION_REPEAT')
FILTER = ('NEAREST', 'LINEAR', 'NEAREST_MIP_NEAREST', 'NEAREST_MIP_LINEAR', 'LINEAR_MIP_NEAREST',
          'LINEAR_MIP_LINEAR', '6', '7')
ABCD = ('Cs', 'Cd', '0', '?')
CABC = ('As', 'Ad', 'FIX', '?')
# Registers (GS manual numbering).
PRIM, RGBAQ, ST, UV, XYZF2, XYZ2, TEX0_1, TEX0_2, CLAMP_1, CLAMP_2, FOG = range(11)
XYZF3, XYZ3 = 0x0C, 0x0D
TEX1_1, TEX2_1, XYOFFSET_1, PRMODECONT, PRMODE, TEXCLUT = 0x14, 0x16, 0x18, 0x1A, 0x1B, 0x1C
TEXA, FOGCOL, TEXFLUSH, SCISSOR_1, ALPHA_1, DIMX, DTHE, COLCLAMP = 0x3B, 0x3D, 0x3F, 0x40, 0x42, 0x44, 0x45, 0x46
TEST_1, PABE, FBA_1, FRAME_1, ZBUF_1 = 0x47, 0x49, 0x4A, 0x4C, 0x4E
BITBLTBUF, TRXPOS, TRXREG, TRXDIR, HWREG, SIGNAL, FINISH, LABEL = 0x50, 0x51, 0x52, 0x53, 0x54, 0x60, 0x61, 0x62
REGNAME = {0: 'PRIM', 1: 'RGBAQ', 2: 'ST', 3: 'UV', 4: 'XYZF2', 5: 'XYZ2', 6: 'TEX0_1', 7: 'TEX0_2',
           8: 'CLAMP_1', 9: 'CLAMP_2', 10: 'FOG', 12: 'XYZF3', 13: 'XYZ3', 0x14: 'TEX1_1', 0x15: 'TEX1_2',
           0x16: 'TEX2_1', 0x17: 'TEX2_2', 0x18: 'XYOFFSET_1', 0x19: 'XYOFFSET_2', 0x1A: 'PRMODECONT',
           0x1B: 'PRMODE', 0x1C: 'TEXCLUT', 0x22: 'SCANMSK', 0x34: 'MIPTBP1_1', 0x35: 'MIPTBP1_2',
           0x36: 'MIPTBP2_1', 0x37: 'MIPTBP2_2', 0x3B: 'TEXA', 0x3D: 'FOGCOL', 0x3F: 'TEXFLUSH',
           0x40: 'SCISSOR_1', 0x41: 'SCISSOR_2', 0x42: 'ALPHA_1', 0x43: 'ALPHA_2', 0x44: 'DIMX', 0x45: 'DTHE',
           0x46: 'COLCLAMP', 0x47: 'TEST_1', 0x48: 'TEST_2', 0x49: 'PABE', 0x4A: 'FBA_1', 0x4B: 'FBA_2',
           0x4C: 'FRAME_1', 0x4D: 'FRAME_2', 0x4E: 'ZBUF_1', 0x4F: 'ZBUF_2', 0x50: 'BITBLTBUF', 0x51: 'TRXPOS',
           0x52: 'TRXREG', 0x53: 'TRXDIR', 0x54: 'HWREG', 0x60: 'SIGNAL', 0x61: 'FINISH', 0x62: 'LABEL'}
VERTICES = {0: 1, 1: 2, 2: 2, 3: 3, 4: 3, 5: 3, 6: 2, 7: 1}   # vertices of a primitive's first draw
KEEP = {0: 0, 1: 0, 2: 1, 3: 0, 4: 2, 5: 2, 6: 0, 7: 0}       # kept for the next (strips, fans)


def bits(v, lo, n):
    return v >> lo & ((1 << n) - 1)


def page_dims(psm):
    """Pixels per 8 KB page (width, height) of a format."""
    if psm in (0x13, 0x1B):
        return 128, 64
    if psm in (0x14, 0x24, 0x2C):
        return 128, 128
    if psm in (0x02, 0x0A, 0x32, 0x3A):
        return 64, 64
    return 64, 32


def pages(base_block, width_px, height_px, psm):
    """The 8 KB pages a rectangle of a buffer at base_block (256-byte blocks) covers."""
    pw, ph = page_dims(psm)
    across = max(1, -(-width_px // pw)); down = max(1, -(-height_px // ph))
    first = base_block // 32
    return range(first, first + across * down)


class Census:
    def __init__(self):
        self.c = collections.defaultdict(collections.Counter)
        self.fields = 0

    def add(self, table, key, n=1):
        self.c[table][key] += n

    def merge(self, other):
        for t, counter in other.c.items():
            self.c[t].update(counter)
        self.fields += other.fields


class Gs:
    def __init__(self, census, regs):
        self.k = census
        self.reg = [0] * 128
        for r, v in regs.items():
            self.reg[r] = v
        self.queue = 0              # vertices waiting for the current primitive
        self.sig = None             # draw state signature, None when a register changed
        self.target = None          # (FRAME, ZBUF) of the last draw, for switches
        self.page_owner = {}        # page -> 'host' | 'copy' | ('render', psm) | ('z', psm)
        self.marked = set()         # targets whose pages are marked as drawn this field
        self.path = {p: None for p in (1, 2, 3)}   # [left, flg, nreg, regs, reg]
        self.image = None           # bytes left of a host-to-local transfer
        self.draws_since = 0

    # --- registers -------------------------------------------------------------------------
    def write(self, r, v, path):
        if r in (XYZ2, XYZF2, XYZ3, XYZF3):
            self.kick(r in (XYZ2, XYZF2), path)
            return
        if r in (RGBAQ, ST, UV, FOG):
            return
        self.k.add('reg_writes', REGNAME.get(r, hex(r)))
        if r == PRIM:
            self.reg[PRIM] = v & 0x7FF; self.queue = 0; self.sig = None
            return
        if r == TRXDIR:
            self.transfer(v & 3)
        if (r == TEX0_1 or r == TEX0_2) and bits(v, 20, 6) in (0x13, 0x14, 0x1B, 0x24, 0x2C) and bits(v, 61, 3):
            self.k.add('clut_loads', 'CLD %d CPSM %s CSM%d' % (bits(v, 61, 3), PSM.get(bits(v, 51, 4), '?'),
                                                               bits(v, 55, 1) + 1))
        if r in (SIGNAL, FINISH, LABEL, TEXFLUSH):
            self.k.add('sync', REGNAME[r])
        self.reg[r] = v
        self.sig = None

    def transfer(self, direction):
        b, pos, size = self.reg[BITBLTBUF], self.reg[TRXPOS], self.reg[TRXREG]
        w, h = bits(size, 0, 12), bits(size, 32, 12)
        spsm, dpsm = bits(b, 24, 6), bits(b, 56, 6)
        if direction == 0:
            self.k.add('transfer_host_to_local', '%s %dx%d' % (PSM.get(dpsm, hex(dpsm)), w, h))
            self.k.add('transfer_host_to_local_pixels', PSM.get(dpsm, hex(dpsm)), w * h)
            for p in pages(bits(b, 32, 14), bits(pos, 32, 11) + w, bits(pos, 48, 11) + h, dpsm):
                self.page_owner[p] = 'host'
            bpp = {0x00: 32, 0x01: 24, 0x02: 16, 0x0A: 16, 0x13: 8, 0x14: 4, 0x1B: 8, 0x24: 4, 0x2C: 4,
                   0x30: 32, 0x31: 24, 0x32: 16, 0x3A: 16}.get(dpsm, 32)
            self.image = w * h * bpp // 8
        elif direction == 1:
            self.k.add('transfer_local_to_host', '%s %dx%d' % (PSM.get(spsm, hex(spsm)), w, h))
        elif direction == 2:
            self.k.add('transfer_local_to_local', '%s->%s %dx%d' % (PSM.get(spsm, hex(spsm)),
                                                                    PSM.get(dpsm, hex(dpsm)), w, h))
            for p in pages(bits(b, 32, 14), bits(pos, 32, 11) + w, bits(pos, 48, 11) + h, dpsm):
                self.page_owner[p] = 'copy'

    # --- primitives ------------------------------------------------------------------------
    def prim(self):
        return self.reg[PRIM] if self.reg[PRMODECONT] & 1 else (self.reg[PRIM] & 7) | (self.reg[PRMODE] & 0x7F8)

    def kick(self, draws, path):
        p = self.prim(); kind = p & 7
        self.k.add('vertices', PRIMS[kind]); self.k.add('vertices_by_path', 'PATH%d' % path)
        self.queue += 1
        if self.queue < VERTICES[kind]:
            return
        self.queue = KEEP[kind] if kind in (2, 4, 5) else 0
        if not draws:
            return
        if self.sig is None:
            self.sig = self.signature(p)
        self.k.add('prims', PRIMS[kind]); self.k.add('prims_by_path', 'PATH%d' % path)
        for key in self.sig:
            self.k.add(key[0], key[1])

    def signature(self, p):
        """(table, key) pairs counted once per primitive under the current state."""
        ctx = p >> 9 & 1
        tex0, tex1, clamp = self.reg[TEX0_1 + ctx], self.reg[TEX1_1 + ctx], self.reg[CLAMP_1 + ctx]
        alpha, test = self.reg[ALPHA_1 + ctx], self.reg[TEST_1 + ctx]
        frame, zbuf = self.reg[FRAME_1 + ctx], self.reg[ZBUF_1 + ctx]
        kind = p & 7
        attrs = ''.join(n for n, b in (('iip ', 3), ('tme ', 4), ('fge ', 5), ('abe ', 6), ('aa1 ', 7),
                                       ('fst ', 8), ('ctxt2 ', 9), ('fix ', 10)) if p >> b & 1).strip()
        out = [('prims_by_attrs', '%s %s' % (PRIMS[kind], attrs or '-'))]
        fpsm, zpsm = bits(frame, 24, 6), bits(zbuf, 24, 4) | 0x30
        fbmsk, zmsk = bits(frame, 32, 32), bits(zbuf, 32, 1)
        fbp, fbw, zbp = bits(frame, 0, 9), bits(frame, 16, 6), bits(zbuf, 0, 9)
        sc = self.reg[SCISSOR_1 + ctx]
        height = bits(sc, 48, 11) + 1
        target = (fbp, fbw, fpsm, zbp, zpsm)
        out.append(('targets', 'FRAME %03X w%d %s / ZBUF %03X %s, scissor %dx%d' % (
            fbp, fbw, PSM.get(fpsm, hex(fpsm)), zbp, PSM.get(zpsm, hex(zpsm)), bits(sc, 16, 11) + 1, height)))
        if target != self.target:
            self.k.add('target_switches', 'switch')
            self.target = target
        if (fbp, fpsm) not in self.marked:
            self.marked.add((fbp, fpsm))
            for pg in pages(fbp * 32, fbw * 64, height, fpsm):
                self.page_owner[pg] = ('render', fpsm, fbp)
        ate, atst, aref, afail = bits(test, 0, 1), bits(test, 1, 3), bits(test, 4, 8), bits(test, 12, 2)
        date, datm, zte, ztst = bits(test, 14, 1), bits(test, 15, 1), bits(test, 16, 1), bits(test, 17, 2)
        out.append(('frame_formats', '%s%s' % (PSM.get(fpsm, hex(fpsm)), ' fbmsk %08X' % fbmsk if fbmsk else '')))
        if zte and ztst != 1:
            out.append(('depth', 'test %s %s, %s' % (ZTST[ztst], PSM.get(zpsm, hex(zpsm)), 'no write' if zmsk else 'write')))
        else:
            out.append(('depth', 'no test, %s' % ('no write' if zmsk else 'write %s' % PSM.get(zpsm, hex(zpsm)))))
            if zmsk == 0 and zte == 0:
                pass
        if zmsk == 0:
            if (zbp, zpsm) not in self.marked:
                self.marked.add((zbp, zpsm))
                for pg in pages(zbp * 32, fbw * 64, height, zpsm):
                    self.page_owner[pg] = ('z', zpsm, zbp)
        if ate and atst != 1:
            out.append(('alpha_test', '%s ref %02X fail %s' % (ATST[atst], aref, AFAIL[afail])))
        else:
            out.append(('alpha_test', 'off'))
        if date:
            out.append(('dest_alpha_test', 'DATM %d' % datm))
        if p >> 6 & 1 or p >> 7 & 1:
            a, b, c, d, fix = bits(alpha, 0, 2), bits(alpha, 2, 2), bits(alpha, 4, 2), bits(alpha, 6, 2), bits(alpha, 32, 8)
            eq = '(%s-%s)*%s+%s' % (ABCD[a], ABCD[b], CABC[c], ABCD[d]) + (' FIX %02X' % fix if c == 2 else '')
            out.append(('blend', eq))
            if self.reg[PABE] & 1:
                out.append(('blend_extra', 'PABE'))
        if self.reg[FBA_1 + ctx] & 1:
            out.append(('blend_extra', 'FBA'))
        if not self.reg[COLCLAMP] & 1:
            out.append(('blend_extra', 'COLCLAMP off (wrap)'))
        if self.reg[DTHE] & 1:
            out.append(('blend_extra', 'DTHE'))
        if p >> 5 & 1:
            out.append(('fog', 'FOGCOL %06X' % (self.reg[FOGCOL] & 0xFFFFFF)))
        if p >> 4 & 1:
            tbp, tbw, tpsm = bits(tex0, 0, 14), bits(tex0, 14, 6), bits(tex0, 20, 6)
            tw, th, tcc, tfx = bits(tex0, 26, 4), bits(tex0, 30, 4), bits(tex0, 34, 1), bits(tex0, 35, 2)
            cpsm, csm = bits(tex0, 51, 4), bits(tex0, 55, 1)
            name = PSM.get(tpsm, hex(tpsm))
            if tpsm in (0x13, 0x14, 0x1B, 0x24, 0x2C):
                name += ' clut %s CSM%d' % (PSM.get(cpsm, hex(cpsm)), csm + 1)
            out.append(('texture_formats', name))
            out.append(('texture_function', '%s %s' % (TFX[tfx], 'RGBA' if tcc else 'RGB')))
            out.append(('texture_sizes', '%dx%d' % (1 << tw, 1 << th)))
            out.append(('textures', '%04X %s %dx%d' % (tbp, name, 1 << tw, 1 << th)))
            mmag, mmin, mxl, lcm = bits(tex1, 5, 1), bits(tex1, 6, 3), bits(tex1, 2, 3), bits(tex1, 0, 1)
            out.append(('texture_filter', 'mag %s min %s%s' % (FILTER[mmag], FILTER[mmin],
                                                              ' mips %d %s' % (mxl, 'fixed K' if lcm else 'LOD') if mxl else '')))
            out.append(('texture_clamp', '%s/%s' % (WM[bits(clamp, 0, 2)], WM[bits(clamp, 2, 2)])))
            if tpsm == 0x01 or (tcc and tpsm in (0x02, 0x0A)):
                out.append(('texa', 'TEXA %016X' % self.reg[TEXA]))
            owners = collections.Counter(self.page_owner.get(pg, 'unknown')
                                         for pg in pages(tbp, max(tbw, 1) * 64, 1 << th, tpsm))
            drawn = [o for o in owners if isinstance(o, tuple)]
            if drawn:
                o = drawn[0]
                if tpsm in (0x1B, 0x24, 0x2C) and o[1] in (0x01, 0x31):
                    # 8H/4HL/4HH texels live in the top byte a 24-bit colour or Z buffer leaves alone
                    out.append(('texture_in_24bit_spare_byte', '%s over %s %03X' % (name, PSM.get(o[1]), o[2])))
                else:
                    same = any(pg in pages(fbp * 32, fbw * 64, height, fpsm)
                               for pg in pages(tbp, max(tbw, 1) * 64, 1 << th, tpsm))
                    out.append(('render_to_texture', '%s %03X %s read as %s%s' % (
                        'Z' if o[0] == 'z' else 'frame', o[2], PSM.get(o[1], hex(o[1])), name,
                        ' (the bound frame)' if same else '')))
            elif 'copy' in owners:
                out.append(('render_to_texture', 'copied pages read as %s' % name))
        return out

    # --- GIF -----------------------------------------------------------------------------------
    def feed(self, path, data):
        st = self.path[path]
        for q in range(0, len(data) - 15, 16):
            lo, hi = struct.unpack_from('<QQ', data, q)
            if st is None or st[0] == 0:
                nloop, flg, nreg = lo & 0x7FFF, bits(lo, 58, 2), bits(lo, 60, 4) or 16
                self.k.add('gif_tags', 'PATH%d %s' % (path, ('PACKED', 'REGLIST', 'IMAGE', 'IMAGE')[flg]))
                if nloop and flg == 0 and lo >> 46 & 1:
                    self.write(PRIM, bits(lo, 47, 11), path)
                st = self.path[path] = [nloop, flg, nreg, hi, 0]
                continue
            self.k.add('qwords_by_path', 'PATH%d' % path)
            if st[1] >= 2:
                self.k.add('image_qwords', 'PATH%d' % path)
                st[0] -= 1
                continue
            for half in ((lo, hi),) if st[1] == 0 else ((lo, None), (hi, None)):
                if not st[0]:
                    break
                r = st[3] >> (st[4] * 4) & 15
                if st[1] == 0:
                    self.packed(r, half[0], half[1], path)
                elif r not in (14, 15, 11):
                    self.write(r, half[0], path)
                st[4] += 1
                if st[4] == st[2]:
                    st[4] = 0; st[0] -= 1

    def packed(self, r, lo, hi, path):
        if r == 0x0E:
            self.write(hi & 0x7F, lo, path)
        elif r in (XYZF2, XYZ2):
            self.write(r + 8 if hi >> 47 & 1 else r, 0, path)   # ADC: XYZ(F)3, no drawing
        elif r == PRIM:
            self.write(PRIM, lo & 0x7FF, path)
        elif r in (TEX0_1, TEX0_2, CLAMP_1, CLAMP_2):
            self.write(r, lo, path)
        elif r in (XYZF3, XYZ3):
            self.write(r, 0, path)
        elif r == 0x0F:
            pass

    def vsync(self, priv):
        self.k.fields += 1
        self.marked.clear()
        self.target = None
        pmode = struct.unpack_from('<Q', priv, 0x00)[0]
        smode2 = struct.unpack_from('<Q', priv, 0x20)[0]
        circuits = []
        for n, (fb, disp) in enumerate(((0x70, 0x80), (0x90, 0xA0))):
            if pmode >> n & 1:
                f = struct.unpack_from('<Q', priv, fb)[0]; d = struct.unpack_from('<Q', priv, disp)[0]
                circuits.append('RC%d FBP %03X w%d %s %dx%d' % (n + 1, bits(f, 0, 9), bits(f, 9, 6),
                                                                PSM.get(bits(f, 15, 5), '?'),
                                                                (bits(d, 32, 12) + 1) // (bits(d, 23, 4) + 1),
                                                                bits(d, 44, 11) + 1))
        mix = ''
        if (pmode & 3) == 3:
            mix = ' merge %s ALP %02X' % ('RC1 alpha' if not pmode >> 5 & 1 else 'ALP', bits(pmode, 8, 8))
        self.k.add('display', ' + '.join(circuits) + mix + (' interlaced' if smode2 & 1 else '') +
                   (' field mode' if (smode2 & 3) == 1 else ''))


def parse(path, census):
    data = Path(path).read_bytes()
    crc, size = struct.unpack_from('<II', data, 0)
    state = 8
    version = struct.unpack_from('<I', data, state)[0]
    regs = {}
    if version == 8:
        env = (PRIM, PRMODECONT, TEXCLUT, 0x22, TEXA, FOGCOL, DIMX, DTHE, COLCLAMP, PABE, BITBLTBUF, TRXDIR,
               TRXPOS, TRXREG, TRXREG)
        ctx = (XYOFFSET_1, TEX0_1, TEX1_1, CLAMP_1, 0x34, 0x36, SCISSOR_1, ALPHA_1, TEST_1, FBA_1, FRAME_1, ZBUF_1)
        at = state + 4
        for r in env:
            regs[r] = struct.unpack_from('<Q', data, at)[0]; at += 8
        for c in range(2):
            for r in ctx:
                regs[r + c] = struct.unpack_from('<Q', data, at)[0]; at += 8
    gs = Gs(census, regs)
    at = 8 + size + 8192
    priv = data[8 + size: at]
    while at < len(data):
        kind = data[at]; at += 1
        if kind == 0:
            p, n = data[at], struct.unpack_from('<I', data, at + 1)[0]; at += 5
            gs.feed(p if p in (1, 2, 3) else 1, data[at:at + n]); at += n
        elif kind == 1:
            at += 1
            gs.vsync(priv)
        elif kind == 2:
            at += 4
        elif kind == 3:
            priv = data[at:at + 8192]; at += 8192
        else:
            raise SystemExit('%s: unknown packet %d at %d' % (path, kind, at - 1))


def report(name, k, out):
    f = max(k.fields, 1)
    out.append('== %s: %d fields' % (name, k.fields))
    order = ('prims', 'prims_by_path', 'vertices', 'vertices_by_path', 'qwords_by_path', 'image_qwords', 'gif_tags',
             'prims_by_attrs', 'targets', 'target_switches', 'frame_formats', 'depth', 'alpha_test',
             'dest_alpha_test', 'blend', 'blend_extra', 'fog', 'texture_formats', 'texture_function',
             'texture_sizes', 'texture_filter', 'texture_clamp', 'texa', 'render_to_texture',
             'texture_in_24bit_spare_byte', 'clut_loads',
             'transfer_host_to_local', 'transfer_host_to_local_pixels', 'transfer_local_to_local',
             'transfer_local_to_host', 'sync', 'display', 'textures')
    for table in order:
        counter = k.c.get(table)
        if not counter:
            continue
        if table == 'textures':
            out.append('  textures: %d distinct (TBP0, format, size) in the group' % len(counter))
            continue
        total = sum(counter.values())
        out.append('  %s (%.1f a field):' % (table, total / f))
        for key, n in counter.most_common(12 if table not in ('display',) else 6):
            out.append('    %8.1f  %5.1f%%  %s' % (n / f, 100 * n / total, key))
        if len(counter) > 12:
            out.append('    ... %d more' % (len(counter) - 12))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dumps', nargs='+', type=Path)
    ap.add_argument('--group', action='append', default=[], help='NAME=FIRST-LAST (fields)')
    ap.add_argument('--log', type=Path, help="the game's log: each dump's first field from its 'compare: dump' line")
    ap.add_argument('--json', type=Path)
    a = ap.parse_args()
    first = {}
    if a.log:
        for m in re.finditer(r'compare: dump (\S+) field=0 vsync=(\d+)', a.log.read_text(errors='replace')):
            first[Path(m.group(1)).name] = int(m.group(2))
    groups = [(g.split('=')[0], *map(int, g.split('=')[1].split('-'))) for g in a.group]
    totals = collections.OrderedDict((g[0], Census()) for g in groups)
    totals.setdefault('other', Census())
    for dump in sorted(a.dumps):
        k = Census()
        parse(dump, k)
        field = first.get(dump.stem)
        name = next((g for g, lo, hi in groups if field is not None and lo <= field <= hi), 'other')
        totals[name].merge(k)
        print('%s: field %s, %d fields -> %s' % (dump.name, field, k.fields, name), file=sys.stderr)
    out = []
    for name, k in totals.items():
        if k.fields:
            report(name, k, out)
    print('\n'.join(out))
    if a.json:
        a.json.write_text(json.dumps({n: {'fields': k.fields, **{t: dict(c) for t, c in k.c.items()}}
                                      for n, k in totals.items() if k.fields}, indent=1))


if __name__ == '__main__':
    main()
