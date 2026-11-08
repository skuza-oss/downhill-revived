"""Measure real gameplay-channel RTT on a PC and an SSH-accessible Mac.

Uses private rooms on the official service (or, with --server local, this checkout's server, which
the Mac reaches by an ssh reverse tunnel), the same two-player race, fixed seed,
60 FPS and 640x480 windows. No screenshots or SSH polling during measurement.
Both games must be built with the opt-in DHD_NET_BENCH instrumentation.
Run --matrix for interleaved SFU/P2P trials and separate forced-TURN controls.
Remote machine/app/disc paths come from arguments or DHD_REMOTE* variables.
"""
import argparse
import contextlib
import csv
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time

import game_lock
import local_server
import owned_process
from hidden_game import test_iso
from online_race import iso_files_sha256

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('out', type=Path)
    p.add_argument('--remote', default=os.environ.get('DHD_REMOTE'), required=not os.environ.get('DHD_REMOTE'))
    p.add_argument('--remote-app', default=os.environ.get('DHD_REMOTE_APP'), required=not os.environ.get('DHD_REMOTE_APP'))
    p.add_argument('--remote-iso', default=os.environ.get('DHD_REMOTE_ISO'), required=not os.environ.get('DHD_REMOTE_ISO'))
    p.add_argument('--exe', type=Path, default=ROOT / 'build/host64-exe/dhd64.exe')
    p.add_argument('--server', default='wss://dhd.skuza.app', help="the room server, or 'local': this checkout's")
    p.add_argument('--p2p-server', help='the server for P2P trials, when not --server (as the official P2P Worker)')
    p.add_argument('--remote-dir', default='dhd-netbench')
    p.add_argument('--ssh-option', action='append', default=[])
    p.add_argument('--backend', choices=('sfu', 'p2p'), default='p2p')
    p.add_argument('--host', choices=('pc', 'mac'), default='pc')
    p.add_argument('--force-turn', action='store_true')
    p.add_argument('--seconds', type=int, default=60)
    p.add_argument('--matrix', action='store_true')
    p.add_argument('--rounds', type=int, default=0, help='with --matrix: this many trials of every case, '
                   'automatic paths in alternating order (default: two automatic, one forced TURN)')
    p.add_argument('--env', action='append', default=[], metavar='NAME=VALUE', help='an extra variable for both games')
    a = p.parse_args()
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    iso_hash = iso_files_sha256(test_iso(), ROOT / 'build/port/iso-check.txt')

    def ssh(command, timeout=30):
        options = [x for option in a.ssh_option for x in ('-o', option)]
        r = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', *options, a.remote, command],
                           capture_output=True, text=True, timeout=timeout)
        if r.returncode:
            raise RuntimeError(f'remote command failed ({r.returncode}): {r.stderr[-400:]}')
        return r.stdout

    def trial(number, backend, host, force):
        label = f'{number:02d}-{backend}-{host}-' + ('turn' if force else 'auto')
        folder = out / label
        folder.mkdir(exist_ok=False)
        remote_dir = f'{a.remote_dir}/{out.name}/{label}'
        remote_q = shlex.quote(remote_dir)
        pc_log_path = folder / 'pc-stderr.log'
        remote_pid = None
        pc = None
        result = dict(label=label, backend=backend, host=host, force_turn=force,
                      seconds=a.seconds, started_epoch=time.time(), status='running')
        here, there = (a.p2p_server, a.p2p_server) if backend == 'p2p' and a.p2p_server else servers
        base = dict(DHD_NET_SERVER=here, DHD_NET_BENCH='bench.csv',
                    DHD_NET_ISO_SHA256=iso_hash, DHD_SKIP_MOVIES='1', DHD_SND_OFF='1',
                    DHD_MEMCARD='1', DHD_MEMCARD_DIR='card', DHD_SETTINGS='none', DHD_RENDER='native',
                    DHD_FPS='60', DHD_SCALE='1', DHD_WINDOW='windowed', DHD_WINDOW_WIDTH='640',
                    DHD_WINDOW_HEIGHT='480', DHD_NET_SCRIPTED='1', DHD_RACE_PAD='0:600:cross',
                    DHD_NET_TEST_FINISH=str(a.seconds + 180))
        if force:
            base['DHD_NET_P2P_UDP' if backend == 'p2p' else 'DHD_NET_SFU_UDP'] = '0'
        base.update(item.split('=', 1) for item in a.env)

        def start(side, code=None):
            nonlocal pc, remote_pid
            env = dict(base, DHD_NET_NAME=side.upper(), DHD_NET_RIDER='4' if side == host else '8')
            if code is None: env.update(DHD_NET_CREATE='p2p' if backend == 'p2p' else '1', DHD_NET_PLAYERS='2')
            else: env['DHD_NET_JOIN'] = code
            if side == 'pc':
                (folder / 'card').mkdir()
                native = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
                native.update(env, DHD_ISO=test_iso(), SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='0')
                pc = stack.enter_context(owned_process.start([str(a.exe.resolve())], cwd=folder, env=native,
                    stdout=log, stderr=log, minimized=True))
                result['pc_pid'] = pc.pid
            else:
                env.update(DHD_ISO=a.remote_iso, DHD_NET_SERVER=there)
                assigns = ' '.join(f'{k}={shlex.quote(v)}' for k, v in env.items())
                reply = ssh(f'mkdir -p {remote_q}/card; cd {remote_q}; '
                            f'env {assigns} nohup {shlex.quote(a.remote_app)} > stdout.log 2> stderr.log < /dev/null & echo $!')
                remote_pid = int(reply.strip().split()[-1])
                result['mac_pid'] = remote_pid

        def read(side):
            if side == 'pc':
                return pc_log_path.read_text(errors='replace') if pc_log_path.exists() else ''
            return ssh(f'cat {remote_q}/stderr.log 2>/dev/null || true')

        def wait(read_fn, pattern, timeout):
            until = time.monotonic() + timeout
            while time.monotonic() < until:
                m = re.search(pattern, read_fn())
                if m: return m
                if pc is not None and pc.poll() is not None:
                    raise RuntimeError('PC game exited before the measurement')
                time.sleep(2)
            raise TimeoutError(f'waiting for {pattern}')

        print(f'START {label}', flush=True)
        with contextlib.ExitStack() as stack:
            log = stack.enter_context(pc_log_path.open('w'))
            try:
                start(host)
                code = wait(lambda: read(host), r'online: room (\w{6,7}) \(host\)', 120).group(1)
                result['room'] = code
                result['room_open_seconds'] = time.time() - result['started_epoch']
                start('mac' if host == 'pc' else 'pc', code)
                wait(lambda: read('pc'), r'countdown starts', 150)
                wait(lambda: read('mac'), r'countdown starts', 150)
                result['both_countdowns_epoch'] = time.time()
                print(f'MEASURE {label}: both games started; {a.seconds}s samples + warmup/drain', flush=True)
                # Quiet window: no repeated SSH reads or screenshots competing with gameplay.
                until = time.monotonic() + a.seconds + 25
                while time.monotonic() < until:
                    if pc.poll() is not None: raise RuntimeError('PC game exited during measurement')
                    time.sleep(1)
                for mode in ('udp', 'tcp'):
                    data = ssh(f'nettop -m {mode} -p {remote_pid} -L 1 -J bytes_in,bytes_out 2>/dev/null || true')
                    (folder / f'mac-{mode}.csv').write_text(data)
                (folder / 'mac-process.txt').write_text(ssh(f'ps -p {remote_pid} -o time=,%cpu=,rss= || true'))
                result['status'] = 'collected'
            except Exception as e:
                result['status'] = 'failed'; result['error'] = str(e)
                print(f'FAIL {label}: {e}', flush=True)
            finally:
                if remote_pid:
                    try:
                        ssh(f'kill {remote_pid} 2>/dev/null || true')
                        time.sleep(1)
                        (folder / 'mac-stderr.log').write_text(ssh(f'cat {remote_q}/stderr.log 2>/dev/null || true'))
                        (folder / 'mac-bench.csv').write_text(ssh(f'cat {remote_q}/bench.csv 2>/dev/null || true'))
                    except Exception as e:
                        result['collection_error'] = str(e); result['status'] = 'failed'
        (folder / 'trial.json').write_text(json.dumps(result, indent=2) + '\n')
        if result['status'] == 'collected':
            for name in ('bench.csv', 'mac-bench.csv'):
                with (folder / name).open() as f:
                    rows = list(csv.DictReader(f))
                if not any(r['event'] == 'rtt' for r in rows):
                    result['status'] = 'failed'; result['error'] = f'no RTT samples in {name}'
            (folder / 'trial.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f'END {label}: {result["status"]}', flush=True)
        return result

    cases = [(a.backend, a.host, a.force_turn)]
    if a.matrix:
        first = [('sfu', 'pc', False), ('p2p', 'pc', False), ('sfu', 'mac', False), ('p2p', 'mac', False)]
        turn = [('sfu', 'pc', True), ('p2p', 'pc', True), ('p2p', 'mac', True), ('sfu', 'mac', True)]
        if a.rounds:
            cases = [c for r in range(a.rounds) for c in (first if r % 2 == 0 else list(reversed(first)))]
            cases += [c for r in range(a.rounds) for c in (turn if r % 2 == 0 else list(reversed(turn)))]
        else:
            cases = first + list(reversed(first)) + turn
    results = []
    with contextlib.ExitStack() as held:
        servers = (held.enter_context(local_server.tunnelled(a.remote, a.ssh_option)) if a.server == 'local'
                   else (a.server, a.server))
        held.enter_context(game_lock.held('cross_bench', timed=True))
        for number, case in enumerate(cases, 1):
            results.append(trial(number, *case))
            (out / 'trials.json').write_text(json.dumps(results, indent=2) + '\n')
            if results[-1]['status'] == 'failed':
                raise SystemExit('Measurement failed; raw evidence retained. Resolve before continuing.')


if __name__ == '__main__':
    main()
