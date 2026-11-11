/* The SIF: the EE's link to the IOP, which runs the CD/DVD, controller, memory card, file and
   sound modules (finding 020). The game's libraries talk to those modules by RPC (bind a client
   to a server id, then call function numbers with a send and a receive buffer); here the call
   queues a native server and delivers its reply at a later kernel interrupt. The
   libraries' own code (semaphores, callbacks, buffers) stays recompiled MIPS. */
#include "hle.h"

#include <string.h>

typedef struct {               /* sceSifClientData */
    uint32_t packet, packet_id, sema, mode;  /* sceSifRpcData */
    uint32_t command, buffer, cbuffer, end_function, end_param;
    uint32_t server;           /* nonzero once bound: here the server's table index + 1 */
} SifClient;

typedef void (*EndFunction)(uint32_t param);

static const struct {
    uint32_t id;
    const char *name;
    RpcServer fn;
} servers[] = {
    {0x80000001, "fileio", rpc_fileio},
    {0x80000003, "IOP heap", rpc_iop_heap},
    {0x80000006, "loadfile", rpc_loadfile},
    {0x80000100, "pad", rpc_pad},
    {0x80000101, "pad (ext)", rpc_pad_ext},
    {0x80000400, "memory card", rpc_mc},
    {0x80000592, "cdvd init", rpc_cd_init},
    {0x80000593, "cdvd S-command", rpc_cd_scmd},
    {0x80000595, "cdvd N-command", rpc_cd_ncmd},
    {0x80000597, "cdvd search file", rpc_cd_search},
    {0x8000059C, "cdvd disk ready", rpc_cd_diskready},
    {0x80000901, "multitap port open", rpc_mtap_open},
    {0x80000902, "multitap 2", rpc_mtap},
    {0x80000903, "multitap connection", rpc_mtap_connection},
    {0x80000904, "multitap 4", rpc_mtap},
    {0x80000905, "multitap version", rpc_mtap_version},
    {0x00123456, "sound driver", rpc_sound},
    {0x00123457, "sound driver (2)", rpc_sound2},
};
#define NSERVERS (int)(sizeof servers / sizeof *servers)

/* The SDK polls packet word 4 (allocated) and word 6 (RPC id). Synchronous
   calls wait on a semaphore; NOWAIT retains the caller's buffers until reply. The client's packet
   word points at the packet, and the game keeps the client in its RAM: the packets are in the HLE
   window (memory.h), at the same address in every build, not in this file's .bss, which moves with
   the core's layout (finding 205). */
typedef struct { int count, max_count, init_count, wait_threads; uint32_t attr, option; } RpcSema;
extern int func_0025A6E0(RpcSema *);
extern int func_0025A6F0(int);
extern int func_0025A720(int);
extern int func_0025A710(int);

typedef struct {
    SifClient *client;
    uint32_t *packet;          /* 16 words in the HLE window */
    double due;
    int server, fno, ssize, rsize, sema;
    void *send, *recv;
    EndFunction end;
    uint32_t param;
} RpcPending;
static RpcPending pending[32];
static uint32_t rpc_id;
_Static_assert(sizeof pending / sizeof *pending * 16 * 4 <= 0x800, "SIF packets: their part of the HLE window");

static uint32_t *rpc_packet(unsigned slot)
{
    return (uint32_t *)(uintptr_t)PS2_HLE_WINDOW + slot * 16;
}

double sif_next_event(void)
{
    double due = 1e100;
    for (unsigned i = 0; i < sizeof pending / sizeof *pending; i++)
        if (pending[i].client && pending[i].due < due) due = pending[i].due;
    return due;
}

/* Called with the EE register bank saved and interrupt scheduling deferred. */
int sif_poll(void)
{
    int any = 0;
    double now = host_seconds();
    for (unsigned i = 0; i < sizeof pending / sizeof *pending; i++) {
        RpcPending *p = &pending[i];
        if (!p->client || p->due > now) continue;
        servers[p->server].fn(p->fno, p->send, p->ssize, p->recv, p->rsize);
        if (p->end) p->end(p->param);
        p->packet[4] = 0;
        p->client->packet = 0;
        if (p->sema >= 0) func_0025A710(p->sema);
        p->client = NULL;
        any = 1;
    }
    return any;
}

void sceSifInitRpc(int mode) RETAIL(func_0025C7D0);
void sceSifInitRpc(int mode) { (void)mode; }

int sceSifBindRpc(SifClient *cd, uint32_t id, int mode) RETAIL(func_0025CDB8);
int sceSifBindRpc(SifClient *cd, uint32_t id, int mode)
{
    (void)mode;
    kernel_poll();
    cd->command = 0;
    cd->server = 0;
    for (int i = 0; i < NSERVERS; i++)
        if (servers[i].id == id) {
            cd->server = (uint32_t)i + 1;
            return 0;
        }
    hle_log("sceSifBindRpc: no server 0x%08X\n", id);
    return 0;  /* not bound: the library keeps retrying, as with an IOP module not loaded */
}

int sceSifCallRpc(SifClient *cd, int fno, int mode, void *send, int ssize, void *recv, int rsize,
                  EndFunction end, uint32_t end_param) RETAIL(func_0025CF88);
int sceSifCallRpc(SifClient *cd, int fno, int mode, void *send, int ssize, void *recv, int rsize,
                  EndFunction end, uint32_t end_param)
{
    kernel_poll();
    if (cd->server == 0 || cd->server > (uint32_t)NSERVERS) {
        hle_log("sceSifCallRpc: client %p not bound\n", (void *)cd);
        return -1;
    }
    int s = (int)cd->server - 1;
    if (hle_verbose)
        hle_log("rpc %s %d (send %d, receive %d, mode %d)\n", servers[s].name, fno, ssize, rsize, mode);
    if (cd->packet) return -1;
    RpcPending *p = NULL;
    unsigned slot = 0;
    for (unsigned i = 0; i < sizeof pending / sizeof *pending; i++)
        if (!pending[i].client) { p = &pending[i]; slot = i; break; }
    if (!p) return -1;
    int sema = -1;
    if (!(mode & 1)) {
        RpcSema param = {0, 1, 0, 0, 0, 0};
        sema = func_0025A6E0(&param);
        if (sema < 0) return -1;
    }
    memset(p, 0, sizeof *p);
    p->packet = rpc_packet(slot);
    memset(p->packet, 0, 16 * 4);
    p->client = cd;
    p->due = host_seconds() + 0.001;  /* synthetic IOP service latency */
    p->server = s; p->fno = fno; p->ssize = ssize; p->rsize = rsize;
    p->send = send; p->recv = recv; p->end = end; p->param = end_param;
    p->sema = sema;
    p->packet[4] = 1;
    p->packet[6] = ++rpc_id;
    cd->packet = (uint32_t)(uintptr_t)p->packet;
    cd->packet_id = rpc_id;
    cd->sema = (uint32_t)sema;
    cd->end_function = (uint32_t)(uintptr_t)end;
    cd->end_param = end_param;
    if (sema >= 0) {
        func_0025A720(sema);
        func_0025A6F0(sema);
    }
    return 0;
}

/* sceSifRebootIop / sceSifSyncIop: the IOP restarts with the game's IOPRP image (the modules
   are native here, so there is nothing to restart). */
int sceSifRebootIop(const char *image) RETAIL(func_0025ED48);
int sceSifRebootIop(const char *image)
{
    if (hle_verbose)
        hle_log("sceSifRebootIop(%s)\n", image);
    return 1;
}

int sceSifSyncIop(void) RETAIL(func_0025ED10);
int sceSifSyncIop(void) { return 1; }

/* SIF commands: the IOP's messages to the EE (cdvdfsv's 0x80000012 reports a finished
   command for sceCdCallback). sceSifAddCmdHandler / sceSifRemoveCmdHandler keep the handlers;
   sif_command runs one. */
typedef void (*CmdHandler)(void *packet, uint32_t data);

static struct {
    CmdHandler fn;
    uint32_t data;
} cmd_handlers[2][32];  /* system (0x80000000 | n), user */

void sceSifAddCmdHandler(uint32_t cid, CmdHandler fn, uint32_t data) RETAIL(func_0025C3E8);
void sceSifAddCmdHandler(uint32_t cid, CmdHandler fn, uint32_t data)
{
    cmd_handlers[!(cid >> 31)][cid & 31].fn = fn;
    cmd_handlers[!(cid >> 31)][cid & 31].data = data;
}

void sceSifRemoveCmdHandler(uint32_t cid) RETAIL(func_0025C418);
void sceSifRemoveCmdHandler(uint32_t cid) { cmd_handlers[!(cid >> 31)][cid & 31].fn = NULL; }

void sif_command(uint32_t cid, void *packet)
{
    CmdHandler fn = cmd_handlers[!(cid >> 31)][cid & 31].fn;
    if (fn)
        fn(packet, cmd_handlers[!(cid >> 31)][cid & 31].data);
}

/* SIF registers (syscalls 0x79, 0x7A); SMFLAG (4) says the IOP is up. */
static uint32_t sif_regs[32] = {[4] = 0x60000};
uint32_t SifSetReg(uint32_t reg, uint32_t value) RETAIL(func_0025AAE0);
uint32_t SifSetReg(uint32_t reg, uint32_t value)
{
    uint32_t old = sif_regs[reg & 31];
    sif_regs[reg & 31] = value;
    return old;
}

uint32_t SifGetReg(uint32_t reg) RETAIL(func_0025AAF0);
uint32_t SifGetReg(uint32_t reg) { return sif_regs[reg & 31]; }

/* A server not written yet: logs the call once per function number and replies with zeros. */
void rpc_unknown(const char *server, int fno, void *send, int ssize, void *recv, int rsize)
{
    static struct { const char *server; int fno; } seen[256];
    static int nseen;
    int i;
    for (i = 0; i < nseen; i++)
        if (seen[i].server == server && seen[i].fno == fno)
            break;
    if (i == nseen && nseen < 256) {
        seen[nseen].server = server, seen[nseen++].fno = fno;
        hle_log("rpc %s: function %d not written (send %d bytes:", server, fno, ssize);
        for (int k = 0; k < ssize && k < 32; k += 4)
            hle_log(" %08X", *(uint32_t *)((char *)send + k));
        hle_log(", receive %d)\n", rsize);
    }
    if (recv && rsize > 0)
        memset(recv, 0, (size_t)rsize);
}
