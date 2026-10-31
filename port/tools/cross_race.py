"""The release check for online changes: a room race between this PC and a Mac (or any machine with
ssh), both ways, through this checkout's server on this PC (local_server.py), which the other
machine reaches by an ssh reverse tunnel (its 127.0.0.1:8787 is this PC's server).

    python port/tools/cross_race.py OUT --host pc  --remote user@mac --remote-app '/path/Downhill Revived.app/Contents/MacOS/dhd'
    python port/tools/cross_race.py OUT --host mac ...                 # the other way round

The host side makes a public room from the ONLINE menu (CREATE A PUBLIC RACE); the other takes QUICK
RACE into it. Both ready by DHD_NET_TEST_READY after each race, pedal by DHD_RACE_PAD; the host
finishes at 20 s and watches (Right at 26 s), the guest finishes at 40 s, punching on the way. The
run lasts until --races countdowns have started on both. This PC's game runs minimised
(owned_process); the remote one by ssh, and only its PID is stopped. The summary lists each
side's room, load, countdown, finish, watching and back-to-the-room lines; a freeze line or a
side that never got its countdowns is a failure (exit 1).
Defaults from the environment: DHD_REMOTE (user@host), DHD_REMOTE_APP, DHD_REMOTE_ISO."""
import argparse
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import local_server  # noqa: E402
import owned_process  # noqa: E402
from hidden_game import test_iso  # noqa: E402
from online_race import iso_files_sha256  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
# fields at 60 a second: the title, the main menu's ONLINE, then the online menu's row
MENUS = '450:8:cross,1150:6:down,1170:6:down,1190:6:down,1230:8:cross'
HOST_NAV = MENUS + ',1450:6:down,1470:6:down,1490:6:down,1530:8:cross,2130:8:cross,2530:8:cross'
GUEST_NAV = MENUS + ',1450:6:down,1490:8:cross,2130:8:cross,2530:8:cross,2930:8:cross'
PEDAL = '0:400:cross'
PUNCHES = ','.join(f'{1 + k * 0.5:.1f}:0.15:{"square" if k % 2 == 0 else "circle"}' for k in range(80))
SUMMARY = re.compile(r'online menu: (room|quick race|joining|refused)|has loaded|loaded at|countdown starts|freeze:|'
                     r'render wait|finished|results|left at|back to the room|TIME UP|host now|failed|error|'
                     r'watching [A-Z]|camera cut|race closed')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('out', type=Path)
    p.add_argument('--host', choices=('pc', 'mac'), required=True, help='which side makes the room')
    p.add_argument('--races', type=int, default=2)
    p.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    p.add_argument('--remote', default=os.environ.get('DHD_REMOTE'), help='ssh destination of the other machine')
    p.add_argument('--remote-app', default=os.environ.get('DHD_REMOTE_APP'), help="the game's executable there")
    p.add_argument('--remote-iso', default=os.environ.get('DHD_REMOTE_ISO'), help='the disc image there')
    p.add_argument('--remote-dir', default='dhd-cross-race', help='its run folder (relative: under its home)')
    p.add_argument('--ssh-option', action='append', default=[], help='extra ssh -o options, e.g. HostKeyAlias=...')
    a = p.parse_args()
    if not (a.remote and a.remote_app and a.remote_iso):
        p.error('--remote, --remote-app and --remote-iso (or DHD_REMOTE, DHD_REMOTE_APP, DHD_REMOTE_ISO) are needed')
    ssh_base = ['ssh', '-o', 'BatchMode=yes', *[x for o in a.ssh_option for x in ('-o', o)]]
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    rdir = f'{a.remote_dir}/{out.name}'

    def ssh(cmd):
        return subprocess.run(ssh_base + [a.remote, cmd], capture_output=True, text=True)

    iso_hash = iso_files_sha256(test_iso(), ROOT / 'build' / 'port' / 'iso-check.txt')
    common = dict(DHD_NET_SERVER='ws://127.0.0.1:8787', DHD_NET_ISO_SHA256=iso_hash, DHD_SKIP_MOVIES='1',
                  DHD_MEMCARD='1', DHD_SND_OFF='1', DHD_RENDER='native', DHD_SETTINGS='none', DHD_NET_SCRIPTED='1',
                  DHD_NET_TEST_READY='1', DHD_FPS='60', DHD_ONLINE_MENU_TRACE='1', DHD_SCALE='1',
                  DHD_CAPTURE_AT='20', DHD_CAPTURE_INTERVAL='15')
    pc_name, remote_name = ('PCHOST', 'MACGUEST') if a.host == 'pc' else ('PCGUEST', 'MACHOST')
    host_env = dict(DHD_AUTOPAD_FRAMES=HOST_NAV, DHD_RACE_PAD=PEDAL + ',26:0.15:right', DHD_NET_TEST_FINISH='20')
    guest_env = dict(DHD_AUTOPAD_FRAMES=GUEST_NAV, DHD_RACE_PAD=PEDAL + ',' + PUNCHES, DHD_NET_TEST_FINISH='40')
    pc_role, remote_role = (host_env, guest_env) if a.host == 'pc' else (guest_env, host_env)

    def remote_start():
        env = dict(common, **remote_role, DHD_NET_NAME=remote_name, DHD_ISO=a.remote_iso, DHD_WINDOW='windowed',
                   DHD_WINDOW_WIDTH='640', DHD_WINDOW_HEIGHT='480', DHD_SCREENSHOT='shot', DHD_MEMCARD_DIR='card')
        assigns = ' '.join(f'{k}={shlex.quote(v)}' for k, v in env.items())
        r = ssh(f'rm -rf {rdir}; mkdir -p {rdir}/card; cd {rdir}; env {assigns} nohup {shlex.quote(a.remote_app)} '
                f'> stdout.log 2> stderr.log < /dev/null & echo $!')
        pid = int(r.stdout.strip().split()[-1])
        print('remote pid', pid, flush=True)
        return pid

    def remote_log():
        return ssh(f'cat {rdir}/stderr.log 2>/dev/null').stdout

    pc_log_path = out / 'pc-stderr.log'

    def pc_log():
        return pc_log_path.read_text(errors='replace') if pc_log_path.exists() else ''

    def wait(read, pattern, seconds):
        until = time.time() + seconds
        while time.time() < until:
            m = re.search(pattern, read())
            if m: return m
            time.sleep(3)
        return None

    card = out / 'card'
    card.mkdir(exist_ok=True)
    for f in card.glob('*'): f.unlink()
    pc_env = {k: v for k, v in os.environ.items() if not k.startswith('DHD_')}
    pc_env.update(common, **pc_role, DHD_NET_NAME=pc_name, DHD_ISO=test_iso(), DHD_MEMCARD_DIR=str(card),
                  DHD_SCREENSHOT=str(out / 'shot'), SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='0')
    done = rf'(?s)(countdown starts.*){{{a.races}}}'
    ok, remote_pid = False, None
    with local_server.running() as url:
        port = url.rsplit(':', 1)[1]
        pc_env['DHD_NET_SERVER'] = url
        tunnel = owned_process.start(ssh_base + ['-N', '-o', 'ExitOnForwardFailure=yes', '-R', f'8787:127.0.0.1:{port}',
                                                 a.remote])
        try:
            time.sleep(3)
            if tunnel.poll() is not None: sys.exit('the ssh tunnel did not start')
            print('tunnel:', ssh('curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8787/v1/lobbies').stdout, flush=True)
            with open(pc_log_path, 'w') as pl:
                if a.host == 'pc':
                    with owned_process.start([str(a.exe)], cwd=out, env=pc_env, stdout=pl, stderr=pl, minimized=True) as pc:
                        pc.minimize()
                        m = wait(pc_log, r'online menu: room (\w{6}) \(host\)', 120)
                        print('room', m and m.group(1), flush=True)
                        remote_pid = remote_start()
                        ok = bool(wait(lambda: pc_log() if re.search(done, remote_log()) else '', done, 330 * a.races))
                        time.sleep(60)
                else:
                    remote_pid = remote_start()
                    m = wait(remote_log, r'online menu: room (\w{6}) \(host\)', 120)
                    print('room', m and m.group(1), flush=True)
                    with owned_process.start([str(a.exe)], cwd=out, env=pc_env, stdout=pl, stderr=pl, minimized=True) as pc:
                        pc.minimize()
                        ok = bool(wait(lambda: remote_log() if re.search(done, pc_log()) else '', done, 330 * a.races))
                        time.sleep(60)
        finally:
            if remote_pid:
                ssh(f'kill {remote_pid}; sleep 3; kill -9 {remote_pid} 2>/dev/null; true')
                print('remote game stopped', flush=True)
            tunnel.stop()
    (out / 'remote-stderr.log').write_text(remote_log())
    print('all races started on both:', ok)
    freezes = 0
    for side in ('pc', 'remote'):
        for line in (out / f'{side}-stderr.log').read_text(errors='replace').splitlines():
            if SUMMARY.search(line) and not line.startswith(' '):
                print(f'  {side}: {line}')
                freezes += 'freeze:' in line
    sys.exit(0 if ok and not freezes else 1)


if __name__ == '__main__':
    main()
