"""The puppet bench (finding 202): how other riders look in an online race, measured on one PC
without a network. Real rides are the truth; they are sent to puppets through a bad link and the
puppets as drawn are compared with them.

    python port/tools/puppet_bench.py record --track N [--seconds S]     # the truth: every rider
    python port/tools/puppet_bench.py play TRUTH --link "delay=80,jitter=20" [--fps F] [--name X]
    python port/tools/puppet_bench.py score TRUTH TRACE                    # numbers of one play
    python port/tools/puppet_bench.py matrix [--tracks 0,1,2] [--label before]  # every scenario
    python port/tools/puppet_bench.py --label before profiles [--tracks 0,1]   # finding 254's networks

record: a deterministic race (DHD_FRAME_CLOCK=1, seed 81257) on unlocked track N of the chooser
(state_trace_run.UNLOCKED_TRACKS), this PC's rider pedalling and steering (state_trace_run's
RACE_PAD), the nine AI riders racing as they do (steering, braking, jumping, crashing, respawning),
every rider recorded each race step (DHD_PUPPET_RECORD with DHD_PUPPET_FROM=all, with each wheel's
clearance above the ground from the game's own world query).

play: the same race again, every AI slot a puppet of its recorded rider, sent as online (30 a
second, the link's rate= otherwise) through DHD_PUPPET_LINK (port/hle/puppet_link.h), this game at
--fps steps a second (deterministic); DHD_PUPPET_TRACE has each puppet as drawn.

score: per drawn step, against the recorded rider at the moment the puppet should show (the
sender's race clock now, its hitch included); steps within 0.5 s of a crash of the truth or the
puppet, or of a respawn, are left out (no model shows those the same way):
  error         distance from the truth (units; riders move about 95 a second)
  behind        how far in the past the drawn position is on the truth's path (ms)
  sunk, deep    share of steps where a drawn wheel is more than SUNK (DEEP) units in the ground,
                beyond what the truth itself shows there (its own clearance at that moment; its
                wheels press in up to about 0.3), and the p99 depth of the drawn wheels below it
  pops          steps whose motion differs from the truth's motion over the same step by more than
                POP units a second (a visible jump: 0.6 units in a 1/60 s step), per minute
  jolts         steps where the drawn rider's own speed changes by more than JOLT units a second
                from the step before while the truth's changes by less than half that, per minute
                (a discontinuity: a snap, a hold, a correction taken at once)
  resync_*      riders put where their snapshots say at once (the game's log): in the first 5 s
                after GO, later, and after a crash (a respawn)
  back          steps where the drawn rider moves backwards along the truth's motion while the
                truth moves faster than 10 units a second, per minute
  frozen        share of time the drawn rider stands still while the truth moves
  stuck_s       the longest such stretch of one rider (s)
  teleports     steps whose motion differs from the truth's over the same step by more than
                TELEPORT units (a rider seen to jump, not a twitch), per minute; step_max the
                largest such difference (units)
  heading_p95   the drawn rider's heading (its forward axis on the ground plane) off the truth's
                at the same moment (degrees)
  flips         steps where the drawn heading turns more than FLIP degrees beyond what the
                truth's turns over the same step, per minute (a rider seen to snap round)

profiles: finding 254's networks (PROFILES: an ordinary good link, mobile data, a bad link, outages
of 0.5-2 s lost or held and then delivered at once, a 5 s disconnect), each for the computer riders
(30 a second) and for a player's rider (the scripted human replayed onto slot 1, 60 a second).
Everything runs on the hidden desktop (hidden_game.py), untimed: positions on the race clock, not
speed. Outputs go under build/puppet-bench/ (game data and recordings are never committed)."""
import argparse
import bisect
import json
import math
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import puppet_lag  # noqa: E402
from state_trace_run import DEFAULT_SEED, RACE_PAD, WINDOWS_EXE, track_nav  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build' / 'puppet-bench'
SUNK = 0.15     # units below the truth's own clearance that count as sinking
DEEP = 0.5      # and as clearly sunk (a wheel's radius is 1.25)
JOLT = 36.0     # units a second of change in the drawn speed from one step to the next (see jolts)
POP = 36.0      # units a second of motion the truth does not make (0.6 units in a 1/60 s step) read as a jump
SKIP_NEAR = 0.5  # s around crashes and respawns left out
TELEPORT = 3.0  # units of motion in one step the truth does not make: a rider seen to jump
FLIP = 8.0      # degrees of turn in one step beyond the truth's: a rider seen to snap round
FORWARD = 1     # the orientation row that is the rider's forward axis (puppet_bench.py axis TRUTH)

# The scenarios (name, link). Delays one way; jitter is exponential with that mean.
SCENARIOS = [
    ('perfect', ''),
    ('30ms', 'delay=30,jitter=5,seed=1'),
    ('80ms', 'delay=80,jitter=15,loss=1,seed=2'),
    ('150ms', 'delay=150,jitter=30,loss=2,reorder=2,seed=3'),
    ('250ms', 'delay=250,jitter=50,loss=3,reorder=3,seed=4'),
    ('80ms-60hz', 'delay=80,jitter=15,loss=1,rate=60,seed=5'),
    ('bad', 'delay=150,jitter=60,loss=5,reorder=5,burst=4:200-500,seed=6'),
    ('bursts', 'delay=80,jitter=15,burst=3:200-500,seed=7'),
    ('skew+40', 'delay=80,jitter=15,skew=40,seed=8'),
    ('skew-40', 'delay=80,jitter=15,skew=-40,seed=9'),
    ('send-hitch', 'delay=80,jitter=15,hitch=20:800,seed=10'),
    ('recv-hitch', 'delay=80,jitter=15,rhitch=20:800,seed=11'),
    ('send-behind', 'delay=80,jitter=15,hitch=20:3000,seed=12'),   # a sender 3 s behind, catching up
    ('recv-behind', 'delay=80,jitter=15,rhitch=20:3000,seed=13'),
]
FPS = (60, 30, 144)

# Finding 254's networks: what players on mobile data or weak Wi-Fi have (round trips 100-400 ms,
# jitter, 2-8 % loss), outages of 0.5-2 s about every 6 s (lost, as on a P2P link or the sender's own
# unordered leg; held, as the SFU's reliable relay to the receiver and TURN over TLS deliver them:
# nothing, then everything at once), and one 5 s disconnect.
PROFILES = [
    ('good', 'delay=25,jitter=5,loss=0.5,seed=21'),
    ('mobile', 'delay=60,jitter=30,loss=2,reorder=2,seed=22'),
    ('bad', 'delay=120,jitter=50,loss=8,reorder=5,seed=23'),
    ('burst', 'delay=60,jitter=20,loss=2,burst=6:500-2000,seed=24'),
    ('burst-held', 'delay=60,jitter=20,loss=2,stall=6:500-2000,seed=25'),
    ('cut5', 'delay=60,jitter=20,loss=2,cut=30:5000,seed=26'),
]
KINDS = (('ai', {}), ('player', {'DHD_PUPPET_MAP': '0:1'}))


def game_env(track, seconds, extra):
    env = {'DHD_FRAME_CLOCK': '1', 'DHD_NAV': track_nav(track), 'DHD_RACE_SYNC': '1', 'DHD_RACE_PAD': RACE_PAD,
           'DHD_RACE_SEED': str(DEFAULT_SEED), 'DHD_SCREENSHOT': '<unset>', 'DHD_CAPTURE_AT': '<unset>',
           'DHD_CAPTURE_INTERVAL': '<unset>', 'DHD_SCREENSHOT_WINDOW': '<unset>', 'DHD_EXACT_PROBE': '1'}
    env.update(extra)
    return env


def last_time(path, text):
    """The newest race time in a recording (binary) or a trace (text) being written."""
    try:
        if not text:
            size = path.stat().st_size // puppet_lag.SNAP_SIZE * puppet_lag.SNAP_SIZE
            if size < puppet_lag.SNAP_SIZE:
                return 0.0
            with open(path, 'rb') as f:
                f.seek(size - puppet_lag.SNAP_SIZE)
                return puppet_lag.SNAP.unpack(f.read(puppet_lag.SNAP.size))[1]
        with open(path, 'rb') as f:
            f.seek(max(0, path.stat().st_size - 400))
            lines = f.read().decode(errors='replace').splitlines()
        return float(lines[-2].split()[0]) if len(lines) > 1 else 0.0
    except (OSError, ValueError, IndexError):
        return 0.0


def run_game(exe, out, name, env, watch, text, seconds, timeout=900):
    """A hidden game until `watch` reaches race second `seconds`."""
    if watch.exists():
        watch.unlink()
    g = hidden_game.Game(exe, out, env=env, name='puppet_bench ' + name, shot_window=None)
    start = time.time()
    try:
        while time.time() - start < timeout:
            if not g.alive():
                raise RuntimeError(f'the game exited (see {out / "game.log"})')
            if 'puppet: cannot' in g.log():
                raise RuntimeError(f'the game could not open a file (see {out / "game.log"})')
            if last_time(watch, text) >= seconds:
                break
            time.sleep(2)
        else:
            raise RuntimeError(f'race second {seconds} not reached in {timeout} s ({out / "game.log"})')
    finally:
        g.stop()


def record(args):
    out = OUT / f'{args.truths}-t{args.track}'
    out.mkdir(parents=True, exist_ok=True)
    truth = out / 'truth.bin'
    env = game_env(args.track, args.seconds, {'DHD_PUPPET_RECORD': str(truth), 'DHD_PUPPET_FROM': 'all',
                                              **dict(e.split('=', 1) for e in args.env)})
    run_game(args.exe, out, f'record t{args.track}', env, truth, False, args.seconds)
    print('truth', truth)
    return truth


def play(args, truth, link, fps, name):
    truth = Path(truth).resolve()
    track = int(truth.parent.name.split('-t')[-1]) if '-t' in truth.parent.name else args.track
    out = OUT / (args.label or 'play') / name
    out.mkdir(parents=True, exist_ok=True)
    trace = out / 'trace.txt'
    env = game_env(track, args.seconds, {'DHD_PUPPET_PLAY': str(truth), 'DHD_PUPPET_LINK': link,
                                         'DHD_PUPPET_TRACE': str(trace), 'DHD_FPS': str(fps),
                                         **dict(e.split('=', 1) for e in args.env)})
    run_game(args.exe, out, name, env, trace, True, args.seconds)
    return trace


def load_truth(path):
    """Each recorded rider's path: slot -> (times, positions, crashing, clearance, headings)."""
    paths = {}
    rows = rotations(path)
    for n, (slot, _, t, pos, _, crashing, ground) in enumerate(puppet_lag.records(path)):
        times, ps, cs, gs, hs = paths.setdefault(slot, ([], [], [], [], []))
        if times and t <= times[-1]:
            continue
        times.append(t), ps.append(pos), cs.append(crashing), gs.append(ground), hs.append(heading(rows[n][FORWARD]))
    return paths


def rotations(path):
    """Each record's orientation rows (three world-space axes), in file order."""
    data = Path(path).read_bytes()
    out = []
    for at in range(0, len(data) - len(data) % puppet_lag.SNAP_SIZE, puppet_lag.SNAP_SIZE):
        f = puppet_lag.SNAP.unpack_from(data, at)
        out.append((f[7:10], f[11:14], f[15:18]))
    return out


def heading(row):
    """An axis's direction on the ground plane (world z is up), radians; nan when it points up."""
    return math.atan2(row[1], row[0]) if math.hypot(row[0], row[1]) > 0.2 else float('nan')


def turn(a, b):
    """b - a as an angle in (-pi, pi]."""
    d = (b - a) % (2 * math.pi)
    return d - 2 * math.pi if d > math.pi else d


def interp_angle(times, values, t):
    i = bisect.bisect_right(times, t)
    if i <= 0 or i >= len(times) or math.isnan(values[i - 1]) or math.isnan(values[i]):
        return float('nan')
    a, b = times[i - 1], times[i]
    f = (t - a) / (b - a) if b > a else 0
    return values[i - 1] + turn(values[i - 1], values[i]) * f


def axis(truth_path):
    """Which orientation row is a rider's forward axis: votes for the row most along its motion."""
    hits = [0, 0, 0]
    rows = rotations(truth_path)
    for n, (slot, _, t, pos, vel, crashing, _) in enumerate(puppet_lag.records(truth_path)):
        v = math.hypot(vel[0], vel[1])
        if crashing or v < 30:
            continue
        cos = [(r[0] * vel[0] + r[1] * vel[1]) / (v * max(math.hypot(r[0], r[1]), 1e-6)) for r in rows[n]]
        hits[max(range(3), key=lambda k: cos[k])] += 1
    return hits


def interp(times, values, t):
    i = bisect.bisect_right(times, t)
    if i <= 0 or i >= len(times):
        return None
    a, b = times[i - 1], times[i]
    f = (t - a) / (b - a) if b > a else 0
    return tuple(values[i - 1][k] + (values[i][k] - values[i - 1][k]) * f for k in range(3))


def pct(values, p):
    return puppet_lag.pct(values, p) if values else float('nan')


def score(truth_path, trace_path, start=5.0, end=1e9):
    paths = load_truth(truth_path)
    # windows to leave out per recorded rider: its crashes and respawns
    bad = {}
    for slot, (times, pos, crash, *_) in paths.items():
        marks = [times[i] for i in range(len(times)) if crash[i] or
                 (i and puppet_lag.dist(pos[i], pos[i - 1]) > 8)]
        bad[slot] = marks
    def near_bad(slot, t):
        marks = bad.get(slot, [])
        i = bisect.bisect_left(marks, t - SKIP_NEAR)
        return i < len(marks) and marks[i] <= t + SKIP_NEAR
    errors, lags, depth, heading_errors = [], [], [], []
    steps = sunk = deep = pops = back = jolts = teleports = flips = 0
    step_max = 0.0
    velocity = {}
    frozen_time = total_time = 0.0
    frozen_run, stuck = {}, 0.0
    prev = {}
    puppet_crash = {}
    for line in Path(trace_path).read_text().splitlines():
        f = line.split()
        if len(f) < 11:
            continue
        t, slot, src = float(f[0]), int(f[2]), int(f[10])
        p, truth_t = tuple(map(float, f[3:6])), float(f[6])
        g = [float(x) for x in f[7:9]]
        crashing = f[9] == '1'
        drawn_heading = heading(tuple(map(float, f[11 + 3 * FORWARD:14 + 3 * FORWARD]))) if len(f) >= 20 else float('nan')
        if crashing:
            puppet_crash[slot] = t
        last = prev.get(slot)
        prev[slot] = (t, p, truth_t, drawn_heading)
        if t < start or t > end or src not in paths or crashing or t - puppet_crash.get(slot, -99) < SKIP_NEAR or \
                near_bad(src, truth_t):
            continue
        times, pos, _, ground, headings = paths[src]
        now = interp(times, pos, truth_t)
        if now is None:
            continue
        steps += 1
        errors.append(puppet_lag.dist(p, now))
        e, when = puppet_lag.nearest_time(times, pos, p, truth_t)
        if when is not None and e < 2.0:
            lags.append((truth_t - when) * 1000)
        # the truth's own clearance there: a wheel of the truth in a hollow is no sinking
        tg = interp(times, [(a, b, 0) for a, b in ground], truth_t)
        own = min(tg[0], tg[1]) if tg and not any(map(math.isnan, tg[:2])) else 0.0
        shown = min((x for x in g if not math.isnan(x)), default=float('nan'))
        if not math.isnan(shown):
            if shown < 0:
                depth.append(-shown)
            if shown < min(own, 0.0) - SUNK:
                sunk += 1
            if shown < min(own, 0.0) - DEEP:
                deep += 1
        true_heading = interp_angle(times, headings, truth_t)
        if not math.isnan(drawn_heading) and not math.isnan(true_heading):
            heading_errors.append(abs(math.degrees(turn(true_heading, drawn_heading))))
        if last and 0 < t - last[0] < 0.1 and last[2] < truth_t:
            dt = t - last[0]
            before = interp(times, pos, last[2])
            was_heading = interp_angle(times, headings, last[2])
            if not any(map(math.isnan, (drawn_heading, last[3], true_heading, was_heading))):
                if abs(math.degrees(turn(last[3], drawn_heading) - turn(was_heading, true_heading))) > FLIP:
                    flips += 1
            if before is not None:
                moved = [p[k] - last[1][k] for k in range(3)]
                truth_moved = [now[k] - before[k] for k in range(3)]
                diff = math.sqrt(sum((moved[k] - truth_moved[k]) ** 2 for k in range(3)))
                speed = math.sqrt(sum(x * x for x in truth_moved)) / (truth_t - last[2])
                if diff / dt > POP:
                    pops += 1
                if diff > TELEPORT:
                    teleports += 1
                step_max = max(step_max, diff)
                # a jolt: the drawn rider's own speed jumps from one step to the next while the
                # truth's does not (a discontinuity, where pops also count motion that is only
                # different, such as a bump taken a little differently)
                v = [moved[k] / dt for k in range(3)]
                tv = [truth_moved[k] / (truth_t - last[2]) for k in range(3)]
                was = velocity.get(slot)
                if was and was[0] == last[0]:
                    change = math.sqrt(sum((v[k] - was[1][k]) ** 2 for k in range(3)))
                    truth_change = math.sqrt(sum((tv[k] - was[2][k]) ** 2 for k in range(3)))
                    if change > JOLT and truth_change < JOLT / 2:
                        jolts += 1
                velocity[slot] = (t, v, tv)
                if speed > 10:
                    along = sum(moved[k] * truth_moved[k] for k in range(3)) / (speed * (truth_t - last[2]))
                    if along < -0.02:
                        back += 1
                    total_time += dt
                    if math.sqrt(sum(x * x for x in moved)) < 0.02 * speed * dt:
                        frozen_time += dt
                        frozen_run[slot] = frozen_run.get(slot, 0.0) + dt
                        stuck = max(stuck, frozen_run[slot])
                    else:
                        frozen_run[slot] = 0.0
    minutes = max(steps, 1) / 60.0 / 60.0  # steps are per drawn frame; normalised below by time
    span = total_time / 60.0 if total_time else minutes
    return {
        'steps': steps,
        'error_p50': pct(errors, 50), 'error_p95': pct(errors, 95), 'error_p99': pct(errors, 99),
        'behind_p50': pct(lags, 50), 'behind_p95': pct(lags, 95),
        'sunk_pct': 100.0 * sunk / max(steps, 1), 'deep_pct': 100.0 * deep / max(steps, 1), 'depth_p99': pct(depth, 99) if depth else 0.0,
        'pops_min': pops / span if span else float('nan'), 'jolts_min': jolts / span if span else float('nan'), 'back_min': back / span if span else float('nan'),
        'frozen_pct': 100.0 * frozen_time / total_time if total_time else float('nan'), 'stuck_s': stuck,
        'teleports_min': teleports / span if span else float('nan'), 'step_max': step_max,
        'heading_p95': pct(heading_errors, 95), 'flips_min': flips / span if span else float('nan'),
        **resync_counts(Path(trace_path).parent / 'game.log'),
    }


def resync_counts(log):
    """Re-syncs (a rider put where its snapshot says at once) in a play's log, by race phase."""
    if not log.exists():
        return {}
    counts = puppet_lag.resyncs(log.read_text(errors='replace'))
    return {'resync_early': counts['early'], 'resync_later': counts['later'], 'resync_crash': counts['after crash'],
            'catchups': counts['catch early'] + counts['catch later']}


def truth_baseline(truth_path, start=5.0):
    """The recorded riders' own wheels against the ground (what no sinking looks like)."""
    depth, steps, below = [], 0, 0
    for slot, _, t, _, _, crashing, ground in puppet_lag.records(truth_path):
        if slot == 0 or t < start or crashing:
            continue
        g = [x for x in ground if not math.isnan(x)]
        if not g:
            continue
        steps += 1
        m = min(g)
        if m < 0:
            depth.append(-m)
        if m < -SUNK:
            below += 1
    return {'steps': steps, 'below_pct': 100.0 * below / max(steps, 1), 'depth_p50': pct(depth, 50) if depth else 0,
            'depth_p99': pct(depth, 99) if depth else 0}


COLUMNS = (('error_p50', '{:.2f}'), ('error_p95', '{:.2f}'), ('error_p99', '{:.2f}'), ('behind_p50', '{:.0f}'),
           ('sunk_pct', '{:.2f}'), ('deep_pct', '{:.2f}'), ('depth_p99', '{:.2f}'), ('pops_min', '{:.1f}'), ('jolts_min', '{:.1f}'), ('back_min', '{:.1f}'),
           ('frozen_pct', '{:.2f}'), ('resync_early', '{:.0f}'), ('resync_later', '{:.0f}'), ('catchups', '{:.0f}'))


def table(results):
    head = '| scenario | fps | track | ' + ' | '.join(c for c, _ in COLUMNS) + ' |'
    rows = [head, '|' + '---|' * (3 + len(COLUMNS))]
    for (name, fps, track), r in sorted(results.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2])):
        rows.append(f'| {name} | {fps} | {track} | ' + ' | '.join(fmt.format(r.get(c, float('nan'))) for c, fmt in COLUMNS) + ' |')
    return '\n'.join(rows)


def matrix(args):
    tracks = [int(x) for x in args.tracks.split(',')]
    wanted = set(args.only.split(',')) if args.only else None
    label = args.label or 'play'
    results_path = OUT / label / 'results.json'
    results_path.parent.mkdir(parents=True, exist_ok=True)
    done = json.loads(results_path.read_text()) if results_path.exists() else {}
    for track in tracks:
        truth = OUT / f'{args.truths}-t{track}' / 'truth.bin'
        if not truth.exists():
            args.track = track
            record(args)
        for name, link in SCENARIOS:
            if wanted and name not in wanted:
                continue
            for fps in [int(x) for x in args.fps.split(',')]:
                if fps != 60 and name not in ('80ms', '150ms', 'bad'):
                    continue  # the other rates on the scenarios that matter most
                key = f'{name}|{fps}|{track}'
                run = f'{name}-{fps}-t{track}'
                if args.rescore:
                    trace = OUT / label / run / 'trace.txt'
                    if trace.exists():
                        done[key] = score(truth, trace)
                        results_path.write_text(json.dumps(done, indent=1))
                    continue
                if key in done and not args.again:
                    continue
                print('play', run, flush=True)
                trace = play(args, truth, link, fps, run)
                done[key] = score(truth, trace)
                results_path.write_text(json.dumps(done, indent=1))
    results = {tuple(k.split('|')[:1]) + (int(k.split('|')[1]), int(k.split('|')[2])): v for k, v in done.items()}
    text = table(results)
    (OUT / label / 'results.md').write_text(text + '\n')
    print(text)


PROFILE_COLUMNS = (('error_p50', '{:.2f}'), ('error_p95', '{:.2f}'), ('error_p99', '{:.1f}'), ('teleports_min', '{:.1f}'),
                   ('step_max', '{:.1f}'), ('jolts_min', '{:.1f}'), ('frozen_pct', '{:.2f}'), ('stuck_s', '{:.2f}'),
                   ('sunk_pct', '{:.2f}'), ('heading_p95', '{:.1f}'), ('flips_min', '{:.1f}'), ('resync_later', '{:.0f}'))


def profiles(args):
    """Finding 254's networks for computer riders and a player's rider: profiles.md under the label."""
    tracks = [int(x) for x in args.tracks.split(',')]
    wanted = set(args.only.split(',')) if args.only else None
    label = args.label or 'play'
    results_path = OUT / label / 'profiles.json'
    results_path.parent.mkdir(parents=True, exist_ok=True)
    done = json.loads(results_path.read_text()) if results_path.exists() else {}
    base_env = list(args.env)
    for track in tracks:
        truth = OUT / f'{args.truths}-t{track}' / 'truth.bin'
        if not truth.exists():
            args.track = track
            record(args)
        for name, link in PROFILES:
            if wanted and name not in wanted:
                continue
            for kind, env in KINDS:
                if args.kinds and kind not in args.kinds.split(','):
                    continue
                key = f'{name}|{kind}|{track}'
                run = f'p-{name}-{kind}-t{track}'
                trace = OUT / label / run / 'trace.txt'
                if args.rescore:
                    if trace.exists():
                        done[key] = score(truth, trace)
                        results_path.write_text(json.dumps(done, indent=1))
                    continue
                if key in done and not args.again:
                    continue
                print('play', run, flush=True)
                args.env = base_env + [f'{k}={v}' for k, v in env.items()]
                trace = play(args, truth, link, 60, run)
                done[key] = score(truth, trace)
                results_path.write_text(json.dumps(done, indent=1))
    args.env = base_env
    order = {n: i for i, (n, _) in enumerate(PROFILES)}
    head = '| network | rider | track | ' + ' | '.join(c for c, _ in PROFILE_COLUMNS) + ' |'
    rows = [head, '|' + '---|' * (3 + len(PROFILE_COLUMNS))]
    for key in sorted(done, key=lambda k: (order.get(k.split('|')[0], 99), k.split('|')[1], int(k.split('|')[2]))):
        name, kind, track = key.split('|')
        r = done[key]
        rows.append(f'| {name} | {kind} | {track} | ' + ' | '.join(fmt.format(r.get(c, float('nan'))) for c, fmt in PROFILE_COLUMNS) + ' |')
    text = '\n'.join(rows)
    (OUT / label / 'profiles.md').write_text(text + '\n')
    print(text)


CLIP_SCALE = 4             # the clips' internal resolution (DHD_SCALE)
CLIP_SIZE = (960, 840)     # each side of a clip (the native picture's 512:448), so two fit 1080p


def frames(folder):
    """A race capture's frames (prefix-g+N+delta.png) in order."""
    def delta(p):
        return int(p.stem.rsplit('+', 1)[1])
    return sorted(Path(folder).glob('*.png'), key=delta)


def shrink(folder, keep_newest):
    """The captures (BMP, the renderer's own size at CLIP_SCALE) as CLIP_SIZE PNGs, the BMPs deleted
    (a 4x capture is 11 MB); the newest kept while the game may still be writing it."""
    from PIL import Image
    shots = sorted(Path(folder).glob('*.bmp'), key=os.path.getmtime)
    for bmp in shots[:-1] if keep_newest else shots:
        try:
            with Image.open(bmp) as im:
                im.convert('RGB').resize(CLIP_SIZE, Image.LANCZOS).save(bmp.with_suffix('.png'))
            bmp.unlink()
        except OSError:
            pass


def capture_run(exe, out, name, track, slot, at, length, extra):
    """A race on the hidden desktop with the camera on `slot`, every field captured from race
    second `at` for `length` seconds (the game's own GS captures)."""
    shots = out / 'frames'
    shutil.rmtree(shots, ignore_errors=True)
    shots.mkdir(parents=True)
    fields = int(length * 60)
    env = game_env(track, at + length, {'DHD_PUPPET_CAMERA': '1', 'DHD_PUPPET_SLOT': str(slot), 'DHD_SCALE': str(CLIP_SCALE),
                                        'DHD_SCREENSHOT': str(shots / 'shot'), 'DHD_RACE_CAPTURE': f'g+{int(at * 60)}',
                                        'DHD_RACE_CAPTURE_WINDOW': str((fields + 1) // 2), **extra})
    g = hidden_game.Game(exe, out, env=env, name='puppet_bench clip ' + name, shot_window=None)
    start = time.time()
    try:
        while time.time() - start < 1800 and len(list(shots.glob('*.png'))) + len(list(shots.glob('*.bmp'))) < fields:
            if not g.alive():
                raise RuntimeError(f'the game exited (see {out / "game.log"})')
            shrink(shots, True)
            time.sleep(1)
        time.sleep(3)
    finally:
        g.stop()
    shrink(shots, False)
    return frames(shots)[:fields]


def clip(args):
    """Side by side: the recorded rider (left) and its puppet through the link (right), same
    camera, same race moments; an mp4 by ffmpeg (else the frames and a contact sheet).
    --live RUN (an online_race.py --lag run): the host's recording of its rider (left) beside the
    guest's recording of that rider as the guest drew it (right), both replayed exactly
    (DHD_PUPPET_EXACT: each recorded pose as it is, a jump a jump), on the race's track."""
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    work = OUT / 'clips' / args.name
    if args.live:
        run = Path(args.live).resolve()
        exact = {'DHD_PUPPET_EXACT': '1', 'DHD_PUPPET_LINK': ''}
        left = capture_run(args.exe, OUT / 'clips' / (args.name + '-host'), args.name + ' host', args.track, args.slot,
                           args.at, args.length, {'DHD_PUPPET_PLAY': str(run / 'A' / 'own.bin'), **exact})
        right = capture_run(args.exe, work, args.name, args.track, args.slot, args.at, args.length,
                            {'DHD_PUPPET_PLAY': str(run / 'B' / 'own.bin'), **exact})
        truth_dir = OUT / 'clips' / (args.name + '-host')
        return side_by_side(args, out, work, truth_dir, left, right, 'the rider on its own game (the host)')
    truth_dir = OUT / 'clips' / f'truth-t{args.track}-s{args.slot}-{args.at:g}-{args.length:g}'
    truth = OUT / f'{args.truths}-t{args.track}' / 'truth.bin'
    left = frames(truth_dir / 'frames') if (truth_dir / 'frames').exists() else []
    if len(left) < int(args.length * 60):
        left = capture_run(args.exe if not args.truth_exe else args.truth_exe, truth_dir, 'truth', args.track, args.slot,
                           args.at, args.length, {})
    right = capture_run(args.exe, work, args.name, args.track, args.slot, args.at, args.length,
                        {'DHD_PUPPET_PLAY': str(truth.resolve()), 'DHD_PUPPET_LINK': args.link})
    return side_by_side(args, out, work, truth_dir, left, right, 'the rider (truth)')


def side_by_side(args, out, work, truth_dir, left, right, left_label):
    n = min(len(left), len(right))
    lst = work / 'pairs'
    shutil.rmtree(lst, ignore_errors=True)
    lst.mkdir()
    for i in range(n):  # numbered copies ffmpeg can read as two sequences
        shutil.copy(left[i], lst / f'l{i:05d}.png')
        shutil.copy(right[i], lst / f'r{i:05d}.png')
    target = out / f'{args.name}.mp4'
    ffmpeg = shutil.which('ffmpeg')
    if ffmpeg:
        label = args.label_text or args.link or 'perfect link'
        draw = lambda text: (f"drawtext=text='{text}':x=12:y=12:fontsize=30:fontcolor=white:box=1:boxcolor=black@0.6")
        subprocess.run([ffmpeg, '-y', '-loglevel', 'error', '-framerate', '60', '-i', str(lst / 'l%05d.png'),
                        '-framerate', '60', '-i', str(lst / 'r%05d.png'), '-filter_complex',
                        f"[0]{draw(left_label)}[a];[1]{draw('as another player sees it - ' + label.replace(':', ' ').replace(',', ' '))}[b];[a][b]hstack",
                        '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '18', str(target)], check=True)
        print('clip', target)
        # the frames are 1 MB each, 360 a side: gone once the clip is made (the truth's too)
        for folder in (lst, work / 'frames', truth_dir / 'frames'):
            shutil.rmtree(folder, ignore_errors=True)
    else:
        from PIL import Image
        picks = [lst / f'{side}{i:05d}.png' for i in range(0, n, max(1, n // 12)) for side in 'lr']
        ims = [Image.open(p) for p in picks]
        w, h = ims[0].size
        sheet = Image.new('RGB', (w * 2, h * (len(ims) // 2)))
        for k, im in enumerate(ims):
            sheet.paste(im, ((k % 2) * w, (k // 2) * h))
        sheet.save(out / f'{args.name}-sheet.png')
        print('no ffmpeg: frames in', lst, 'sheet', out / f'{args.name}-sheet.png')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', default=str(ROOT / WINDOWS_EXE))
    ap.add_argument('--seconds', type=float, default=75, help='race seconds recorded and played')
    ap.add_argument('--env', action='append', default=[], help='extra NAME=VALUE for the game')
    ap.add_argument('--label', help='a name for this build\'s results (build/puppet-bench/LABEL)')
    ap.add_argument('--truths', default='truth', help='the recordings\' folder (build/puppet-bench/TRUTHS-tN)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('record')
    r.add_argument('--track', type=int, default=0)
    p = sub.add_parser('play')
    p.add_argument('truth')
    p.add_argument('--link', default='')
    p.add_argument('--fps', type=int, default=60)
    p.add_argument('--name', default='one')
    p.add_argument('--track', type=int, default=0)
    s = sub.add_parser('score')
    s.add_argument('truth')
    s.add_argument('trace')
    b = sub.add_parser('baseline')
    b.add_argument('truth')
    c = sub.add_parser('clip')
    c.add_argument('--track', type=int, default=0)
    c.add_argument('--slot', type=int, default=1, help='the AI rider (and its puppet) the camera follows')
    c.add_argument('--at', type=float, default=30, help='race second the clip starts')
    c.add_argument('--length', type=float, default=6)
    c.add_argument('--link', default='')
    c.add_argument('--name', required=True)
    c.add_argument('--out', required=True, help='folder for the mp4 (keep it outside the repository)')
    c.add_argument('--truth-exe', help='the build that films the truth (default --exe; the race is the same)')
    c.add_argument('--label-text', help='the caption of the right side (default the link)')
    c.add_argument('--live', help="an online_race.py --lag run: the host's recording beside the guest's, exact")
    q = sub.add_parser('profiles')
    q.add_argument('--tracks', default='0,1')
    q.add_argument('--only', help='profile names, comma separated')
    q.add_argument('--kinds', help='ai, player or both (default)')
    q.add_argument('--again', action='store_true', help='run profiles that already have results')
    q.add_argument('--rescore', action='store_true', help='score the traces already there again, run nothing')
    a = sub.add_parser('axis')
    a.add_argument('truth')
    m = sub.add_parser('matrix')
    m.add_argument('--tracks', default='0,1,2')
    m.add_argument('--fps', default='60,30,144')
    m.add_argument('--only', help='scenario names, comma separated')
    m.add_argument('--again', action='store_true', help='run scenarios that already have results')
    m.add_argument('--rescore', action='store_true', help='score the traces already there again, run nothing')
    args = ap.parse_args()
    args.exe = str(Path(args.exe).resolve())
    if args.cmd == 'record':
        record(args)
    elif args.cmd == 'play':
        trace = play(args, args.truth, args.link, args.fps, args.name)
        print(json.dumps(score(args.truth, trace), indent=1))
    elif args.cmd == 'score':
        print(json.dumps(score(args.truth, args.trace), indent=1))
    elif args.cmd == 'baseline':
        print(json.dumps(truth_baseline(args.truth), indent=1))
    elif args.cmd == 'clip':
        clip(args)
    elif args.cmd == 'profiles':
        profiles(args)
    elif args.cmd == 'axis':
        print('forward-axis votes by orientation row:', axis(args.truth))
    else:
        matrix(args)


if __name__ == '__main__':
    sys.exit(main())
