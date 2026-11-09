"""The Rust library's cache key (port/tools/rustgs.py cache_key) covers every crate the library
links: each path dependency of the workspace's crates has its sources in the key, so changing only
a member crate's code (dhd_star, finding 186) builds the library again instead of taking a stale one.

    python port/tools/test_rustgs_key.py"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rustgs  # noqa: E402


def main():
    covered = {rel for rel, _ in rustgs.source_files()}
    workspace = (rustgs.RENDER / 'Cargo.toml').read_text(encoding='utf-8')
    members = re.search(r'members\s*=\s*\[([^\]]*)\]', workspace).group(1)
    linked = [m.strip().strip('"') for m in members.split(',') if m.strip().strip('"') not in ('', '.', 'tools')]
    for name in linked:
        sources = sorted((rustgs.RENDER / name / 'src').rglob('*.rs'))
        assert sources, name
        missing = [p for p in sources if 'port/render/%s/%s' % (name, p.relative_to(rustgs.RENDER / name).as_posix()) not in covered]
        assert not missing, 'rustgs cache key misses %s: %s' % (name, missing[:3])
    print('test_rustgs_key: every linked crate (%s) is in the cache key' % ', '.join(linked))


if __name__ == '__main__':
    main()
