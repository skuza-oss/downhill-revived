#!/usr/bin/env python3
"""Docs linter: every problem on one line, `ERROR <file>:<line> <reason>` or `WARN ...`.

Checks, over the tracked files (git ls-files):
- every "finding NNN" / "findings NNN, MMM" citation names a file in docs/findings/ (ERROR);
- code that cites a finding whose Status is `superseded by` (WARN: read the newer one);
- every finding has a valid Status line right under its title, and every `superseded by` target
  exists and is itself current or partly superseded (ERROR);
- docs/findings/README.md's generated table is current (tools/findings_index.py --check);
- every docs/...md path named in a tracked file exists (ERROR); build/audit/... paths (WARN: that
  folder is gitignored, so nobody else can read it);
- every row of AGENTS.md's "Current decisions" table cites a current finding (ERROR);
- AGENTS.md files stay short: the root at most 200 lines, nested ones at most 100 (ERROR);
- no machine paths in tracked docs and agent files: drive letters, home folders, LAN addresses
  (ERROR).

Usage: python tools/check_docs.py [--quiet]   (exit 1 when there is an ERROR)
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import findings_index  # noqa: E402

TEXT = ('.md', '.txt', '.py', '.c', '.h', '.inc', '.cpp', '.rs', '.wgsl', '.ts', '.js', '.mjs', '.json',
        '.jsonc', '.toml', '.yml', '.yaml', '.sh', '.ps1', '.cmd', '.iss', '.kts', '.gradle', '.java', '.kt',
        '.html', '.css', '.cfg', '')
DOCS = ('.md',)
# "finding 041", "findings 041, 044", "findings 107/112", "findings 146-148"; list items after the
# first need three digits, so "finding 067, 60-240 fps" is one citation.
CITE = re.compile(r'\b[Ff]indings?\s+(\d{2,3}(?![\w.])(?:\s*(?:,|/|and|-|–|—|or)\s*\d{3}(?![\w.]))*)')
NUM = re.compile(r'\d{2,3}')
DOC_PATH = re.compile(r'(?<![\w/.-])docs/[\w./-]+?\.md\b')  # not inside a URL or another path
AUDIT_PATH = re.compile(r'\bbuild/audit/[\w./-]+')
# Machine paths: a drive letter path, a home folder, a LAN address. Placeholders (<drive>, X:) and
# the patterns this file itself names are fine.
MACHINE = [
    (re.compile(r'(?<![\w`<])[A-WYZ]:[\\/](?!/)[\w$]'), 'drive letter path'),
    (re.compile(r'(?i)[\\/](?:Users|home)[\\/](?!<|\$|USER|\*|you|name|runner)[\w.-]+'), 'home folder path'),
    (re.compile(r'\b(?:192\.168|10\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b'), 'LAN address'),
]
MACHINE_FILES = ('.md', '.txt')  # docs and agent files; code is covered by tools/check_tree.py
# docs/ paths that are fine to name although they are not tracked: local-only notes (AGENTS.md says
# they may be absent) and files of other projects named as sources.
OPTIONAL_DOCS = ('docs/private/', 'docs/LEAD.md')
OTHER_PROJECTS = {'docs/p2s-savestates.md'}  # Pedroj-64/DownHill-Port-PC, finding 040's source


def tracked():
    r = subprocess.run(['git', 'ls-files', '-z'], cwd=ROOT, capture_output=True)
    files = [p for p in r.stdout.decode('utf-8', 'replace').split('\0') if p]
    if r.returncode or not files:  # not a repository, safe.directory: checking nothing must not pass
        sys.exit('check_docs: git ls-files failed: %s' % r.stderr.decode('utf-8', 'replace').strip())
    return files


def main():
    problems = []

    def report(kind, path, line, why):
        problems.append((kind, f'{kind} {path}:{line} {why}'))

    found = findings_index.findings()
    have = {n: name for n, name, *_ in found}
    status = {}
    for n, name, title, line, at in found:
        p = findings_index.parse(line)
        path = f'docs/findings/{name}'
        if p is None:
            if n in findings_index.LANE_OWNED:
                report('WARN', path, 1, 'no Status line yet (owned by a running lane)')
                status[n] = (findings_index.LANE_OWNED[n], '')
            else:
                report('ERROR', path, at or 1, f'no valid Status line under the title ({line or "missing"})')
            continue
        status[n] = (p[0], p[1])
        if at != 3:
            report('WARN', path, at, 'the Status line belongs on line 3, right under the title')
    for n, (kind, target) in status.items():
        if not target:
            continue
        path = f'docs/findings/{have[n]}'
        if target not in have:
            report('ERROR', path, 3, f'{kind} by {target}: no such finding')
        elif status.get(target, ('',))[0] not in ('current', 'partly superseded', 'in progress'):
            report('ERROR', path, 3, f'{kind} by {target}, which is {status[target][0]}: point at a live finding')
    if subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'findings_index.py'), '--check'],
                                                         cwd=ROOT, capture_output=True).returncode:
        report('ERROR', 'docs/findings/README.md', 1, 'index out of date: run python tools/findings_index.py')

    # AGENTS.md's current decisions must rest on current findings
    agents = open(os.path.join(ROOT, 'AGENTS.md'), encoding='utf-8').read().split('\n')
    in_table = False
    for i, line in enumerate(agents, 1):
        if line.startswith('## '):
            in_table = line.startswith('## Current decisions')
        elif in_table and line.startswith('|') and not line.startswith(('| Decision', '|---')):
            cells = [c.strip() for c in line.strip('|').split('|')]
            for k in NUM.findall(cells[2] if len(cells) > 2 else ''):
                n = '%03d' % int(k)
                if status.get(n, ('',))[0] != 'current':
                    report('ERROR', 'AGENTS.md', i, f'decision rests on finding {n}, which is '
                           f'{status.get(n, ("missing",))[0]}: point it at a current finding')

    for path in tracked():
        ext = os.path.splitext(path)[1].lower()
        if ext not in TEXT or path.startswith(('asm/', 'port/render/vendor/', 'port/third_party/')):
            continue
        full = os.path.join(ROOT, path)
        try:
            text = open(full, encoding='utf-8').read()
        except (UnicodeDecodeError, OSError):
            continue
        in_findings = path.startswith('docs/findings/')
        is_code = ext not in DOCS
        base = os.path.basename(path)
        for i, line in enumerate(text.split('\n'), 1):
            for m in CITE.finditer(line):
                nums = NUM.findall(m.group(1))
                for k in nums:
                    n = '%03d' % int(k)
                    if n not in have:
                        report('ERROR', path, i, f'cites finding {n}, which does not exist'
                               + (f' ({findings_index.NOT_HERE[n][0]})' if n in findings_index.NOT_HERE else ''))
                    elif is_code and status.get(n, ('',))[0] == 'superseded':
                        report('WARN', path, i, f'cites finding {n}, superseded: read the finding that replaced it')
            for m in DOC_PATH.finditer(line):
                p = m.group(0)
                if '<' in line[max(0, m.start() - 1):m.end() + 1] or 'NNN' in p or '*' in p:
                    continue
                if p.startswith(OPTIONAL_DOCS) or p in OTHER_PROJECTS or base == 'check_docs.py':
                    continue
                if not os.path.exists(os.path.join(ROOT, p)):
                    report('ERROR', path, i, f'names {p}, which does not exist')
            if not in_findings and AUDIT_PATH.search(line) and base != 'check_docs.py':
                report('WARN', path, i, f'names {AUDIT_PATH.search(line).group(0)}: build/ is not tracked, nobody else can read it')
            if ext in MACHINE_FILES and base != 'check_docs.py':
                for rx, why in MACHINE:
                    if rx.search(line):
                        report('ERROR', path, i, f'{why}: {rx.search(line).group(0)}')
        if base == 'AGENTS.md':
            n = text.count('\n')
            cap = 200 if path == 'AGENTS.md' else 100
            if n > cap:
                report('ERROR', path, n, f'{n} lines, over the {cap}-line cap: move detail into docs or a skill')

    errors = sum(1 for k, _ in problems if k == 'ERROR')
    warns = len(problems) - errors
    for _, line in problems:
        if '--quiet' not in sys.argv or line.startswith('ERROR'):
            print(line)
    print(f'check_docs: {errors} error(s), {warns} warning(s)')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
