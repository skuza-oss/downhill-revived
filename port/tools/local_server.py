"""The online server of this checkout (online/server, `wrangler dev`) on a free local port, for the
length of a test: started, waited for, and stopped with everything it started (workerd too).

    python port/tools/local_server.py -- python port/tools/live_game.py ...   # DHD_NET_SERVER is set for it
    python port/tools/local_server.py --port 8787 -- COMMAND ...             # a fixed port

    with local_server.running() as url:      # 'ws://127.0.0.1:<port>'
        ...

Each run gets fresh server state (its own --persist-to folder) and a scratch TEMP, both under
DHD_SCRATCH (else the system temp) and deleted afterwards; the server's log stays at
<scratch>/server-<port>.log (printed). Needs online/server's node_modules (`npm ci` there).
The process tree is owned_process's: nothing is found or stopped by name, and a crash or Ctrl+C
in the caller ends the server too, so no workerd is left holding the port."""
import argparse
import contextlib
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import owned_process  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SERVER = ROOT / 'online' / 'server'
WRANGLER = SERVER / 'node_modules' / 'wrangler' / 'bin' / 'wrangler.js'


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def busy(port):
    with socket.socket() as s:
        return s.connect_ex(('127.0.0.1', port)) == 0


@contextlib.contextmanager
def running(port=None, timeout=90, say=print):
    """Yields the server's ws:// address; the server ends with the block."""
    if not WRANGLER.exists():
        raise RuntimeError(f'no wrangler in {SERVER / "node_modules"}: run `npm ci` in online/server')
    port = port or free_port()
    if busy(port):
        raise RuntimeError(f'port {port} is already in use (another server? leave --port out for a free one)')
    scratch = Path(os.environ.get('DHD_SCRATCH') or tempfile.gettempdir())
    run_dir = scratch / f'dhd-server-{port}-{os.getpid()}'
    shutil.rmtree(run_dir, ignore_errors=True)
    (run_dir / 'tmp').mkdir(parents=True)
    log_path = scratch / f'server-{port}.log'
    env = dict(os.environ, TEMP=str(run_dir / 'tmp'), TMP=str(run_dir / 'tmp'), TMPDIR=str(run_dir / 'tmp'),
               WRANGLER_SEND_METRICS='false')
    cmd = ['node', str(WRANGLER), 'dev', '--port', str(port), '--ip', '127.0.0.1',
           '--persist-to', str(run_dir / 'state'), '--show-interactive-dev-session=false']
    with open(log_path, 'w') as log:
        with owned_process.start(cmd, cwd=SERVER, env=env, stdout=log, stderr=subprocess.STDOUT) as server:
            until = time.time() + timeout
            while 'Ready on' not in log_path.read_text(errors='replace'):
                if server.poll() is not None or time.time() > until:
                    raise RuntimeError(f'the server did not start (see {log_path})')
                time.sleep(0.5)
            say(f'local server: ws://127.0.0.1:{port} (log {log_path})')
            try:
                yield f'ws://127.0.0.1:{port}'
            finally:
                server.stop()
    shutil.rmtree(run_dir, ignore_errors=True)


def main():
    argv = sys.argv[1:]
    if '--' not in argv:
        sys.exit(__doc__)
    i = argv.index('--')
    p = argparse.ArgumentParser()
    p.add_argument('--port', type=int)
    a = p.parse_args(argv[:i])
    command = argv[i + 1:]
    with running(a.port) as url:
        code = subprocess.run(command, env=dict(os.environ, DHD_NET_SERVER=url)).returncode
    sys.exit(code)


if __name__ == '__main__':
    main()
