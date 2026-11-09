"""A crowded P2P room (finding 180): this PC's game hosts, nettool bots join over P2P (each its own
process with its own peer connection), optionally the Mac's game too, and they race once. Since
the 0.7.12 outage every online release gets a 6+ racer setup test: this is P2P's.

    python port/tools/p2p_crowd.py OUT [--bots 5] [--turn-bots 1] [--remote user@mac --remote-app PATH --remote-iso PATH]
                                       [--server wss://dhd.skuza.app]

The bots are `nettool --races 1 --echo-ai` (they ready, mirror a host AI rider as their own, so
every one sends real snapshots, and finish); --turn-bots of them run with DHD_NET_P2P_UDP=0, so
the relay carries theirs. Without --server the room server is this checkout's local one; the
Mac (DHD_REMOTE, DHD_REMOTE_APP, DHD_REMOTE_ISO) reaches the local one by an ssh reverse tunnel.

Measured, in OUT/summary.json and on stdout: the setup time from the room's opening to the
host's countdown and to every bot's race; the host's CPU time and upload while racing (its
DHD_NET_PEER_STATS counters per peer); each peer's path; and the snapshot age on arrival per sender,
from the host's own 30 s `net:` lines (the age is the host's clock against the sender's stamp,
both on the server's clock). Untimed: it takes the game lock's untimed place."""
import argparse
import contextlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

import psutil

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock  # noqa: E402
import local_server  # noqa: E402
import owned_process  # noqa: E402
from hidden_game import test_iso  # noqa: E402
from online_race import iso_files_sha256  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
NETSTATS = re.compile(r'netstats: epoch_ms=(\d+) backend=p2p path=(\w+) remote_scope=(\w+) rx=(\d+) tx=(\d+) .*?conn=(\d+)')
AGE = re.compile(r'slot (\d+) snapshot age p50/p95/p99 (\d+)/(\d+)/(\d+) ms .*?on arrival (-?\d+) ms, gaps>100ms (\d+), messages ([\d.]+)/s')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('out', type=Path)
    p.add_argument('--bots', type=int, default=5)
    p.add_argument('--turn-bots', type=int, default=0, help='bots with direct UDP off (the relay only)')
    p.add_argument('--race-seconds', type=int, default=75, help='the bots finish after this long')
    p.add_argument('--exe', type=Path, default=ROOT / 'build/host64-exe/dhd64.exe')
    p.add_argument('--nettool', type=Path, default=ROOT / 'build/port/net/nettool.exe')
    p.add_argument('--server', help='the room server (default: a local one for this run)')
    p.add_argument('--remote', default=os.environ.get('DHD_REMOTE'))
    p.add_argument('--remote-app', default=os.environ.get('DHD_REMOTE_APP'))
    p.add_argument('--remote-iso', default=os.environ.get('DHD_REMOTE_ISO'))
    p.add_argument('--remote-dir', default='dhd-p2p-crowd')
    p.add_argument('--ssh-option', action='append', default=[])
    a = p.parse_args()
    use_mac = bool(a.remote and a.remote_app and a.remote_iso)
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    iso_hash = iso_files_sha256(test_iso(), ROOT / 'build/port/iso-check.txt')
    players = 1 + a.bots + use_mac
    ssh_base = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', *[x for o in a.ssh_option for x in ('-o', o)]]
    remote_dir = f'{a.remote_dir}/{out.name}'

    def ssh(command):
        return subprocess.run(ssh_base + [a.remote, command], capture_output=True, text=True, timeout=30).stdout

    summary = dict(players=players, bots=a.bots, turn_bots=a.turn_bots, mac=use_mac)
    host_log = out / 'host-stderr.log'
    with contextlib.ExitStack() as stack:
        stack.enter_context(game_lock.held('p2p_crowd'))
        if a.server:
            server = mac_server = a.server
        elif use_mac:
            server, mac_server = stack.enter_context(local_server.tunnelled(a.remote, a.ssh_option))
        else:
            server = mac_server = stack.enter_context(local_server.running())
        env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
        # DHD_NET_LOG_CODE: the room's code in full in the host's log, which is where this reads it.
        env.update(DHD_NET_SERVER=server, DHD_NET_ISO_SHA256=iso_hash, DHD_NET_LOG_CODE='1', DHD_ISO=test_iso(), DHD_SKIP_MOVIES='1',
                   DHD_SND_OFF='1', DHD_MEMCARD='1', DHD_MEMCARD_DIR=str(out / 'card'), DHD_SETTINGS='none',
                   DHD_RENDER='native', DHD_FPS='60', DHD_SCALE='1', DHD_WINDOW='windowed', DHD_WINDOW_WIDTH='640',
                   DHD_WINDOW_HEIGHT='480', DHD_NET_SCRIPTED='1', DHD_RACE_PAD='0:600:cross',
                   DHD_NET_CREATE='p2p', DHD_NET_PLAYERS=str(players), DHD_NET_NAME='HOST', DHD_NET_RIDER='4',
                   DHD_NET_PEER_STATS='1', DHD_NET_TEST_FINISH=str(a.race_seconds + 20),
                   SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='0')
        (out / 'card').mkdir(exist_ok=True)
        log = stack.enter_context(host_log.open('w'))
        opened = time.time()
        host = stack.enter_context(owned_process.start([str(a.exe)], cwd=out, env=env, stdout=log, stderr=log, minimized=True))
        code = wait(host_log, r'online: room (\w{7}) \(host\)', 120).group(1)
        version = wait(host_log, r'version (dhd-port [\w.-]+(?: \w+)?)', 30).group(1)
        summary.update(room_open_s=round(time.time() - opened, 2))
        print('room', code, 'version', version, flush=True)
        bot_logs = []
        for k in range(a.bots):
            path = out / f'bot{k}.log'
            bot_env = dict(os.environ, DHD_NET_SERVER=server, **({'DHD_NET_P2P_UDP': '0'} if k < a.turn_bots else {}))
            bot_log = stack.enter_context(path.open('w'))
            stack.enter_context(owned_process.start(
                [str(a.nettool), '--version', version, '--iso', iso_hash, '--name', f'BOT{k}', '--pick', str(k + 1),
                 '--players', str(players),  # a session bot readies once the room has this many
                 '--races', '1', '--ready-after', '1', '--echo-ai', '--finish-after', str(a.race_seconds),
                 '--seconds', str(a.race_seconds + 240), 'join', code],
                env=bot_env, stdout=bot_log, stderr=subprocess.STDOUT))
            bot_logs.append(path)
        mac_pid = None
        if use_mac:
            assigns = ' '.join(f'{k}={shlex.quote(v)}' for k, v in dict(
                DHD_NET_SERVER=mac_server, DHD_NET_ISO_SHA256=iso_hash, DHD_ISO=a.remote_iso, DHD_SKIP_MOVIES='1',
                DHD_SND_OFF='1', DHD_MEMCARD='1', DHD_MEMCARD_DIR='card', DHD_SETTINGS='none', DHD_RENDER='native',
                DHD_FPS='60', DHD_SCALE='1', DHD_WINDOW='windowed', DHD_WINDOW_WIDTH='640', DHD_WINDOW_HEIGHT='480',
                DHD_NET_SCRIPTED='1', DHD_RACE_PAD='0:600:cross', DHD_NET_JOIN=code, DHD_NET_NAME='MAC',
                DHD_NET_RIDER='8', DHD_NET_TEST_FINISH=str(a.race_seconds)).items())
            q = shlex.quote(remote_dir)
            mac_pid = int(ssh(f'mkdir -p {q}/card; cd {q}; env {assigns} nohup {shlex.quote(a.remote_app)} '
                              f'> stdout.log 2> stderr.log < /dev/null & echo $!').split()[-1])
        try:
            wait(host_log, r'online: countdown starts', 240)
            summary['countdown_s'] = round(time.time() - opened, 2)
            print('countdown after', summary['countdown_s'], 's', flush=True)
            proc = psutil.Process(host.pid)
            cpu0, t0 = proc.cpu_times(), time.time()
            time.sleep(min(60, a.race_seconds - 10))
            cpu1, t1 = proc.cpu_times(), time.time()
            summary['host_cpu_percent_of_one_core'] = round(100 * (cpu1.user + cpu1.system - cpu0.user - cpu0.system) / (t1 - t0), 1)
            wait(host_log, r'online: result 1\.', a.race_seconds + 200)
            for path in bot_logs:  # each bot's RESULT line comes as it leaves
                with contextlib.suppress(TimeoutError):
                    wait(path, r'RESULT \{', 30)
        finally:
            if mac_pid:
                (out / 'mac-stderr.log').write_text(ssh(f'kill {mac_pid}; sleep 2; cat {shlex.quote(remote_dir)}/stderr.log'))
    host_text = host_log.read_text(errors='replace')
    summary['peers'] = peers(host_text)
    summary['host_upload_kB_s'] = round(sum(p['tx_kB_s'] for p in summary['peers'].values()), 1)
    summary['snapshot_age_at_host'] = ages(host_text)
    summary['bots'] = [bot(path.read_text(errors='replace')) for path in bot_logs]
    summary['freezes'] = host_text.count('freeze:')
    summary['paths'] = re.findall(r'net: P2P: member (\d+) connected: (.+)', host_text)
    (out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    return 0 if all(b['race'] for b in summary['bots']) and not summary['freezes'] else 1


def wait(path, pattern, seconds):
    until = time.time() + seconds
    while time.time() < until:
        m = re.search(pattern, path.read_text(errors='replace') if path.exists() else '')
        if m:
            return m
        time.sleep(1)
    raise TimeoutError(f'{path.name}: no {pattern} within {seconds} s')


def peers(text):
    """Per peer connection at the host (its netstats lines, one a second): its path and its mean
    upload and download over the lines' span."""
    first, last = {}, {}
    for m in NETSTATS.finditer(text):
        epoch, path, scope, rx, tx, conn = m.groups()
        row = (int(epoch), path, scope, int(rx), int(tx))
        first.setdefault(conn, row)
        last[conn] = row
    out = {}
    for conn, (t1, path, scope, rx1, tx1) in last.items():
        t0, _, _, rx0, tx0 = first[conn]
        span = max(1.0, (t1 - t0) / 1000)
        out[conn] = dict(path=path, scope=scope, tx_kB_s=round((tx1 - tx0) / span / 1000, 1),
                         rx_kB_s=round((rx1 - rx0) / span / 1000, 1), seconds=round(span))
    return out


def ages(text):
    """Snapshot age per sender over the race: from the 30 s `net:` lines whose sender was still
    racing (10 snapshot messages a second or more), the median across those windows of each
    percentile and of the arrival age (the window around the finish has the wait for the results
    in it), and the gaps over 100 ms in all."""
    windows = {}
    for m in AGE.finditer(text):
        slot, p50, p95, p99, arrival, gaps, rate = m.groups()
        if float(rate) >= 10:
            windows.setdefault(f'slot{slot}', []).append((int(p50), int(p95), int(p99), int(arrival), int(gaps), float(rate)))
    median = lambda values: sorted(values)[len(values) // 2]
    return {slot: dict(windows=len(w), p50=median([x[0] for x in w]), p95=median([x[1] for x in w]), p99=median([x[2] for x in w]),
                       arrival_median=median([x[3] for x in w]), gaps=sum(x[4] for x in w), messages_s=median([x[5] for x in w]))
            for slot, w in sorted(windows.items())}


def bot(text):
    result = re.search(r'RESULT (\{.*\})', text)
    return dict(race=bool(re.search(r'RACE 1 slot', text)), result=json.loads(result.group(1)) if result else None,
                ages=ages(text), failed=bool(re.search(r'FAILED', text)))


if __name__ == '__main__':
    sys.exit(main())
