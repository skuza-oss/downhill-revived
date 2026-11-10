/* The EE kernel (syscalls): threads, semaphores, interrupt handlers, alarms (finding 020).

   PS2 threads are Windows fibers, so exactly one runs at a time and switches happen only where
   the PS2 kernel would make them: a thread blocks (WaitSema, SleepThread), one of higher
   priority becomes ready, or an interrupt handler readies one. The emulated MIPS registers of
   recompiled code (ps2asm.h) are per thread and switched with it.
   Interrupts are delivered at kernel entries and while every thread waits: VBLANK_START and
   VBLANK_END at 59.94 Hz of host time, the alarms (SetAlarm) on the same clock. */
#include "hle.h"
#include "../gs/gs.h"
#include "../gs/present.h"
#include "timer.h"
#include "state_trace.h"
#include "../include/fps.h"
#include "pace_grid.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ps2asm.h"
#include "../runtime/portable.h"

int hle_verbose;

void hle_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

/* The game's debug prints (empty in the retail build, port/patches.py DEBUG_PRINT). */
void hle_debug_print(const char *format, va_list args)
{
    if (hle_verbose) {
        fputs("game: ", stderr);
        char message[4096];
        int n=vsnprintf(message,sizeof message,format,args);
        if (n>0) host_console_write(GADDR(message),(unsigned)n<sizeof message ? (unsigned)n : sizeof message-1);
    }
}

/* ---- errors (Sony's kernel codes) ---- */
#define KE_ERROR (-1)
#define KE_SEMA_ZERO (-419)
#define KE_RELEASE_WAIT (-418)
#define KE_UNKNOWN_THID (-407)
#define KE_UNKNOWN_SEMID (-408)
#define KE_DORMANT (-413)
#define KE_NOT_WAIT (-415)

/* ---- clock: EE bus cycles (147.456 MHz) from host time ---- */
#define BUS_HZ 147456000.0
#define VSYNC_CYCLES ((uint64_t)(BUS_HZ * 1001 / 60000)) /* NTSC 59.94 Hz */
#define HSYNC_CYCLES (VSYNC_CYCLES / 262)               /* the alarms' unit */

/* The game clock in seconds since the first call: the frame clock while it runs (DHD_FRAME_CLOCK),
   else the wall clock (host_api.h). */
double host_seconds(void)
{
    return ee_frame_clock_enabled() ? (double)ee_frame_clock_cycles() / 147456000.0 : host_wall_seconds();
}

static uint64_t now_cycles(void)
{
    if (ee_frame_clock_enabled()) return ee_frame_clock_cycles();
    return (uint64_t)(host_seconds() * BUS_HZ);
}

/* ---- threads ---- */
enum { TS_RUN = 1, TS_READY = 2, TS_WAIT = 4, TS_SUSPEND = 8, TS_DORMANT = 0x10 };
enum { WAIT_NONE, WAIT_SLEEP, WAIT_SEMA };

typedef struct {               /* the game's thread parameter block (CreateThread, ReferThreadStatus) */
    int32_t status;
    uint32_t entry, stack, stack_size, gp;
    int32_t init_priority, current_priority;
    uint32_t attr, option;
    int32_t wait_type, wait_id, wakeup_count;
} ThreadParam;

typedef struct {
    ps2_q128 gpr[32];
    uint64_t lo, hi, lo1, hi1;
    float fpr[32], fpu_acc;
} Registers;

typedef struct {
    int used, status, priority, init_priority;
    uint32_t entry, stack, stack_size, gp, attr, option, arg;
    int wait, wait_id, wait_result, wakeup_count;
    int64_t order;  /* place in its priority's ready queue */
    HostFiber fiber, dead_fiber;
    Registers regs;
} Thread;

#define MAX_THREADS 256
static Thread threads[MAX_THREADS];
static Thread *current;         /* NULL while the idle loop runs */
static HostFiber idle_fiber;
static int64_t order_back = 1, order_front = 0;
static int in_interrupt;
static void (*root_function)(void);
static int game_exited;

static int thread_id(Thread *t) { return (int)(t - threads); }

static Thread *thread_of(int id)
{
    if (id == 0)  /* TH_SELF */
        return current;
    if (id <= 0 || id >= MAX_THREADS || !threads[id].used)
        return NULL;
    return &threads[id];
}

static void save_registers(Registers *r)
{
    memcpy(r->gpr, ps2.gpr, sizeof r->gpr);
    r->lo = ps2.lo, r->hi = ps2.hi, r->lo1 = ps2.lo1, r->hi1 = ps2.hi1;
    memcpy(r->fpr, ps2.fpr, sizeof r->fpr);
    r->fpu_acc = ps2.fpu_acc;
}

static void load_registers(const Registers *r)
{
    memcpy(ps2.gpr, r->gpr, sizeof r->gpr);
    ps2.lo = r->lo, ps2.hi = r->hi, ps2.lo1 = r->lo1, ps2.hi1 = r->hi1;
    memcpy(ps2.fpr, r->fpr, sizeof r->fpr);
    ps2.fpu_acc = r->fpu_acc;
}

static void make_ready(Thread *t, int front)
{
    t->status = TS_READY;
    t->wait = WAIT_NONE;
    t->order = front ? order_front-- : order_back++;
}

/* The ready thread that should run: lowest priority number, then queue order. */
static Thread *pick(void)
{
    Thread *best = NULL;
    for (int i = 1; i < MAX_THREADS; i++) {
        Thread *t = &threads[i];
        if (t->used && t->status == TS_READY
            && (!best || t->priority < best->priority || (t->priority == best->priority && t->order < best->order)))
            best = t;
    }
    return best;
}

static void run_thread(Thread *t)
{
    current = t;
    t->status = TS_RUN;
}

/* Switches to the thread that should run now, if that isn't the current one. The current
   thread, if still running, goes back to the front of its queue (preempted). */
static void reschedule(void)
{
    if (in_interrupt || !current)
        return;
    Thread *self = current, *next = pick();
    if (self->status == TS_RUN) {
        if (!next || next->priority >= self->priority)
            return;
        make_ready(self, 1);
    }
    if (next) {
        run_thread(next);
        save_registers(&self->regs);
        host_fiber_switch(next->fiber);
    } else {
        save_registers(&self->regs);
        current = NULL;
        host_fiber_switch(idle_fiber);
    }
    /* back on this thread (someone made it current) */
    load_registers(&self->regs);
    for (int i = 1; i < MAX_THREADS; i++)  /* fibers of threads that exited, now not running */
        if (threads[i].dead_fiber && &threads[i] != current) {
            host_fiber_delete(threads[i].dead_fiber);
            threads[i].dead_fiber = HOST_FIBER_NONE;
        }
}

/* Blocks the current thread until something readies it; returns its wait result. */
static int block(int kind, int id)
{
    Thread *self = current;
    self->status = TS_WAIT;
    self->wait = kind;
    self->wait_id = id;
    self->wait_result = 0;
    reschedule();
    return self->wait_result;
}

static void exit_current(void);

/* The EE FPU as PCSX2 runs it by default (Pcsx2Config.cpp DEFAULT_FPU_FP_CONTROL_REGISTER): results
   round toward zero and denormals are flushed. Each fiber keeps its own MXCSR (host_fiber_switch). */
static void ee_float_mode(void)
{
    host_fp_mode(HOST_FP_EE);  /* toward zero, FTZ, DAZ, exceptions masked */
}

static void thread_main(void *param)
{
    ee_float_mode();
    Thread *t = param;
    memset(&t->regs, 0, sizeof t->regs);
    load_registers(&t->regs);
    ps2.gpr[28].d[0] = t->gp;
    ps2.gpr[29].d[0] = (t->stack + t->stack_size - 0x40) & ~15u;  /* the stack of recompiled code */
    if (thread_id(t) == 1)
        root_function();
    else
        ((void (*)(uint32_t))(uintptr_t)t->entry)(t->arg);
    exit_current();
}

static void start(Thread *t, uint32_t arg)
{
    t->arg = arg;
    t->priority = t->init_priority;
    t->wakeup_count = 0;
    if (t->fiber && t->fiber != host_fiber_current())
        host_fiber_delete(t->fiber);
    else if (t->fiber)
        t->dead_fiber = t->fiber;
    t->fiber = host_callback_fiber(HOST_CALLBACK(thread_main), GADDR(t));
    make_ready(t, 0);
}

static void exit_current(void)
{
    Thread *self = current;
    if (thread_id(self) == 1) {  /* the main thread returned: the game is over */
        game_exited = 1;
        current = NULL;
        host_fiber_switch(idle_fiber);
    }
    self->status = TS_DORMANT;
    self->dead_fiber = self->fiber;
    self->fiber = HOST_FIBER_NONE;
    reschedule();
}

int CreateThread(ThreadParam *p) RETAIL(func_0025A4E0);
int CreateThread(ThreadParam *p)
{
    for (int i = 2; i < MAX_THREADS; i++)
        if (!threads[i].used) {
            Thread *t = &threads[i];
            memset(t, 0, sizeof *t);
            t->used = 1;
            t->status = TS_DORMANT;
            t->entry = p->entry, t->stack = p->stack, t->stack_size = p->stack_size, t->gp = p->gp;
            t->init_priority = t->priority = p->init_priority;
            t->attr = p->attr, t->option = p->option;
            if (hle_verbose)
                hle_log("CreateThread %d: entry %08X stack %08X+%X priority %d\n", i, p->entry, p->stack,
                        p->stack_size, p->init_priority);
            return i;
        }
    return KE_ERROR;
}

int DeleteThread(int id) RETAIL(func_0025A4F0);
int DeleteThread(int id)
{
    Thread *t = thread_of(id);
    if (!t || t == current)
        return KE_UNKNOWN_THID;
    if (t->status != TS_DORMANT)
        return KE_ERROR;
    if (t->fiber)
        host_fiber_delete(t->fiber);
    t->used = 0;
    return 0;
}

int StartThread(int id, uint32_t arg) RETAIL(func_0025A500);
int StartThread(int id, uint32_t arg)
{
    kernel_poll();
    Thread *t = thread_of(id);
    if (!t || t == current)
        return KE_UNKNOWN_THID;
    if (t->status != TS_DORMANT)
        return KE_ERROR;
    start(t, arg);
    reschedule();
    return 0;
}

void ExitThread(void) RETAIL(func_0010A1D0);  /* crt0's: the root function's return */
void ExitThread(void)
{
    exit_current();
}

static int terminate(int id)
{
    Thread *t = thread_of(id);
    if (!t || t == current)
        return KE_UNKNOWN_THID;
    if (t->status == TS_DORMANT)
        return KE_DORMANT;
    t->status = TS_DORMANT;
    t->wait = WAIT_NONE;
    if (t->fiber)
        host_fiber_delete(t->fiber);
    t->fiber = HOST_FIBER_NONE;
    return 0;
}

int TerminateThread(int id) RETAIL(func_0025A530);
int TerminateThread(int id) { return terminate(id); }
int iTerminateThread(int id) RETAIL(func_0025A540);
int iTerminateThread(int id) { return terminate(id); }

static int change_priority(int id, int priority)
{
    Thread *t = thread_of(id);
    if (!t)
        return KE_UNKNOWN_THID;
    int old = t->priority;
    t->priority = priority;
    if (t->status == TS_READY)
        t->order = order_back++;
    reschedule();
    return old;
}

int ChangeThreadPriority(int id, int priority) RETAIL(func_0025A570);
int ChangeThreadPriority(int id, int priority) { return change_priority(id, priority); }

static int rotate(int priority)
{
    Thread *self = current;
    if (self && self->priority == priority && !in_interrupt) {
        make_ready(self, 0);
        reschedule();
        return 0;
    }
    Thread *first = NULL;
    for (int i = 1; i < MAX_THREADS; i++) {
        Thread *t = &threads[i];
        if (t->used && t->status == TS_READY && t->priority == priority && (!first || t->order < first->order))
            first = t;
    }
    if (first)
        first->order = order_back++;
    return 0;
}

int RotateThreadReadyQueue(int priority) RETAIL(func_0025A590);
int RotateThreadReadyQueue(int priority) { kernel_poll(); return rotate(priority); }
int iRotateThreadReadyQueue(int priority) RETAIL(func_0025A5A0);
int iRotateThreadReadyQueue(int priority) { return rotate(priority); }

static int release_wait(int id)
{
    Thread *t = thread_of(id);
    if (!t)
        return KE_UNKNOWN_THID;
    if (t->status != TS_WAIT)
        return KE_NOT_WAIT;
    t->wait_result = KE_RELEASE_WAIT;
    make_ready(t, 0);
    reschedule();
    return 0;
}

int ReleaseWaitThread(int id) RETAIL(func_0025A5B0);
int ReleaseWaitThread(int id) { return release_wait(id); }
int iReleaseWaitThread(int id) RETAIL(func_0025A5C0);
int iReleaseWaitThread(int id) { return release_wait(id); }

int GetThreadId(void) RETAIL(func_0025A5D0);
int GetThreadId(void) { return current ? thread_id(current) : 0; }

int ReferThreadStatus(int id, ThreadParam *info) RETAIL(func_0025A5E0);
int ReferThreadStatus(int id, ThreadParam *info)
{
    Thread *t = thread_of(id);
    if (!t)
        return KE_UNKNOWN_THID;
    if (info) {
        info->status = t->status;
        info->entry = t->entry, info->stack = t->stack, info->stack_size = t->stack_size, info->gp = t->gp;
        info->init_priority = t->init_priority, info->current_priority = t->priority;
        info->attr = t->attr, info->option = t->option;
        info->wait_type = t->status == TS_WAIT ? (t->wait == WAIT_SLEEP ? 1 : 2) : 0;
        info->wait_id = t->wait == WAIT_SEMA ? t->wait_id : 0;
        info->wakeup_count = t->wakeup_count;
    }
    return t->status;
}

int SleepThread(void) RETAIL(func_0025A600);
int SleepThread(void)
{
    kernel_poll();
    if (current->wakeup_count > 0) {
        current->wakeup_count--;
        return 0;
    }
    return block(WAIT_SLEEP, 0);
}

static int wakeup(int id)
{
    Thread *t = thread_of(id);
    if (!t || t == current)
        return KE_UNKNOWN_THID;
    if (t->status == TS_DORMANT)
        return KE_DORMANT;
    if (t->status == TS_WAIT && t->wait == WAIT_SLEEP) {
        make_ready(t, 0);
        reschedule();
    } else {
        t->wakeup_count++;
    }
    return 0;
}

int WakeupThread(int id) RETAIL(func_0025A610);
int WakeupThread(int id) { return wakeup(id); }
int iWakeupThread(int id) RETAIL(func_0025A620);
int iWakeupThread(int id) { return wakeup(id); }

/* func_0025B280: iGetThreadId */
int iGetThreadId(void) RETAIL(func_0025B280);
int iGetThreadId(void) { return current ? thread_id(current) : 0; }

/* ---- semaphores ---- */
typedef struct {
    int32_t count, max_count, init_count, wait_threads;
    uint32_t attr, option;
} SemaParam;

typedef struct {
    int used, count, max_count, init_count;
    uint32_t attr, option;
} Sema;

#define MAX_SEMAS 512
static Sema semas[MAX_SEMAS];

static Sema *sema_of(int id) { return id > 0 && id < MAX_SEMAS && semas[id].used ? &semas[id] : NULL; }

static int waiting_on(int id)
{
    int n = 0;
    for (int i = 1; i < MAX_THREADS; i++)
        n += threads[i].used && threads[i].status == TS_WAIT && threads[i].wait == WAIT_SEMA && threads[i].wait_id == id;
    return n;
}

int CreateSema(SemaParam *p) RETAIL(func_0025A6E0);
int CreateSema(SemaParam *p)
{
    for (int i = 1; i < MAX_SEMAS; i++)
        if (!semas[i].used) {
            Sema *s = &semas[i];
            s->used = 1;
            s->count = s->init_count = p->init_count;
            s->max_count = p->max_count;
            s->attr = p->attr, s->option = p->option;
            return i;
        }
    return KE_ERROR;
}

int DeleteSema(int id) RETAIL(func_0025A6F0);
int DeleteSema(int id)
{
    Sema *s = sema_of(id);
    if (!s)
        return KE_UNKNOWN_SEMID;
    for (int i = 1; i < MAX_THREADS; i++)
        if (threads[i].used && threads[i].status == TS_WAIT && threads[i].wait == WAIT_SEMA && threads[i].wait_id == id) {
            threads[i].wait_result = KE_ERROR;
            make_ready(&threads[i], 0);
        }
    s->used = 0;
    reschedule();
    return id;
}

static int signal(int id)
{
    Sema *s = sema_of(id);
    if (!s)
        return KE_UNKNOWN_SEMID;
    Thread *first = NULL;  /* waiters wake in the order they started waiting */
    for (int i = 1; i < MAX_THREADS; i++) {
        Thread *t = &threads[i];
        if (t->used && t->status == TS_WAIT && t->wait == WAIT_SEMA && t->wait_id == id
            && (!first || t->order < first->order))
            first = t;
    }
    if (first) {
        make_ready(first, 0);
        reschedule();
    } else {
        s->count++;
    }
    return id;
}

int SignalSema(int id) RETAIL(func_0025A700);
int SignalSema(int id) { kernel_poll(); return signal(id); }
int iSignalSema(int id) RETAIL(func_0025A710);
int iSignalSema(int id) { return signal(id); }

int WaitSema(int id) RETAIL(func_0025A720);
int WaitSema(int id)
{
    kernel_poll();
    Sema *s = sema_of(id);
    if (!s)
        return KE_UNKNOWN_SEMID;
    if (s->count > 0) {
        s->count--;
        return id;
    }
    current->order = order_back++;  /* the waiters' order */
    int r = block(WAIT_SEMA, id);
    return r < 0 ? r : id;
}

int PollSema(int id) RETAIL(func_0025A730);
int PollSema(int id)
{
    kernel_poll();
    Sema *s = sema_of(id);
    if (!s)
        return KE_UNKNOWN_SEMID;
    if (s->count <= 0)
        return KE_SEMA_ZERO;
    s->count--;
    return id;
}

int iPollSema(int id) RETAIL(func_0025A740);
int iPollSema(int id) { return PollSema(id); }

int ReferSemaStatus(int id, SemaParam *info) RETAIL(func_0025A750);
int ReferSemaStatus(int id, SemaParam *info)
{
    Sema *s = sema_of(id);
    if (!s)
        return KE_UNKNOWN_SEMID;
    info->count = s->count, info->max_count = s->max_count, info->init_count = s->init_count;
    info->wait_threads = waiting_on(id);
    info->attr = s->attr, info->option = s->option;
    return id;
}

/* ---- interrupt handlers ---- */
typedef int (*Handler)(int cause, uint32_t arg, uint32_t addr);

typedef struct {
    int used, cause, id;
    Handler fn;
    uint32_t arg;
} HandlerEntry;

#define MAX_HANDLERS 64
static HandlerEntry intc_handlers[MAX_HANDLERS], dmac_handlers[MAX_HANDLERS];
static uint32_t intc_enabled, dmac_enabled;
static int next_handler_id = 1;

static int add_handler(HandlerEntry *table, int cause, Handler fn, int next, uint32_t arg)
{
    (void)next;
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (!table[i].used) {
            table[i] = (HandlerEntry){1, cause, next_handler_id++, fn, arg};
            if (hle_verbose)
                hle_log("Add%sHandler(%d, %p) = %d\n", table == intc_handlers ? "Intc" : "Dmac", cause, fn,
                        table[i].id);
            return table[i].id;
        }
    return KE_ERROR;
}

static int remove_handler(HandlerEntry *table, int cause, int id)
{
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (table[i].used && table[i].cause == cause && table[i].id == id) {
            table[i].used = 0;
            return 0;
        }
    return KE_ERROR;
}

static void run_handlers(HandlerEntry *table, uint32_t enabled, int cause)
{
    if (!(enabled & (1u << cause)))
        return;
    Registers saved;  /* an interrupt leaves the interrupted code's registers alone */
    save_registers(&saved);
    in_interrupt++;
    for (int i = 0; i < MAX_HANDLERS; i++)
        if (table[i].used && table[i].cause == cause)
            table[i].fn(cause, table[i].arg, 0);
    in_interrupt--;
    load_registers(&saved);
}

int AddIntcHandler(int cause, Handler fn, int next, uint32_t arg) RETAIL(func_0025A3C0);
int AddIntcHandler(int cause, Handler fn, int next, uint32_t arg) { return add_handler(intc_handlers, cause, fn, next, arg); }
int RemoveIntcHandler(int cause, int id) RETAIL(func_0025A3E0);
int RemoveIntcHandler(int cause, int id) { return remove_handler(intc_handlers, cause, id); }
int AddDmacHandler(int channel, Handler fn, int next, uint32_t arg) RETAIL(func_0025A3F0);
int AddDmacHandler(int channel, Handler fn, int next, uint32_t arg) { return add_handler(dmac_handlers, channel, fn, next, arg); }
int AddDmacHandler2(int channel, Handler fn, int next, uint32_t arg) RETAIL(func_0025A400);
int AddDmacHandler2(int channel, Handler fn, int next, uint32_t arg) { return add_handler(dmac_handlers, channel, fn, next, arg); }
int RemoveDmacHandler(int channel, int id) RETAIL(func_0025A410);
int RemoveDmacHandler(int channel, int id) { return remove_handler(dmac_handlers, channel, id); }

static int switch_bit(uint32_t *mask, int bit, int on)
{
    int was = (*mask >> bit) & 1;
    *mask = on ? *mask | (1u << bit) : *mask & ~(1u << bit);
    return was;
}

int EnableIntc(int cause) RETAIL(func_0025A420);
int EnableIntc(int cause) { return switch_bit(&intc_enabled, cause, 1); }
int DisableIntc(int cause) RETAIL(func_0025A430);
int DisableIntc(int cause) { return switch_bit(&intc_enabled, cause, 0); }
int EnableDmac(int channel) RETAIL(func_0025A440);
int EnableDmac(int channel) { return switch_bit(&dmac_enabled, channel, 1); }
int DisableDmac(int channel) RETAIL(func_0025A450);
int DisableDmac(int channel) { return switch_bit(&dmac_enabled, channel, 0); }
int iEnableIntc(int cause) RETAIL(func_0025A480);
int iEnableIntc(int cause) { return switch_bit(&intc_enabled, cause, 1); }
int iDisableIntc(int cause) RETAIL(func_0025A490);
int iDisableIntc(int cause) { return switch_bit(&intc_enabled, cause, 0); }
int iEnableDmac(int channel) RETAIL(func_0025A4A0);
int iEnableDmac(int channel) { return switch_bit(&dmac_enabled, channel, 1); }
int iDisableDmac(int channel) RETAIL(func_0025A4B0);
int iDisableDmac(int channel) { return switch_bit(&dmac_enabled, channel, 0); }

void kernel_dmac_interrupt(int channel)
{
    run_handlers(dmac_handlers, dmac_enabled, channel);
}

/* ---- alarms (in HSYNC units) ---- */
typedef void (*AlarmHandler)(int id, uint16_t time, uint32_t arg);

typedef struct {
    int used;
    uint64_t due;
    uint16_t ticks;
    AlarmHandler fn;
    uint32_t arg;
} Alarm;

#define MAX_ALARMS 64
static Alarm alarms[MAX_ALARMS];

static int set_alarm(uint16_t ticks, AlarmHandler fn, uint32_t arg)
{
    for (int i = 0; i < MAX_ALARMS; i++)
        if (!alarms[i].used) {
            alarms[i] = (Alarm){1, now_cycles() + (uint64_t)ticks * HSYNC_CYCLES, ticks, fn, arg};
            return i + 1;
        }
    return KE_ERROR;
}

int SetAlarm(uint16_t ticks, AlarmHandler fn, uint32_t arg) RETAIL(func_0025A460);
int SetAlarm(uint16_t ticks, AlarmHandler fn, uint32_t arg) { return set_alarm(ticks, fn, arg); }
int iSetAlarm(uint16_t ticks, AlarmHandler fn, uint32_t arg) RETAIL(func_0025A4C0);
int iSetAlarm(uint16_t ticks, AlarmHandler fn, uint32_t arg) { return set_alarm(ticks, fn, arg); }

static int release_alarm(int id)
{
    if (id <= 0 || id > MAX_ALARMS || !alarms[id - 1].used)
        return KE_ERROR;
    alarms[id - 1].used = 0;
    return 0;
}

int ReleaseAlarm(int id) RETAIL(func_0025A470);
int ReleaseAlarm(int id) { return release_alarm(id); }
int iReleaseAlarm(int id) RETAIL(func_0025A4D0);
int iReleaseAlarm(int id) { return release_alarm(id); }

/* ---- vsync ---- */
static uint64_t next_vsync;
/* Delivered NTSC fields from boot; independent of host speed and buffer flips. */
uint64_t kernel_vsync_count;
static uint32_t vsync_flag_addr, vsync_csr_addr;  /* SetVSyncFlag */

#define GS_CSR (*(volatile uint64_t *)0x12001000)

/* INTC_STAT: the causes raised since the game last cleared them (a store of 1 clears a bit). */
static uint32_t intc_stat;

static void intc_raise(int cause)
{
    intc_stat |= 1u << cause;
    *(volatile uint32_t *)(uintptr_t)PS2_INTC_STAT = intc_stat;
}

/* The store has already reached the mapped word; it holds the bits still raised. */
void kernel_intc_write(uint32_t value)
{
    intc_stat &= ~value;
    *(volatile uint32_t *)(uintptr_t)PS2_INTC_STAT = intc_stat;
}

/* Normal play runs the virtual frame clock (one game frame per VBLANK, as in the PCSX2
   comparisons) and holds each frame to its own wall deadline at the selected rate (59.94 Hz at
   60): never faster. A port that falls behind moves the deadlines instead of catching up. The
   game's step is the wall time since the last frame (fps.c, the delta-time clock, finding 139),
   so a slow frame moves the game as far as the time it took (up to two fields), and the game
   keeps real time down to 30 frames a second. DHD_FRAME_CLOCK=1 runs unpaced, with a fixed step
   (deterministic runs). */
static int paced;
/* fps.c's wall-clock steps (port_clock_wall): normal paced play, or a host that paces the frames
   itself on the frame clock and says so with DHD_FPS_WALL=1 (the browser's worker.js, which then
   gives host_wall_seconds real time). Deterministic runs (DHD_FRAME_CLOCK=1 alone) keep the
   nominal step. */
static int wall_steps;
static PaceGrid pace_grid;

/* An online race waited `seconds` for the other players: the following frames keep their spacing
   from the end of the wait instead of catching up (finding 042: a PC that started a little early
   otherwise waits about one field on every field), and the wait is no game time (no step
   covers it). */
void kernel_pace_slip(double seconds)
{
    if (pace_grid.base) pace_grid.base += seconds;
    port_clock_rebase();
}

/* An online race from its agreed start (kernel_pace_shared). Each PC times its own race (finding
   044): a PC that fell behind (a hitch, a stall, a PC too slow for the race) catches up through
   its steps, which online keep all the wall time the two-field cap leaves over and give it back
   when the frames are quicker (fps.c, finding 114), so its race clock stays true to wall time.
   The frames are paced as offline ones, without just-in-time frames (jit_on). */
static int pace_shared;
unsigned kernel_pace_late_ms;  /* for the online log: game time owed to wall time */

void kernel_pace_shared(int on)
{
    if (on && !pace_shared) pace_grid.base = 0;  /* from this field */
    pace_shared = on;
    port_clock_rebase();  /* from this step (the start's wait is no game time) */
}

static void pace(void)
{
    double due = pace_grid_next(&pace_grid, host_wall_seconds(), port_frame_period());
    if (pace_shared) kernel_pace_late_ms = (unsigned)(port_fps_behind() * 1000);
    if (due) host_wait_wall(due);  /* 0: the first field, or behind: the grid starts again from now */
}

/* Just-in-time frames (round 38, finding 052; DHD_LATENCY=0 turns them off). One VBLANK both
   ends a frame (its handler shows the picture) and starts the next (padman's new pad state), so
   pace()'s wait before it lies between the pad read and the picture: the input is a little over
   a field old (17.1 ms in a race) when presented. Here the VBLANK runs as soon as the game reaches it and the picture is
   handed on at once; the wait comes after it, and the pad is read again (pad_refresh) when it
   ends. Every picture has a due time on a fixed 59.94 Hz grid, and the presenter holds it until
   then (pacing_present), so the time between pictures stays one field. The wait ends `work`
   before the next due: the longest a frame took in the last 5 s (not counting the ones longer
   than a field), from that pad read to its picture ready to show, plus 1.5 ms. A picture that is late anyway moves the grid: the next one
   still comes a whole field after it (one long frame, not a long and a short one). Only wall
   time moves: the game sees the same fields, frames and pad frames (the frame clock stays
   virtual). Off with DHD_INTERP and in online races (pace()). The idea is RetroArch's frame
   delay and NVIDIA Reflex's just-in-time frame start. */
static int jit = -1;
static int jit_on(void)
{
    if (jit < 0) {
        const char *s = host_getenv("DHD_LATENCY");
        jit = paced && !host_getenv("DHD_INTERP") && (!s || atoi(s) != 0);
        hle_log("pacing: %s\n", jit ? "just-in-time frames" : paced ? "wait before VBLANK" : "unpaced");
    }
    return jit && !pace_shared;
}

int graphics_ordered;  /* hle.h */

/* Deferred vsync (finding 090 HO1, after finding 048): when the host offers it
   (host_service_graphics_threaded() == 2), the EE does not wait for the drawing at vsync. The host
   queues the VBLANK (gs_vblank: CSR VSINT and FIELD, the readback field) behind the drawing, hands
   the picture on after the VBLANK handlers' packets (present.c), and the game goes on with the next
   field while VU1 and the GS finish this one; at most two fields are in flight (graphics_queue.c).
   The CSR the game reads is its MMIO page, which only the EE writes. The bits the GS sets while
   drawing (SIGNAL, FINISH) are known only once it has drawn; the rest only the EE changes. So the
   host writes a provisional CSR there, exact but for those two bits (the last value with the
   VBLANK applied), and its next wait (host_service_graphics_wait: every fence, every wait here)
   puts the exact value the VBLANK left in its place, the value a full wait at vsync reads.
   Unfenced, only CSR.FIELD is read (func_0020BED0; sceGsSyncV's load, port/tools/recomp.py), which
   the provisional CSR has exact. The vsync waits as before with SetVSyncFlag (the CSR is copied to
   RAM) and until the first exact CSR. No function of the core changes its address: the state trace
   compares function-table indices the game keeps in RAM. */
static int vsync_deferred, csr_exact;

/* DHD_SLOW_FRAME=MS[,JITTER] (tests, finding 139): a slow PC on a fast one. Each game frame lasts
   at least MS milliseconds, give or take up to JITTER (uniform, a fixed random sequence), spent
   busy on the EE thread at its VBLANK, so a race runs at a chosen frame rate with uneven frames. */
static void slow_frame(void)
{
    static int init;
    static double ms, jitter, last;
    static uint32_t seed = 0x139u;
    if (!init) {
        init = 1;
        const char *s = host_getenv("DHD_SLOW_FRAME"), *comma = s ? strchr(s, ',') : NULL;
        if (s) ms = strtod(s, NULL), jitter = comma ? strtod(comma + 1, NULL) : 0;
        if (ms > 0) hle_log("slow frame: each frame at least %.2f ms, +-%.2f ms (DHD_SLOW_FRAME)\n", ms, jitter);
    }
    if (!(ms > 0)) return;
    seed = seed * 1664525u + 1013904223u;
    double want = (ms + jitter * ((double)(seed >> 8) / (1 << 23) - 1)) / 1000;
    while (host_wall_seconds() - last < want)
        ;
    last = host_wall_seconds();
}

static void vsync(void)
{
    slow_frame();
    port_clock_observe();
    int fresh = paced && jit_on();
    if (paced && !fresh) pace();
    host_pacing_handoff(fresh);
    ++kernel_vsync_count;
    /* host_pacing_wait logs this field, as jit_wait did */
    host_pacing_config(port_frame_period(),kernel_vsync_count);
    host_frame_context(kernel_vsync_count,host_seconds());
    port_scenery_observe();  /* DHD_SCENERY_TRACE, before DHD_FPS_STOP can end the run */
    port_fps_observe();  /* DHD_FPS_TRACE */
    if (vsync_deferred && csr_exact && !vsync_flag_addr)
        host_service_graphics_vblank();  /* the CSR page turns provisional */
    else {
        /* Presentation and the VBLANK handler consume completed graphics state. */
        host_service_graphics_wait();
        /* CSR acknowledgements and VBLANK must share the read-only FIELD state. */
        host_service_gs_vblank();
        GS_CSR = host_service_gs_priv_read(0x1000);
        csr_exact = 1;
    }
    if (vsync_flag_addr) {
        *(uint32_t *)(uintptr_t)vsync_flag_addr = 1;
        *(uint64_t *)(uintptr_t)vsync_csr_addr = GS_CSR;
    }
    pad_vsync();  /* padman updates the controllers once a frame */
    if (!fresh) host_pacing_input();
    host_widescreen_vsync();
    /* On the delta-time clock the mixer/SPU time is wall time at the NTSC field rate,
       independently of the frames and of the VBLANKs per frame (port_vblank_seconds): the sound
       plays at its speed whenever the game does. The original clock keeps its call per VBLANK. */
    if (!port_delta_clock()) sound_tick();
    else {
        static double audio_time;
        audio_time += port_vblank_seconds();
        while (audio_time >= 1001.0/60000.0) {
            audio_time -= 1001.0/60000.0;
            sound_tick();
        }
    }
    intc_raise(INTC_VBLANK_START);
    run_handlers(intc_handlers, intc_enabled, INTC_VBLANK_START);
    /* handlers may have submitted another packet (graphics_ordered: gs_vsync is queued after it) */
    if (!graphics_ordered) host_service_graphics_wait();
#if defined(__wasm__)
    extern int host_widescreen_flat_limit(void);
    widescreen_card_mode(host_frame_video(host_widescreen_aspect(),host_widescreen_flat_limit()));
    static HleEnv screenshot_env;
    const char *capture_prefix=hle_env(&screenshot_env,"DHD_SCREENSHOT");
    if (capture_prefix && host_getenv("DHD_RACE_SYNC")) {
        char capture_name[1024];
        race_sync_vsync(kernel_vsync_count);
        int ready=race_sync_capture(capture_prefix,capture_name,sizeof capture_name);
        host_capture_name(GADDR(capture_name),ready);
    }
#endif
    host_service_gs_vsync();  /* latch the display buffer selected by the VBLANK handler */
    intc_raise(INTC_VBLANK_END);
    run_handlers(intc_handlers, intc_enabled, INTC_VBLANK_END);
    if (fresh) {
        host_pacing_wait();
        pad_refresh();  /* the game reads the pad after this VBLANK: the state at the end of the wait */
    }
    state_trace_field(kernel_vsync_count, next_vsync);  /* DHD_STATE_TRACE: the end of the field */
}

static int deliver(void)
{
    int any = 0;
    uint64_t now = now_cycles();
    timer_poll(now);
    /* With no RPC in flight sif_poll does nothing: skip it and the two register-bank copies
       (about 1.5 KB of memmove per kernel entry, hundreds per field; finding 090 SP14). */
    if (sif_next_event() < 1e100) {
        Registers rpc_saved;
        save_registers(&rpc_saved);
        in_interrupt++;
        any |= sif_poll();
        in_interrupt--;
        load_registers(&rpc_saved);
    }
    if (now >= next_vsync) {
        uint64_t cycles = port_fps() == 60 ? VSYNC_CYCLES : (uint64_t)(BUS_HZ / port_fps());
        next_vsync += cycles;
        if (next_vsync < now)  /* fell far behind (a debugger stop): don't replay the frames */
            next_vsync = now + cycles;
        vsync();
        any = 1;
    }
    for (int i = 0; i < MAX_ALARMS; i++)
        if (alarms[i].used && now >= alarms[i].due) {
            alarms[i].used = 0;
            Registers saved;
            save_registers(&saved);
            in_interrupt++;
            alarms[i].fn(i + 1, alarms[i].ticks, alarms[i].arg);
            in_interrupt--;
            load_registers(&saved);
            any = 1;
        }
    return any;
}

void kernel_poll(void)
{
    if (in_interrupt)
        return;
    ee_frame_clock_poll();
#if defined(__wasm__)
    uint32_t registers[5]={ps2.gpr[29].w[0],ps2.gpr[31].w[0],ps2.gpr[4].w[0],ps2.gpr[5].w[0],ps2.gpr[2].w[0]};
    host_frame_registers(GADDR(registers));
#endif
    host_callbacks_drain();
    if (host_service_host_poll_dump()) kernel_dump();
    dma_poll();
    if (deliver())
        reschedule();
}

static uint64_t next_event(void);

/* Lets time run to `due`: the frame clock jumps there, host time sleeps and then spins. */
static void wait_until(uint64_t due)
{
    uint64_t now = now_cycles();
    if (ee_frame_clock_enabled()) ee_frame_clock_advance(due);
    else if (due > now) {
        unsigned ms = (unsigned)((due - now) * 1000 / (uint64_t)BUS_HZ);
        host_sleep_ms(ms);
    }
    while (now_cycles() < due)
        ;
}

/* A read of INTC_STAT. Each game frame ends in sceGsSyncV (func_00243CE8), which clears
   VBLANK_START and spins on it with interrupts enabled (func_0025AC00 also stops on the
   SetVSyncFlag word); that wait is what holds the PS2 to one frame per field (PCSX2: boot, menus,
   loads and race all run 1.000 frames per field). The EE only spins meanwhile, so while the bit is
   clear a read lets time run to the next interrupt, delivered as usual; the game's loop then
   reads again. */
static void intc_read(void)
{
    if (in_interrupt)
        return;
    if (intc_stat & 1u << INTC_VBLANK_START)
        return;
    wait_until(next_event());
#if defined(__wasm__)
    uint32_t registers[5]={ps2.gpr[29].w[0],ps2.gpr[31].w[0],ps2.gpr[4].w[0],ps2.gpr[5].w[0],ps2.gpr[2].w[0]};
    host_frame_registers(GADDR(registers));
#endif
    host_callbacks_drain();
    if (host_service_host_poll_dump()) kernel_dump();
    dma_poll();
    if (deliver())
        reschedule();
}

static uint64_t next_event(void)
{
    uint64_t t = next_vsync;
    double rpc_due = sif_next_event();
    if (rpc_due < (double)t / BUS_HZ) {
        t = (uint64_t)(rpc_due * BUS_HZ);
        /* A fractional RPC deadline must not strand the virtual idle loop one
           bus cycle before sif_poll's double-precision due time. */
        if (ee_frame_clock_enabled()) ++t;
    }
    for (int i = 0; i < MAX_ALARMS; i++)
        if (alarms[i].used && alarms[i].due < t)
            t = alarms[i].due;
    return t;
}

/* ---- the rest of the kernel ---- */
void Exit(int code) RETAIL(func_0025A300);
void Exit(int code)
{
    hle_log("Exit(%d) from %p (ra %08X)\n", code, PS2_CALLER_ADDRESS(), ps2.gpr[31].w[0]);
    host_service_host_backtrace();
    exit(code);
}

void FlushCache(int mode) RETAIL(func_0025A960);
void FlushCache(int mode) { (void)mode; kernel_poll(); }

static uint32_t heap_end;
uint32_t EndOfHeap(void) RETAIL(func_0025A6C0);
uint32_t EndOfHeap(void) { return heap_end; }

static uint64_t gs_imr = 0x7F00;
uint64_t GsGetIMR(void) RETAIL(func_0025AA00);
uint64_t GsGetIMR(void) { return gs_imr; }
uint64_t GsPutIMR(uint64_t imr) RETAIL(func_0025AA20);
uint64_t GsPutIMR(uint64_t imr)
{
    uint64_t old = gs_imr;
    gs_imr = imr;
    return old;
}

void SetVSyncFlag(uint32_t flag, uint32_t csr) RETAIL(func_0025AA50);
void SetVSyncFlag(uint32_t flag, uint32_t csr) { vsync_flag_addr = flag, vsync_csr_addr = csr; }

void SetGsCrt(int interlace, int mode, int field) RETAIL(func_0025A2E0);
void SetGsCrt(int interlace, int mode, int field)
{
    if (hle_verbose)
        hle_log("SetGsCrt(%d, %d, %d)\n", interlace, mode, field);
}

int GetMemorySize(void) RETAIL(func_0025AB40);
int GetMemorySize(void) { return PS2_RAM_SIZE; }

void InitTLB(void) RETAIL(func_0025F6E0);
void InitTLB(void) {}  /* the host maps the memory (memory.c) */

/* Unknown syscalls the SDK makes: logged, and 0. */
static int unknown(int n)
{
    hle_log("syscall 0x%X: not implemented\n", n);
    return 0;
}

int Syscall80(void) RETAIL(func_00243670);
int Syscall80(void) { return unknown(0x80); }
int Syscall6B(void) RETAIL(func_0025A9B0);
int Syscall6B(void) { return unknown(0x6B); }
int Syscall82(void) RETAIL(func_0025AB50);
int Syscall82(void) { return unknown(0x82); }
int Deci2Call(int call, void *arg) RETAIL(func_0025AB10);
int Deci2Call(int call, void *arg) { (void)call, (void)arg; return -1; }

/* ---- startup and the idle loop ---- */
void kernel_init(void)
{
    const char *frame_clock = host_getenv("DHD_FRAME_CLOCK");
    ee_frame_clock_reset(1);
    paced = !(frame_clock && !strcmp(frame_clock, "1"));
    const char *fps_wall = host_getenv("DHD_FPS_WALL");
    wall_steps = paced || (fps_wall && atoi(fps_wall) == 1);
    port_clock_wall(wall_steps);
    host_service_host_init();
    host_service_gs_reset();
    state_trace_init();  /* after gs_reset, which clears the GS struct; no-op unless DHD_STATE_TRACE */
    ps2_mmio_write = dma_mmio_write;
    ps2_intc_read = intc_read;
    intc_stat = 0;
    *(volatile uint32_t *)(uintptr_t)PS2_INTC_STAT = 0;
    int threaded = host_service_graphics_threaded();
    ps2_graphics_sync = threaded ? host_service_graphics_wait : NULL;
    /* the host offers deferred vsyncs only where it queues the picture in order (never with
       DHD_SCREENSHOT): the waits that only order graphics work go too */
    vsync_deferred = threaded == 2;
    /* screenshot runs (DHD_SCREENSHOT) keep every wait */
    graphics_ordered = ps2_graphics_sync && (vsync_deferred || host_getenv("DHD_GRAPHICS_ORDERED")) &&
                       !host_getenv("DHD_SCREENSHOT");
    ps2_vu1_regs_sync = ps2_graphics_sync;
    host_seconds();
    kernel_vsync_count = 0;
    next_vsync = VSYNC_CYCLES;
    ps2.cop0[12] = 0x10001;  /* Status: EIE, IE */
    hle_verbose = host_getenv("DHD_LOG") != NULL;
}

void kernel_run(void (*root)(void), uint32_t gp, uint32_t stack, uint32_t stack_size)
{
    idle_fiber = host_fiber_main();
    ee_float_mode();
    root_function = root;
    Thread *main = &threads[1];
    memset(main, 0, sizeof *main);
    main->used = 1;
    main->status = TS_DORMANT;
    main->stack = stack, main->stack_size = stack_size, main->gp = gp;
    main->init_priority = 0;
    start(main, 0);
    heap_end = stack + stack_size;  /* SetupHeap(_end, -1): up to the stack, which grows down to meet it */
    while (!game_exited) {
        Thread *next = pick();
        if (next) {
            run_thread(next);
            host_fiber_switch(next->fiber);
            continue;
        }
        /* every thread waits: sleep until the next interrupt */
        wait_until(next_event());
        deliver();
        int any = 0;
        for (int i = 1; i < MAX_THREADS; i++)
            any |= threads[i].used && threads[i].status != TS_DORMANT;
        if (!any) {
            hle_log("kernel: no threads left\n");
            return;
        }
    }
}

void kernel_dump(void)
{
    for (int i = 1; i < MAX_THREADS; i++) if (threads[i].used) {
        Thread *t = &threads[i];
        fprintf(stderr, "  thread %d entry=%08X state=%X priority=%d wait=%d/%d wake=%d\n",
                i, t->entry, t->status, t->priority, t->wait, t->wait_id, t->wakeup_count);
    }
    for (int i = 1; i < MAX_SEMAS; i++) if (semas[i].used)
        fprintf(stderr, "  semaphore %d count=%d max=%d\n", i, semas[i].count, semas[i].max_count);
}
