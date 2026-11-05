#!/usr/bin/env python3
"""The public never-commit rules (AGENTS.md), checked on files: no game data, no secrets, no
server address other than the official wss://dhd.skuza.app, no machine paths. One line per
problem, `ERROR <file>:<line> <reason>`, then a summary; exit 1 on any ERROR.

    python tools/check_tree.py            # every tracked file
    python tools/check_tree.py --staged   # the files staged for the next commit (the pre-commit hook)
    python tools/check_tree.py FILE...    # these files

Binary files that are staged or named are also scanned for runs of the retail executable when
your iso/SCUS_971.77 is present (port/tools/scan_retail.py). This check covers the public rules
only; a maintainer's local push guard may check more.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OFFICIAL = 'dhd.skuza.app'
RUN_ON = re.compile(r'^dhd\.skuza\.app(?:name|wss)\w*$', re.I)  # see url_ok

# Game data and other never-commit files, by name.
DATA_EXT = {'.iso', '.elf', '.irx', '.bnk', '.vag', '.ngp', '.rtx', '.tex', '.pss', '.ps2', '.mc2',
            '.ps2img', '.gs', '.p2s', '.p2m2', '.img', '.cue', '.chd', '.cso', '.mcd', '.sav', '.dmp',
            '.wav', '.bmp'}
DATA_NAMES = re.compile(r'(?i)(^|/)(SCUS_971\.77|SLES_522\.02|SYSTEM\.CNF|[^/]*\.IRX|bios[^/]*\.bin)$')
DATA_DIRS = ('asm/', 'assets/', 'iso/', 'saves/', 'build/')
KEY_EXT = {'.keystore', '.jks', '.p12', '.pfx', '.pem', '.key'}
BIG = 1 << 20  # binary files over 1 MiB need a reason
BIG_OK = {'port/launcher/src-tauri/icons/icon.icns': 'the launcher icon for macOS'}

SECRETS = [
    (re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----'), 'private key'),
    (re.compile(r'\bgh[pousr]_[A-Za-z0-9]{36,}\b|\bgithub_pat_[A-Za-z0-9_]{40,}'), 'GitHub token'),
    (re.compile(r'\bAKIA[0-9A-Z]{16}\b'), 'AWS access key'),
    (re.compile(r'\bxox[baprs]-[A-Za-z0-9-]{10,}'), 'Slack token'),
    (re.compile(r'\bsk-(?:ant-|proj-)?[A-Za-z0-9_-]{32,}'), 'API key'),
    (re.compile(r'(?i)\b(?:password|passwd|secret|api[_-]?key|access[_-]?token)\s*[:=]\s*["\'][^"\'\s<>{}$]{12,}["\']'),
     'a secret-looking assignment'),
]
# case-insensitive, and past any user@ part (wss://x@host names host, not x)
URL = re.compile(r'(?i)\b(wss?|https?)://(?:[^\s/@\'"<>]*@)?([A-Za-z0-9.\-]+|\[[0-9a-fA-F:]+\])')
LOCAL_HOSTS = re.compile(r'^(localhost|127\.\d+\.\d+\.\d+|0\.0\.0\.0|\[::1?\]|[^.]+)$')  # no dot: a test string
EXAMPLE = re.compile(r'(^|\.)(example\.(org|com|net)|test|invalid|localhost)$')
MACHINE = [
    # a drive path into a personal folder or this project's usual work folders; system folders
    # (Program Files) and generic examples (D:\Games in tests) are fine
    (re.compile(r'(?<![\w`<%$])[A-WYZ]:[\\/]{1,2}(?:Users[\\/]{1,2}[A-Za-z]|Apps\b|Temp\b|DevCaches\b|UserData\b|Backups\b|t3\b)', re.I),
     'drive letter path'),
    # a home folder by name; /home/deck is the Steam Deck's standard user, not a machine of ours
    (re.compile(r'(?i)(?<![\w.])(?:[A-Z]:)?[\\/](?:Users|home)[\\/](?!<|\$|USER|\*|you\b|name\b|runner\b|me\b|deck\b|\.\.\.)[A-Za-z][\w.-]+'),
     'home folder path'),
    (re.compile(r'\b(?:192\.168|10\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b'), 'LAN address'),
]
SKIP_TEXT = ('port/render/vendor/', 'port/third_party/', 'progress/', 'objdiff.json')
SELF = ('tools/check_tree.py',)
# Other machines' paths a tool names on purpose (path, text on the line), with the reason.
ALLOW = {
    ('port/tools/rustgs.py', 'FOREIGN_ROOTS'): "the ring crate author's build folder, which rustgs.py normalises",
    ('port/tools/package_release.py', 'runneradmin'): "a CI runner's folder, an example in a docstring",
}


def git(*args):
    r = subprocess.run(['git', *args], cwd=ROOT, capture_output=True)
    if r.returncode:  # not a repository, safe.directory: checking nothing must not pass
        sys.exit('check_tree: git %s failed: %s' % (' '.join(args), r.stderr.decode('utf-8', 'replace').strip()))
    return r.stdout


def content(path, staged):
    if staged:
        return git('show', f':{path}')
    try:
        with open(os.path.join(ROOT, path), 'rb') as f:
            return f.read()
    except OSError:
        return None


def is_binary(data):
    return b'\0' in data[:8192]


def url_ok(scheme, host, line, at):
    # the official host exactly (a sentence's final dot allowed), or it run into the next string of
    # a binary as package_release's docs show it ("wss://dhd.skuza.appnamedhd": dhd_settings' keys
    # "name"/"wss" follow it); dhd.skuza.apple or dhd.skuza.app.other.org fail
    official = host.rstrip('.') == OFFICIAL or RUN_ON.match(host)
    if official or LOCAL_HOSTS.match(host) or EXAMPLE.search(host):
        return True
    near = line[at:at + 80]
    if '<' in near or '{' in near or '...' in near:  # a placeholder: wss://dhd-online.<account>.workers.dev
        return True
    if scheme.startswith('http') and not re.search(r'\.workers\.dev$', host):
        return True  # web links (docs, sources) are fine; only race servers are restricted
    return False


def main(argv):
    staged = '--staged' in argv
    named = [a for a in argv if not a.startswith('--')]
    if named:
        paths = named
    elif staged:
        paths = [p for p in git('diff', '--cached', '--name-only', '--diff-filter=ACMR', '-z').decode('utf-8', 'replace').split('\0') if p]
    else:
        paths = [p for p in git('ls-files', '-z').decode('utf-8', 'replace').split('\0') if p]
    elf = os.path.join(ROOT, 'iso', 'SCUS_971.77')
    problems = []

    def error(path, line, why):
        problems.append(f'ERROR {path}:{line} {why}')

    for path in paths:
        path = path.replace('\\', '/')
        low = path.lower()
        ext = os.path.splitext(low)[1]
        if path.startswith(DATA_DIRS):
            error(path, 1, f'{path.split("/")[0]}/ holds game data or build output: never commit it')
        if ext in DATA_EXT or DATA_NAMES.search(path):
            error(path, 1, 'game data, a capture, a save or a dump by its name: never commit it')
        if ext in KEY_EXT:
            error(path, 1, 'a key or certificate file: keep it outside the repository')
        data = content(path, staged)
        if data is None:
            continue
        if is_binary(data):
            if len(data) > BIG and path not in BIG_OK:
                error(path, 1, f'binary file of {len(data) >> 10} KiB: give a reason in BIG_OK or keep it out')
            if (staged or named) and os.path.exists(elf):
                tmp = os.path.join(ROOT, 'build', 'check', 'scan.bin')
                os.makedirs(os.path.dirname(tmp), exist_ok=True)
                with open(tmp, 'wb') as f:
                    f.write(data)
                r = subprocess.run([sys.executable, os.path.join(ROOT, 'port', 'tools', 'scan_retail.py'), tmp],
                                   cwd=ROOT, capture_output=True, text=True)
                if r.returncode:
                    error(path, 1, 'holds a run of the retail executable (port/tools/scan_retail.py)')
            continue
        if path in SELF or path.startswith(SKIP_TEXT):
            continue
        text = data.decode('utf-8', 'replace')
        for i, line in enumerate(text.split('\n'), 1):
            for rx, why in SECRETS:
                if rx.search(line):
                    error(path, i, f'{why}: keep secrets in the environment, never in a file')
            for m in URL.finditer(line):
                if not url_ok(m.group(1).lower(), m.group(2).lower(), line, m.start()):
                    error(path, i, f'server address {m.group(0)}: only wss://{OFFICIAL} may be named')
            if any(p == path and marker in line for p, marker in ALLOW):
                continue
            for rx, why in MACHINE:
                m = rx.search(line)
                if m:
                    error(path, i, f'{why} ({m.group(0)}): use the variables in docs/TESTING.md')
    for line in problems:
        print(line)
    print(f'check_tree: {len(problems)} error(s) in {len(paths)} file(s)')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
