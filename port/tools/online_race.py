"""An online race in a room with state sync (findings 044, 047): two game instances on this PC,
the host and one that joins, each riding its own scripted line (DHD_NET_SCRIPTED), plus
optionally nettool bots (--bots, port/net/nettool.c) that replay a recorded rider (--replay) or,
without one, mirror a host AI rider as their own (--echo-ai) and finish after 60 s. Each
game shows the others' riders as puppets; the host runs the AI riders. Both take screenshots at
the given race-clock moments (DHD_RACE_CAPTURE) and the run goes on until both games have the
host's results (or --seconds). The summary has the start (GO on the server's clock), the lag of
each puppet, hits, finishes and results.

    python port/tools/online_race.py [--seconds 420] [--out DIR] [--capture g+600,g+1200]
                                     [--bots N [--replay FILE]] [--kill-guest-at SECONDS | --kill-host-at SECONDS] [--lag]

--kill-host-at stops the room's creator mid-race: in a P2P room (--a DHD_NET_CREATE=p2p) its hub, so
the room picks the next hub and the others race on over it and get the results (finding 186).

--exe can be another build than --nettool's: a release's games with this checkout's client code
in the bots (the bots claim the host's version, as rooms take only one build).

--lag: both games trace their puppets and record their own rider, and the summary ends with how
far behind real time and how far from the real rider each shows the other (puppet_lag.py).

Needs DHD_ISO, DHD_NET_SERVER (the official wss://dhd.skuza.app or
your own; never write any other deployed address into a tracked file) and the
x64 build (build_host64.py, default --exe build/host64-exe/dhd64.exe). At most two games of its own
run; owned_process ends them and the bots, with whatever they started."""
import argparse, contextlib, hashlib, os, re, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock
import owned_process

ROOT = Path(__file__).resolve().parents[2]
# Both pedal all the way from GO. The guest throws punches and kicks for the first 40 s, while
# the two are still side by side from the grid.
PEDAL = '0:400:cross'
PUNCHES = ','.join(f'{1 + k * 0.5:.1f}:0.15:{"square" if k % 2 == 0 else "circle"}' for k in range(80))
HOST_PAD = PEDAL
GUEST_PAD = PEDAL + ',' + PUNCHES
KEEP = ('went at', 'countdown starts', 'race is over', 'back to the room', 'rides', 'hit kind', 'finished', 'result', 'left', 'AI now',
        'starts at grid', 'Online play stopped', 'crash kind', 'has loaded')


def iso_files_sha256(iso, cache):
    """The image's game files hash (finding 098), the value the game sends and the server compares:
    dhd_disc's rule (port/render/disc/src/lib.rs `entries`, `record`, `hash`) in Python. Every file of
    the ISO 9660 file system, its path from the root (no ";1", "FILE." is "FILE"), a 0, its size (8
    bytes LE) and its contents' SHA-256; the SHA-256 of those records sorted. Cached in cache in
    iso-check.txt's line ("files:size:ns:hash:count:"), so the game's own iso-check.txt fits too."""
    st = os.stat(iso)
    key = f'files:{st.st_size}:{st.st_mtime_ns}:'
    if cache.exists():
        line = cache.read_text().partition('\n')[0]
        if line.startswith(key) and len(line) >= len(key) + 64:
            return line[len(key):len(key) + 64]
    sector = 2048
    with open(iso, 'rb') as f:
        def read(at, size):
            f.seek(at * sector)
            data = f.read(size)
            if len(data) != size:
                sys.exit(f"{iso}: a file or directory lies past the image's end")
            return data
        pvd = read(16, sector)
        if pvd[0] != 1 or pvd[1:6] != b'CD001':
            sys.exit(f'{iso} has no ISO 9660 volume')
        le32 = lambda b, at: int.from_bytes(b[at:at + 4], 'little')
        todo, seen, files = [(le32(pvd, 158), le32(pvd, 166), '')], set(), []
        while todo:
            at, size, prefix = todo.pop()
            if at in seen:
                continue
            seen.add(at)
            data, i, found = read(at, size), 0, []
            while i < len(data):
                n = data[i]
                if n == 0:   # no record crosses a sector
                    i = (i // sector + 1) * sector
                    continue
                rec = data[i:i + n]
                i += n
                name = rec[33:33 + rec[32]]
                if name in (b'\0', b'\1'):
                    continue
                if rec[25] & 2:
                    found.append((le32(rec, 2), le32(rec, 10), f'{prefix}{name.decode(errors="replace")}/'))
                    continue
                name = name.decode(errors='replace').split(';')[0]
                files.append((le32(rec, 2), le32(rec, 10), prefix + (name[:-1] if name.endswith('.') else name)))
            todo.extend(reversed(found))
        records = []
        for at, size, path in sorted(files):
            f.seek(at * sector)
            d, left = hashlib.sha256(), size
            while left:
                block = f.read(min(left, 1 << 22))
                if not block:
                    sys.exit(f"{iso}: {path} lies past the image's end")
                d.update(block)
                left -= len(block)
            records.append(path.encode() + b'\0' + size.to_bytes(8, 'little') + d.digest())
    h = hashlib.sha256(b''.join(sorted(records))).hexdigest()
    cache.write_text(f'{key}{h}:{len(records)}:\n')
    return h


def game_env(iso_hash, pad, extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    env.update(DHD_ISO=os.environ['DHD_ISO'], DHD_NET_SERVER=os.environ['DHD_NET_SERVER'], DHD_NET_ISO_SHA256=iso_hash, DHD_NET_LOG_CODE='1',
               DHD_SKIP_MOVIES='1', DHD_MEMCARD='0', DHD_SND_OFF='1', DHD_RENDER='native', DHD_RACE_SYNC='1', DHD_NET_SCRIPTED='1',
               DHD_RACE_PAD=pad, DHD_SCREENSHOT='shot', DHD_SCALE='1', DHD_SETTINGS='none')
    env.update(extra)
    return {k: v for k, v in env.items() if v != ''}


def wait_line(paths, pattern, seconds, procs, stop_if):
    """Until every file in paths has a line matching pattern; the matches (None where missing)."""
    until = time.time() + seconds
    found = [None] * len(paths)
    while time.time() < until:
        for i, path in enumerate(paths):
            if found[i] is None and path.exists():
                found[i] = re.search(pattern, path.read_text(errors='replace'))
        if all(found):
            break
        if any(p.poll() is not None for p in procs) or (stop_if and os.path.exists(stop_if)):
            break
        time.sleep(0.5)
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=str(ROOT / 'build' / 'host64-exe' / 'dhd64.exe'))
    ap.add_argument('--nettool', default=str(ROOT / 'build' / 'port' / 'net' / 'nettool.exe'))
    ap.add_argument('--seconds', type=int, default=420, help='at most this long once both race')
    ap.add_argument('--out', default=str(ROOT / 'build' / 'port' / 'online-race'))
    ap.add_argument('--track', default='0')
    ap.add_argument('--capture', default='g+300,g+900', help='DHD_RACE_CAPTURE moments for both')
    ap.add_argument('--bots', type=int, default=0, help='nettool players: --replay, else echoing an AI rider')
    ap.add_argument('--replay', help='a STATE/EVENT recording (DHD_NET_RECORD) for the bots (else they echo an AI rider)')
    ap.add_argument('--kill-guest-at', type=float, help='stop the guest this many seconds after GO')
    ap.add_argument('--kill-host-at', type=float, help='stop the host (in a P2P room, its hub) this many seconds after GO')
    ap.add_argument('--stop-if', default=os.path.join(ROOT, 'build', 'port', 'BENCH.lock'),
                    help='stop everything as soon as this file exists (the benchmark lock)')
    ap.add_argument('--minimized', action='store_true', help='both games start minimised (the PC stays usable)')
    ap.add_argument('--lag', action='store_true', help='measure how far behind each puppet is drawn (puppet_lag.py)')
    ap.add_argument('--a', action='append', default=[], help='extra NAME=VALUE for the host game')
    ap.add_argument('--b', action='append', default=[], help='extra NAME=VALUE for the joining game')
    a = ap.parse_args()
    for v in ('DHD_ISO', 'DHD_NET_SERVER'):
        if not os.environ.get(v):
            sys.exit('set ' + v)
    if a.stop_if and os.path.exists(a.stop_if):
        sys.exit(f'{a.stop_if} exists: not starting')
    out = Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    iso_hash = iso_files_sha256(os.environ['DHD_ISO'], ROOT / 'build' / 'port' / 'iso-check.txt')
    procs, dirs = [], []
    owned = contextlib.ExitStack()   # the logs and the processes, closed and ended at its end

    def game(name, pad, extra):
        d = out / name
        d.mkdir(exist_ok=True)
        for f in d.glob('shot-*.bmp'):
            f.unlink()
        (d / 'stderr.log').unlink(missing_ok=True)
        env = game_env(iso_hash, pad, {'DHD_RACE_CAPTURE': a.capture, **extra})
        stdout, stderr = owned.enter_context(open(d / 'stdout.log', 'w')), owned.enter_context(open(d / 'stderr.log', 'w'))
        procs.append(owned.enter_context(owned_process.start([os.path.abspath(a.exe)], cwd=d, env=env,
                                                             stdout=stdout, stderr=stderr, minimized=a.minimized)))
        if a.minimized:
            procs[-1].minimize()
        dirs.append(d)
        return d

    if a.lag:
        for side in (a.a, a.b):
            side[:0] = ['DHD_PUPPET_TRACE=puppet.txt', 'DHD_PUPPET_RECORD=own.bin']
    players = 2 + a.bots
    owned.enter_context(game_lock.held('online_race'))  # untimed: one place for its two games
    with owned:
        da = game('A', HOST_PAD, {'DHD_NET_CREATE': '1', 'DHD_NET_NAME': 'Host', 'DHD_NET_PLAYERS': str(players),
                                  'DHD_NET_TRACK': a.track, 'DHD_NET_RIDER': '4', **dict(x.split('=', 1) for x in a.a)})
        m = wait_line([da / 'stderr.log'], r'online: room (\w+) \(host\)', 120, procs, a.stop_if)[0]
        if not m:
            print('the host never opened a room; see', da / 'stderr.log')
            return 1
        code = m.group(1)
        print('room', code)
        # The bots join first and ready only once every player is in the room (nettool --players):
        # a room locks its grid as soon as everyone in it is ready, so a bot joining after the two
        # games would wait for the next race.
        version = wait_line([da / 'stderr.log'], r'version (dhd-port [\w.-]+(?: \w+)?)', 30, procs, a.stop_if)[0].group(1)
        for k in range(a.bots):
            d = out / f'bot{k}'
            d.mkdir(exist_ok=True)
            log = owned.enter_context(open(d / 'stdout.log', 'w'))
            ride = ['--replay', a.replay, '--rider', str(k * 2 + 1)] if a.replay else \
                ['--pick', str(k * 2 + 1), '--races', '1', '--ready-after', '1', '--echo-ai', '--finish-after', '60']
            procs.append(owned.enter_context(owned_process.start(
                [a.nettool, '--version', version, '--iso', iso_hash, '--name', f'Bot{k}', '--players', str(players), *ride,
                 'join', code], stdout=log, stderr=subprocess.STDOUT)))
        db = game('B', GUEST_PAD, {'DHD_NET_JOIN': code, 'DHD_NET_NAME': 'Guest', 'DHD_NET_RIDER': '8',
                                   **dict(x.split('=', 1) for x in a.b)})
        guest = procs[-1]
        go = wait_line([da / 'stderr.log', db / 'stderr.log'], r'countdown starts', 400, procs, a.stop_if)
        for p in procs if a.minimized else ():
            p.minimize()  # a window SDL showed after the start
        if not all(go):
            print('a game did not reach the countdown')
        elif a.kill_guest_at or a.kill_host_at:
            time.sleep(4 + (a.kill_guest_at or a.kill_host_at))  # the countdown, then the race
            gone = guest if a.kill_guest_at else procs[0]
            procs.remove(gone)
            gone.stop()
            print('guest stopped' if a.kill_guest_at else 'host stopped')
        logs = ([] if a.kill_host_at else [da / 'stderr.log']) + ([] if a.kill_guest_at else [db / 'stderr.log'])
        done = wait_line(logs, r'online: result 1\.', a.seconds, procs, a.stop_if)
        time.sleep(6)
        if not all(done):
            print('not every game got the results')
    ok = True
    for d in dirs:
        text = (d / 'stderr.log').read_text(errors='replace') if (d / 'stderr.log').exists() else ''
        lines = [l for l in text.splitlines() if l.startswith(('online:', 'puppet:'))]
        keep = [l for l in lines if any(k in l for k in KEEP)]
        clocks = [l for l in lines if 'race clock' in l]
        print(f'--- {d.name}: {len(keep)} lines')
        for l in keep[:80]:
            print(' ', l)
        for l in clocks[2:12:3] + clocks[-1:]:
            print('  ', l)
        if 'Online play stopped' in text or not clocks:
            ok = False
        print('  screenshots:', ' '.join(sorted(f.name for f in d.glob('shot-*.bmp'))))
    for k in range(a.bots):
        print(f'--- bot{k}:', (out / f'bot{k}' / 'stdout.log').read_text(errors='replace')[-600:])
    if a.lag and not (a.kill_guest_at or a.kill_host_at):
        import puppet_lag
        print('--- puppets (puppet_lag.py):')
        puppet_lag.report(out)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
