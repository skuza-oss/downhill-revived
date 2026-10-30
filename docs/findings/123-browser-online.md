# 123: Online in the browser

The browser build had no online: every `host_service_net_*` service was a stub (`worker.js`), so
ONLINE could never connect. The browser now plays on the same server as the native builds
(`wss://dhd.skuza.app`, or dhd.ini's `server`), with the same client.

## One client, built twice

The native game's lobby and race client is C: `port/net/client.c` (the state machine),
`json.c`, and `net.c` (net.h's API and the game's `host_service_net_*` boundary). It talks to the
network only through `ws.h` (`ws_start`, `ws_send`, `ws_stop`, `http_get`, and a handler's
open/message/close/tick callbacks), implemented natively by the Rust crate `dhd_net`.

The browser builds the same three files into their own wasm32 module, `build/web/dhd_net.wasm`
(`port/web/build_net.py`, run by `build.py`). Nothing in the protocol is written twice:

- **Locks.** `port/web/net/SDL3/SDL_mutex.h` makes the client's locks and condition do nothing:
  in the browser it runs on the game worker's one thread.
- **Clock.** `SDL_timer.h` takes the clock from the worker (`env.web_now_ns`, `performance.now`).
- **Addresses.** `GUEST()` is the identity on wasm32, so net.c's `host_service_net_*` work
  unchanged on addresses in the client's own memory.
- **`ws.h`.** `www/net.js` implements it over the worker's `WebSocket`, with the native
  client's rules:
  - The handler is copied at `ws_start` (net.c keeps it on its stack).
  - `on_close` decides whether to reconnect and where to, with the native backoff (1, 2, 4,
    8 s).
  - `ws_stop` calls `on_close` before it returns, as the native receive thread does, so the
    client hears of the close before net.c sets up the next room.
  - The tick runs every 10 ms.
  - A drop for a silent server is reported as 1006, a lost connection: a 4000s code would read
    as the server's refusal (`client_on_closed`).
- **Copying.** `net.js` copies each service's strings and buffers from the game's memory into
  the client's and its answers back. `NetLobby` and `NetLobbyInfo` are copied as bytes:
  both modules are wasm32, so the layouts agree (`net_web.c` exports their sizes).
- **The public list.** `host_sdl.c` fetches it on a thread; here `fetch()` starts it and the
  poll hands the body to net.c's own parser through `http_get`.
- **`hash_ranges`.** It is 0 in the browser: only `DHD_HASH_LOG`, a debugging log, uses it.

The worker gives the browser its thread at every vsync, so socket events and ticks run between
fields and never during a call into the client.

The server already allows it: `/v1/lobbies` sends `Access-Control-Allow-Origin: *`, and rooms
check no origin (`online/server/src/index.ts`). Checked on the official server: 200, `*`,
protocol 1, minVersion 0.7.3.

## Text fields and the clipboard

Typing the join code and the rider name used `host_service_text_input_*`, also stubs in the
browser. The page now pushes typed characters into a ring in the shared input buffer
(`main.js`: printable keys, Enter, Backspace, Escape, and pasted text). The worker applies
`port/hle/text_input.c`'s rules:

- upper case, and only the field's allowed characters;
- Enter accepts, Escape cancels, Backspace deletes;
- while a field is open, the typing keys are the field's. The arrows still move the pad
  picker, and a key typed into the field presses nothing until it is let go.

"Copy the join code" posts the text to the page, which writes the clipboard.

## The disc check

Online needs the disc's game-files hash (`dhd_disc`, finding 098). Its browser build is separate
work (finding 122). Until it is wired in, the check can't start in the browser, and ONLINE says
so. `?env.DHD_NET_ISO_SHA256=` gives the hash for tests, as `DHD_NET_ISO_SHA256` does for the
native tools.

## Builds must match

`hello.version` is `dhd-port <release> <core id>` (`online.c`), and the server keeps a room to
one version. A browser build joins the native players whose build has the same `dhd_core.wasm`.
So for the platforms to share rooms, a release must ship the browser build from the same core as
the native ones.

## Checks

- `node port/web/check_net.mjs ws://127.0.0.1:8787`: two clients through net.js and
  dhd_net.wasm against `wrangler dev` (online/server).
  - The host creates a room and the guest joins it by its code.
  - Both pick and ready, and the host starts.
  - Each receives the other's STATE frame with the right slot.
  - The public list is read, and both leave.
  - PASS, 3 runs of 3.
- Not yet run: the browser build itself in Chrome or Edge against the real server, and a race
  between a browser and a native game.
