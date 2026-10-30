"""The browser's online client: port/net's lobby client (client.c, json.c, net.c), the C the native
game links, as a wasm32 reactor with its own memory, build/web/dhd_net.wasm (finding 123).

    python port/web/build_net.py

Imports: env.ws_start, ws_send, ws_stop and http_get (ws.h, implemented by www/net.js over the
browser's WebSocket and fetch), env.web_now_ns (its clock, port/web/net/SDL3/SDL_timer.h) and
WASI's fd_write and environ (its log lines, getenv). Exports: net.c's host_service_net_* (their
addresses are this module's: GUEST is the identity on wasm32), net_web.c's struct sizes, malloc
and the function table, through which net.js calls a ws.h handler's callbacks. The client runs on
the game worker's one thread, so port/web/net/SDL3's locks do nothing."""
import os,subprocess,sys
from pathlib import Path
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'build/web'
WASI=DEVCACHES/'wasi-sdk/wasi-sdk-34.0-x86_64-windows'
SOURCES=[ROOT/'port/net'/n for n in ('client.c','json.c','net.c')]+[ROOT/'port/web/net/net_web.c']
EXPORTS=['host_service_net_'+n for n in ('init','create_room','join_room','leave','get_lobby','pick','set_ready','start_race',
         'racing','seconds','server_ms','send_frame','receive_frame','list_lobbies','set_room')]+['web_net_lobby_size','web_net_info_size','malloc','free']
def build(out=OUT/'dhd_net.wasm'):
    out.parent.mkdir(parents=True,exist_ok=True)
    cmd=[str(WASI/'bin/clang'),'--target=wasm32-wasip1','--sysroot='+str(WASI/'share/wasi-sysroot'),'-mexec-model=reactor','-O2',
         '-Wall','-Wno-unused-function','-I',str(ROOT/'port/web/net'),*map(str,SOURCES),'-o',str(out),
         '-Wl,--allow-undefined','-Wl,--export-table','-Wl,--no-entry',*['-Wl,--export='+e for e in EXPORTS]]
    env=dict(os.environ,TEMP=str(out.parent),TMP=str(out.parent))
    subprocess.run(cmd,check=True,env=env,cwd=ROOT)
    return out
if __name__=='__main__':print(build())
