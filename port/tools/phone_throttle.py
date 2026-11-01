"""Make a rooted Snapdragon 888 phone (Mi 11i) behave like a low-end phone, and put it back.

    python port/tools/phone_throttle.py --show
    python port/tools/phone_throttle.py --apply t612 [--big-max KHZ] [--little-max KHZ] [--gpu-level 9]
    python port/tools/phone_throttle.py --restore

Needs adb (`ADB` or `adb` on PATH), the phone's serial (`--serial` or `ANDROID_SERIAL`, else the only
device) and root through Magisk (`su -c`). Finding 127 explains the profile and its calibration.

`--apply t612` stands for a Unisoc T612 (2x Cortex-A75 + 6x Cortex-A55 at 1.82 GHz, Mali-G57 MC1):

- cpu6 and cpu7 (one A78, the X1) offline; cpu4-5 (A78) capped at `--big-max` kHz (an A78 does about
  1.3x an A75's work per clock, so ~1.4 GHz stands for a 1.82 GHz A75); cpu0-3 (A55) at 1.8 GHz;
- the Adreno 660 held at one power level (`--gpu-level`, 9 = its lowest, 315 MHz) through kgsl's
  `max_pwrlevel`/`min_pwrlevel`;
- governors stay schedutil (the T612 runs schedutil too) and the minimum clocks are left alone.

Taking cpu7 (policy7's only core) offline exits its governor, so its schedutil tunables come back with
kernel defaults when it returns: they are saved and `--restore` writes back any that changed.

Before it changes anything it saves every value it is going to touch (cpu online states, each policy's
governor/min/max, the kgsl levels) to a JSON file on the PC (`--state`, default
`DHD_SCRATCH/phone_throttle-<serial>.json`) and a copy on the phone. MIUI's thermal and perf services
are *not* stopped (the phone keeps its thermal protection); instead a root watchdog on the phone
re-asserts the throttle twice a second and counts every value it had to put back, in
/data/local/tmp/phone_throttle.log. `--restore` stops the watchdog, writes the saved values back,
reads every one again and fails loudly if any differs.

A policy's `scaling_max_freq` reads back the *effective* cap: the smaller of the user request and the
kernel's thermal/perf QoS caps (MIUI's thermal driver caps policy7 through the `thermal-cpufreq-7`
cooling device, which userspace cannot see as a request). Writing that read value back would leave a
user cap of its own until the next reboot. So `--apply` first works out what the user request was
(it writes the core's `cpuinfo_max_freq` and reads back: if the read value does not move, the cap was a
kernel QoS cap and the user request is restored as `cpuinfo_max_freq`; if it moves, the cap was a user
write and the saved value is put straight back), and `--restore` checks the effective value again.
"""
import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

CPU = '/sys/devices/system/cpu'
KGSL = '/sys/class/kgsl/kgsl-3d0'
PHONE_SAVED = '/data/local/tmp/phone_throttle.saved.json'
PHONE_WD = '/data/local/tmp/phone_throttle_wd.sh'
PHONE_WD_PID = '/data/local/tmp/phone_throttle_wd.pid'
PHONE_WD_LOG = '/data/local/tmp/phone_throttle.log'

PROFILES = {
    # cpus offline, per-cpu max kHz (written to the cpu's policy), kgsl power level
    't612': dict(offline=[6, 7], max_khz={0: 1804800, 4: 1440000}, gpu_level=9),
}


class Phone:
    def __init__(self, serial):
        self.adb = [os.environ.get('ADB') or shutil.which('adb') or 'adb'] + (['-s', serial] if serial else [])

    def su(self, script, check=True):
        """Run a shell script as root; returns stdout text."""
        assert script.strip(), 'an empty su -c would open an interactive root shell'
        r = subprocess.run(self.adb + ['shell', 'su -c ' + shlex.quote(script)],
                           capture_output=True, timeout=60)
        out = r.stdout.decode(errors='replace')
        if check and r.returncode:
            raise SystemExit('su failed (%d): %s\n%s' % (r.returncode, script[:200],
                                                         r.stderr.decode(errors='replace')[:400]))
        return out

    def read(self, paths):
        """Read many sysfs files in one adb round trip -> {path: text or None}."""
        script = '; '.join('echo "%s=$(cat %s 2>/dev/null || echo \'?ERR\')"' % (p, p) for p in paths)
        out = {}
        for line in self.su(script).splitlines():
            if '=' in line:
                k, v = line.split('=', 1)
                out[k] = None if v == '?ERR' else v.strip()
        return out

    def write(self, pairs):
        """Write (path, value) in order; returns the paths whose write failed."""
        if not pairs:
            return []
        script = '; '.join('echo %s > %s || echo "FAIL %s"' % (shlex.quote(str(v)), p, p) for p, v in pairs)
        return [l.split(' ', 1)[1] for l in self.su(script, check=False).splitlines() if l.startswith('FAIL ')]


def policies(phone):
    names = phone.su('ls -d %s/cpufreq/policy*' % CPU).split()
    rel = phone.read(['%s/related_cpus' % p for p in names])
    return {int(p.rsplit('policy', 1)[1]): [int(c) for c in rel['%s/related_cpus' % p].split()] for p in names}


def snapshot(phone, pols):
    """Every value --apply touches or --restore checks."""
    paths = ['%s/cpu%d/online' % (CPU, c) for pol in pols.values() for c in pol if c]
    for p in pols:
        base = '%s/cpufreq/policy%d' % (CPU, p)
        paths += [base + '/' + f for f in ('scaling_governor', 'scaling_min_freq', 'scaling_max_freq',
                                            'cpuinfo_max_freq', 'cpuinfo_min_freq')]
        paths += [base + '/schedutil/' + f for f in phone.su('ls %s/schedutil 2>/dev/null' % base,
                                                               check=False).split()]
    for first in sorted(min(cpus) for cpus in pols.values()):
        paths += ['%s/cpu%d/core_ctl/%s' % (CPU, first, f) for f in ('enable', 'min_cpus', 'max_cpus')]
    paths += ['%s/%s' % (KGSL, f) for f in ('max_pwrlevel', 'min_pwrlevel', 'thermal_pwrlevel', 'max_gpuclk',
                                             'num_pwrlevels', 'gpu_available_frequencies')]
    return phone.read(paths)


def policy_of(pols, cpu):
    return next(p for p, cpus in pols.items() if cpu in cpus)


def show(phone):
    pols = policies(phone)
    s = snapshot(phone, pols)
    cur = phone.read(['%s/cpufreq/policy%d/scaling_cur_freq' % (CPU, p) for p in pols] + ['%s/gpuclk' % KGSL])
    print('cpus online:', phone.su('cat %s/online' % CPU).strip())
    for p, cpus in pols.items():
        b = '%s/cpufreq/policy%d/' % (CPU, p)
        print('policy%d cpus %s: %s min %s max %s cur %s (hw max %s)' % (
            p, ','.join(map(str, cpus)), s[b + 'scaling_governor'], s[b + 'scaling_min_freq'],
            s[b + 'scaling_max_freq'], cur[b + 'scaling_cur_freq'], s[b + 'cpuinfo_max_freq']))
    print('gpu: max_pwrlevel %s min_pwrlevel %s thermal_pwrlevel %s max_gpuclk %s gpuclk %s' % (
        s[KGSL + '/max_pwrlevel'], s[KGSL + '/min_pwrlevel'], s[KGSL + '/thermal_pwrlevel'],
        s[KGSL + '/max_gpuclk'], cur[KGSL + '/gpuclk']))
    cool = phone.su('for d in /sys/class/thermal/cooling_device*; do t=$(cat $d/type); v=$(cat $d/cur_state); '
                    'case $t in thermal-cpufreq-*|thermal-devfreq-*|cpu-hotplug*|cpu-isolate*) '
                    '[ "$v" != 0 ] && echo "$t=$v";; esac; done', check=False).split()
    print('thermal cooling devices engaged:', ' '.join(cool) or 'none')
    wd = phone.su('[ -f %s ] && kill -0 $(cat %s) 2>/dev/null && echo running $(cat %s) || echo stopped'
                  % (PHONE_WD_PID, PHONE_WD_PID, PHONE_WD_PID), check=False).strip()
    fixes = phone.su('[ -f %s ] && grep -c fix %s || echo 0' % (PHONE_WD_LOG, PHONE_WD_LOG), check=False).strip()
    print('watchdog: %s, values put back so far: %s' % (wd, fixes))
    saved = phone.su('[ -f %s ] && echo yes || echo no' % PHONE_SAVED, check=False).strip()
    print('throttle applied (saved state on the phone):', saved)


def user_max_requests(phone, pols, saved):
    """For each policy, the user's scaling_max_freq request behind the effective value (see the docstring)."""
    out = {}
    for p in pols:
        b = '%s/cpufreq/policy%d/' % (CPU, p)
        eff, hw = saved[b + 'scaling_max_freq'], saved[b + 'cpuinfo_max_freq']
        if eff == hw:
            out[p] = eff
            continue
        phone.write([(b + 'scaling_max_freq', hw)])
        moved = phone.read([b + 'scaling_max_freq'])[b + 'scaling_max_freq']
        if moved == eff:      # a kernel QoS cap: the user request was (or now is) the hardware max
            out[p] = hw
        else:                 # a user-space write: put it straight back
            phone.write([(b + 'scaling_max_freq', eff)])
            out[p] = eff
    return out


def watchdog_script(targets):
    checks = '\n'.join(
        '  v=$(cat %s); if [ "$v" != "%s" ]; then echo %s > %s 2>/dev/null; '
        'echo "$(date +%%T) fix %s was $v now $(cat %s)" >> %s; fi' % (p, v, v, p, p, p, PHONE_WD_LOG)
        for p, v in targets)
    return '#!/system/bin/sh\necho $$ > %s\necho "$(date +%%T) start" >> %s\nwhile true; do\n%s\n  sleep 0.5\ndone\n' % (
        PHONE_WD_PID, PHONE_WD_LOG, checks)


def stop_watchdog(phone):
    phone.su('[ -f {p} ] && kill $(cat {p}) 2>/dev/null; rm -f {p} {s}'.format(p=PHONE_WD_PID, s=PHONE_WD),
             check=False)


def apply(phone, profile, state, big_max, little_max, gpu_level):
    prof = dict(PROFILES[profile])
    if big_max:
        prof['max_khz'] = {**prof['max_khz'], 4: big_max}
    if little_max:
        prof['max_khz'] = {**prof['max_khz'], 0: little_max}
    if gpu_level is not None:
        prof['gpu_level'] = gpu_level
    if state.exists() or phone.su('[ -f %s ] && echo yes || echo no' % PHONE_SAVED).strip() == 'yes':
        raise SystemExit('a throttle is already applied (%s or %s on the phone): --restore first'
                         % (state, PHONE_SAVED))
    pols = policies(phone)
    online_paths = ['%s/cpu%d/online' % (CPU, c) for pol in pols.values() for c in pol if c]
    was_online = phone.read(online_paths)
    saved_online = phone.su('cat %s/online' % CPU).strip()
    # bring every cpu online so every policy is readable, then save (keeping the online states read first)
    phone.write([(p, 1) for p in online_paths if was_online[p] != '1'])
    saved = dict(snapshot(phone, pols), **was_online)
    record = dict(serial=phone.adb[-1], profile=profile, settings=prof, taken=time.strftime('%Y-%m-%d %H:%M:%S'),
                  online_mask=saved_online, values=saved)
    record['user_max'] = user_max_requests(phone, pols, saved)
    state.parent.mkdir(parents=True, exist_ok=True)
    state.write_text(json.dumps(record, indent=1))
    tmp = Path(tempfile.gettempdir()) / ('phone_throttle_%d.json' % os.getpid())
    tmp.write_text(json.dumps(record, indent=1))
    subprocess.run(phone.adb + ['push', str(tmp), PHONE_SAVED], capture_output=True, check=True)
    tmp.unlink()

    targets = []
    for cpu, khz in prof['max_khz'].items():
        p = policy_of(pols, cpu)
        targets.append(('%s/cpufreq/policy%d/scaling_max_freq' % (CPU, p), khz))
    lv = prof['gpu_level']
    targets += [('%s/max_pwrlevel' % KGSL, lv), ('%s/min_pwrlevel' % KGSL, lv)]
    off = [('%s/cpu%d/online' % (CPU, c), 0) for c in prof['offline']]
    # min_pwrlevel first when raising max_pwrlevel's index is refused below min: write min then max
    order = targets[:-2] + [('%s/min_pwrlevel' % KGSL, lv), ('%s/max_pwrlevel' % KGSL, lv)] + off
    failed = phone.write(order)
    if failed:
        print('WARNING: writes failed:', failed)
    phone.su('cat > %s << "EOF"\n%sEOF\nchmod 755 %s; rm -f %s' % (PHONE_WD, watchdog_script(off + targets),
                                                                    PHONE_WD, PHONE_WD_LOG))
    phone.su('nohup %s > /dev/null 2>&1 &' % PHONE_WD)
    time.sleep(2)
    print('applied %s; saved state in %s' % (profile, state))
    show(phone)


def restore(phone, state):
    if state.exists():
        record = json.loads(state.read_text())
    else:
        text = phone.su('cat %s 2>/dev/null' % PHONE_SAVED, check=False)
        if not text.strip():
            raise SystemExit('no saved state (%s, nor %s on the phone)' % (state, PHONE_SAVED))
        record = json.loads(text)
    stop_watchdog(phone)
    saved = record['values']
    online = [(p, v) for p, v in saved.items() if p.endswith('/online') and v is not None]
    # every cpu online first (an offline cpu's policy is not writable), its saved state last
    failed = phone.write([(p, 1) for p, _ in online])
    pairs = []
    for pol, khz in record['user_max'].items():
        pairs.append(('%s/cpufreq/policy%s/scaling_max_freq' % (CPU, pol), khz))
    # kgsl: max_pwrlevel (the fast end) first so min_pwrlevel never sits above it
    pairs += [('%s/max_pwrlevel' % KGSL, saved['%s/max_pwrlevel' % KGSL]),
              ('%s/min_pwrlevel' % KGSL, saved['%s/min_pwrlevel' % KGSL])]
    failed += phone.write(pairs)
    # governors, schedutil tunables and core_ctl are not written by --apply, but taking a policy's last
    # cpu offline exits its governor and re-creates the tunables with kernel defaults: put back any
    # that changed (a governor first, its tunables after); minimum clocks are only checked (see above)
    now = phone.read(list(saved))
    fix = [(p, saved[p]) for p in saved if p.endswith('/scaling_governor') and now.get(p) != saved[p]]
    fix += [(p, saved[p]) for p in saved if ('/schedutil/' in p or '/core_ctl/' in p)
            and saved[p] is not None and now.get(p) != saved[p]]
    if fix:
        print('putting back %d governor/tunable values: %s' % (len(fix), ', '.join(p for p, _ in fix)))
        failed += phone.write(fix)
    failed += phone.write([(p, v) for p, v in online if v != '1'])
    time.sleep(1)
    now = phone.read(list(saved))
    check = [p for p in saved if not p.endswith(('/max_gpuclk', '/thermal_pwrlevel'))]
    bad = [(p, saved[p], now.get(p)) for p in check if now.get(p) != saved[p]]
    # a max that a kernel QoS cap held below the user request when it was saved moves with that cap:
    # any value from the saved read up to the user request is the stock state
    for pol, khz in record['user_max'].items():
        p = '%s/cpufreq/policy%s/scaling_max_freq' % (CPU, pol)
        if khz != saved[p] and now.get(p) and int(saved[p]) <= int(now[p]) <= int(khz):
            bad = [b for b in bad if b[0] != p]
            print('%s: saved %s under a thermal cap, now %s (user request %s; the cap moved)' % (p, saved[p], now[p], khz))
    for p, was, got in bad:
        print('DIFFERS %s: saved %s, now %s' % (p, was, got))
    for p in ('%s/max_gpuclk' % KGSL, '%s/thermal_pwrlevel' % KGSL):
        print('%s: saved %s, now %s (thermal state, not written)' % (p, saved[p], now.get(p)))
    online = phone.su('cat %s/online' % CPU).strip()
    print('cpus online: saved %s, now %s' % (record['online_mask'], online))
    if failed or bad or online != record['online_mask']:
        raise SystemExit('RESTORE INCOMPLETE (failed writes: %s); the saved state is kept in %s' % (failed, state))
    log = phone.su('cat %s 2>/dev/null' % PHONE_WD_LOG, check=False)
    fixes = [l for l in log.splitlines() if ' fix ' in l]
    print('watchdog put back %d values during the throttle%s' % (len(fixes), ''.join('\n  ' + l for l in fixes[-20:])))
    phone.su('rm -f %s %s' % (PHONE_SAVED, PHONE_WD_LOG), check=False)
    if state.exists():
        state.replace(state.with_suffix('.restored.json'))
    print('RESTORED: every saved value reads back unchanged')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--apply', choices=sorted(PROFILES))
    g.add_argument('--restore', action='store_true')
    g.add_argument('--show', action='store_true')
    ap.add_argument('--serial', default=os.environ.get('ANDROID_SERIAL'))
    ap.add_argument('--state', type=Path, help='where the saved values go on the PC')
    ap.add_argument('--big-max', type=int, help='kHz cap for the A78 cores that stay online (cpu4-5)')
    ap.add_argument('--little-max', type=int, help='kHz cap for the A55 cores (cpu0-3)')
    ap.add_argument('--gpu-level', type=int, help='kgsl power level to hold the GPU at (0 fastest)')
    a = ap.parse_args()
    phone = Phone(a.serial)
    serial = a.serial or subprocess.run(phone.adb + ['get-serialno'], capture_output=True).stdout.decode().strip()
    phone.adb = phone.adb[:1] + ['-s', serial]
    state = a.state or Path(os.environ.get('DHD_SCRATCH') or tempfile.gettempdir()) / ('phone_throttle-%s.json' % serial)
    if a.show:
        show(phone)
    elif a.apply:
        apply(phone, a.apply, state, a.big_max, a.little_max, a.gpu_level)
    else:
        restore(phone, state)


if __name__ == '__main__':
    sys.exit(main())
