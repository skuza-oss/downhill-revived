#!/usr/bin/env python3
"""The findings index: one row per docs/findings/NNN-*.md, from each file's title and Status line.

docs/findings/README.md holds a hand-written area map, then the generated table between the
`<!-- index:begin -->` and `<!-- index:end -->` markers. This script rewrites that table; with
--check it only says whether the README is current (tools/check_docs.py runs that).

Every finding carries, right under its title:
    Status: current | partly superseded by NNN (<part>) | superseded by NNN | history | rejected
    | in progress
    Supersedes: NNN, ... (optional, on the finding that replaced others)
    Checked: YYYY-MM-DD   (optional: the day the finding was last compared with the code)

Usage: python tools/findings_index.py [--check]
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FINDINGS = os.path.join(ROOT, 'docs', 'findings')
README = os.path.join(FINDINGS, 'README.md')
BEGIN, END = '<!-- index:begin -->', '<!-- index:end -->'
STATUS = re.compile(r'^Status: (current|history|rejected|in progress|superseded by (\d{3})'
                    r'|partly superseded by (\d{3}) \(([^()]+)\))$')
# Numbers that were cited without a file of their own (the comments now cite the real number; the
# rows stay so an old citation in a branch or in history still resolves).
NOT_HERE = {
    '075': ('see 081', 'the portable-core plan was numbered 075 on its branch and merged as 081'),
    '113': ('see 139', 'the delta-time clock work was numbered 113 on its branch; the mixer\'s '
                       'VBLANK time is in 139'),
}


# Findings a running lane still owns, without a Status line yet: the index shows this status; the
# lane adds the line itself (check_docs.py warns until then).
LANE_OWNED = {}


def findings():
    """[(number, file name, title, status line or None, line number of the status)]"""
    out = []
    for name in sorted(os.listdir(FINDINGS)):
        m = re.match(r'(\d{3})-.+\.md$', name)
        if not m:
            continue
        lines = open(os.path.join(FINDINGS, name), encoding='utf-8', errors='replace').read().split('\n')
        title = re.sub(r'^#\s*', '', lines[0].lstrip('﻿')).strip()
        title = re.sub(r'^\d{3}\s*[:.\-—]+\s*', '', title)
        status, at = None, 0
        for i, line in enumerate(lines[1:8], 2):
            if line.startswith('Status:'):
                status, at = line.strip(), i
                break
        out.append((m.group(1), name, title, status, at))
    return out


def parse(status):
    """(kind, target, part) or None for a line that isn't in the vocabulary."""
    m = STATUS.match(status or '')
    if not m:
        return None
    kind = m.group(1)
    if m.group(2):
        return 'superseded', m.group(2), ''
    if m.group(3):
        return 'partly superseded', m.group(3), m.group(4)
    return kind, '', ''


def table():
    rows = ['| No. | Finding | Status | Read instead |', '|---|---|---|---|']
    have = {}
    for n, name, title, status, _ in findings():
        have[n] = name
        p = parse(status) or ((LANE_OWNED[n] + ' (no Status line yet)', '', '') if n in LANE_OWNED
                              else ('(no valid Status line)', '', ''))
        kind, target, part = p
        instead = f'[{target}]({have.get(target) or target})' if target else ''
        if part:
            instead += f' for {part}'
        title = title.replace('|', '\\|')
        if len(title) > 90:
            title = title[:87].rstrip() + '...'
        rows.append(f'| {n} | [{title}]({name}) | {kind} | {instead} |')
    # resolve links to later files once all names are known
    rows = [re.sub(r'\]\((\d{3})\)', lambda m: f']({have.get(m.group(1), m.group(1))})', r) for r in rows]
    for n, (see, why) in NOT_HERE.items():
        rows.append(f'| {n} | no file: {why} | renumbered | {see} |')
    return '\n'.join(rows)


def main():
    text = open(README, encoding='utf-8').read()
    if BEGIN not in text or END not in text:
        sys.exit(f'ERROR {os.path.relpath(README, ROOT)}:1 index markers missing')
    head, rest = text.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    new = head + BEGIN + '\n' + table() + '\n' + END + tail
    if '--check' in sys.argv:
        if new != text:
            print(f'ERROR docs/findings/README.md:1 index out of date: run python tools/findings_index.py')
            return 1
        return 0
    if new != text:
        open(README, 'w', encoding='utf-8', newline='\n').write(new)
        print('docs/findings/README.md: index rewritten')
    return 0


if __name__ == '__main__':
    sys.exit(main())
