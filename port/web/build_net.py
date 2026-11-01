"""The browser's online client: port/net's lobby client (client.c, json.c, net.c), the C the native
game links, as a wasm32 reactor with its own memory, build/web/dhd_net.wasm (finding 138), with voice chat: port/host/voice.c
and libopus (port/tools/opus.py wasm_objects; finding 131).

    python port/web/build_net.py

Imports: env.ws_start, ws_send, ws_stop and http_get (ws.h, implemented by www/net.js over the
browser's WebSocket and fetch), env.web_now_ns (its clock, port/web/net/SDL3/SDL_timer.h),
env.web_getenv and web_key_held (voice chat's settings and push-to-talk key, voice_web.c) and
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
SOURCES=[ROOT/'port/net'/n for n in ('client.c','json.c','join.c','net.c')]+[ROOT/'port/web/net/net_web.c',
         ROOT/'port/web/net/voice_web.c',ROOT/'port/host/voice.c']
EXPORTS=['host_service_net_'+n for n in ('init','create_room','join_room','leave','get_lobby','pick','set_ready','start_race',
         'racing','seconds','server_ms','send_frame','receive_frame','list_lobbies','set_room',
         'send_chat','get_chat')]+['web_net_lobby_size','web_net_info_size','web_net_chat_size','malloc','free']
# voice chat (voice.c): the game's host_service_voice_*, the output's mix, the page's microphone
EXPORTS+=['host_service_voice_'+n for n in ('frame','place','mute','drop','state')]+['voice_open','voice_mix',
         'voice_web_wanted','voice_web_mic','voice_web_capture']
def build(out=OUT/'dhd_net.wasm'):
    out.parent.mkdir(parents=True,exist_ok=True)
    sys.path.insert(0,str(ROOT/'port/tools'))
    import opus
    opus_objects,opus_include=opus.wasm_objects(WASI/'bin/clang.exe',WASI/'share/wasi-sysroot')
    cmd=[str(WASI/'bin/clang'),'--target=wasm32-wasip1','--sysroot='+str(WASI/'share/wasi-sysroot'),'-mexec-model=reactor','-O2',
         '-Wall','-Wno-unused-function','-DDHD_VOICE=1','-DDHD_VOICE_WEB=1','-I',str(ROOT/'port/web/net'),'-I',str(opus_include),
         '-I',str(ROOT/'port/include'),*map(str,SOURCES),*map(str,opus_objects),'-o',str(out),
         '-Wl,--allow-undefined','-Wl,--export-table','-Wl,--no-entry',*['-Wl,--export='+e for e in EXPORTS]]
    env=dict(os.environ,TEMP=str(out.parent),TMP=str(out.parent))
    subprocess.run(cmd,check=True,env=env,cwd=ROOT)
    return out
if __name__=='__main__':print(build())
