"""PC build, data stage: the PS2 data image. The splat data sections (asm/data/*.s, generated
from your own ELF and never committed) become one block of x86 assembler, build/port/data/image.s,
that the runtime copies into the emulated PS2 RAM (port/runtime/memory.c), byte for byte where
the PS2 had it. The game's data lives there, at PS2_RAM_BASE + its PS2 address, so the uncached
mirrors (0x20000000/0x30000000 | address), DMA tag addresses and fixed RAM addresses keep working.

- The image runs from .vutext (assets/vutext.bin, extracted by splat from your ELF) through
  .data, .rodata, .gcc_except_table and .sdata, in PS2 address order. .sbss and .bss are zero in
  the RAM block and need no bytes.
- Every data label (D_..., jtbl_..., ehtbl_..., named ones) becomes an absolute symbol,
  PS2_RAM_BASE + address (wasm32 C symbols carry no prefix: LABEL_PREFIX). C and C++ units
  declare these extern (transform.extern_image_data), so there is one copy, the retail one.
- .word values: symbols become their absolute or code addresses; switch-table entries (.L...)
  their PS2 address (the case values of recompiled switches, port/tools/recomp.py); exception
  ranges ($LEH_...) 0. Some words splat took for addresses are text or instructions (the
  "m0:" of "cdrom0:" is 0x003A306D): a D_ target that is not word-aligned, or a word that follows
  text and ends a string, stays its number (not_pointer).
- Wanted symbols outside the image: D_<address> below the program (VU memory addresses) and
  inside the code (labels in asm functions, callbacks) stay PS2 numbers, as recompiled code sees
  code; scratchpad addresses (D_7000xxxx) stay numbers too (mapped at 0x70000000, port/runtime/memory.c); any other RAM
  address (the heap start D_8A0400, the stack) is PS2_RAM_BASE + address.
The label offsets in the assembled image are checked against their addresses (check_image).
usage: python port/tools/data.py   (normally run by port/tools/build_wasm.py's image stage)"""
import ast, bisect, glob, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
P = LABEL_PREFIX = ''  # the module's C symbols carry no prefix (finding 082, "W5")
# The emulated PS2 RAM (32 MB) sits at this guest address, with the uncached mirrors at
# 0x20000000/0x30000000 + it (port/runtime/memory.c). Below 0x10000000, so a PS2 DMA address
# (pointer & 0x0FFFFFFF) is still the guest pointer.
PS2_RAM_BASE = 0x08000000
from recomp import data_entries  # noqa: E402
import recomp  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VUTEXT = (0x263BC0, 0x26DD00)  # vram of .vutext (config/splat.yaml)
RAM_END = 0x2000000
GP = 0x2A40F0  # _gp
SCRATCHPAD = (0x70000000, 0x70004000)
OUT = os.path.join(ROOT, 'build', 'port', 'data')
FILES = ['data.data.s', 'rodata.rodata.s', 'gcc_except_table.gcc_except_table.s', 'sdata.sdata.s',
         'sbss.sbss.s', 'bss.bss.s']  # PS2 address order
ZERO = ('sbss.sbss.s', 'bss.bss.s')
ADDR_COMMENT = re.compile(r'/\*\s+(?:[0-9A-F]+\s+)?([0-9A-F]{8})(?:\s+[0-9A-F]+)?\s+\*/')
COMMENT = re.compile(r'/\*.*?\*/')
CODE_LABEL = re.compile(r'^(\.L|\$LEH_)')
SYMBOL = re.compile(r'(?<![\w.$])[A-Za-z_.$][\w.$]*')  # not the x10 of 0x10


def labels(path):
    """[(name, PS2 address)] of the dlabels in one splat data file."""
    out, pending = [], []
    for line in open(path, encoding='utf-8'):
        s = line.strip()
        if s.startswith('dlabel '):
            pending.append(s.split()[1])
            continue
        m = ADDR_COMMENT.search(line)
        if m and pending:
            out += [(n, int(m.group(1), 16)) for n in pending]
            pending = []
    return out


def image_symbols():
    """{name: PS2 address} of every data label (what C units must declare extern)."""
    out = {}
    for f in FILES:
        out.update(labels(os.path.join(ROOT, 'asm', 'data', f)))
    return out


class ImageNames:
    """Names the image defines: its labels, and any D_<address> in the RAM data range (splat may
    fold an address into a larger label while a C unit defines it as its own variable)."""

    def __init__(self):
        self._labels = None

    @property
    def labels(self):  # read on first use: importing build_wasm.py needs no asm/ (testprog.py)
        if self._labels is None:
            self._labels = image_symbols()
        return self._labels

    def __contains__(self, name):
        if name in self.labels:
            return True
        m = re.fullmatch(r'D_([0-9A-F]{8})', name)
        return bool(m) and VUTEXT[0] <= int(m.group(1), 16) < RAM_END

    def __bool__(self):
        return True


def absolute(addr):
    """The host value of a wanted PS2 address."""
    if addr < VUTEXT[0] or SCRATCHPAD[0] <= addr < SCRATCHPAD[1]:
        return '0x%X' % addr  # VU memory address, code, scratchpad
    if addr >> 28 in (2, 3):  # uncached (accelerated) RAM: its mirror (port/runtime/memory.c)
        return '0x%X' % (addr & 0xF0000000 | PS2_RAM_BASE + (addr & (RAM_END - 1)))
    return '0x%X' % (PS2_RAM_BASE + addr)


def operand(expr, refs=None):
    m = re.match(r'^\.L([0-9A-F]{8})$', expr)
    if m:  # a switch-table entry: its PS2 address, which recompiled code switches on
        return '0x' + m.group(1)
    if CODE_LABEL.match(expr):
        return '0'
    names = SYMBOL.findall(expr)
    if refs is not None:
        refs.update(P + n for n in names)
    return SYMBOL.sub(lambda s: P + s.group(0), expr)


WORD_BYTES = re.compile(r'/\*\s*\w+\s+[0-9A-F]{8}\s+([0-9A-F]{8})\s*\*/')


def is_text(word):
    return all(32 <= c < 127 for c in word)


def not_pointer(target, word, prev):
    """A .word D_<target> splat took for an address, given its bytes and the previous word's."""
    m = re.match(r'^D_([0-9A-F]{8})$', target)
    if not m or word is None:
        return False
    return int(m.group(1), 16) % 4 != 0 or (word[3] == 0 and is_text(word[:3]) and prev is not None
                                            and is_text(prev))


# The rider-help string table starts with a pointer whose bytes look like an
# ASCIZ string to splat. These slots belong to the char* menu tables in
# 001EB1D8.cpp; keep the verified pointers separate from real text.
ASCIZ_POINTERS = {
    0x00275728: 'D_00294A68',
    0x00275870: 'D_00295F28',
    0x00275874: 'D_00295F38',
    0x00275878: 'D_00295F48',
    0x0027587C: 'D_00295F58',
    0x00275880: 'D_00295F68',
    0x00275884: 'D_00295F78',
    0x002758E8: 'D_00296148',
    0x002758EC: 'D_00296168',
    0x00275938: 'D_00296220',
    0x0027593C: 'D_00296240',
    0x00275940: 'D_00296250',
    0x00275944: 'D_00296260',
    0x00275948: 'D_00296278',
    0x002759D0: 'D_00296478',
    0x00275A08: 'D_00296540',
    0x00275A0C: 'D_00296240',
    0x00275A10: 'D_00296558',
    0x00275A18: 'D_00296570',
    0x00275A80: 'D_00296728',
    0x00275A84: 'D_00296728',
    0x00275A88: 'D_00296728',
    0x00275A8C: 'D_00296728',
    0x00275A90: 'D_00296750',
    0x00275A94: 'D_00296750',
    0x00275A98: 'D_00296750',
    0x00275A9C: 'D_00296750',
    0x00275AA0: 'D_00296760',
    0x00275AA4: 'D_00296760',
    0x00275AA8: 'D_00296760',
    0x00275AAC: 'D_00296760',
    0x00275B10: 'D_00296830',
    0x00275B14: 'D_00296830',
    0x00275B18: 'D_00296830',
    0x00275B1C: 'D_00296830',
    0x00275B20: 'D_00296840',
    0x00275B24: 'D_00296840',
    0x00275B28: 'D_00296840',
    0x00275B2C: 'D_00296840',
    0x00275B30: 'D_00296850',
    0x00275B34: 'D_00296850',
    0x00275B38: 'D_00296850',
    0x00275B3C: 'D_00296850',
    0x00275B50: 'D_00296868',
    0x0029D888: 'D_00296120',
    0x0029D88C: 'D_00296130',
}


# String pointers splat left as numbers: they point inside a labelled block of strings (no
# label of their own), so the .word holds the raw PS2 address. Natively that is not a pointer:
# OPTIONS > CONTROLS drew D_0029D8B0's title from address 0x002963C0 and crashed (finding 050).
# Slot -> the string's PS2 address (rodata 0x2949E0-0x2968E8, checked against the bytes).
RAW_STRING_POINTERS = {
    0x0029D854: 0x00295E90,  # D_0029D848: four {" ", "The undisputed extreme ..."} pairs
    0x0029D85C: 0x00295E90,
    0x0029D864: 0x00295E90,
    0x0029D86C: 0x00295E90,
    0x0029D8B0: 0x002963C0,  # "SELECT CONTROL OPTIONS" (CONTROLS' title, D_0029E074)
    0x0029D904: 0x002968E8,  # D_0029D8F8: " !\"#$%..." after "TARGET"
}


# Particle defaults are 0x40-byte records with vector/matrix pointers at
# +0x14/+0x18 (0022CFB8 and 0022C468). Splat marked these 13 records as floats.
FLOAT_POINTERS = {}
for _index, _position, _matrix in (
        (0, 0x27BD00, 0x27BD80), (1, 0x27BCC0, 0x27BD80),
        (2, 0x27BD50, 0x27BD80), (3, 0x27BD00, 0x27BD80),
        (4, 0x27BD00, 0x27BD80), (5, 0x27BD20, 0x27BD80),
        (6, 0x27BD00, 0x27BD80), (7, 0x27BCC0, 0x27BD80),
        (8, 0x27BD00, 0x27BD80), (9, 0x27BD00, 0x27BD80),
        (10, 0x27BD40, 0x27BE40), (17, 0x27BD30, 0x27BE40),
        (18, 0x27BD00, 0x27BD80)):
    FLOAT_POINTERS[0x27BFC0 + _index * 0x40 + 0x14] = 'D_%08X' % _position
    FLOAT_POINTERS[0x27BFC0 + _index * 0x40 + 0x18] = 'D_%08X' % _matrix


# The view descriptors D_0027ECC0 (12 records of 0xC0, func_0023C8D8) hold halfword pairs at +4
# (width, height) and +0xC (x, y of the view's corner). Where the pair reads as an address in
# the image (256 x 112 is 0x00700100), splat wrote it as one and the image relocated it: the 3-4
# player views got height 2160 and the top row's draws missed the screen (finding 041).
NUMBERS = {0x27ECC0 + _index * 0xC0 + _offset for _index in range(12) for _offset in (4, 0xC)}

# D_0027D300: 77 particle scale records, 0x50 bytes each (0022C468).
# +0x18 packs two timing halfwords, not a pointer. Relocating e.g. 0x00780000
# to 0x08780000 changes duration 120 to 2168; the pickup's flare then stays
# bright instead of decaying before it is drawn (finding 061).
NUMBERS.update(0x27D300 + _index * 0x50 + 0x18 for _index in range(77))

# Round 46 audit (finding 062): these SDK blocks are copied MIPS instructions,
# not pointers, including aligned AND/MULT words which splat names D_<address>.
# Keep the intervening syscall descriptor tables (native function pointers).
INSTRUCTION_WORDS = set()
for _start, _end in ((0x282230, 0x2829D8), (0x2829F8, 0x283160),
                     (0x2831A0, 0x283518)):
    INSTRUCTION_WORDS.update(range(_start, _end, 4))
NUMBERS.update(INSTRUCTION_WORDS)

# InitTLB's 16-byte {PageMask, EntryHi, EntryLo0, EntryLo1} records,
# consumed by syscall 0x56 in 0025F6E0. Even the word splat named
# func_00180000 is an EntryHi value. D_002837D0's table pointers stay pointers.
TLB_WORDS = set(range(0x283560, 0x2837D0, 4))
NUMBERS.update(TLB_WORDS)

# Inline rider text (Japan / '?' in D_0026F9D0), a popup's retry text,
# a NUL-separated sound-name block, and the trick-help button-glyph string.
# Text may start mid-word or contain glyph bytes beyond printable ASCII.
TEXT_WORDS = {0x26FA6C, 0x270130, 0x28C82C, 0x2935D0, 0x2935D4, 0x29460C}
NUMBERS.update(TEXT_WORDS)

# inflate's cplext (D_002994F8): 32 unsigned-short extra-bit counts,
# including trailing invalid-code sentinels 0,99,99,0; 00240CB8 reads e[*p-s].
INFLATE_EXTRA_WORDS = set(range(0x2994F8, 0x299538, 4))
NUMBERS.update(INFLATE_EXTRA_WORDS)


NUMBER = re.compile(r'0x[0-9A-Fa-f]+|\d+')


class Fixups(list):
    """The words of the image that differ from the retail bytes: [(label, [operand, ...])], one
    entry per .long line, its words at label, label + 4, ... A plain number equal to the
    retail word (a switch-table entry) needs none."""

    def add(self, out, operands, word):
        if len(operands) == 1 and word is not None and NUMBER.fullmatch(operands[0]) \
                and int(operands[0], 0) == int.from_bytes(word, 'little'):
            out.append('.long ' + operands[0])
            return
        label = 'fix.%d' % len(self)
        self.append((label, operands))
        out += [label + ':', '.long ' + ', '.join(operands)]


def convert(path, out, addrs, refs, fixups):
    """Appends one data file's bytes to out (a list of assembler lines). Each label is placed
    with .org at its PS2 offset (filling the gaps between sections; the assembler stops if data
    ran past it) and marked with a local label img.<name> for check_image. The words that are
    not the retail bytes go to fixups as well."""
    word = None  # the bytes of the current line, and of the line before (None: not one word)
    for raw in open(path, encoding='utf-8'):
        b = WORD_BYTES.search(raw)
        prev, word = word, bytes.fromhex(b.group(1)) if b else None
        line = COMMENT.sub('', raw).strip()
        if not line or line.startswith('.include') or line.startswith('nonmatching') \
                or line.startswith('.section') or line.startswith('enddlabel'):
            continue
        op, rest = (line.split(None, 1) + [''])[:2]
        if op == 'dlabel':
            out += ['.org 0x%X' % (addrs[rest] - VUTEXT[0]), 'img.%s:' % rest]
        elif op == '.align':
            out.append('.p2align %s' % rest)
        elif op == '.word' and (m := ADDR_COMMENT.search(raw)) and int(m.group(1), 16) in RAW_STRING_POINTERS:
            target = RAW_STRING_POINTERS[int(m.group(1), 16)]
            if int(rest.strip(), 16) != target:
                raise ValueError('%s: unexpected word for string pointer 0x%08X' % (path, target))
            fixups.add(out, [absolute(target)], word)
        elif op == '.word' and (not_pointer(rest.strip(), word, prev) or (
                (m := ADDR_COMMENT.search(raw)) and int(m.group(1), 16) in NUMBERS and word is not None)):
            out.append('.long 0x%08X' % int.from_bytes(word, 'little'))
        elif op == '.word' and 'func_%s' % rest.strip()[2:] in data_entries():
            # a code entry splat left as a number (recomp.data_entries): its recompiled function
            fixups.add(out, [operand('func_%s' % rest.strip()[2:], refs)], word)
        elif op == '.word':
            fixups.add(out, [operand(s.strip(), refs) for s in rest.split(',')], word)
        elif op == '.asciz' and (m := ADDR_COMMENT.search(raw)) and int(m.group(1), 16) in ASCIZ_POINTERS:
            target = ASCIZ_POINTERS[int(m.group(1), 16)]
            encoded = ast.literal_eval(rest).encode('latin1') + b'\0'
            if len(encoded) != 4 or int.from_bytes(encoded, 'little') != int(target[2:], 16):
                raise ValueError('%s: unexpected bytes for pointer %s' % (path, target))
            fixups.add(out, [operand(target, refs)], None)
        elif op == '.float' and (m := ADDR_COMMENT.search(raw)) and int(m.group(1), 16) in FLOAT_POINTERS:
            target = FLOAT_POINTERS[int(m.group(1), 16)]
            expected = int(target[2:], 16).to_bytes(4, 'little')
            if word != expected or struct.pack('<f', float(rest)) != expected:
                raise ValueError('%s: unexpected float bytes for pointer %s' % (path, target))
            fixups.add(out, [operand(target, refs)], None)
        elif op in ('.float', '.byte', '.short', '.asciz', '.space'):
            out.append(line)
        else:
            raise ValueError('%s: unknown line %r' % (path, line))


def forwarding_aliases(wanted):
    """Data-named text entries which only forward to a native function.
    Inspect the whole labelled body, including delay slots, before aliasing.
    Other code/data addresses retain their existing meaning.
    """
    paths = sorted(glob.glob(os.path.join(recomp.ASM, 'func_*.s')))
    starts = [int(os.path.basename(p)[5:13], 16) for p in paths]
    aliases = {}
    for symbol in sorted(wanted):
        m = re.fullmatch(re.escape(P) + r'D_([0-9A-F]{8})', symbol)
        if not m: continue
        address = int(m[1], 16)
        if not 0x10A000 <= address < VUTEXT[0]: continue
        index = bisect.bisect_right(starts, address) - 1
        if index < 0: continue
        insns, active = [], False
        for line in open(paths[index], encoding='utf-8'):
            if line.strip() == 'alabel D_' + m[1]:
                active = True
                continue
            if active and re.match(r'\s*(?:endlabel|[ag]label)\b', line): break
            instruction = recomp.INSN.match(line)
            if active and instruction:
                args = recomp.split_args(instruction[4]) if instruction[4] else []
                insns.append((int(instruction[1], 16), instruction[3], args))
        target = recomp.transparent_target(insns)
        if target: aliases[symbol] = P + target
    return aliases


def alias_directives(aliases):
    if not aliases: return []
    # This object contains only data/directives, hence no unsafe SEH handlers.
    return ['.def @feat.00; .scl 3; .type 0; .endef', '.set @feat.00, 1',
            '.section .drectve,"yn"'] + [
        '.ascii " /alternatename:%s=%s"' % (name, target)
        for name, target in sorted(aliases.items())]


def convert_all(wanted=()):
    """Writes image.s (the bytes, for the checks only: dhd.exe doesn't hold them), symbols.s
    (every label and wanted address as an absolute symbol) and the fixup list; returns the paths
    of the first two, {name: address} of the image labels and the fixups."""
    os.makedirs(OUT, exist_ok=True)
    syms, refs, fixups = image_symbols(), set(), Fixups()
    img = ['.section .ps2img,"dr"', '.p2align 6', '_ps2_image:',
           '.incbin "%s"' % os.path.join(ROOT, 'assets', 'vutext.bin').replace(os.sep, '/')]
    for f in FILES:
        if f not in ZERO:
            convert(os.path.join(ROOT, 'asm', 'data', f), img, syms, refs, fixups)
    img += ['_ps2_image_end:']
    out = ['.globl %s_gp' % P, '.set %s_gp, %s' % (P, absolute(GP))]  # the small data's base
    for name, addr in sorted(syms.items()):
        out += ['.globl %s%s' % (P, name), '.set %s%s, %s' % (P, name, absolute(addr))]
    aliases = forwarding_aliases(set(wanted) | refs)
    for sym in sorted((set(wanted) | refs) - {P + n for n in syms} - aliases.keys()):  # the objects' and the image's
        m = re.match(r'^(?:D_|\.L)([0-9A-F]+)$', sym[len(P):] if sym.startswith(P) else sym)
        if m and (int(m.group(1), 16) & 0xCFFFFFFF < RAM_END or SCRATCHPAD[0] <= int(m.group(1), 16) < SCRATCHPAD[1]):
            out += ['.globl %s' % sym, '.set %s, %s' % (sym, absolute(int(m.group(1), 16)))]
    out += alias_directives(aliases)
    paths = []
    for name, lines in (('image.s', img), ('symbols.s', out)):
        dst = os.path.join(OUT, name)
        with open(dst, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
        paths.append(dst)
    return paths, syms, fixups


def coff_section(obj, name):
    """The raw bytes of one section of a COFF object (clang's output for image.s)."""
    data = open(obj, 'rb').read()
    count, = struct.unpack_from('<H', data, 2)
    optional, = struct.unpack_from('<H', data, 16)
    for k in range(count):
        at = 20 + optional + 40 * k
        if data[at:at + 8].rstrip(b'\0') == name.encode():
            size, offset = struct.unpack_from('<II', data, at + 16)
            return data[offset:offset + size]
    raise ValueError('%s: no section %s' % (obj, name))


def write_fixups(obj, fixups, nm):
    """Writes fixups.s, what dhd.exe holds of the image instead of its bytes: {PS2 address,
    value} pairs for the words that differ from the retail ELF (port/runtime/memory.c applies
    them to the bytes read from the player's disc). Checks on the way that the image assembled
    from asm/data (obj) is exactly the retail ELF's bytes everywhere else; returns
    (fixups.s, number of words)."""
    at = {}
    for line in nm:
        parts = line.split()
        if len(parts) == 3 and parts[2].startswith('fix.'):
            at[parts[2]] = int(parts[0], 16)
    image = coff_section(obj, '.ps2img')
    if VUTEXT[0] + len(image) != IMAGE_END:
        raise ValueError('the image is 0x%X bytes, ends at 0x%X, not 0x%X' % (
            len(image), VUTEXT[0] + len(image), IMAGE_END))
    retail = bytearray(retail_image())
    lines = ['.section .rdata,"dr"', '.p2align 2', '.globl _ps2_fixups', '_ps2_fixups:']
    words = 0
    for label, operands in fixups:
        for k, value in enumerate(operands):
            offset = at[label] + 4 * k
            retail[offset:offset + 4] = image[offset:offset + 4]  # the field as assembled
            lines.append('.long 0x%X, %s' % (VUTEXT[0] + offset, value))
            words += 1
    if bytes(retail) != image:
        bad = next(k for k in range(len(image)) if retail[k] != image[k])
        raise ValueError('the image differs from the retail ELF at 0x%X outside the fixups' % (VUTEXT[0] + bad))
    lines += ['.globl _ps2_fixups_end', '_ps2_fixups_end:']
    dst = os.path.join(OUT, 'fixups.s')
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    return dst, words


IMAGE_END = 0x29DCF0  # the end of .sdata (port/runtime/memory.h PS2_IMAGE_END)
ELF = os.path.join(ROOT, 'iso', 'SCUS_971.77')  # config/splat.yaml target_path


def retail_image():
    """The retail ELF's bytes from VUTEXT[0] to IMAGE_END, as the runtime reads them from the
    disc (port/hle/game_data.c): from its one loadable segment."""
    elf = open(ELF, 'rb').read()
    phoff, = struct.unpack_from('<I', elf, 0x1C)
    _, offset, vaddr, _, filesz, _, _, _ = struct.unpack_from('<8I', elf, phoff)
    if not vaddr <= VUTEXT[0] < IMAGE_END <= vaddr + filesz:
        raise ValueError('%s: the loadable segment does not hold the data image' % ELF)
    return elf[offset + VUTEXT[0] - vaddr:offset + IMAGE_END - vaddr]


def check_image(obj, syms, nm):
    """Label offsets in the assembled image (llvm-nm output lines 'value type name') against
    their PS2 addresses; returns the mismatches."""
    bad = []
    for line in nm:
        parts = line.split()
        if len(parts) == 3 and parts[2].startswith('img.'):
            name = parts[2][4:]
            want = syms[name] - VUTEXT[0]
            if int(parts[0], 16) != want:
                bad.append('%s at +0x%X, want +0x%X' % (name, int(parts[0], 16), want))
    return bad


if __name__ == '__main__':
    ps, sy, fx = convert_all()
    print('%d labels, %d fixup lines; %s' % (len(sy), len(fx), ', '.join(ps)))
