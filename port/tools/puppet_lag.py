"""How far behind real time, and how far from where its rider really is, each puppet is drawn in a
two-game online race (port/tools/online_race.py with DHD_PUPPET_TRACE and DHD_PUPPET_RECORD on both
games; finding 169).

    python port/tools/puppet_lag.py RUN_DIR [--from S] [--to S]   (race seconds; default from 5)

RUN_DIR holds A/ and B/ (online_race.py's games; `online_race.py --lag` records what this needs and
runs it), each with stderr.log, puppet.txt (DHD_PUPPET_TRACE: the puppets as shown, local race time,
shown moment, slot, x y z) and own.bin (DHD_PUPPET_RECORD: this game's own rider every race step,
port/hle/puppet.c's Snapshot). The other game's GO on the server clock relative to this one
("online: player N went at +X ms from us") maps a moment on this game's race clock to the sender's.

For every traced step of the other human's puppet:
  error now   the distance from the puppet as drawn to where the sender's own rider is at the
              same real moment (game units);
  lag         how far behind that moment the drawn position is: the time t' at which the sender's
              path passes nearest to the drawn position (searched 0.5 s back to 0.3 s ahead,
              linear between recorded steps), lag = now - t'. A lag near 0 means drawn at now.
Steps where the sender is crashing or the path jumps (a respawn) are skipped. Times in ms."""
import argparse, bisect, re, struct, sys
from pathlib import Path

SNAP = struct.Struct('<i4xdi4f12f4f4f4f')  # clock, time, state, pos[4], rot[12], vel[4], spin[4]
SNAP_SIZE = 824


def own_path(path):
    data = Path(path).read_bytes()
    if len(data) % SNAP_SIZE:
        sys.exit(f'{path}: {len(data)} bytes is not a whole number of {SNAP_SIZE}-byte snapshots')
    times, pos, crash = [], [], []
    for at in range(0, len(data), SNAP_SIZE):
        f = SNAP.unpack_from(data, at)
        t = f[1]
        if times and t <= times[-1]:
            if t < times[-1] - 1:   # a new race
                times, pos, crash = [], [], []
            else:
                continue
        times.append(t)
        pos.append(f[3:6])
        crash.append(data[at + 134] & 4)
    return times, pos, crash


def at_time(times, pos, t):
    i = bisect.bisect_right(times, t)
    if i <= 0 or i >= len(times):
        return None
    a, b = times[i - 1], times[i]
    f = (t - a) / (b - a)
    return tuple(pos[i - 1][k] + (pos[i][k] - pos[i - 1][k]) * f for k in range(3))


def dist(p, q):
    return sum((p[k] - q[k]) ** 2 for k in range(3)) ** 0.5


def nearest_time(times, pos, p, t0, back=0.5, ahead=0.3):
    """The moment in [t0-back, t0+ahead] the path passes nearest to p (linear between steps)."""
    lo, hi = bisect.bisect_left(times, t0 - back), bisect.bisect_right(times, t0 + ahead)
    best = (1e30, None)
    for i in range(max(lo, 1), min(hi, len(times))):
        a, b = pos[i - 1], pos[i]
        d = [b[k] - a[k] for k in range(3)]
        dd = sum(x * x for x in d)
        f = 0 if dd == 0 else max(0.0, min(1.0, sum((p[k] - a[k]) * d[k] for k in range(3)) / dd))
        q = tuple(a[k] + d[k] * f for k in range(3))
        e = dist(p, q)
        if e < best[0]:
            best = (e, times[i - 1] + (times[i] - times[i - 1]) * f)
    return best


def pct(values, p):
    if not values:
        return float('nan')
    s = sorted(values)
    return s[min(len(s) - 1, int(len(s) * p / 100))]


def measure(viewer, sender, args):
    log = (viewer / 'stderr.log').read_text(errors='replace')
    m = re.findall(r'online: player (\d+) went at ([+-][\d.]+) ms from us', log)
    if not m:
        return None
    skew = float(m[-1][1]) / 1000  # the sender's GO minus ours, s
    times, pos, crash = own_path(sender / 'own.bin')
    lags, errors = [], []
    for line in (viewer / 'puppet.txt').read_text().splitlines():
        f = line.split()
        if len(f) < 6 or f[2] != '1':  # slot 1: the other human on a two-player race
            continue
        t = float(f[0])
        if t < args.start or t > args.end:
            continue
        p = tuple(map(float, f[3:6]))
        t_sender = t - skew   # the same real moment on the sender's race clock
        i = bisect.bisect_right(times, t_sender)
        if i <= 1 or i >= len(times) or any(crash[max(0, i - 40):i + 2]):
            continue
        now = at_time(times, pos, t_sender)
        e, when = nearest_time(times, pos, p, t_sender)
        if now is None or when is None or e > 2.0:
            continue
        errors.append(dist(p, now))
        lags.append((t_sender - when) * 1000)
    return lags, errors


def report(run, start=5, end=1e9):
    args = argparse.Namespace(start=start, end=end)
    run = Path(run)
    for viewer, sender in (('A', 'B'), ('B', 'A')):
        r = measure(run / viewer, run / sender, args)
        if not r:
            print(f'{viewer} shows {sender}: no GO skew in the log')
            continue
        lags, errors = r
        print(f'{viewer} shows {sender}: {len(lags)} steps; lag p50/p95/p99 {pct(lags, 50):.0f}/{pct(lags, 95):.0f}/'
              f'{pct(lags, 99):.0f} ms; error now p50/p95/p99 {pct(errors, 50):.3f}/{pct(errors, 95):.3f}/'
              f'{pct(errors, 99):.3f} units')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--from', dest='start', type=float, default=5)
    ap.add_argument('--to', dest='end', type=float, default=1e9)
    args = ap.parse_args()
    report(args.run, args.start, args.end)


if __name__ == '__main__':
    sys.exit(main())
