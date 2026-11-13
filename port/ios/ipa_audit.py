"""The one audit of every iOS release IPA, built or supplied (finding 227): package_unsigned.py runs it
on the bundle it stages, package_release.py's ios_release on every member of the IPA it publishes.

An unsigned release carries nothing of the builder: no signature or provisioning file, no Mach-O
(the executable, a framework or a dylib) with a code signature in any of its slices, nothing that
names a home folder, a device, a team or the builder's own literals (DHD_IPA_PRIVATE, comma-separated,
kept outside the repository). A Mach-O whose load commands can't be read can't be verified and is
refused. Pure Python: it runs on any machine, no codesign needed."""
import os
import re

LC_CODE_SIGNATURE = 0x1D
THIN = (b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe')     # MH_MAGIC_64, MH_MAGIC (little-endian)
FAT = (b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf')      # FAT_MAGIC, FAT_MAGIC_64 (big-endian)


def _slice_signed(data, base):
    """One thin Mach-O at base: True when its load commands hold LC_CODE_SIGNATURE."""
    magic = data[base:base + 4]
    if magic not in THIN:
        raise ValueError('a slice that is not a Mach-O')
    header = 32 if magic == THIN[0] else 28
    if len(data) < base + header:
        raise ValueError('a cut Mach-O header')
    ncmds = int.from_bytes(data[base + 16:base + 20], 'little')
    end = base + header + int.from_bytes(data[base + 20:base + 24], 'little')
    if ncmds > 4096 or end > len(data):
        raise ValueError('load commands past the end')
    at = base + header
    for _ in range(ncmds):
        if at + 8 > end:
            raise ValueError('a cut load command')
        cmd = int.from_bytes(data[at:at + 4], 'little')
        size = int.from_bytes(data[at + 4:at + 8], 'little')
        if size < 8 or at + size > end:
            raise ValueError('a load command of a wrong size')
        if cmd == LC_CODE_SIGNATURE:
            return True
        at += size
    return False


def macho_signed(data):
    """None when data isn't a Mach-O; True when a slice carries a code signature; False when none
    does. ValueError when it is a Mach-O that can't be read."""
    magic = data[:4]
    if magic in THIN:
        return _slice_signed(data, 0)
    if magic in FAT:
        wide = magic == FAT[1]
        entry = 32 if wide else 20
        count = int.from_bytes(data[4:8], 'big')
        if not 0 < count <= 32 or 8 + count * entry > len(data):
            raise ValueError('a fat header that does not fit')
        signed = False
        for k in range(count):
            e = 8 + k * entry
            offset = int.from_bytes(data[e + 8:e + 16] if wide else data[e + 8:e + 12], 'big')
            if offset >= len(data):
                raise ValueError('a fat slice past the end')
            signed = _slice_signed(data, offset) or signed
        return signed
    return None


def identity_patterns():
    """What no release IPA may name."""
    patterns = [
        (re.compile(rb'/Users/[A-Za-z0-9._-]+/'), 'a home folder path'),
        (re.compile(rb'/home/[A-Za-z0-9._-]+/'), 'a home folder path'),
        (re.compile(rb'[A-Z]:\\Users\\[A-Za-z0-9._ -]+', re.IGNORECASE), 'a home folder path'),
        (re.compile(rb'0000[0-9A-F]{4}-[0-9A-F]{16}'), 'a device UDID'),
        (re.compile(rb'(?<![0-9a-f])[0-9a-f]{40}(?![0-9a-f])'), None),   # an old-style UDID is a SHA-1: too common to refuse
        (re.compile(rb'com\.apple\.developer\.team-identifier'), 'a team entitlement'),
        (re.compile(rb'<key>application-identifier</key>'), 'an application identifier entitlement'),
        (re.compile(rb'<key>ProvisionedDevices</key>'), 'a provisioning profile'),
    ]
    patterns = [(p, d) for p, d in patterns if d]
    for private in filter(None, (s.strip() for s in os.environ.get('DHD_IPA_PRIVATE', '').split(','))):
        patterns.append((re.compile(re.escape(private.encode()), re.IGNORECASE), 'a builder identifier'))
    return patterns


def name_problems(name, patterns=None):
    """An identity in a member's path (its UTF-8 name, directories included): a file or folder named
    after a device, a team or the builder's literals is as public as its bytes (the review of lane
    227)."""
    encoded = name.encode('utf-8', 'surrogateescape')
    return ['%s: %s in its name (%r)' % (name, desc, m.group(0)[:60])
            for pattern, desc in (patterns if patterns is not None else identity_patterns())
            for m in [pattern.search(encoded)] if m]


def member_problems(name, data, patterns=None):
    """What makes one member of an app bundle (its path, its bytes) unfit for an unsigned release."""
    problems = name_problems(name, patterns)
    parts = name.replace('\\', '/').split('/')
    if parts[-1] in ('embedded.mobileprovision', 'CodeResources') or '_CodeSignature' in parts:
        problems.append('%s: a signature or provisioning file (the IPA must be unsigned)' % name)
    try:
        if macho_signed(data):
            problems.append('%s: a Mach-O that still carries a code signature' % name)
    except ValueError as e:
        problems.append("%s: a Mach-O that can't be verified (%s)" % (name, e))
    for pattern, desc in patterns if patterns is not None else identity_patterns():
        m = pattern.search(data)
        if m:
            problems.append('%s: %s (%r)' % (name, desc, m.group(0)[:60]))
    return problems
