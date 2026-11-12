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
import argparse, bisect, os, re, struct, sys

JERK = 30.0  # units a second; riders move about 95
from pathlib import Path

SNAP = struct.Struct('<i4xdi4f12f4f4f4f')  # clock, time, state, pos[4], rot[12], vel[4], spin[4]
SNAP_SIZE = 888    # port/hle/puppet.c's Snapshot
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
    for slot, _, t, p, _, crashing, _ in records(path):
        if slot != 0:   # a recording of every rider (DHD_PUPPET_FROM=all): this game's own is slot 0
            continue
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
    log = (viewer / ('stderr.log' if (viewer / 'stderr.log').exists() else 'game.log')).read_text(errors='replace')
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


RESYNC = re.compile(r'puppet: tick (\d+) slot (\d+) put where its rider is \(([\d.]+) units off(, after a crash)?')
PLACED = re.compile(r'puppet: tick (\d+) slot (\d+) placed \(([a-z ]+): ([\d.]+) units off')
CATCH = re.compile(r'puppet: tick (\d+) slot (\d+) catches up \(([\d.]+) units off')


def resyncs(log, early=5.0):
    """Riders put where their snapshots say at once, from a game's log: 'early' (the first `early`
    seconds after GO) and 'later' outside respawns, 'after crash' (respawns: the jump is real on the
    rider's own screen too), 'first' (a rider's first snapshot); and the catch-ups (an error over
    the jump size since round 3 of finding 202 caught up by riding, not placed) in the first
    seconds and later. Reads the logs from before round 3 too ("put where its rider is")."""
    counts = {'early': 0, 'later': 0, 'after crash': 0, 'first': 0, 'catch early': 0, 'catch later': 0}
    for tick, slot, units, crash in RESYNC.findall(log):
        counts['after crash' if crash else 'early' if int(tick) / 60.0 < early else 'later'] += 1
    for tick, slot, why, units in PLACED.findall(log):
        counts['after crash' if why == 'respawn' else 'first' if why == 'first snapshot'
               else 'early' if int(tick) / 60.0 < early else 'later'] += 1
    for tick, slot, units in CATCH.findall(log):
        counts['catch early' if int(tick) / 60.0 < early else 'catch later'] += 1
    return counts


def riders(run, start=1.0, end=1e9):
    """Every rider of a two-game race as each game draws it, against the other game's recording of
    all its riders (online_race.py --lag records them, DHD_PUPPET_FROM=all): the bench's own scores
    (puppet_bench.score) for the other player's rider and for the AI riders, and the re-syncs by
    phase. Returns {viewer: {'human': scores, 'ai': scores, 'resyncs': counts}}."""
    import tempfile
    import puppet_bench
    run, out = Path(run), {}
    for viewer, sender in (('A', 'B'), ('B', 'A')):
        logf = viewer + '/' + ('stderr.log' if (run / viewer / 'stderr.log').exists() else 'game.log')
        log = (run / logf).read_text(errors='replace')
        m = re.findall(r'online: player (\d+) went at ([+-][\d.]+) ms from us', log)
        own = run / sender / 'own.bin'
        if not m or not own.exists() or not (run / viewer / 'puppet.txt').exists():
            continue
        skew = float(m[-1][1]) / 1000
        present = {slot for slot, *_ in records(own)}
        result = {'resyncs': resyncs(log)}
        for kind in ('human', 'ai'):
            lines = []
            for line in (run / viewer / 'puppet.txt').read_text().splitlines():
                f = line.split()
                if len(f) < 11:
                    continue
                slot = int(f[2])
                src = 0 if slot == 1 else slot   # the other player is slot 1 here, slot 0 on its own PC
                if (kind == 'human') != (slot == 1) or src not in present:
                    continue
                f[6] = '%.6f' % (float(f[0]) - skew)   # the same moment on the sender's race clock
                f[10] = str(src)
                lines.append(' '.join(f))
            if not lines:
                continue
            with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as tmp:
                tmp.write('\n'.join(lines) + '\n')
            try:
                result[kind] = puppet_bench.score(own, tmp.name, start, end)
            finally:
                os.unlink(tmp.name)
        out[viewer] = result
    return out


def riders_report(run, start=1.0, end=1e9):
    for viewer, r in riders(run, start, end).items():
        for kind in ('human', 'ai'):
            s = r.get(kind)
            if s:
                print(f"{viewer} shows the other's {'player' if kind == 'human' else 'AI riders'}: {s['steps']} steps; "
                      f"error p50/p95/p99 {s['error_p50']:.2f}/{s['error_p95']:.2f}/{s['error_p99']:.2f} units; sunk "
                      f"{s['sunk_pct']:.1f} % (deep {s['deep_pct']:.2f} %); jolts {s['jolts_min']:.1f}, back "
                      f"{s['back_min']:.1f} a minute")
        c = r['resyncs']
        print(f"{viewer} re-syncs: {c['early']} in the first 5 s, {c['later']} later, {c['after crash']} after crashes "
              f"(placed: respawns); catch-ups by riding {c['catch early']} in the first 5 s, {c['catch later']} later")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--from', dest='start', type=float, default=5)
    ap.add_argument('--to', dest='end', type=float, default=1e9)
    ap.add_argument('--riders', action='store_true', help='every rider, from recordings of all riders (puppet_bench scores)')
    args = ap.parse_args()
    report(args.run, args.start, args.end)
    if args.riders:
        riders_report(args.run, args.start)


if __name__ == '__main__':
    sys.exit(main())
