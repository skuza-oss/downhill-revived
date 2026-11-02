"""Rate audit: find EE state advanced once per update instead of once per field.

The detector is port/hle/rate_audit.c (DHD_RATE_AUDIT=<file>): each game update (the game's frame
tick, func_00238C00) it compares the whole EE RAM with the previous update and accumulates every
word's total absolute change ("travel") over a window of updates, optionally gated to one game
scene (menu id / race phase).

    python port/tools/rate_audit.py run boot  --rate 60  --name title-60  --window title
    python port/tools/rate_audit.py run boot  --rate 240 --name title-240 --window title
    python port/tools/rate_audit.py compare title-60 title-240

`run` uses the state trace's deterministic headless recipe (state_trace_run.py: virtual clock,
no movies/sound/memcard, native renderer, no settings). The race scenario also takes the pinned
seed and the race-clock pad. The game ends itself when the window is done; the audit file goes to
build/rate-audit/<name>.txt and its log beside it.

`compare` ranks words by signed (net) travel per update: 1.0 means a per-update accumulator (the
same net travel per update at every rate), 60/rate (0.25 at 240) means correct time-based motion.
Net cancels an oscillator (a field-parity word flipping 0/8 has travel but no net); the travel
per update is shown beside it so those are visible. The ranking's reference is net travel per
update, not the raw travel ratio (high/60): 1.0 is a per-update accumulator, 60/rate is correct
time-based motion. The "changed" filter is a test on the 60 Hz run only, so a word that has been
fixed to game time still appears, at 60/rate, with its high-rate change count beside it.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock  # noqa: E402
import owned_process  # noqa: E402
import state_trace_run  # noqa: E402

OUT_DIR = ROOT / 'build' / 'rate-audit'
EXE = ROOT / 'build' / 'host64-exe' / 'dhd64.exe'
# window -> (menu gate, phase gate, skip, samples, scale by rate/60).
# Scaled windows measure the same field range at both rates (a scene driven by the field clock).
# Unscaled windows measure the same update range: for a scene whose own timeline is update-driven
# (the boot title), the two runs then stand at the same scene phase, which is the comparison the
# audit needs. The ranking's 60/rate reference holds either way (at 240 a field lasts 4 updates).
WINDOWS = {
    'none': (None, None, 10, 300, True),
    'title': (15, None, 30, 120, False),  # the boot title (menu 15), its own update timeline
    'race': (9, '2-4', 10, 600, True),    # a running race (menu 9, phase riding/countdown)
}


def game_env(rate, audit_path, skip, samples, scenario, seed, menu, phase):
    env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    env.update(
        DHD_ISO=os.environ.get('DHD_ISO', state_trace_run.DEFAULT_ISO),
        DHD_HEADLESS='1', DHD_FRAME_CLOCK='1', DHD_SKIP_MOVIES='1', DHD_MEMCARD='0',
        DHD_SND_OFF='1', DHD_RENDER='native', DHD_SETTINGS='none', DHD_FPS=str(rate),
        DHD_RATE_AUDIT=str(audit_path), DHD_RATE_AUDIT_SKIP=str(skip),
        DHD_RATE_AUDIT_SAMPLES=str(samples),
        # The audit ends the process itself; this field stop is the safety net when a gate never
        # holds (an unfinished run has no rows and fails the runner).
        DHD_STATE_STOP=str(100000),
    )
    if menu is not None:
        env['DHD_RATE_AUDIT_MENU'] = str(menu)
    if phase is not None:
        env['DHD_RATE_AUDIT_PHASE'] = phase
    if scenario == 'boot':
        env['DHD_NAV'] = state_trace_run.BOOT_NAV
    else:
        env.update(DHD_NAV=state_trace_run.RACE_NAV, DHD_RACE_SYNC='1',
                   DHD_RACE_PAD=state_trace_run.RACE_PAD, DHD_RACE_SEED=str(seed))
    return env


def run(args):
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    menu, phase, skip60, samples60, scale_window = WINDOWS[args.window]
    scale = args.rate / 60.0 if scale_window else 1.0
    skip = round(skip60 * scale)
    samples = round(samples60 * scale)
    audit_path = OUT_DIR / (args.name + '.txt')
    log_path = OUT_DIR / (args.name + '.log')
    for stale in (audit_path, log_path):
        if stale.exists():
            stale.unlink()
    exe = Path(args.exe).resolve()
    env = game_env(args.rate, audit_path, skip, samples, args.scenario, args.seed, menu, phase)
    if args.steps:
        env['DHD_RATE_AUDIT_STEPS'] = str(OUT_DIR / (args.name + '.steps.tsv'))
    print(f'{args.scenario} at {args.rate} fps, window {args.window} '
          f'(menu {menu}, phase {phase}), skip {skip}, samples {samples}: audit {audit_path}')
    start = time.monotonic()
    with game_lock.held('rate_audit ' + args.name), open(log_path, 'wb') as log:  # untimed: the virtual clock
        process = owned_process.run([str(exe)], timeout=args.timeout, cwd=str(OUT_DIR), env=env, stdout=log,
                                    stderr=subprocess.STDOUT)
    status = 'timeout' if process.timed_out else 'ok'
    rows = 0
    summary = ''
    if audit_path.exists():
        for line in open(audit_path, 'r', errors='replace'):
            if line.startswith('#'):
                if 'rate-audit' in line:
                    summary = line.strip()
                continue
            rows += 1
    print(f'{status}: exit {process.returncode}, {rows} rows, {time.monotonic() - start:.0f} s')
    print(summary)
    if status != 'ok' or not rows:
        print(f'run failed (see {log_path})')
        return 1
    return 0


def read_audit(path):
    header, rows = {}, {}
    for line in open(path, 'r', errors='replace'):
        if line.startswith('#'):
            for item in line[1:].split():
                key, _, value = item.partition('=')
                if value:
                    header[key] = value
            continue
        parts = line.split('\t')
        if len(parts) < 4:
            continue
        changes = int(parts[4]) if len(parts) > 4 else None
        net = float(parts[5]) if len(parts) > 5 else 0.0
        rows[int(parts[0], 16)] = (float(parts[1]), parts[2], parts[3].strip(), changes, net)
    return header, rows


def compare(args):
    h60, r60 = read_audit(OUT_DIR / (args.low + '.txt'))
    hhi, rhi = read_audit(OUT_DIR / (args.high + '.txt'))
    n60 = int(h60.get('samples', '0'))
    nhi = int(hhi.get('samples', '0'))
    print(f'60:   {args.low}  rate={h60.get("rate")} samples={n60} updates={h60.get("updates")} '
          f'fields {h60.get("field0")}..{h60.get("field1")} words={len(r60)}')
    print(f'high: {args.high}  rate={hhi.get("rate")} samples={nhi} updates={hhi.get("updates")} '
          f'fields {hhi.get("field0")}..{hhi.get("field1")} words={len(rhi)}')
    if not n60 or not nhi:
        print('one run has no samples')
        return 1
    # A phase/angle/scale moves one way, so rank by net travel per update: 1.0 is a per-update
    # accumulator, 60/rate is correct time-based motion. Travel (sum of |change|) is shown to
    # expose oscillators, whose net cancels even though their travel looks like an accumulator's.
    ranked = []
    skipped = 0
    for addr, (travel, kind, value, changes, net60) in r60.items():
        if travel < args.min_travel:
            continue
        # The change rate is a test on the 60 Hz run (the reference): a per-update accumulator
        # changes at every retail update. The high-rate side is deliberately not filtered on it:
        # a word fixed to game time changes only at every rate/60-th update there, and dropping
        # those would hide exactly the words this tool is used to verify. A word the high run no
        # longer reports moved by less than its minimum travel, i.e. it is time-based too.
        if changes is not None and changes < args.min_changes * n60:
            skipped += 1
            continue
        if addr in rhi:
            high, kind_high, value_high, changes_high, net_hi = rhi[addr]
        else:
            high, kind_high, value_high, changes_high, net_hi = 0.0, kind, '-', 0, 0.0
        per_update = (high / nhi) / (travel / n60)
        # A phase/angle moves one way: its net is most of its travel. An oscillator (a parity word,
        # a buffer the scene rewrites) has travel but little net, and dividing a near-zero net gives
        # meaningless ratios, so require both runs to be mostly monotonic.
        frac60 = abs(net60) / travel if travel else 0.0
        frac_hi = abs(net_hi) / high if high else 0.0
        if frac60 < args.min_monotonic or (high and frac_hi < args.min_monotonic):
            skipped += 1
            continue
        net_per = (abs(net_hi) / nhi) / (abs(net60) / n60) if net60 else 0.0
        ranked.append((net_per, per_update, high / travel, net60, net_hi, addr, kind, value,
                       changes, changes_high))
    ranked.sort(reverse=True)
    print(f'\n{len(ranked)} words with 60-travel >= {args.min_travel} and changed in >= '
          f'{args.min_changes:.0%} of the 60 Hz updates ({skipped} rare-change words skipped), '
          f'ranked by |net| travel per update (1.0 = per-update accumulator; '
          f'{60.0 / float(hhi.get("rate", 240)):.3f} = correct time-based motion; '
          f'travelper > netper means an oscillator)')
    print('  netper  travelper  net60         net_high      kind  addr       chg60  chgHi  value(60)')
    for net_per, per_update, ratio, net60, net_hi, addr, kind, value, changes, changes_high in \
            ranked[:args.top]:
        print(f'  {net_per:6.3f}  {per_update:9.3f}  {net60:12.6g}  {net_hi:12.6g}   {kind}   '
              f'0x{addr:08X}  {changes if changes is not None else "-":>5}  '
              f'{changes_high if changes_high is not None else "-":>5}  {value}')
    if not args.all:
        changing = [r for r in ranked if r[0] >= args.min_per_update]
        fixed = [r for r in ranked if 0 < r[0] < args.min_per_update]
        print(f'\nper-update words (>= {args.min_per_update}): {len(changing)}; '
              f'time-based: {len(fixed)}; no net change: {len(ranked) - len(changing) - len(fixed)}')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command', required=True)
    r = sub.add_parser('run', help='run one audit of one scenario at one rate')
    r.add_argument('scenario', choices=('boot', 'race'))
    r.add_argument('--rate', type=int, default=60)
    r.add_argument('--name', required=True)
    r.add_argument('--window', choices=tuple(WINDOWS), default='none')
    r.add_argument('--steps', action='store_true', help='write <name>.steps.tsv (scene per update)')
    r.add_argument('--seed', type=int, default=state_trace_run.DEFAULT_SEED)
    r.add_argument('--exe', default=str(EXE))
    r.add_argument('--timeout', type=int, default=1800)
    r.set_defaults(func=run)
    c = sub.add_parser('compare', help='rank words from a 60 run and a high-rate run')
    c.add_argument('low', help='name of the 60 run (build/rate-audit/<name>.txt)')
    c.add_argument('high', help='name of the high-rate run')
    c.add_argument('--top', type=int, default=40)
    c.add_argument('--min-travel', type=float, default=0.2)
    c.add_argument('--min-per-update', type=float, default=0.7,
                   help='travel per update at or above this counts as per-update')
    c.add_argument('--min-changes', type=float, default=0.8,
                   help='drop words changed in fewer than this fraction of the 60 Hz updates')
    c.add_argument('--min-monotonic', type=float, default=0.5,
                   help='drop words whose net is less than this fraction of their travel (oscillators)')
    c.add_argument('--all', action='store_true', help='only the ranked list')
    c.set_defaults(func=compare)
    args = ap.parse_args()
    sys.exit(args.func(args))


if __name__ == '__main__':
    main()
