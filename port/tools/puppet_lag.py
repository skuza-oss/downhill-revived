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
  jerks       steps where the puppet's drawn speed (its move since the previous drawn step) differs
              from the sender's real speed by more than JERK units a second: a visible jump or
              stop. Per thousand steps, and per thousand outside stalls (no step within 0.5 s
              drawn more than 0.2 s behind: snapshots that stopped coming, which no way of
              drawing can hide).
Steps where the sender is crashing or the path jumps (a respawn) are skipped. Times in ms."""
import argparse, bisect, re, struct, sys

JERK = 30.0  # units a second; riders move about 95
from pathlib import Path

SNAP = struct.Struct('<i4xdi4f12f4f4f4f')  # clock, time, state, pos[4], rot[12], vel[4], spin[4]
SNAP_SIZE = 840    # port/hle/puppet.c's Snapshot
CRASH_AT = 134     # its crash flags (bit 2: crashing)
TAIL = struct.Struct('<i2f')  # at TAIL_AT: slot, each wheel's clearance above the ground
TAIL_AT = 824


def records(path):
    """Every snapshot of a DHD_PUPPET_RECORD file: (slot, clock, time, pos, vel, crashing, ground),
    in file order."""
    data = Path(path).read_bytes()
    if len(data) % SNAP_SIZE:
        data = data[:len(data) - len(data) % SNAP_SIZE]   # a recording still being written
    for at in range(0, len(data), SNAP_SIZE):
        f = SNAP.unpack_from(data, at)
        slot, g0, g1 = TAIL.unpack_from(data, at + TAIL_AT)
        yield slot, f[0], f[1], f[3:6], f[19:22], bool(data[at + CRASH_AT] & 4), (g0, g1)


def own_path(path):
    times, pos, crash = [], [], []
    for _, _, t, p, _, crashing, _ in records(path):
        if times and t <= times[-1]:
            if t < times[-1] - 1:   # a new race
                times, pos, crash = [], [], []
            else:
                continue
        times.append(t)
        pos.append(p)
        crash.append(crashing)
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
    lags, errors, jerk_at, steps, stalled = [], [], [], 0, []
    prev = None
    for line in (viewer / 'puppet.txt').read_text().splitlines():
        f = line.split()
        if len(f) < 6 or f[2] != '1':  # slot 1: the other human on a two-player race
            continue
        t = float(f[0])
        if t < args.start or t > args.end:
            continue
        if t - float(f[1]) > 0.2:
            stalled.append(t)
        p = tuple(map(float, f[3:6]))
        t_sender = t - skew   # the same real moment on the sender's race clock
        i = bisect.bisect_right(times, t_sender)
        if i <= 1 or i >= len(times) or any(crash[max(0, i - 40):i + 2]):
            prev = None
            continue
        now = at_time(times, pos, t_sender)
        e, when = nearest_time(times, pos, p, t_sender)
        if now is None or when is None or e > 2.0:
            prev = None
            continue
        errors.append(dist(p, now))
        lags.append((t_sender - when) * 1000)
        if prev and 0.002 < t - prev[0] < 0.05:
            dt = t - prev[0]
            before = at_time(times, pos, t_sender - dt)
            if before is not None:
                steps += 1
                if abs(dist(p, prev[1]) / dt - dist(now, before) / dt) > JERK:
                    jerk_at.append(t)
        prev = (t, p)
    def near_stall(t):
        i = bisect.bisect_left(stalled, t - 0.5)
        return i < len(stalled) and stalled[i] <= t + 0.5
    calm = [t for t in jerk_at if not near_stall(t)]
    return lags, errors, len(jerk_at) * 1000.0 / max(steps, 1), len(calm) * 1000.0 / max(steps, 1), len(stalled)


def report(run, start=5, end=1e9):
    args = argparse.Namespace(start=start, end=end)
    run = Path(run)
    for viewer, sender in (('A', 'B'), ('B', 'A')):
        r = measure(run / viewer, run / sender, args)
        if not r:
            print(f'{viewer} shows {sender}: no GO skew in the log')
            continue
        lags, errors, jerks, calm, stalled = r
        print(f'{viewer} shows {sender}: {len(lags)} steps; lag p50/p95/p99 {pct(lags, 50):.0f}/{pct(lags, 95):.0f}/'
              f'{pct(lags, 99):.0f} ms; error now p50/p95/p99 {pct(errors, 50):.3f}/{pct(errors, 95):.3f}/'
              f'{pct(errors, 99):.3f} units; jerks {jerks:.1f} per 1000 steps, {calm:.1f} outside stalls '
              f'({stalled} stalled steps)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--from', dest='start', type=float, default=5)
    ap.add_argument('--to', dest='end', type=float, default=1e9)
    args = ap.parse_args()
    report(args.run, args.start, args.end)


if __name__ == '__main__':
    sys.exit(main())
