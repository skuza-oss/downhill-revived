/* The port's WebSocket client and HTTP(S) GET, the same on every native platform: the Rust crate
   dhd_net (port/render/net: tungstenite over rustls with bundled roots), linked into the one Rust
   static library (dhd_render's native build). A receive thread connects, reads messages and
   reconnects when told to; a send thread drains a queue and ticks the handler every 10 ms. The
   callbacks come from those two threads. The browser has its own WebSocket (port/web). */
#ifndef NET_WS_H
#define NET_WS_H

#include <stddef.h>
#include <stdint.h>

typedef struct WsConn WsConn;

enum { WS_ATTEMPTS_OVER = 1000 };  /* on_close's attempt when no attempt follows */

typedef struct {
    void *ctx;
    void (*on_open)(void *ctx, WsConn *ws);  /* receive thread; the queue is empty */
    void (*on_message)(void *ctx, int binary, const uint8_t *data, size_t n);  /* send thread; text data[n] is 0 */
    /* Receive thread: the connection ended or failed (code -1: connecting failed). Returns the URL
       to connect to next, after a backoff, or NULL to stop. attempt counts failures since the last
       open, or is WS_ATTEMPTS_OVER: no attempt follows (the next would start over 30 s after the
       drop). The backoff (port/render/net lib.rs retry_delay, finding 254): about 1 s, then 2 s
       while nothing reaches the server; failures the server answered 4 s, then 12 s; a refusal by
       its rate limit (4011) 20-30 s; each with a little jitter. Also called for the close ws_stop
       makes. */
    const char *(*on_close)(void *ctx, int code, const char *reason, int attempt);
    int (*on_tick)(void *ctx);  /* send thread, may be NULL. 1: drop the connection (the server went silent) */
} WsHandler;

WsConn *ws_start(const char *url, const WsHandler *h);
/* Queues a message (any thread, callbacks too); dropped when not connected.
   Binary STATE/EVENT use the room SFU state/events channels; other data uses WebSocket. */
void ws_send(WsConn *ws, int binary, const void *data, size_t n);
/* Sends what is queued, closes, stops the threads and frees ws. Not from a callback. */
void ws_stop(WsConn *ws);

/* The race ping, ms: the round trip of the connection's SFU association to Cloudflare Realtime
   (its selected ICE pair's latest check), -1 while it has none (finding 181). Any thread. */
int ws_race_rtt_ms(WsConn *ws);

/* DHD_NET_TEST_CUT (a test switch, finding 254): from now for ms milliseconds this game has no
   network: no room connection opens, no SFU datagram goes out or comes in. */
void ws_test_cut(int ms);

/* A blocking HTTP(S) GET; ws:// and wss:// URLs work too. Returns the body length after a 200
   (at most cap - 1, NUL-terminated), -1 on error. */
int http_get(const char *url, char *out, size_t cap);

#endif
