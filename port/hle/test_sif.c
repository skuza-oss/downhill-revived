/* RPC replies must be deferred: the stream loader needs the IOP wait to
   schedule its decompression thread before ReleaseWaitThread. */
#include "hle.h"
#include <assert.h>
#include <string.h>

typedef struct { uint32_t words[10]; } Client;
extern int func_0025CDB8(Client *, uint32_t, int);
extern int func_0025CF88(Client *, int, int, void *, int, void *, int, void (*)(uint32_t), uint32_t);
static double now;
static int calls, callbacks, waits, live_sema, signaled;
static Client *callback_client;
static int *callback_reply;
int hle_verbose;
void hle_log(const char *fmt, ...) { (void)fmt; }
double host_seconds(void) { return now; }
void kernel_poll(void) { sif_poll(); }
int func_0025A6E0(const int *p) { assert(p[1] == 1 && p[2] == 0 && !live_sema); live_sema = 1; return 7; }
int func_0025A710(int id) { assert(id == 7 && live_sema); signaled++; return id; }
int func_0025A720(int id)
{
    assert(id == 7 && !signaled);
    waits++;
    now += 0.002;
    assert(sif_poll() && signaled == 1);
    return id;
}
int func_0025A6F0(int id) { assert(id == 7 && signaled); live_sema = signaled = 0; return id; }
static void reply(int fno, void *send, int ssize, void *recv, int rsize)
{
    assert(fno == 3 && ssize == 4 && rsize == 4);
    calls++;
    *(int *)recv = *(int *)send + 1;
}
#define SERVER(name) void name(int f, void *s, int sn, void *r, int rn) { reply(f, s, sn, r, rn); }
SERVER(rpc_fileio) SERVER(rpc_iop_heap) SERVER(rpc_loadfile) SERVER(rpc_pad) SERVER(rpc_pad_ext)
SERVER(rpc_mc) SERVER(rpc_cd_init) SERVER(rpc_cd_scmd) SERVER(rpc_cd_ncmd)
SERVER(rpc_cd_search) SERVER(rpc_cd_diskready) SERVER(rpc_mtap) SERVER(rpc_mtap_open) SERVER(rpc_mtap_connection)
SERVER(rpc_mtap_version) SERVER(rpc_sound) SERVER(rpc_sound2)
static void completed(uint32_t arg)
{
    assert(arg == 123 && *callback_reply == 42);
    assert(callback_client->words[0]);  /* SDK clears the busy packet after the callback. */
    callbacks++;
}
static int busy(const Client *c)
{
    const uint32_t *p = (void *)(uintptr_t)c->words[0];
    /* the packet is in the HLE window, at the same address in every build (finding 205) */
    assert(!p || ((uintptr_t)p >= PS2_HLE_WINDOW && (uintptr_t)p < PS2_HLE_WINDOW + 0x800));
    return p && p[6] == c->words[1] && (p[4] & 1);
}
int main(void)
{
    Client c = {{0}};
    int request = 41, response = 0;
    assert(!func_0025CDB8(&c, 0x80000595, 0));
    callback_client = &c; callback_reply = &response;
    assert(!func_0025CF88(&c, 3, 1, &request, 4, &response, 4, completed, 123));
    assert(busy(&c) && !response && !calls && !callbacks);
    assert(func_0025CF88(&c, 3, 1, &request, 4, &response, 4, 0, 0) < 0);
    now = 0.0005;
    assert(!sif_poll() && busy(&c));
    assert(sif_next_event() == 0.001);
    now = 0.001;
    assert(sif_poll() && !busy(&c) && calls == 1 && callbacks == 1 && response == 42);
    response = 0;
    assert(!func_0025CF88(&c, 3, 0, &request, 4, &response, 4, completed, 123));
    assert(waits == 1 && calls == 2 && callbacks == 2 && !busy(&c) && !live_sema);
    Client clients[33] = {{{0}}};
    for (int i = 0; i < 33; i++) {
        assert(!func_0025CDB8(&clients[i], 0x80000595, 0));
        int result = func_0025CF88(&clients[i], 3, 1, &request, 4, &response, 4, 0, 0);
        assert(i == 32 ? result < 0 : result == 0);
    }
    now += 0.002;
    assert(sif_poll() && calls == 34);
    for (int i = 0; i < 33; i++) assert(!busy(&clients[i]));
    assert(!sif_poll() && sif_next_event() > 1e50);
    puts("SIF deferred replies, busy packets, synchronous waits and queue limits passed");
    return 0;
}
