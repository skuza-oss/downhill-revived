"""A game on a hidden desktop (hidden_game.py) run by commands: from --script, then any line appended to
OUT/cmd.txt while it runs. What happened goes to OUT/live.out; the game's output to OUT/game.log.

    python port/tools/local_server.py -- python port/tools/live_game.py OUT --script checks.txt [VAR=VALUE...]

OUT is emptied at the start (a fresh run every time). --script is read once, before the game starts,
so its commands are never lost; cmd.txt is created empty and followed live (append to it only
after live.out exists). The server is DHD_NET_SERVER (local_server.py sets it), else the official
one's default in the game. Lines:
    field N              wait until the game's field count (capture lines) reaches N
    key NAME [HOLD]      one key (left up right down space enter esc tab backspace, or a letter)
    keys NAME NAME ...   keys 0.35 s apart
    hold NAME SECONDS    hold a key down (riding: X pedals)
    sleep S
    wait REGEX [S]       until game.log matches (default 120 s; _ stands for a space)
    shot LABEL           newest capture as OUT/LABEL.png
    bot NAME ARGS...     nettool ARGS... join <this game's room code>, log OUT/bot-NAME.log
    quit
    # ...                a comment"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hidden_game  # noqa: E402
import owned_process  # noqa: E402
from online_race import iso_files_sha256  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('out', type=Path)
    p.add_argument('env', nargs='*', metavar='VAR=VALUE', help="extra game environment ('<unset>' drops a default)")
    p.add_argument('--exe', type=Path, default=ROOT / 'build' / 'host64-exe' / 'dhd64.exe')
    p.add_argument('--nettool', type=Path, default=ROOT / 'build' / 'port' / 'net' / 'nettool.exe')
    p.add_argument('--name', default='ANNA', help="this game's player name")
    p.add_argument('--script', type=Path, help='commands run first, one per line')
    p.add_argument('--interval', type=float, default=0.25, help='seconds between captures')
    a = p.parse_args()
    script = a.script.read_text().splitlines() if a.script else []
    out = a.out.resolve()
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    (out / 'cmd.txt').write_text('')
    iso_hash = iso_files_sha256(hidden_game.test_iso(), ROOT / 'build' / 'port' / 'iso-check.txt')
    server = os.environ.get('DHD_NET_SERVER')
    env = dict(DHD_NET_ISO_SHA256=iso_hash, DHD_NET_NAME=a.name, DHD_ONLINE_MENU_TRACE='1', DHD_FPS='60')
    if server: env['DHD_NET_SERVER'] = server
    if os.environ.get('DHD_SCRATCH'): env.update(TEMP=os.environ['DHD_SCRATCH'], TMP=os.environ['DHD_SCRATCH'])
    env.update(kv.split('=', 1) for kv in a.env)
    g = hidden_game.Game(a.exe, out, env=env, name='live', interval=a.interval)
    bots = []

    def say(text):
        with open(out / 'live.out', 'a') as f:
            f.write(f'{time.strftime("%H:%M:%S")} {text}\n')

    def run(line):
        words = line.split()
        if not words or words[0].startswith('#'): return True
        c = words[0]
        if c == 'field':
            while g.field() < int(words[1]) and g.alive(): time.sleep(0.05)
        elif c == 'key':
            g.key(words[1], float(words[2]) if len(words) > 2 else 0.12)
        elif c == 'keys':
            g.keys(*words[1:])
        elif c == 'hold':
            g.key(words[1], float(words[2]))
        elif c == 'sleep':
            time.sleep(float(words[1]))
        elif c == 'wait':
            try:
                g.wait_log(words[1].replace('_', ' '), float(words[2]) if len(words) > 2 else 120)
            except RuntimeError as e:
                say(f'FAILED {line}: {e}')
                return True
        elif c == 'shot':
            g.shot(words[1], settle=0.6)
        elif c == 'bot':
            m = re.findall(r'online menu: room (\w{6}).*?, version (.+)', g.log())
            if not m:
                say(f'FAILED {line}: no room code in game.log yet')
                return True
            code, version = m[-1][0], m[-1][1].strip()
            cmd = [str(a.nettool), '--seconds', '1500', '--name', words[1], '--version', version, '--iso', iso_hash,
                   *(['--server', server] if server else []), *words[2:], 'join', code]
            bots.append(owned_process.start(cmd, stdout=open(out / f'bot-{words[1]}.log', 'w'),
                                            stderr=subprocess.STDOUT))
            say(f'bot {words[1]} joins {code}')
        elif c == 'quit':
            return False
        else:
            say(f'FAILED {line}: unknown command')
            return True
        say(f'done {line} (field {g.field()})')
        return True

    try:
        g.window()
        say('started')
        for line in script:
            if not g.alive() or not run(line): break
        else:
            done = 0
            while g.alive():
                lines = (out / 'cmd.txt').read_text().splitlines()
                if done < len(lines):
                    done += 1
                    if not run(lines[done - 1]): break
                else:
                    time.sleep(0.2)
        say('game ended' if not g.alive() else 'quit')
    finally:
        g.stop()
        for b in bots: b.stop()


if __name__ == '__main__':
    main()
