#ifndef _WIN32
#define _GNU_SOURCE /* ucontext register names, pthread names, environ */
#endif
#include "text_input.h"
/* The Windows calls of the native PS2 system (hle.h), apart from its code because the PS2
   kernel's names (CreateThread, ExitThread ...) are Windows' too. */
#include "hle.h"
#include "timer.h"
#include "state_trace.h"
#include "pace_grid.h"
#include "stick.h"
#include "controls_table.h"  /* the bindings' tables (port/render/settings, finding 157) */
#include "../gs/button_icons.h"
#include "../host/disc_check.h"
#include "../host/app_away.h"
#include "../gs/gs_mesh.h"   /* gs_mesh_setting_read: Native VU (finding 194) */
#include "../include/version.h"
#include "../runtime/sha256.h"
#include "../runtime/disc_recent.h"  /* the crash report's last disc files (finding 208) */

#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>        /* _get_osfhandle */
#include <fcntl.h>     /* _O_WRONLY, _O_TEXT (the log's pipe) */
#include <signal.h>
#else
#include "../host/backtrace_posix.h"  /* <execinfo.h>, or Android's own (API < 33) */
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <unistd.h>
#endif

#include "ps2asm.h"
#include "../include/guest.h"
#include "../host/host_os.h"
#include "../host/host_end.h"
#include "../render/include/dhd_settings.h"  /* dhd.ini (port/render/settings) */
#include "../render/include/dhd_presence.h"  /* Discord Rich Presence (port/render/presence) */

#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>  /* TARGET_OS_IPHONE: the iOS app (port/ios, finding 224) */
#endif

/* The instruction pointer and stack pointer of a CONTEXT. */
#ifdef _WIN32
#define CTX_IP(c) ((uintptr_t)(c)->Rip)
#define CTX_SP(c) ((uintptr_t)(c)->Rsp)
#else
/* POSIX crash/dump signals hand a ucontext_t: x86_64, or aarch64 (Android, finding 091; macOS,
   whose registers sit behind a pointer, with fp and lr right after x0-x28). CTX_X: x0 up. */
#if defined(__APPLE__)
#define CTX_IP(c) ((uintptr_t)(c)->uc_mcontext->__ss.__pc)
#define CTX_SP(c) ((uintptr_t)(c)->uc_mcontext->__ss.__sp)
#define CTX_X(c) ((const unsigned long long *)(c)->uc_mcontext->__ss.__x)
#elif defined(__aarch64__)
#define CTX_IP(c) ((uintptr_t)(c)->uc_mcontext.pc)
#define CTX_SP(c) ((uintptr_t)(c)->uc_mcontext.sp)
#define CTX_X(c) ((const unsigned long long *)(c)->uc_mcontext.regs)
#else
#define CTX_IP(c) ((uintptr_t)(c)->uc_mcontext.gregs[REG_RIP])
#define CTX_SP(c) ((uintptr_t)(c)->uc_mcontext.gregs[REG_RSP])
#endif
typedef ucontext_t CONTEXT;
#endif

/* Where the game thread is: its registers, the code addresses on its stack (no frame
   pointers, so likely return addresses, some stale; port/tools/host64_rva.py names them from
   dhd64.map, after subtracting the image base) and the
   recompiled code's registers. */
typedef struct {
    CONTEXT context;
    uintptr_t returns[32];  /* host code addresses (the return addresses on the stack) */
    uint32_t registers[5];  /* the game's registers: guest values */
    unsigned count;
} Snapshot;

/* No CRT calls while the game thread is suspended: it may own a CRT lock. */
static uint32_t published_registers[5];
#ifdef _WIN32
/* Whether [p, p + n) can be read (peek, snapshot): a kernel read, which never faults. */
static int host_readable(const void *p, size_t n)
{
    char sink[64];
    SIZE_T got;
    for (size_t at = 0; at < n; at += sizeof sink) {
        size_t part = n - at < sizeof sink ? n - at : sizeof sink;
        if (!ReadProcessMemory(GetCurrentProcess(), (const char *)p + at, sink, part, &got) || got != part) return 0;
    }
    return 1;
}
#else
/* Whether [p, p + n) can be read (peek, safe_vprint, snapshot): a fault guard
   around a volatile probe. The previous SIGSEGV/SIGBUS handlers stay in the
   chain for faults outside the probe (or on other threads). */
static _Thread_local sigjmp_buf probe_env;
static _Thread_local volatile int probe_armed;
static struct sigaction probe_old_segv, probe_old_bus;
static void probe_fault(int sig, siginfo_t *info, void *ctx)
{
    struct sigaction *old = sig == SIGSEGV ? &probe_old_segv : &probe_old_bus;
    (void)info;
    (void)ctx;
    if (probe_armed) siglongjmp(probe_env, 1);
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) old->sa_sigaction(sig, info, ctx);
        else {
            signal(sig, SIG_DFL);
            raise(sig);
        }
    } else if (old->sa_handler == SIG_DFL || old->sa_handler == SIG_IGN) {
        signal(sig, old->sa_handler);
        raise(sig);
    } else if (old->sa_handler) {
        old->sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}
static int host_readable(const void *p, size_t n)
{
    struct sigaction sa;
    volatile const char *q = p;
    volatile char sink;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = probe_fault;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, &probe_old_segv) < 0 || sigaction(SIGBUS, &sa, &probe_old_bus) < 0) return 0;
    probe_armed = 1;
    if (sigsetjmp(probe_env, 1)) {
        probe_armed = 0;
        sigaction(SIGSEGV, &probe_old_segv, NULL);
        sigaction(SIGBUS, &probe_old_bus, NULL);
        return 0;
    }
    for (size_t i = 0; i < n; i++) sink = q[i];
    (void)sink;
    probe_armed = 0;
    sigaction(SIGSEGV, &probe_old_segv, NULL);
    sigaction(SIGBUS, &probe_old_bus, NULL);
    return 1;
}
#endif
static void snapshot(Snapshot *out, const CONTEXT *c)
{
    out->context = *c;
    out->count = 0;
    uintptr_t lo = 0, hi = 0;
    const uintptr_t *sp = (const uintptr_t *)CTX_SP(c);
    host_code_range(&lo, &hi);
    for (unsigned i = 0; i < 2048 && out->count < 32; i++) {
        uintptr_t value;
        if (!host_readable(sp + i, sizeof value)) break;
        value = *(volatile const uintptr_t *)(sp + i);
        if (value >= lo && value < hi) out->returns[out->count++] = value;
    }
    memcpy(out->registers,published_registers,sizeof out->registers);
}

static void print_snapshot(const Snapshot *s)
{
    const CONTEXT *c = &s->context;
#ifdef _WIN32
    fprintf(stderr, "  rip %016llX rax %016llX rbx %016llX rcx %016llX rdx %016llX rsi %016llX rdi %016llX rsp %016llX\n",
            (unsigned long long)c->Rip, (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
            (unsigned long long)c->Rcx, (unsigned long long)c->Rdx, (unsigned long long)c->Rsi,
            (unsigned long long)c->Rdi, (unsigned long long)c->Rsp);
#elif defined(__aarch64__)
    const unsigned long long *x = CTX_X(c);
    fprintf(stderr, "  pc %016llX sp %016llX lr %016llX x0 %016llX x1 %016llX x2 %016llX x19 %016llX x29 %016llX\n",
            (unsigned long long)CTX_IP(c), (unsigned long long)CTX_SP(c), x[30], x[0], x[1], x[2],
            x[19], x[29]);
#else
    const greg_t *r = c->uc_mcontext.gregs;
    fprintf(stderr, "  rip %016llX rax %016llX rbx %016llX rcx %016llX rdx %016llX rsi %016llX rdi %016llX rsp %016llX\n",
            (unsigned long long)r[REG_RIP], (unsigned long long)r[REG_RAX], (unsigned long long)r[REG_RBX],
            (unsigned long long)r[REG_RCX], (unsigned long long)r[REG_RDX], (unsigned long long)r[REG_RSI],
            (unsigned long long)r[REG_RDI], (unsigned long long)r[REG_RSP]);
#endif
    fprintf(stderr, "  code addresses on the stack:");
    for (unsigned i = 0; i < s->count; i++)
        fprintf(stderr, "%s%08llX", i % 8 ? " " : "\n    ", (unsigned long long)s->returns[i]);
    fprintf(stderr, "\n  ps2: sp %08X ra %08X a0 %08X a1 %08X v0 %08X\n",
            s->registers[0], s->registers[1], s->registers[2], s->registers[3], s->registers[4]);
    fflush(stderr);
}

static void report(const CONTEXT *c)
{
    Snapshot s;
    snapshot(&s, c);
    print_snapshot(&s);
}

/* DHD_PEEK=address,bytes (host, hex): memory too, after a dump or a crash report. */
static void peek(void)
{
    const char *peek = getenv("DHD_PEEK");
    unsigned addr, len;
    if (peek && sscanf(peek, "%x,%x", &addr, &len) == 2)
        for (unsigned i = 0; i < len; i += 4) {
            if (!host_readable(GUEST(addr + i), 4)) break;
            fprintf(stderr, "%s%08X", i % 32 ? " " : i ? "\n  " : "  ", *(uint32_t *)GUEST(addr + i));
        }
    fprintf(stderr, "\n");
}

/* The disc files the game looked up last (cdvd.c note_lookup), oldest first: a crash while a
   screen loads names what it was reading (finding 208). */
static void disc_recent(void)
{
    const uint8_t *at = GUEST(PS2_DISC_RECENT);
    char line[PS2_DISC_RECENT_SIZE + 64];
    if (host_readable(at, PS2_DISC_RECENT_SIZE) && disc_recent_line(at, line, sizeof line))
        fputs(line, stderr);
}

#ifdef _WIN32
/* The log dhd.exe opens itself (dhd.log): the C library's stdout and stderr write into a pipe and
   a thread copies it to the file, so a slow write to the disk (half a second at times on a
   player's PC, with DHD_SPIKES' many lines) holds up only that thread, never the game's. The
   rest reaches the file at exit (atexit), on a crash or an abort (log_settle) and in ExitProcess
   (log_last_words, a TLS callback: the copying thread is gone by then). The pipe's reading end is
   overlapped: a synchronous one would make PeekNamedPipe wait for the thread's pending read. */
static HANDLE log_read, log_file, log_event;
static char log_chunk[1 << 16];
static volatile LONG log_chunk_at, log_chunk_end;  /* what the thread read and has not written yet */

static void log_write(void)
{
    DWORD put;
    while (log_chunk_at < log_chunk_end &&
           WriteFile(log_file, log_chunk + log_chunk_at, (DWORD)(log_chunk_end - log_chunk_at), &put, NULL) && put)
        InterlockedExchangeAdd(&log_chunk_at, (LONG)put);
}

/* Up to a chunk from the pipe into log_chunk, waiting at most wait ms; the bytes read (0: none,
   or the pipe is gone). */
static DWORD log_read_chunk(DWORD wait)
{
    OVERLAPPED o;
    DWORD got = 0;
    memset(&o, 0, sizeof o);
    o.hEvent = log_event;
    if (!ReadFile(log_read, log_chunk, sizeof log_chunk, NULL, &o)) {
        if (GetLastError() != ERROR_IO_PENDING) return 0;
        if (WaitForSingleObject(log_event, wait) != WAIT_OBJECT_0) CancelIo(log_read);
    }
    return GetOverlappedResult(log_read, &o, &got, TRUE) ? got : 0;
}

static DWORD WINAPI log_copy(void *unused)
{
    (void)unused;
    for (DWORD got; (got = log_read_chunk(INFINITE)) != 0;) {
        InterlockedExchange(&log_chunk_at, 0);
        InterlockedExchange(&log_chunk_end, (LONG)got);
        log_write();
    }
    return 0;
}

/* Waits (at most 2 s) until the pipe is empty and the thread has written what it read. */
static void log_settle(void)
{
    if (!log_read) return;
    fflush(stdout), fflush(stderr);
    for (int i = 0, quiet = 0; i < 200 && quiet < 2; i++) {
        DWORD waiting = 0;
        if (!PeekNamedPipe(log_read, NULL, 0, NULL, &waiting, NULL)) return;
        quiet = !waiting && log_chunk_at >= log_chunk_end ? quiet + 1 : 0;
        if (quiet < 2) Sleep(10);
    }
}

static void NTAPI log_last_words(PVOID module, DWORD reason, PVOID reserved)
{
    (void)module, (void)reserved;
    if (reason != DLL_PROCESS_DETACH || !log_read) return;
    log_write();  /* the chunk the thread was writing when it was stopped */
    DWORD waiting = 0, got;
    while (PeekNamedPipe(log_read, NULL, 0, NULL, &waiting, NULL) && waiting && (got = log_read_chunk(100)) != 0) {
        log_chunk_at = 0, log_chunk_end = (LONG)got;
        log_write();
    }
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:dhd_log_tls")
#pragma section(".CRT$XLD", long, read)
__declspec(allocate(".CRT$XLD")) const PIMAGE_TLS_CALLBACK dhd_log_tls = log_last_words;

static void log_atexit(void) { log_settle(); }

static void log_aborted(int sig)
{
    (void)sig;
    log_settle();
}

/* stdout and stderr, both on the file just opened, into the pipe. Kept on the file if anything
   fails. */
static void log_start(void)
{
    char name[64];
    snprintf(name, sizeof name, "\\\\.\\pipe\\dhd-log-%lu", GetCurrentProcessId());
    HANDLE read = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   1, 0, 1 << 20, 0, NULL);
    if (read == INVALID_HANDLE_VALUE) return;
    HANDLE write = CreateFileA(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE event = CreateEventA(NULL, TRUE, FALSE, NULL);
    int file = _dup(_fileno(stderr)), fd = -1;
    if (write != INVALID_HANDLE_VALUE && event && file >= 0)
        fd = _open_osfhandle((intptr_t)write, _O_WRONLY | _O_TEXT);
    if (fd < 0) {
        if (write != INVALID_HANDLE_VALUE) CloseHandle(write);
        if (event) CloseHandle(event);
        if (file >= 0) _close(file);
        CloseHandle(read);
        return;
    }
    log_file = (HANDLE)_get_osfhandle(file);
    LARGE_INTEGER end = {0};
    SetFilePointerEx(log_file, end, NULL, FILE_END);  /* appended: stdout's own handle may have written */
    log_read = read, log_event = event;
    HANDLE copier = CreateThread(NULL, 0, log_copy, NULL, 0, NULL);
    if (!copier) { log_read = NULL; CloseHandle(read), CloseHandle(event), _close(fd), _close(file); return; }
    SetThreadPriority(copier, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(copier);
    fflush(stdout), fflush(stderr);
    _dup2(fd, _fileno(stderr));
    _dup2(fd, _fileno(stdout));
    _close(fd);
    /* Direct handle writes (and a relaunch after an update, update.c start_again) use the file. */
    SetStdHandle(STD_ERROR_HANDLE, log_file);
    SetStdHandle(STD_OUTPUT_HANDLE, log_file);
    host_end_add(HOST_END_LOG, "log", log_atexit);
    signal(SIGABRT, log_aborted);
}

static LONG WINAPI crashed(EXCEPTION_POINTERS *e)
{
    EXCEPTION_RECORD *r = e->ExceptionRecord;
    fprintf(stderr, "crash: exception %08lX at %08llX", r->ExceptionCode,
            (unsigned long long)(uintptr_t)r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, " (%s %08llX)", r->ExceptionInformation[0] ? "writing" : "reading",
                (unsigned long long)r->ExceptionInformation[1]);
    fprintf(stderr, "\n");
    report(e->ContextRecord);
    disc_recent();
    peek();
    log_settle();
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
void host_log_drain(void);  /* below: the log's last lines reach the file */

/* What SIGSEGV, SIGBUS, SIGILL and SIGFPE had before crashed (host_init). */
static struct sigaction crash_before[4];
static struct sigaction *crash_before_of(int sig)
{
    return &crash_before[sig == SIGSEGV ? 0 : sig == SIGBUS ? 1 : sig == SIGILL ? 2 : 3];
}

/* Fatal signals end the process with the same words the Windows crash report has. The report is
   drained into dhd.log first (the copying thread dies with the process). On Android the crash then
   goes on to the handlers before ours, down to the system's crash dump (below); a desktop exits
   with code 3, as before. */
static void crashed(int sig, siginfo_t *info, void *ctx)
{
    /* Once only. A fault inside the report itself (an unwinder reading a broken stack) can come
       back here: guard_fault (port/host/host_posix.c) calls this handler directly, so its
       SA_RESETHAND never applies. A second entry, from any thread, skips the report. */
    static int entered;
    if (__atomic_fetch_add(&entered, 1, __ATOMIC_SEQ_CST)) {
#ifdef __ANDROID__
        sigaction(sig, crash_before_of(sig), NULL);
        if (!info || info->si_code <= 0) raise(sig);
        return;
#else
        _exit(3);
#endif
    }
    const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : "SIGFPE";
    char thread[17] = "?";
    pthread_getname_np(pthread_self(), thread, sizeof thread);  /* which of the port's threads (host_thread_name) */
    fprintf(stderr, "crash: %s at %016llX in thread %s\n", name,
            (unsigned long long)(uintptr_t)(info ? info->si_addr : NULL), thread);
    fflush(stderr);
    {
        /* The report's stack walk probes memory (host_readable): a fault there must reach the
           probe's handler. The crash's own signal is blocked while this handler runs, and a
           fault on a blocked signal ends the process at once (no report, no tombstone): seen on
           the Mi 11i with a crash near the top of the game thread's stack (finding 152). */
        sigset_t faults;
        sigemptyset(&faults);
        sigaddset(&faults, SIGSEGV);
        sigaddset(&faults, SIGBUS);
        pthread_sigmask(SIG_UNBLOCK, &faults, NULL);
    }
    report((const CONTEXT *)ctx);
    {
        void *frames[32];
        int n = backtrace(frames, 32);
        backtrace_symbols_fd(frames, n, STDERR_FILENO);
    }
    disc_recent();
    peek();
    fflush(stderr);
    host_log_drain();
#ifdef __ANDROID__
    /* The handler that was there before ours gets the crash next: put back, it sees the fault
       again when the faulting instruction runs again on return (a signal sent with kill is sent
       again instead). Each handler below does the same with the one before it, down to the
       system's crash dump (debuggerd), which writes the tombstone and ends the process, so Android
       records a native crash that the next start reads (finding 152). Calling the handler before
       ours directly instead lost the dump: guest memory's handler (port/host64) answers a fault
       that is not its own with the default action. */
    sigaction(sig, crash_before_of(sig), NULL);
    if (!info || info->si_code <= 0) raise(sig);  /* delivered once this handler returns */
    return;
#endif
    _exit(3);
}
#endif

void host_backtrace(void)
{
#ifdef _WIN32
    CONTEXT c = {0};
    c.Rip = (DWORD64)(uintptr_t)__builtin_return_address(0);
    c.Rsp = (DWORD64)(uintptr_t)__builtin_frame_address(0);
    report(&c);
#else
    void *frames[32];
    int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
#endif
}

/* DHD_DUMP=N: after N seconds, where the game thread is (then it goes on). */
#ifdef _WIN32
static HANDLE game_thread;
#else
static pthread_t game_thread;
static int game_thread_set;
#endif

/* DHD_WATCH=address[:rw],... (EE addresses, hex, up to 4): hardware data watchpoints on the
   game thread (all its fibers), on stores, or with :rw on loads too. Each hit (the first 40 per
   watchpoint) prints the word, the code address after the access and the code addresses on the
   stack, for port/tools/host64_rva.py (as RVAs). A diagnostic (finding 041: host addresses in EE memory). */
static uint32_t watch_address[4];
static atomic_int watch_hits[4];

#ifdef _WIN32
static LONG WINAPI watch_hit(EXCEPTION_POINTERS *e)
{
    CONTEXT *c = e->ContextRecord;
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 15)) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 4; i++)
        if (c->Dr6 & 1u << i && atomic_fetch_add(&watch_hits[i], 1) < 40) {
            extern uint64_t kernel_vsync_count;
            uint32_t value = 0;
            SIZE_T got;  /* a kernel read: a plain load would hit a load watchpoint again */
            ReadProcessMemory(GetCurrentProcess(), GUEST(0x08000000u + watch_address[i]), &value, 4, &got);
            extern char __ImageBase[];
            fprintf(stderr, "watch: EE %08X = %08X field %llu after eip %08llX rva %08llX (map)\n",
                    watch_address[i], value, (unsigned long long)kernel_vsync_count,
                    (unsigned long long)CTX_IP(c),
                    (unsigned long long)((uintptr_t)CTX_IP(c) - (uintptr_t)__ImageBase));
            report(c);
        }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int SDLCALL watch_arm(void *unused)
{
    CONTEXT c = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
    const char *spec = getenv("DHD_WATCH");
    if (!spec) spec = state_trace_watch_spec();  /* DHD_STATE_WATCH (state_trace.c) */
    (void)unused;
    if (SuspendThread(game_thread) == (DWORD)-1) return 0;
    GetThreadContext(game_thread, &c);
    for (int i = 0; i < 4 && spec && *spec; i++) {
        char *end;
        watch_address[i] = (uint32_t)strtoul(spec, &end, 16) & ~3u;
        int rw = !strncmp(end, ":rw", 3);
        (&c.Dr0)[i] = (DWORD_PTR)GUEST(0x08000000u + watch_address[i]);  /* the host address of the EE word */
        c.Dr7 |= 1u << (2 * i) | (rw ? 3u : 1u) << (16 + 4 * i) | 3u << (18 + 4 * i);  /* local, 4 bytes */
        fprintf(stderr, "watch: EE %08X on %s\n", watch_address[i], rw ? "loads and stores" : "stores");
        spec = strchr(end, ',');
        spec = spec ? spec + 1 : NULL;
    }
    SetThreadContext(game_thread, &c);
    ResumeThread(game_thread);
    return 0;
}
#else
/* Hardware data watchpoints need another thread's debug registers; the
   state-trace watch (DHD_STATE_WATCH, state_trace.c) is the portable one. */
static int SDLCALL watch_arm(void *unused)
{
    const char *spec = getenv("DHD_WATCH");
    if (!spec) spec = state_trace_watch_spec();
    (void)unused;
    (void)spec;
    fprintf(stderr, "watch: hardware watchpoints need Windows; use DHD_STATE_WATCH\n");
    return 0;
}
#endif
static atomic_int dump_pending;

/* Kernel details are printed by the game thread at its next safe boundary. */
#ifdef _WIN32
static int SDLCALL watchdog(void *seconds)
{
    SDL_Delay((Uint32)(uintptr_t)seconds * 1000);
    Snapshot s;
    CONTEXT c = {.ContextFlags = CONTEXT_FULL};
    if (SuspendThread(game_thread) == (DWORD)-1) return 0;
    BOOL ok = GetThreadContext(game_thread, &c);
    if (ok) snapshot(&s, &c);
    ResumeThread(game_thread);
    if (!ok) return 0;
    fprintf(stderr, "dump after %u s:\n", (unsigned)(uintptr_t)seconds);
    print_snapshot(&s);
    peek();
    atomic_store(&dump_pending, 1);
    return 0;
}
#else
/* The game thread reports itself in a SIGUSR1 handler (no cross-thread
   register read exists here); a watchdog thread only rings the bell. */
static void dump_signal(int sig, siginfo_t *info, void *ctx)
{
    Snapshot s;
    (void)sig;
    (void)info;
    snapshot(&s, (const CONTEXT *)ctx);
    fprintf(stderr, "dump:\n");
    print_snapshot(&s);
    peek();
    atomic_store(&dump_pending, 1);
}
static int SDLCALL watchdog(void *seconds)
{
    SDL_Delay((Uint32)(uintptr_t)seconds * 1000);
    if (game_thread_set) pthread_kill(game_thread, SIGUSR1);
    return 0;
}
#endif

/* The freeze log, always on: once a second a thread looks whether the game's fields go on
   (kernel_vsync_count, which each vsync publishes; loads and the flyover still count fields),
   from its first change on (the window and the GPU are set up at field 1). After 5 s without one it logs once where the game is: the menu and phase, WaitForRender's flags
   (port/hle/dma.c, hle_render_wait), the registers the game last published and, on Windows, the
   game thread's code addresses as RVAs for port/tools/host64_rva.py. A Mac or Linux game thread
   is not stopped (that needs a signal handler on it); the rest is logged. One line when the
   fields go on again. A phone's app in the background (host_app_away) is not a freeze. */
static uint32_t freeze_word(uint32_t ee)
{
    const void *p = GUEST(0x08000000u + ee);
    return host_readable(p, 4) ? *(volatile const uint32_t *)p : 0xFFFFFFFFu;
}

int host_service_host_came_back(void) { return app_away_came_back(); }   /* app_away.c */

void host_app_away(int away)
{
    app_away_set(away);
    touch_host_app_away(away);
    host_sound_away(away);
}

static int SDLCALL freeze_watch(void *unused)
{
    extern uint64_t kernel_vsync_count;
    uint64_t last = 0;
    unsigned still = 0;
    int armed = 0;
    (void)unused;
    for (;;) {
        SDL_Delay(1000);
        uint64_t field = *(volatile uint64_t *)&kernel_vsync_count;
        if (app_away_now()) {   /* a phone's app in the background waits: no freeze */
            last = field, still = 0;
            continue;
        }
        if (field != last || !armed) {
            armed |= last && field != last;
            if (still >= 5)
                fprintf(stderr, "freeze: the game goes on at field %llu after about %u s\n",
                        (unsigned long long)field, still);
            last = field, still = 0;
            continue;
        }
        if (++still != 5) continue;
        fprintf(stderr, "freeze: no field for 5 s at field %llu: menu %d phase %d, render flags %X %X %X\n",
                (unsigned long long)field, (int)freeze_word(0x29DED8), (int)freeze_word(0x43F348),
                freeze_word(0x29E1C8), freeze_word(0x29E1CC), freeze_word(0x29E1E4));
#ifdef _WIN32
        Snapshot s;
        CONTEXT c = {.ContextFlags = CONTEXT_FULL};
        BOOL ok = FALSE;
        if (game_thread && SuspendThread(game_thread) != (DWORD)-1) {
            ok = GetThreadContext(game_thread, &c);
            if (ok) snapshot(&s, &c);
            ResumeThread(game_thread);
        }
        if (ok) {
            extern char __ImageBase[];
            uintptr_t base = (uintptr_t)__ImageBase;
            print_snapshot(&s);
            uintptr_t lo = 0, hi = 0;
            host_code_range(&lo, &hi);
            fprintf(stderr, "  image base %016llX: ", (unsigned long long)base);
            if (c.Rip >= lo && c.Rip < hi) fprintf(stderr, "rip rva 0x%llx, ", (unsigned long long)(c.Rip - base));
            else fprintf(stderr, "rip outside the exe, ");
            fprintf(stderr, "on the stack");
            for (unsigned i = 0; i < s.count; i++)
                fprintf(stderr, " rva 0x%llx", (unsigned long long)(s.returns[i] - base));
            fprintf(stderr, "\n");
        } else
#endif
            fprintf(stderr, "  ps2: sp %08X ra %08X a0 %08X a1 %08X v0 %08X\n", published_registers[0],
                    published_registers[1], published_registers[2], published_registers[3],
                    published_registers[4]);
        fflush(stderr);
    }
    return 0;
}

/* The named threads and their CPU times: port/host/host_threads.c (finding 217). */

double host_process_cpu(void)
{
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return (((uint64_t)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
            ((uint64_t)user.dwHighDateTime << 32 | user.dwLowDateTime)) * 1e-7;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) return 0;
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
#endif
}

void host_init(void)
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashed);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const char *dump = getenv("DHD_DUMP");
    host_thread_name("game");
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);  /* the watches, DHD_DUMP and the freeze log */
    if (getenv("DHD_WATCH") || state_trace_watch_spec())
        AddVectoredExceptionHandler(1, watch_hit);
#else
    struct sigaction sa, dump_sa;
    const char *dump = getenv("DHD_DUMP");
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crashed;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, crash_before_of(SIGSEGV));
    sigaction(SIGBUS, &sa, crash_before_of(SIGBUS));
    sigaction(SIGILL, &sa, crash_before_of(SIGILL));
    sigaction(SIGFPE, &sa, crash_before_of(SIGFPE));
    game_thread = pthread_self();
    game_thread_set = 1;
    host_thread_name("game");
    /* Snapshot helpers read the executable's code range; warm it outside any handler. */
    {
        uintptr_t lo, hi;
        host_code_range(&lo, &hi);
    }
    if (dump) {
        memset(&dump_sa, 0, sizeof dump_sa);
        dump_sa.sa_sigaction = dump_signal;
        sigemptyset(&dump_sa.sa_mask);
        dump_sa.sa_flags = SA_SIGINFO;
        sigaction(SIGUSR1, &dump_sa, NULL);
    }
#endif
    if (getenv("DHD_WATCH") || state_trace_watch_spec())
        SDL_WaitThread(SDL_CreateThread(watch_arm, "watch", NULL), NULL);
    if (dump) SDL_DetachThread(SDL_CreateThread(watchdog, "watchdog", (void *)(uintptr_t)atoi(dump)));
    SDL_DetachThread(SDL_CreateThread(freeze_watch, "freeze watch", NULL));
}

#ifndef _WIN32
/* The log on Linux, macOS and Android: stdout and stderr write into a pipe and a thread copies its
   lines to dhd.log (and on Android to logcat, tag DownhillRevived), so a slow write to the disk
   holds up only that thread, as on Windows. host_log_drain: the lines still in the pipe reach the
   file before the process ends (host_end, exit, host_exit), at most half a second. */
static int log_pipe[2] = {-1, -1};
static FILE *log_file;
static atomic_int log_busy;  /* the thread is between a read and its last line written */

static void log_line(char *line)
{
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "DownhillRevived", line);
#endif
    fputs(line, log_file);
    fputc('\n', log_file);
    fflush(log_file);
}

/* Only async-signal-safe calls besides fflush: host_exit also runs it after a fault. */
void host_log_drain(void)
{
    if (log_pipe[0] < 0) return;
    fflush(stdout);
    for (int i = 0; i < 250; i++) {
        int pending = 0;
        if (ioctl(log_pipe[0], FIONREAD, &pending) != 0) return;
        if (!pending && !atomic_load(&log_busy)) return;
        usleep(2000);
    }
}

static void *log_copy(void *unused)
{
    (void)unused;
    char buf[2048];
    size_t have = 0;
    for (;;) {
        /* busy goes up before the read empties the pipe, so host_log_drain never sees neither */
        struct pollfd p = {log_pipe[0], POLLIN, 0};
        atomic_store(&log_busy, 0);
        if (poll(&p, 1, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        atomic_store(&log_busy, 1);
        ssize_t n = read(log_pipe[0], buf + have, sizeof buf - 1 - have);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        have += (size_t)n;
        char *start = buf, *nl;
        while ((nl = memchr(start, '\n', (size_t)(buf + have - start)))) {
            *nl = 0;
            log_line(start);
            start = nl + 1;
        }
        have -= (size_t)(start - buf);
        memmove(buf, start, have);
        if (have == sizeof buf - 1) { buf[have] = 0; log_line(buf); have = 0; }  /* a very long line */
    }
    return NULL;
}

static void log_start(const char *path, const char *mode)
{
    if (!(log_file = host_os_fopen(path, mode))) return;
    if (pipe(log_pipe) != 0) { log_pipe[0] = -1; return; }
    fflush(stdout), fflush(stderr);
    dup2(log_pipe[1], STDOUT_FILENO);
    dup2(log_pipe[1], STDERR_FILENO);
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_t thread;
    if (pthread_create(&thread, NULL, log_copy, NULL) == 0) pthread_detach(thread);
    host_end_add(HOST_END_LOG, "log", host_log_drain);
}
#endif

/* The log's first line: the release and the build, "Downhill Revived 0.6.4 (dhd-port 0.6.4
   1a2b3c4d5e6f7a8b)", the same build name the online lobby shows (online.c build_version: the
   start of the program file's SHA-256), so a player's log says which game wrote it. The second:
   the system (OS, CPU, threads, RAM), so a bug report needs nobody to look them up. */
static void log_version(void)
{
    char path[HOST_PATH_MAX], build[17] = "unknown";
    static uint8_t buf[1 << 16];
    uint8_t digest[32];
    Sha256 hash;
    int64_t n;
    host_exe_path(path, sizeof path);
    HostFile f = host_file_open(path, HOST_FILE_READ);
    if (f != HOST_FILE_NONE) {
        sha256_init(&hash);
        while ((n = host_file_read(f, buf, sizeof buf)) > 0) sha256_update(&hash, buf, (size_t)n);
        host_file_close(f);
        sha256_final(&hash, digest);
        for (int i = 0; i < 8; i++) snprintf(build + 2 * i, 3, "%02x", digest[i]);
    }
    fprintf(stderr, "Downhill Revived %s (dhd-port %s %s)\n", DHD_VERSION, DHD_VERSION, build);
    char system[320];
    host_os_system(system, sizeof system);
    fprintf(stderr, "system: %s, %d threads, %.1f GB RAM\n", system, SDL_GetNumLogicalCPUCores(),
            SDL_GetSystemRAM() / 1024.0);
}

/* The run before keeps its log (finding 152): an existing dhd.log becomes dhd-previous.log
   (replacing an older one) just before the game opens a new one, so a crash report survives the
   next start, which is what a player does first after a crash. Only the dhd.log the game opens
   itself: a terminal, a console window and the tools' own files and pipes stay as they are.
   The mode to open the new dhd.log with: "w", or "a" when an existing dhd.log could not be moved
   (dhd-previous.log held open by a viewer on Windows, say), so the run before is never erased. */
static const char *log_keep_previous(const char *path)
{
    char previous[HOST_PATH_MAX];
    size_t n = strlen(path);
    if (n < 4 || n + 10 > sizeof previous) return "a";
    snprintf(previous, sizeof previous, "%.*s-previous.log", (int)(n - 4), path);  /* dhd.log -> dhd-previous.log */
#ifdef _WIN32
    WCHAR from[HOST_PATH_MAX], to[HOST_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, from, HOST_PATH_MAX) ||
        !MultiByteToWideChar(CP_UTF8, 0, previous, -1, to, HOST_PATH_MAX))
        return "a";
    if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING)) return "w";
    return GetLastError() == ERROR_FILE_NOT_FOUND ? "w" : "a";
#else
    if (rename(path, previous) == 0) return "w";
    return errno == ENOENT ? "w" : "a";
#endif
}

#ifdef _WIN32
static void console_open(const char *path)
{
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD type = err && err != INVALID_HANDLE_VALUE ? GetFileType(err) : FILE_TYPE_UNKNOWN;
    if (type != FILE_TYPE_UNKNOWN) return;
    int opened = 0;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stderr);
        freopen("CONOUT$", "w", stdout);
    } else {
        WCHAR wide[HOST_PATH_MAX];  /* the UTF-8 path as Windows' own */
        if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, HOST_PATH_MAX)) return;
        const char *mode = log_keep_previous(path);
        if (!_wfreopen(wide, mode[0] == 'a' ? L"a" : L"w", stderr)) return;
        _wfreopen(wide, L"a", stdout);
        opened = 1;
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);  /* send log output to the copier promptly, even on forced termination */
    SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
    SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    if (opened) log_start();
}
#else
/* A terminal, or the tools' pipe or file, keeps the log; else dhd.log at path. */
static void console_open(const char *path)
{
    setvbuf(stderr, NULL, _IONBF, 0);
    struct stat st;
    if (isatty(STDERR_FILENO) || (fstat(STDERR_FILENO, &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISREG(st.st_mode))))
        return;
    log_start(path, log_keep_previous(path));
}
#endif

/* The log (stderr, stdout) goes where the starter put it: the tools' files and pipes as before;
   started from a console window or a terminal, there; else (the launcher, Explorer, Finder, a
   desktop's menu, Android) a new dhd.log in DHD_DATA_DIR (the launcher's folder for the player's
   files, port/launcher game.rs), else beside the game (on Android the app's files folder, which
   stands for it); the one before becomes dhd-previous.log (log_keep_previous). dhd.exe is a Windows program (/subsystem:windows), so a double-click opens no
   console window. Before anything prints; once (android_main.c calls it before the game does). */
void host_console_init(void)
{
    static int done;
    if (done++) return;
    char path[HOST_PATH_MAX];
    const char *data = getenv("DHD_DATA_DIR");
    unsigned n = data ? 0 : host_exe_path(path, sizeof path);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    snprintf(path + n, sizeof path - n, data ? "%s/dhd.log" : "%sdhd.log", data ? data : "");
    console_open(path);
    log_version();
}

/* ---- controllers: SDL's (gamepad.c) and the keyboard ---- */

/* The DualShock 2's motors (pad.c, scePadSetActDirect): the small one on or off, the large one
   0-255. A pad gets them as its high- and low-frequency motors (every pad with motors, gamepad.c),
   unless DHD_VIBRATION=0 (dhd.ini "vibration", the PC settings page). Output only: nothing comes
   back into the game. */
void host_pad_vibrate(int port, int light, int heavy)
{
    static int last[4] = {-1, -1, -1, -1};
    const char *setting = getenv("DHD_VIBRATION");
    if (setting && !atoi(setting)) light = heavy = 0;
    int key = (light ? 256 : 0) | (heavy & 255);
    if (port < 0 || port > 3 || key == last[port]) return;
    last[port] = key;
    if (gamepad_start()) gamepad_vibrate(port, light, heavy);
}

/* ---- keyboard and controller bindings (finding 157) ----
   The layouts the game plays with: DHD_KEYBOARD, DHD_PAD1 and DHD_PAD2 (dhd.ini keyboard, pad1,
   pad2) as dhd_settings reads them (port/render/settings/src/controls.rs: unset, the defaults),
   read again whenever one of them changes (PC SETTINGS > Controls sets them as it saves), and
   logged then. The CONTROLS page's services below make every change through dhd_settings too. */

static ControlsLayout controls[CONTROL_DEVICES];
typedef char controls_layout_size[sizeof(ControlsLayout) == sizeof(DhdControlsLayout) && sizeof(ControlsLayout) == 52 ? 1 : -1];
static char controls_seen[CONTROL_DEVICES][512];
static int controls_ready;

static void controls_problem(const char *text) { hle_log("controls: %s\n", text); }

static const char *controls_value(int device)
{
    const char *v = getenv(control_devices[device].env);
    return v ? v : "";
}

static void controls_follow(void)
{
    int changed = !controls_ready;
    for (int d = 0; d < CONTROL_DEVICES; d++) changed |= strncmp(controls_value(d), controls_seen[d], sizeof controls_seen[d]) != 0;
    if (!changed) return;
    for (int d = 0; d < CONTROL_DEVICES; d++) snprintf(controls_seen[d], sizeof controls_seen[d], "%s", controls_value(d));
    dhd_controls_layouts(controls_seen[0], controls_seen[1], controls_seen[2], (DhdControlsLayout *)controls, controls_problem);
    hle_log("controls: keyboard %s, controller 1 %s, controller 2 %s\n", *controls_seen[0] ? controls_seen[0] : "(defaults)",
            *controls_seen[1] ? controls_seen[1] : "(defaults)", *controls_seen[2] ? controls_seen[2] : "(defaults)");
    controls_ready = 1;
}

/* A device's layout (CONTROL_KEYBOARD, CONTROL_PAD1, CONTROL_PAD2). */
static const ControlsLayout *controls_layout(int device)
{
    controls_follow();
    return &controls[device];
}

/* Player `port`'s controller layout: players 1 and 2 have their own, 3 and 4 the default. */
static const ControlsLayout *pad_layout(int port)
{
    return port < 2 ? controls_layout(CONTROL_PAD1 + port) : &control_defaults[CONTROL_PAD1];
}

/* The game's controls are in riding mode (D_0029E290, func_00220550): keyboard.c moves the left
   stick with the d-pad's keys then, and the d-pad in menus. */
static int riding_controls(void) { return *(volatile int32_t *)GUEST(0x0829E290) != 0; }

/* The keyboard (its layout: dhd.ini keyboard, keyboard.c), only while the game's window has the
   keyboard (keyboard_focused, fed by the SDL thread: sdl_host.c), and not while Alt, Ctrl or a
   Windows key is held, so Alt+Tab, Alt+Enter, Ctrl+Esc and the like stay the system's. A key let
   go elsewhere is let go here too (keyboard_focus clears the keys when the window loses the
   keyboard). Keys are read by their place on a US keyboard (keyboard.c), and
   text_input_key_allowed takes the character the key types. */
static void keyboard_merge(HostPad *out)
{
    uint8_t down[256] = {0}, keys[CONTROL_TARGETS * CONTROL_SLOTS + 1];
    const ControlsLayout *layout = controls_layout(CONTROL_KEYBOARD);
    if (keyboard_focused() && !keyboard_is_down(VKEY_MENU) && !keyboard_is_down(VKEY_CONTROL) &&
        !keyboard_is_down(VKEY_LWIN) && !keyboard_is_down(VKEY_RWIN)) {
        int count = keyboard_keys(layout, keys);
        for (int i = 0; i < count; i++) {
            unsigned place = keys[i];
            if (text_input_key_allowed(keyboard_typed_of(place)) && keyboard_position_down(place)) down[keys[i]] = 1;
        }
    }
    keyboard_pad(layout, down, riding_controls(), host_seconds(), out);
    /* typing a name or a room code (text_input.c): the arrows still move the field's pad picker,
       whatever keys the d-pad has */
    static const struct { uint8_t vk; uint16_t bit; uint8_t pressure; } arrows[] = {
        {0x27, PAD_RIGHT, 0}, {0x25, PAD_LEFT, 1}, {0x26, PAD_UP, 2}, {0x28, PAD_DOWN, 3}};
    if (keyboard_focused() && text_input_active())
        for (unsigned i = 0; i < sizeof arrows / sizeof *arrows; i++)
            if (keyboard_position_down(arrows[i].vk) && text_input_key_allowed(arrows[i].vk))
                out->buttons |= arrows[i].bit, out->pressure[arrows[i].pressure] = 255;
}

/* Player `port`'s controller (SDL, gamepad.c) through the player's layout, and for player 1 the
   keyboard as well, merged. */
void host_pad_device(int port, HostPad *out)
{
    GamepadRaw raw;
    memset(out, 0, sizeof *out);
    out->right_x = out->right_y = out->left_x = out->left_y = 128;
    /* D_004B25A0 + 0x288: local human count, set by the player-count screen and
       kept through split-screen setup/racing. Online races use one local human. */
    gamepad_set_multiplayer(*(volatile int32_t *)GUEST(0x084B2828) > 1);
    int connected = gamepad_start() && gamepad_raw(port, &raw);
    /* the sticks' dead zone (stick.h), followed here on the game thread, where PC SETTINGS sets it */
    if (connected) gamepad_apply(pad_layout(port), &raw, stick_dead_zone_of(getenv("DHD_STICK_DEAD_ZONE")), out);
    if (port == 0) {
        /* the button icons (dhd.ini prompts, PC SETTINGS > Button Icons): auto follows player 1's pad,
           and they follow its layout while a pad is there or icons are chosen */
        const char *prompts = getenv("DHD_PROMPTS");
        int named = connected && touch_host_pad_prompts();  /* on a touch screen: the device in use (finding 227) */
        int kind = named ? gamepad_kind(0) : PAD_KIND_NONE;
        int follow = named || (prompts && SDL_strcasecmp(prompts, "auto"));
        button_icons_choose(prompts, kind == PAD_KIND_XBOX ? BUTTON_ICONS_XBOX : kind == PAD_KIND_NINTENDO ? BUTTON_ICONS_NINTENDO
                                                                                                           : BUTTON_ICONS_GAME,
                            follow ? controls_layout(CONTROL_PAD1) : NULL);
        keyboard_merge(out);
        touch_host_merge(out);  /* the touch controls (touch_host.c, finding 141) */
    }
    out->connected = (uint8_t)(connected || port == 0);  /* player 1 always has the keyboard */
}

/* The startup movies (func_001EBFD8: the logos, then the attract movie) play unless dhd.ini's
   skip_movies (DHD_SKIP_MOVIES, PC SETTINGS > Skip Movies) is nonzero; movies play natively
   (port/hle/movie.c, finding 055). The race, gate and bench tools set DHD_SKIP_MOVIES=1, so their
   runs stay as they were. OPTIONS > VIDEO CLIPS always plays the clip the player chose. */
int host_service_host_skip_movies(void)
{
    static int logged;
    const char *skip = getenv("DHD_SKIP_MOVIES");
    if (!skip || !atoi(skip)) return 0;
    if (!logged++) hle_log("DHD_SKIP_MOVIES: bypassing startup movie sequence\n");
    return 1;
}

/* PC settings: dhd.ini (the file DHD_SETTINGS names, else beside dhd.exe), read and written by the
   Rust crate dhd_settings (port/render/settings, finding 101), which holds every key, the DHD_*
   variable it stands for, the player's defaults and the choices. At the start each setting becomes
   its variable, unless the environment has that already: the environment overrides the file.
   Options are host-side only; none reaches game state. */

/* dhd.ini's path: DHD_SETTINGS, else beside dhd.exe. */
static void settings_path(char *path, size_t size)
{
    const char *named = getenv("DHD_SETTINGS");
    if (named) { snprintf(path, size, "%s", named); return; }
    unsigned n = host_exe_path(path, (unsigned)size);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    snprintf(path + n, size - n, "dhd.ini");
}

static int iso_from_environment;  /* DHD_ISO was set before dhd.ini was read: it overrides the file */

/* The PC settings the environment sets (so dhd.ini can't), for the settings page to show them
   locked: DHD_ENV_OVERRIDES, which the load writes. */
int host_setting_locked(const char *name)
{
    char key[64];
    const char *list = getenv("DHD_ENV_OVERRIDES");
    snprintf(key, sizeof key, ",%s,", name);
    return list && strstr(list, key) != NULL;
}

static const char *settings_get(const char *name) { return getenv(name); }
static void settings_unknown(const char *key) { hle_log("settings: unknown key %s\n", key); }

/* The settings into the log as one line, "settings: FPS=240 SCALE=8 ...", without paths (they
   may hold the player's name); those the environment set (over dhd.ini) after "| from the environment:". */
static char settings_seen[1024];
static void settings_note(const char *name, const char *value)
{
    size_t n = strlen(settings_seen);
    int path = strchr(value, '\\') || (strchr(value, '/') && !strstr(value, "://"));
    snprintf(settings_seen + n, sizeof settings_seen - n, " %s=%s", name + (strncmp(name, "DHD_", 4) ? 0 : 4),
             path ? "(a path)" : value);
}

static void settings_set(const char *name, const char *value)
{
    if (strcmp(name, "DHD_ENV_OVERRIDES")) settings_note(name, value);
    host_putenv_s(name, value);
}

void host_settings_load(void)
{
    char path[HOST_PATH_MAX], name[64];
    int marked = 0;
    iso_from_environment = getenv("DHD_ISO") != NULL;
    settings_path(path, sizeof path);
    dhd_settings_load(path, settings_get, settings_set, settings_unknown);
    const char *list = getenv("DHD_ENV_OVERRIDES");
    for (const char *at = list ? list : ""; *at;) {
        size_t n = strcspn(at, ",");
        snprintf(name, sizeof name, "%.*s", (int)n, at);
        const char *value = n ? getenv(name) : NULL;
        if (value) {
            if (!marked++) strncat(settings_seen, " | from the environment:", sizeof settings_seen - strlen(settings_seen) - 1);
            settings_note(name, value);
        }
        at += n + (at[n] == ',');
    }
    hle_log("settings:%s\n", *settings_seen ? settings_seen : " (none)");
    gs_mesh_setting_read();   /* Native VU for the VU thread (vu1_mesh.inc), before its first run */
    /* The install id for the online hello (finding 161; port/net/net.c): made once and kept in
       dhd.ini. Set straight into the environment, never through settings_set: dhd.log, which
       players post, must not show it (whoever copied it could get its player banned). */
    if (!getenv("DHD_NET_INSTALL_ID")) {
        char id[40];
        if (dhd_settings_install_id(path, id, sizeof id)) host_putenv_s("DHD_NET_INSTALL_ID", id);
    }
}

/* One dhd.ini line (the others kept). Nonzero when saved. */
int host_setting_save(const char *key, const char *value)
{
    char path[HOST_PATH_MAX];
    settings_path(path, sizeof path);
    return dhd_settings_save(path, key, value);
}

/* Read afresh after each game-data load; these values never become DHD_* overrides. */
int host_game_options_read(char *value, size_t size)
{
    char path[HOST_PATH_MAX];
    settings_path(path, sizeof path);
    return dhd_settings_read(path, "game_options", value, size);
}

/* ---- the disc image (first start) ---- */


/* The game's other releases (finding 100), from the table the launcher reads too. */
#define OTHER_RELEASE(boot, what) {boot, what},
#define OTHER_RELEASES_MORE(url)
static const struct { const char *boot, *what; } other_releases[] = {
#include "other_releases.inc"
};
#undef OTHER_RELEASE
#undef OTHER_RELEASES_MORE
#define OTHER_RELEASE(boot, what)
#define OTHER_RELEASES_MORE(url) url
static const char other_releases_more[] =
#include "other_releases.inc"
    ;
#undef OTHER_RELEASE
#undef OTHER_RELEASES_MORE

/* A boot file's letters and digits, upper case: SCPS_150.62 and SCPS-15062 alike. */
static void serial_key(const char *s, char *out, size_t size)
{
    size_t n = 0;
    for (; *s && n + 1 < size; s++)
        if (SDL_isalnum((unsigned char)*s)) out[n++] = (char)SDL_toupper((unsigned char)*s);
    out[n] = 0;
}

/* What the release booting boot is, when it is one of the game's other releases; else NULL. */
static const char *other_release(const char *boot)
{
    char key[32], known[32];
    serial_key(boot, key, sizeof key);
    for (unsigned i = 0; *key && i < sizeof other_releases / sizeof *other_releases; i++) {
        serial_key(other_releases[i].boot, known, sizeof known);
        if (!strcmp(key, known)) return other_releases[i].what;
    }
    return NULL;
}

/* Whether path is Downhill Domination (USA): disc_check_image (port/host/disc_check.c, finding 227:
   the ISO 9660 root holds a SYSTEM.CNF whose boot line names SCUS_971.77, and the file holds the
   whole volume it declares), then SCUS_971.77 is the retail one (game_data_read: the game's data
   comes from it). NULL if it is, else why not. disc_shallow (a phone's launcher, import and start,
   before the core exists) leaves out game_data_read, which runs in the core; host_disc_ready
   checks it later. */
static int disc_shallow;
static int disc_read(void *file, uint64_t offset, void *buffer, size_t n)
{
    HostFile f = *(HostFile *)file;
    return host_file_seek(f, offset) == 0 && host_file_read(f, buffer, n) == (int64_t)n;
}
static const char *disc_problem(const char *path, char *why, size_t size)
{
    HostFile f = host_file_open(path, HOST_FILE_READ);
    if (f == HOST_FILE_NONE) return "The file can't be opened.";
    char id[32];
    uint64_t volume = 0, length = host_file_size(f);
    int kind = disc_check_image(disc_read, &f, length, id, sizeof id, &volume);
    host_file_close(f);
    switch (kind) {
    case DISC_NOT_ISO: return "It isn't a disc image (no ISO 9660 volume in it).";
    case DISC_NO_CNF: return "It has no SYSTEM.CNF, so it isn't a PlayStation 2 game disc.";
    case DISC_CUT:
        snprintf(why, size, "The disc image is incomplete: it has %.1f MB of the %.1f MB its volume declares (a download or "
                 "copy cut short). Make or copy it again.", length / 1048576.0, volume / 1048576.0);
        return why;
    case DISC_OTHER: {
        const char *release = other_release(id);
        if (release)
            snprintf(why, size, "This is %s. Downhill Revived needs the USA disc (SCUS-97177) for now. More: %s", release,
                     other_releases_more);
        else
            snprintf(why, size, "It is another PlayStation 2 disc%s%s%s, not Downhill Domination (USA), SCUS-97177.",
                     *id ? " (" : "", id, *id ? ")" : "");
        return why;
    }
    default:
        return disc_shallow ? NULL : game_data_read(path, NULL);  /* the game's program and data, unchanged (game_data.c) */
    }
}

/* At start: the disc image dhd.ini names (iso), if it is there and is Downhill Domination (USA).
   The launcher (port/launcher, disc.rs) chooses it and checks it the same way on every desktop;
   Android's start picks it (port/host/android_main.c). DHD_ISO in the environment overrides the
   file and is never questioned (the tools set it). Without a usable one: a message (in the log;
   DHD_HEADLESS shows no box), then the end. Returns 0 to go on, 1 to end. */
int host_disc_ready(void)
{
    static const char title[] = "Downhill Revived";
    static char why[512], message[1536];
    if (iso_from_environment) return 0;
    const char *iso = getenv("DHD_ISO");
    const char *problem = iso ? disc_problem(iso, why, sizeof why) : NULL;
    if (iso && !problem) return 0;
#ifdef __ANDROID__
    /* The copy android_main.c made passed its first look but not the game's data: say so and drop
       the copy, so the next start asks for the disc again. */
    static const char choose[] = "Start the game again to choose your Downhill Domination (USA) disc image (SCUS-97177).";
    if (iso) remove(iso);
#else
    static const char choose[] = "Start the game from its launcher (DownhillRevived) and choose your Downhill "
                                 "Domination (USA) disc image (SCUS-97177) there.";
#endif
    if (!iso)
        snprintf(message, sizeof message, "No disc image is chosen.\n\nThe game plays from your own copy of the "
                 "disc, as an .iso file (SCUS-97177). %s", choose);
    else if (!host_path_exists(iso))
        snprintf(message, sizeof message, "The disc image chosen before is gone:\n%s\n\n%s", iso, choose);
    else
        snprintf(message, sizeof message, "The disc image chosen before can't be used:\n%s\n%s\n\n%s", iso, problem,
                 choose);
    host_error_box(title, message);
    return 1;
}

/* Widescreen: port/hle/widescreen.c. */

/* Guest-address-only module boundary; native device code stays above. */
#include "../host/host_api.h"
#include "../include/guest.h"
void host_service_host_init(void) { host_init(); }
void host_service_host_console_init(void) { host_console_init(); }
int host_service_host_disc_ready(void) { return host_disc_ready(); }
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
/* The phones' launchers and starts (port/host/android_main.c, android_launcher.c; iOS's
   port/ios/app/launcher.m, ios_main.m; finding 227) check the player's disc image and its copy:
   NULL when it is Downhill Domination (USA), else why not. */
const char *host_disc_check(const char *path)
{
    static char why[512];
    disc_shallow = 1;
    const char *problem = disc_problem(path, why, sizeof why);
    disc_shallow = 0;
    return problem;
}
#endif
int host_service_host_poll_dump(void) { return atomic_exchange(&dump_pending, 0) != 0; }
/* ---- the CONTROLS page's services (port/hle/controls_settings.c, finding 157) ---- */

/* A device's layout as the host plays with it into out (guest, 52 bytes). */
int host_service_controls_layout(int device, uint32_t out)
{
    if (device < 0 || device >= CONTROL_DEVICES) return 0;
    memcpy(GUEST(out), controls_layout(device), sizeof(ControlsLayout));
    return 1;
}

/* Player 1's or 2's controller: its PAD_KIND_* plus 1, 0 none connected. */
static int controls_pad_connected(int player)
{
    GamepadRaw raw;
    if (player < 1 || player > 2 || !gamepad_start() || !gamepad_raw(player - 1, &raw)) return 0;
    return gamepad_kind(player - 1) + 1;
}

/* The same for the pages' button names (SETTINGS' "Tabs" line, CONTROLS): on a touch screen a
   controller counts once it has sent input since the last touch (touch_host_pad_prompts), so a
   phone shows touch words until a real pad is used (finding 227). */
int host_service_controls_pad(int player)
{
    return touch_host_pad_prompts() ? controls_pad_connected(player) : 0;
}

/* A change by dhd_settings' rules (dhd_controls_edit): saved to dhd.ini and played with at once
   (the device's variable). The page's words into message (guest, size bytes). */
int host_service_controls_change(int device, int edit, int target, int slot, int input, uint32_t message, int size)
{
    char value[512], *said = size > 0 ? (char *)GUEST(message) : NULL;
    if (said) *said = 0;
    if (device < 0 || device >= CONTROL_DEVICES) return CONTROL_UNCHANGED;
    if (host_setting_locked(control_devices[device].env)) {
        if (said) snprintf(said, (size_t)size, "Set by %s outside the game", control_devices[device].env);
        return CONTROL_REFUSED;
    }
    controls_follow();
    int kind = device == CONTROL_KEYBOARD ? PAD_KIND_NONE : gamepad_start() ? gamepad_kind(device - 1) : PAD_KIND_NONE;
    int done = dhd_controls_edit(controls_seen[0], controls_seen[1], controls_seen[2], device, edit, target, slot, input, kind,
                                 value, sizeof value, said, size > 0 ? (size_t)size : 0);
    if (done == CONTROL_CHANGED) {
        if (!host_setting_save(control_devices[device].key, value)) hle_log("controls: dhd.ini not saved\n");
        host_putenv_s(control_devices[device].env, value);
    }
    return done;
}

/* What is held for a wait: the bindable keys and Esc, and each input of a controller (a trigger
   past half way) as a bit of its place through `layout`'s shoulder swaps (the layout the wait
   binds for, so the input it gets reads back as the button pressed; finding 189); one player's
   controller (1 or 2), or every one (0). */
static int controls_key_held(int vk) { return keyboard_focused() && keyboard_position_down((unsigned)vk); }

static uint32_t pad_held(int player, const ControlsLayout *layout)
{
    uint32_t held = 0;
    for (int p = player ? player - 1 : 0; p < (player ? player : 4); p++) {
        GamepadRaw raw;
        if (!gamepad_start() || !gamepad_raw(p, &raw)) continue;
        uint32_t inputs = raw.buttons;
        for (int i = 0; i < CONTROL_PAD_INPUTS; i++)
            if (control_pad_inputs[i].trigger && raw.trigger[control_pad_inputs[i].trigger - 1] > 128) inputs |= 1u << control_pad_inputs[i].id;
        for (int id = 1; id < 32; id++)
            if (inputs & 1u << id) held |= 1u << gamepad_place(layout, id);
    }
    return held;
}

static int system_keys_held(void)
{
    return keyboard_is_down(VKEY_MENU) || keyboard_is_down(VKEY_CONTROL) || keyboard_is_down(VKEY_LWIN) || keyboard_is_down(VKEY_RWIN);
}

/* The CONTROLS page waits for a key or a button for device: command 1 begins (what is held now
   doesn't count until it is let go and pressed again); 0 asks: the key's vk or the controller
   input's id pressed since (that player's controller, else any), -2 Esc (give up), -1 nothing yet;
   2: 1 while any key or controller input is still held. */
int host_service_controls_capture(int device, int command)
{
    static uint8_t keys_before[256];
    static uint32_t pads_before;
    int player = device == CONTROL_PAD1 ? 1 : device == CONTROL_PAD2 ? 2 : 0;
    const ControlsLayout *layout = player ? controls_layout(device) : &control_defaults[CONTROL_PAD1];
    if (player && !controls_pad_connected(player)) player = 0;  /* that player's pad isn't there: any */
    if (command == 2) {
        if (pad_held(0, layout) || controls_key_held(0x1B)) return 1;
        for (int i = 0; i < CONTROL_KEYS; i++)
            if (controls_key_held(control_keys[i].vk)) return 1;
        return 0;
    }
    if (command == 1) {
        memset(keys_before, 0, sizeof keys_before);
        keys_before[0x1B] = (uint8_t)controls_key_held(0x1B);
        for (int i = 0; i < CONTROL_KEYS; i++) keys_before[control_keys[i].vk] = (uint8_t)controls_key_held(control_keys[i].vk);
        pads_before = pad_held(0, layout);
        return 0;
    }
    /* a key held at the beginning counts once it was let go */
    int esc = controls_key_held(0x1B);
    if (!esc) keys_before[0x1B] = 0;
    else if (!keys_before[0x1B]) return -2;
    int pressed = 0;
    for (int i = 0; i < CONTROL_KEYS; i++) {
        int vk = control_keys[i].vk, down = controls_key_held(vk);
        if (!down) keys_before[vk] = 0;
        else if (!keys_before[vk] && !pressed) pressed = vk;
    }
    uint32_t held = pad_held(0, layout), mine = pad_held(player, layout);
    pads_before &= held;
    uint32_t fresh = mine & ~pads_before;
    if (device == CONTROL_KEYBOARD) return pressed && !system_keys_held() ? pressed : -1;
    for (int id = 1; id < 32; id++)
        if (fresh & 1u << id) return id;
    return -1;
}

/* A controller's state through player's layout (DHD_TEST_PAD, autopad.c): raw (guest GamepadRaw)
   into out (guest HostPad), as host_pad_device turns a real pad's. */
void host_service_controls_map(int player, uint32_t raw, uint32_t out)
{
    HostPad *pad = (HostPad *)GUEST(out);
    gamepad_apply(pad_layout(player), (const GamepadRaw *)GUEST(raw), stick_dead_zone_of(getenv("DHD_STICK_DEAD_ZONE")), pad);
}

void host_service_host_settings_load(void) { host_settings_load(); }
int host_service_host_setting_save(uint32_t a0, uint32_t a1) { return host_setting_save((const char *)GUEST(a0), (const char *)GUEST(a1)); }
int host_service_host_setting_locked(uint32_t a0) { return host_setting_locked((const char *)GUEST(a0)); }
void host_service_host_pad_device(int a0, uint32_t a1) { host_pad_device(a0, (HostPad *)GUEST(a1)); }
void host_service_host_pad_vibrate(int a0, int a1, int a2) { host_pad_vibrate(a0, a1, a2); }

/* Discord Rich Presence (finding 106): the core's state (port/hle/presence.c) to dhd_presence
   (port/render/presence), which runs only when the discord setting is on, and is cleared at exit. */
static void presence_log(const char *line) { hle_log("%s\n", line); }
static void presence_stop(void) { dhd_presence_stop(); }
void host_service_presence(uint32_t a0)
{
    static int running = -1;
    if (running < 0 && (running = dhd_presence_start(getenv("DHD_DISCORD"), presence_log)))
        host_end_add(HOST_END_DEVICES, "presence", presence_stop);
    if (!running) return;
    const HostPresence *p = GUEST(a0);
    char track[sizeof p->track + 1], room[sizeof p->room + 1];
    snprintf(track, sizeof track, "%.*s", (int)sizeof p->track, p->track);
    snprintf(room, sizeof room, "%.*s", (int)sizeof p->room, p->room);
    dhd_presence_set(p->kind, track, p->place, p->riders, room, p->members, p->max_members);
}


/* Wall-clock pacing is shared by the EE producer and native presenter. */
#define WORK_SAMPLES 300
#define FIELD_SECONDS pacing_period
static double pacing_period=1001.0/60000.0;
static uint64_t pacing_field;
static double jit_due, frame_began;           /* the next picture's due; when this frame's pad was read */
static double handed_due, handed_began;       /* the same for the picture being handed on now */
static volatile float work_ms[WORK_SAMPLES];  /* pad read to picture ready, by the presenter */
static volatile unsigned work_count;
static volatile double shown_due, shown_at;   /* the presenter's last picture: its due (0 while written), when shown */
static volatile int shown_late;               /* ... and whether it was late (ready after its due) */
static FILE *pacing_log[2];  /* DHD_PACING_LOG=prefix: prefix-ee.csv, prefix-present.csv (each frame) */

/* Pictures above 60 a second follow the display's refresh (finding 086, "Frame-time spikes at 144").
   The requested grid (1/144 s) is exact on the QueryPerformanceCounter clock, but a "144 Hz" monitor
   refreshes at its own rate (143.855 Hz by its mode on the test PC, 143.858 Hz on the QPC clock by
   DWM's vblanks): every 1 / (144 - 143.858) = 7 s one refresh got two pictures, which drops one on a
   fixed-refresh display and, with G-SYNC at its top rate, makes a present wait a whole refresh (a
   frame-time spike every 7 s). With "monitor" (143, the whole hertz) the grid was slower than the
   display: a repeated picture every 1.2 s.
   Now, when the requested rate is within 2% of the primary display's refresh (or of a whole fraction
   of it), a picture lasts n refreshes: the mode's exact rate (QueryDisplayConfig), refined from DWM's
   vblanks (DwmGetCompositionTimingInfo's qpcVBlank) to the QPC clock, plus 20 ppm so that with a
   variable refresh rate the pictures stay just under its top. (DWM's own qpcRefreshPeriod is a noisy
   running estimate that falls back to 144.000 here, so it isn't used.) While DWM's new vblanks lie
   on one fixed grid (two in a row: not a variable refresh rate, where the period alone is right),
   each due also moves a quarter of its distance, at most 1% of a refresh, towards half a refresh
   after a vblank, so presents keep clear of the compositor's latch. Only an on-time picture after
   an on-time one may come that 1% sooner than a period after it; a late picture (the CPU behind),
   and the one after it, wait the whole period, so late pictures never come faster than the refresh
   and the first on-time one never falls back over the vblank after them (two in one refresh).
   Late pictures still move the grid as before and never wait for a vblank. 60 a second (and
   online, always 60) keeps its 59.94 grid; DHD_PACE_LOCK=0 turns this off. Windows only (DWM). */
static struct {
    double next_query;         /* EE thread: when to ask again */
    double nominal;            /* the display mode's refresh period (s); 0 unknown */
    double refresh;            /* the refresh period on the QPC clock (nominal, refined from vblanks) */
    double vblank, anchor;     /* the newest vblank DWM reported, and an older one on its grid (host_wall_seconds) */
    unsigned long long vblank_qpc;
    int fits, misfits;         /* new vblanks in a row on (off) the grid of the one before */
    int frames;                /* refreshes per picture while the pictures follow the display; 0 not */
    int logged;                /* the state last logged (-1 none) */
} display = {0, 0, 0, 0, 0, 0, 0, 0, 0, -1};
static volatile double pacing_slack;  /* how much sooner than a period an on-time (steered) picture may come */

#if defined(_WIN64)
#include <dwmapi.h>
/* The primary display's refresh period by its mode (the path whose source is at 0,0), 0 unknown. */
static double display_nominal(void)
{
    static DISPLAYCONFIG_PATH_INFO path[64];
    static DISPLAYCONFIG_MODE_INFO mode[128];
    UINT32 paths = 0, modes = 0;
    double period = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || !paths || paths > 64 ||
        modes > 128 || QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path, &modes, mode, NULL) != ERROR_SUCCESS)
        return 0;
    for (UINT32 i = 0; i < paths; i++) {
        UINT32 m = path[i].sourceInfo.modeInfoIdx;
        int primary = m < modes && mode[m].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
                      mode[m].sourceMode.position.x == 0 && mode[m].sourceMode.position.y == 0;
        DISPLAYCONFIG_RATIONAL r = path[i].targetInfo.refreshRate;
        if ((primary || !period) && r.Numerator && r.Denominator) period = (double)r.Denominator / r.Numerator;
        if (primary) break;
    }
    return period > 1.0 / 1000 && period < 1.0 / 20 ? period : 0;
}

/* EE thread, every 0.5 s while above 60 a second: the refresh period and DWM's newest vblank. */
static void display_query(void)
{
    static HRESULT (WINAPI *timing)(HWND, DWM_TIMING_INFO *);
    static int loaded;
    if (!loaded) {
        loaded = 1;
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        timing = dwm ? (HRESULT (WINAPI *)(HWND, DWM_TIMING_INFO *))(void *)GetProcAddress(dwm, "DwmGetCompositionTimingInfo")
                     : NULL;
    }
    if (!display.nominal || display.misfits >= 10) {  /* first, or the mode may have changed */
        double nominal = display_nominal();
        if (nominal != display.nominal) display.refresh = nominal, display.anchor = 0, display.fits = 0;
        display.nominal = nominal;
        display.misfits = 0;
    }
    DWM_TIMING_INFO info;
    LARGE_INTEGER freq, qpc;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    if (!display.nominal || !timing || FAILED(timing(NULL, &info)) || !QueryPerformanceFrequency(&freq) ||
        !info.qpcVBlank || info.qpcVBlank == display.vblank_qpc)
        return;
    QueryPerformanceCounter(&qpc);
    double refresh = display.refresh,
           vblank = host_wall_seconds() - (double)((LONGLONG)qpc.QuadPart - (LONGLONG)info.qpcVBlank) / (double)freq.QuadPart;
    if (display.vblank_qpc) {
        double span = vblank - display.vblank, k = floor(span / refresh + 0.5);
        int fit = k >= 1 && fabs(span - k * refresh) < 0.0002 + span * 5e-5;  /* 0.2 ms, plus 50 ppm of the span */
        display.fits = fit ? display.fits + 1 : 0;
        display.misfits = fit ? 0 : display.misfits + 1;
        if (!fit) display.anchor = 0;
        else if (!display.anchor) display.anchor = display.vblank;
        else {  /* the period on the QPC clock: whole refreshes since the anchor (2 s and more) */
            double whole = vblank - display.anchor, n = floor(whole / refresh + 0.5);
            if (whole >= 2 && fabs(whole / n / display.nominal - 1) < 5e-4) display.refresh = whole / n;
            if (whole > 600) display.anchor = vblank;
        }
    }
    display.vblank_qpc = info.qpcVBlank;
    display.vblank = vblank;
}
#endif

/* host_pacing_config: the period the pictures keep (EE thread). */
static double display_period(double period)
{
#if defined(_WIN64)
    static int off = -1;
    if (off < 0) {
        const char *s = host_getenv("DHD_PACE_LOCK");
        off = s && !atoi(s);
    }
    int frames = 0;
    if (!off && period < 1.0 / 61) {  /* above 60 a second */
        double now = host_wall_seconds();
        if (now >= display.next_query) {
            display.next_query = now + 0.5;
            display_query();
        }
        double n = display.refresh ? floor(period / display.refresh + 0.5) : 0;
        if (n >= 1 && fabs(n * display.refresh - period) <= 0.02 * period) frames = (int)n;
        if (display.refresh && frames != display.logged) {
            display.logged = frames;
            /* (the EE thread rounds toward zero, so printf truncates the last digit) */
            if (frames)
                fprintf(stderr, "pacing: %.0f pictures/s follow the display's refresh, %.4f Hz (%d per picture)\n",
                        1 / period, 1 / display.refresh, frames);
            else
                fprintf(stderr, "pacing: the display refreshes at %.4f Hz, not near %.0f pictures/s: they keep their own grid\n",
                        1 / display.refresh, 1 / period);
        }
    }
    display.frames = frames;
    pacing_slack = frames ? 0.01 * display.refresh : 0;  /* display_steer's limit */
    return frames ? frames * display.refresh * (1 + 2e-5) : period;
#else
    return period;
#endif
}

/* How far to move a due towards half a refresh after a vblank (0 unless DWM's vblanks lie on a grid). */
static double display_steer(double due)
{
    double refresh = display.refresh;
    if (!display.frames || display.fits < 2 || due - display.vblank > 30) return 0;
    double error = fmod(display.vblank + 0.5 * refresh - due, refresh), limit = 0.01 * refresh;
    if (error > 0.5 * refresh) error -= refresh;
    else if (error < -0.5 * refresh) error += refresh;
    error *= 0.25;
    return error > limit ? limit : error < -limit ? -limit : error;
}

static FILE *pacing_file(int which)
{
    static int opened;
    if (!opened) {
        opened = 1;
        const char *prefix = host_getenv("DHD_PACING_LOG");
        char path[512];
        for (int i = 0; prefix && i < 2; i++) {
            snprintf(path, sizeof path, "%s-%s.csv", prefix, i ? "present" : "ee");
            pacing_log[i] = fopen(path, "w");
            if (pacing_log[i]) setvbuf(pacing_log[i], NULL, _IOFBF, 1 << 16);
        }
        if (pacing_log[0]) fprintf(pacing_log[0], "field,now,shown,due,start,work_ms,began\n");
        if (pacing_log[1]) fprintf(pacing_log[1], "due,began,ready,presented,refresh_phase\n");
        /* the logs' clock, to set them against other timestamps of the performance counter (Windows'
           QueryPerformanceCounter: DWM's vblanks) */
        Uint64 freq = SDL_GetPerformanceFrequency(), counter = SDL_GetPerformanceCounter();
        if (prefix)
            fprintf(stderr, "pacing: log seconds = performance counter / %lld - %.7f\n", (long long)freq,
                    (double)counter / (double)freq - host_wall_seconds());
    }
    return pacing_log[which];
}

void host_pacing_wait(void)
{
    const double period = FIELD_SECONDS;
    unsigned count = work_count, n = count < WORK_SAMPLES ? count : WORK_SAMPLES;
    double work = period;  /* until there are samples (no presenter: always), pace()'s timing */
    if (n >= 30) {
        /* Frames longer than a field (a load, a stall) are late however early they start, so they
           don't count, unless they are more than one in ten (a slow stretch: then no delay). */
        float worst = 0, field_ms = (float)(period * 1000);
        unsigned longer = 0;
        for (unsigned i = 0; i < n; i++) {
            float w = work_ms[i];
            if (w > field_ms) longer++;
            else if (w > worst) worst = w;
        }
        if (longer * 10 <= n) work = worst / 1000.0 + 0.0015;
        else {
            /* A slow stretch (a phone that can't hold 60): no frame waits for the last picture to
               be shown. That wait (start = shown, below) put the EE's frame and the render
               thread's picture in series, 25 ms a field on a T612-class phone where they overlap
               in 18 (finding 127). Fields keep pace()'s grid instead: each starts a period after
               the last one's place on it, so fields that come in quick repay a slow one, and the
               grid starts again from now only once it is more than two periods behind
               (pace_grid.h: a change of period keeps the frames before on their own). The
               presenter keeps the pictures a field apart. */
            static PaceGrid slow;
            double now = host_wall_seconds(), start = pace_grid_next(&slow, now, period);
            if (start > now) host_wait_wall(start);
            jit_due = frame_began = host_wall_seconds();  /* shown as soon as it is ready */
            return;
        }
    }
    double now = host_wall_seconds(), shown = jit_due;
    /* The presenter moved the grid if the picture handed now, or the one before it, was late. Its
       least spacing is the period, less pacing_slack while the pictures follow the display and both
       are on time (this one isn't known yet: on time is the least it can wait). */
    double last_due, last_at, spacing;
    int last_late;
    do {  /* the writer's order (below), kept with fences (arm64 reorders loads; x86 only the compiler) */
        last_due = shown_due;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        last_at = shown_at, last_late = shown_late;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while (last_due != shown_due);
    spacing = period - (last_late ? 0 : pacing_slack);
    int previous = display.frames ? fabs(shown - last_due - period) < 0.03 * period  /* dues steered a little */
                                  : last_due + period - shown < 1e-6 && shown - last_due - period < 1e-6;
    if (shown && last_due == shown && last_at > shown) shown = last_at;
    else if (shown && last_due > 0 && previous && last_at + spacing > shown) shown = last_at + spacing;
    else if (shown && last_due > 0 && shown - last_due > 1.5 * period && shown - last_due < 8 * period &&
             now - last_at < 4 * period) {
        /* Two or more pictures still before the presenter (the start or a load left a queue): it
           presents one a period, so the queue stayed for good and every pad read waited for it
           (key to present 90 ms on the title at 144 Hz, 25 ms without it). The picture handed now
           is shown after them, so the grid follows that once and the queue is gone. Only while the
           presenter is presenting (a minimised window presents nothing), and only a queue a few
           pictures long: after a long gap (a load, or the wait at an online race's start) the
           picture handed when the grid started again from now still carried the due from before
           it, and the gap read as a queue of that many pictures, so the game slept as long again
           (issue #22: about 54 s before an online race, 14 s after a load). */
        double behind = floor((shown - last_due) / period + 0.5);
        if (last_at + behind * spacing > shown) shown = last_at + behind * spacing;
    }
    double due = shown + period;
    if (!shown || now > due) {  /* the first field, or more than a field late: start again from now */
        jit_due = now + (work < period ? work : period);
        frame_began = now;
        return;
    }
    due += display_steer(due);
    double start = due - work;
    if (start < shown) start = shown;  /* never two frames in one field */
    host_wait_wall(start);
    jit_due = due;
    frame_began = host_wall_seconds();
    FILE *log = pacing_file(0);
    if (log) fprintf(log, "%llu,%.6f,%.6f,%.6f,%.6f,%.3f,%.6f\n", (unsigned long long)pacing_field, now, shown,
                     due, start, work * 1000, frame_began);
}

/* The picture handed on at this VBLANK: when to show it (0: at once) and when its pad was read. */
void pacing_handed(double *due, double *began)
{
    *due = handed_due;
    *began = handed_began;
}

/* The presenter, just before it presents a picture pacing_handed described: notes how long the
   frame took, then waits for the due time, and at least a field after its last picture. */
void pacing_present(double due, double began)
{
    static double last_at;
    static int last_late;
    double now = host_wall_seconds(), target = due;
    if (due > 0) {
        if (began > 0 && now > began) {
            unsigned i = work_count;
            work_ms[i % WORK_SAMPLES] = (float)((now - began) * 1000);
            work_count = i + 1;
        }
        int late = now > due;  /* this or the last picture late: a whole period after the last */
        double spacing = FIELD_SECONDS - (late || last_late ? 0 : pacing_slack);
        if (last_at + spacing > target) target = last_at + spacing;
        if (target > now && target < now + 0.05)
            host_wait_wall(target);
        last_at = host_wall_seconds();
        last_late = late;
        shown_due = 0;  /* a reader sees due 0 or all three values: the fences keep the order */
        __atomic_thread_fence(__ATOMIC_RELEASE);
        shown_at = last_at;
        shown_late = late;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        shown_due = due;
    }
    FILE *log = pacing_file(1);
    if (log) {
        /* where in the display's refresh this picture went, from DWM's newest vblank (-1 unknown) */
        double at = host_wall_seconds(), refresh = display.refresh, phase = -1;
        if (refresh > 0 && display.vblank) phase = fmod(at - display.vblank, refresh) / refresh;
        if (refresh > 0 && display.vblank && phase < 0) phase += 1;  /* a vblank newer than the picture */
        fprintf(log, "%.6f,%.6f,%.6f,%.6f,%.4f\n", due, began, now, at, phase);
    }
    if (log && !(work_count % 60)) fflush(log), fflush(pacing_log[0]);  /* DHD_BENCH_EXIT ends with ExitProcess */
    /* DHD_PACING_STATS=1: every 600 pictures, how old the pad state was when each was presented. */
    static int stats = -1;
    static double sum, worst;
    static unsigned count;
    if (stats < 0) stats = host_getenv("DHD_PACING_STATS") != NULL;
    if (stats && began > 0) {
        double age = (host_wall_seconds() - began) * 1000;
        sum += age, count++;
        if (age > worst) worst = age;
        if (count == 600) {
            fprintf(stderr, "pacing: %s, pad to present mean %.2f ms max %.2f ms\n",
                    due > 0 ? "just-in-time" : "wait before VBLANK", sum / count, worst);
            sum = worst = 0, count = 0;
        }
    }
}

void host_pacing_config(double period, uint64_t field) { pacing_period=display_period(period); pacing_field=field; }
void host_pacing_handoff(int fresh) { handed_due=fresh ? jit_due : 0; handed_began=frame_began; }
void host_pacing_input(void) { frame_began=host_wall_seconds(); }

/* Native diagnostics consume published scalar state, never call the module
   from graphics, presenter or device OS threads. */
uint64_t kernel_vsync_count;
static double module_seconds;
double host_seconds(void) { return module_seconds; }
void host_frame_context(uint64_t field, double seconds) { kernel_vsync_count=field; module_seconds=seconds; }

void host_frame_registers(uint32_t address) {
    memcpy(published_registers,GUEST(address),sizeof published_registers);
}

double host_pacing_period(void) { return pacing_period; }

static double published_aspect;
static int published_flat_limit;
double host_widescreen_aspect(void) { return published_aspect; }
int host_widescreen_flat_limit(void) { return published_flat_limit; }
/* Widescreen's card mode (finding 140): the renderer's answer (gs_native_card) at one vsync is how
   the game draws the next field's 2D; card_drawn is the mode of the field being handed to the
   renderer now (present.c), which shows it 4:3 only when the game drew it so. */
int gs_native_card(void);  /* port/gs/native_core.inc; 0 without the native renderer */
static int card_next, card_drawn;
int host_widescreen_card_drawn(void) { return card_drawn; }
int host_frame_video(double aspect, int flat_limit)
{
#if defined(__APPLE__) && TARGET_OS_IPHONE
    /* iOS: the picture's shape follows the display while the game runs (widescreen.c's live
       aspect); a package VU1 already holds gets its sprite widths back first. */
    if (aspect != published_aspect) {
        void vif1_update_widescreen_sprites(double previous);
        graphics_wait();
        vif1_update_widescreen_sprites(published_aspect);
    }
#endif
    published_aspect=aspect; published_flat_limit=flat_limit;
    card_drawn=card_next;
#if defined(__APPLE__) && TARGET_OS_IPHONE
    card_next=aspect>0 && flat_limit>0 && gs_native_card();
#else
    card_next=aspect>0 && gs_native_card();
#endif
    return card_next;
}

/* Guest-address-only module services. */
#include "../host/host_api.h"
#include "../include/guest.h"
void host_service_host_backtrace(void) { host_backtrace(); }
int host_service_host_game_options_read(uint32_t a0, uint32_t a1) { return host_game_options_read((char *)GUEST(a0), a1); }
