/* The POSIX host of host_api.h (port/host): what has no portable answer on Linux, Android, macOS
   (and other POSIX hosts where the noted calls exist); host_sdl.c has the rest, on SDL3 and the C
   library. Guest memory protection and the access-fault guard (signals), the FP control word
   (MXCSR, or arm64's FPCR), the cycle counter, the program's path, code range and memory regions,
   the environment's writes, the heartbeat and the process's end. Counterpart of host_win32.c; the
   module sees only host_api.h. Fibers, guest RAM, the FP mode, the guest-safe callback contracts
   and the thread name live in the host64 glue (fiber*.c, guest_memory*.c, fp_mode.c, the
   generated imports_gen.c, host.c), which provide those symbols. */
#define _GNU_SOURCE /* readlink, sigaction, dladdr */
#include "host_api.h"
#include "host_os.h"
#include "../include/guest.h"

#include <dlfcn.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>     /* _NSGetExecutablePath */
#include <mach-o/getsect.h>  /* getsegmentdata */
#include <mach/mach.h>       /* the frame waits' time-constraint sleep */
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <sys/sysctl.h>     /* the system's names (host_os_system) */
#else
#include <elf.h>
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <xmmintrin.h>
#include <x86intrin.h>
#elif defined(__aarch64__)
#else
#error Implement the POSIX host for this architecture
#endif

/* ---- environment, errors and the program ---- */
unsigned long host_last_error(void) { return (unsigned long)errno; }

unsigned host_exe_path(char *out, unsigned size)
{
#ifdef __ANDROID__
    /* No executable of our own (the app is app_process with libmain.so): the app's files folder
       stands for dhd.exe's (android_main.c), so dhd.ini, saves and dhd.log go there. */
    const char *android_exe_path(void);
    int k = snprintf(out, size, "%s", android_exe_path());
    return k > 0 && (unsigned)k < size ? (unsigned)k : 0;
#elif defined(__APPLE__)
    char given[4096], real[PATH_MAX];
    uint32_t length = sizeof given;
    int k = _NSGetExecutablePath(given, &length) == 0 && realpath(given, real)
            ? snprintf(out, size, "%s", real) : -1;
    if (k <= 0 || (unsigned)k >= size) {
        if (size) out[0] = 0;
        return 0;
    }
    return (unsigned)k;
#endif
    ssize_t n = readlink("/proc/self/exe", out, size ? size - 1 : 0);
    if (n < 0) {
        if (size) out[0] = 0;
        return 0;
    }
    out[n] = 0;
    return (unsigned)n;
}

void host_exit(int code)
{
#ifdef __ANDROID__
    extern int __llvm_profile_write_file(void) __attribute__((weak));  /* --pgo-generate builds only */
    if (__llvm_profile_write_file) __llvm_profile_write_file();
#endif
    void host_log_drain(void);  /* port/hle/host.c: the last lines reach dhd.log (and logcat) */
    host_log_drain();
    _exit(code);
}

/* The callers (vu0_interp.c, ps2asm.c) speak MXCSR: they read the word, set rounding toward zero
   with FTZ/DAZ ((c & ~0x603F) | 0xFFC0) and restore what they read. On arm64 the word is translated
   to and from FPCR: MXCSR RC (bits 13-14: nearest, down, up, zero) is FPCR RMode (bits 22-23:
   nearest, up, down, zero), FTZ or DAZ is FZ (bit 24, which flushes inputs and results), and every
   exception reads as masked (arm64 has no FP traps here). Writing the MXCSR bits into FPCR as they
   are (AND1) left RMode and FZ alone: VU1 on the vu thread (FPCR IEEE) then rounded to nearest with
   denormals, unlike the PS2 and the x64 build. */
#if defined(__aarch64__)
static const unsigned rc_to_rmode[4] = {0, 2, 1, 3}, rmode_to_rc[4] = {0, 2, 1, 3};
#endif

unsigned host_fp_csr(void)
{
#if defined(__x86_64__)
    return _mm_getcsr();
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return 0x1F80u | rmode_to_rc[(v >> 22) & 3] << 13 | ((v >> 24) & 1 ? 0x8040u : 0);
#endif
}

void host_fp_set_csr(unsigned csr)
{
#if defined(__x86_64__)
    _mm_setcsr(csr);
#elif defined(__aarch64__)
    uint64_t now, v = (uint64_t)rc_to_rmode[(csr >> 13) & 3] << 22 | (csr & 0x8040u ? 1ull << 24 : 0);
    __asm__ volatile("mrs %0, fpcr" : "=r"(now));
    if (now != v) __asm__ volatile("msr fpcr, %0\n\tisb" :: "r"(v) : "memory");
#endif
}

uint64_t host_cycles(void)
{
#if defined(__x86_64__)
    return __rdtsc();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

/* "NAME=value", as Windows' _putenv: a copy (putenv would keep the caller's buffer, often on the
   module's stack: online.c's set_env, so DHD_NAV read garbage and the online race never started),
   and "NAME=" unsets it. */
void host_putenv(const char *text)
{
    const char *eq = strchr(text, '=');
    char name[256];
    if (!eq || eq == text || (size_t)(eq - text) >= sizeof name) return;
    memcpy(name, text, (size_t)(eq - text));
    name[eq - text] = 0;
    host_putenv_s(name, eq + 1);
}

void host_putenv_s(const char *name, const char *value)
{
    if (!value || !*value) unsetenv(name);
    else setenv(name, value, 1);
}

/* ---- memory protection and access faults (graphics.c, deferred store-image reads) ---- */
static uintptr_t os_page(void)
{
    static uintptr_t page;
    if (!page) page = (uintptr_t)sysconf(_SC_PAGESIZE);
    return page;
}

int host_mem_protect(uint32_t at, uint32_t size, int protection)
{
    /* Page-align the range the way the OS requires (its page: 16 KiB on Apple silicon and some
       Android devices); the caller's exact range keeps its protection inside it. */
    uintptr_t page = os_page(), base = (uintptr_t)GUEST(at);
    uintptr_t lo = base & ~(page - 1), hi = (base + size + page - 1) & ~(page - 1);
    if (hi < base) return 0;
    return mprotect((void *)lo, hi - lo, protection == HOST_MEM_NOACCESS ? PROT_NONE : PROT_READ | PROT_WRITE) == 0;
}

/* The access-fault guard: a SIGSEGV/SIGBUS handler for the thread that
   installed it. The module's handler commits the page and returns 1 (the
   faulting access is retried); 0 passes the fault to the previous handler
   (the guest allocator's reporter, which ends the process with the guest
   address). Only the installing thread is guarded. The module guards 4 KiB
   pages; where the OS page is larger, a fault anywhere in it is offered for
   each 4 KiB page it holds. */
static _Thread_local int (*guard_on_fault)(uint32_t address);
static _Thread_local int guard_armed;
static struct sigaction guard_old_segv, guard_old_bus;
static pthread_mutex_t guard_installed_lock = PTHREAD_MUTEX_INITIALIZER;
static int guard_installed;

static void guard_fault(int sig, siginfo_t *info, void *ctx)
{
    uintptr_t host = (uintptr_t)info->si_addr;
    (void)ctx;
    if (guard_armed && guard_on_fault && host >= guest_base && host - guest_base < 0x100000000ull) {
        uint32_t at = (uint32_t)(host - guest_base), page = (uint32_t)os_page();
        if (guard_on_fault(at)) return; /* retry the access */
        for (uint32_t sub = at & ~(page - 1); page > 4096 && sub < (at & ~(page - 1)) + page; sub += 4096)
            if (sub != (at & ~4095u) && guard_on_fault(sub)) return;
    }
    /* Not ours, or not handled: run the previous handler. */
    struct sigaction *old = sig == SIGSEGV ? &guard_old_segv : &guard_old_bus;
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

int host_fault_guard(int (*on_fault)(uint32_t address))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = guard_fault;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;
    pthread_mutex_lock(&guard_installed_lock);
    if (!guard_installed) {
        if (sigaction(SIGSEGV, &sa, &guard_old_segv) < 0 || sigaction(SIGBUS, &sa, &guard_old_bus) < 0) {
            pthread_mutex_unlock(&guard_installed_lock);
            return 0;
        }
        guard_installed = 1;
    }
    pthread_mutex_unlock(&guard_installed_lock);
    guard_on_fault = on_fault;
    guard_armed = 1;
    return 1;
}

int host_fault_guard_here(void) { return guard_armed; }

/* dhd64's .text: the section of the image that holds the code, by its ELF
   program headers or its Mach-O __TEXT segment (the PE walk's counterpart in
   host_win32.c). */
void host_code_range(uintptr_t *lo, uintptr_t *hi)
{
    static uintptr_t base, extent;
    static int have;
    *lo = *hi = 0;
#ifdef __APPLE__
    if (!have) {
        Dl_info self;
        unsigned long size = 0;
        uint8_t *text = dladdr((void *)host_code_range, &self) && self.dli_fbase
                        ? getsegmentdata((const struct mach_header_64 *)self.dli_fbase, "__TEXT", &size) : NULL;
        base = (uintptr_t)text, extent = text ? size : 0, have = 1;
    }
#else
    if (!have) {
        char path[4096];
#ifdef __ANDROID__
        /* the code is libmain.so's, not the process's executable (app_process) */
        Dl_info self;
        ssize_t n = dladdr((void *)host_code_range, &self) && self.dli_fname
                    ? snprintf(path, sizeof path, "%s", self.dli_fname) : -1;
#else
        ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
#endif
        if (n > 0) {
            path[n] = 0;
            FILE *f = fopen(path, "rb");
            if (f) {
                Elf64_Ehdr eh;
                if (fread(&eh, 1, sizeof eh, f) == sizeof eh && memcmp(eh.e_ident, "\177ELF", 4) == 0) {
                    Elf64_Phdr ph;
                    if (fseeko(f, (off_t)eh.e_phoff, SEEK_SET) == 0) {
                        for (int i = 0; i < eh.e_phnum; i++) {
                            if (fread(&ph, 1, sizeof ph, f) != sizeof ph) break;
                            if (ph.p_type == PT_LOAD && (ph.p_flags & (PF_R | PF_X)) == (PF_R | PF_X)) {
                                Dl_info info;
                                if (dladdr((void *)host_code_range, &info) && info.dli_fbase) {
                                    base = (uintptr_t)info.dli_fbase + ph.p_vaddr;
                                    extent = ph.p_memsz;
                                    have = 1;
                                }
                                break;
                            }
                        }
                    }
                }
                fclose(f);
            }
        }
    }
#endif
    if (extent) {
        *lo = base;
        *hi = base + extent;
    }
}

/* ---- the heartbeat (autopad.c): a host thread calls beat() every 30 s ---- */
static void (*heartbeat_beat)(void);

static void *heartbeat_run(void *unused)
{
    (void)unused;
    for (;;) {
        struct timespec t = {30, 0};
        nanosleep(&t, NULL);
        heartbeat_beat();
    }
    return NULL;
}

void host_heartbeat_start(void (*beat)(void))
{
    pthread_t t;
    pthread_attr_t attr;
    heartbeat_beat = beat;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, heartbeat_run, NULL) == 0) pthread_detach(t);
    pthread_attr_destroy(&attr);
}

/* The owner-thread drain (imports_gen.c) cannot suspend itself to sample a
   host PC the way the i686 watchdog does; the generated boundary reports 0. */
uintptr_t host_heartbeat_eip(void) { return 0; }

#ifdef __APPLE__
/* The frame waits' sleep (host_os.h). macOS coalesces the timers of a process it doesn't treat as
   interactive: a 15.7 ms sleep from a process started in the background (ssh) woke 4.6 ms late on
   average, 8.2 at worst (300 sleeps, an M1, 2026-10-27; public PR #6 saw ~6). For the sleep alone
   the thread is a time-constraint one, whose timers aren't coalesced: 0.02 ms late, 0.04 at worst.
   None of the game's work runs at that priority. */
void host_os_sleep_ns(unsigned long long ns)
{
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    thread_port_t self = pthread_mach_thread_np(pthread_self());
    uint32_t ms = (uint32_t)(1000000ull * base.denom / base.numer);  /* a millisecond in Mach time units */
    thread_time_constraint_policy_data_t rt = {16 * ms, ms / 2, 16 * ms, 1};
    int precise = thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&rt,
                                    THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
    struct timespec t = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
    if (precise) {
        thread_standard_policy_data_t standard = {0};
        thread_policy_set(self, THREAD_STANDARD_POLICY, (thread_policy_t)&standard, THREAD_STANDARD_POLICY_COUNT);
    }
}
#endif

/* ---- the system (host_os.h) ---- */
#if !defined(__APPLE__) && !defined(__ANDROID__)
/* The value of path's first line that starts with key, after sep, quotes and spaces trimmed. */
static void text_value(const char *path, const char *key, char sep, char *out, size_t size)
{
    char line[256];
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *at = strchr(line, sep);
        if (strncmp(line, key, strlen(key)) || !at) continue;
        for (at++; *at == ' ' || *at == '\t' || *at == '"'; at++) {}
        snprintf(out, size, "%.*s", (int)strcspn(at, "\"\n"), at);
        break;
    }
    fclose(f);
}
#endif

/* macOS: its product version and the CPU's brand string by sysctl; Android: the release, the
   phone and its chip from the system properties; Linux: os-release's name, the kernel and
   /proc/cpuinfo's model name. */
void host_os_system(char *out, unsigned size)
{
    char os[160], cpu[128] = "";
    struct utsname u;
    if (uname(&u)) snprintf(u.machine, sizeof u.machine, "unknown");
#if defined(__APPLE__)
    char version[32] = "";
    size_t n = sizeof version;
    sysctlbyname("kern.osproductversion", version, &n, NULL, 0);
    snprintf(os, sizeof os, "macOS %s", version);
    n = sizeof cpu;
    if (sysctlbyname("machdep.cpu.brand_string", cpu, &n, NULL, 0)) cpu[0] = 0;
#elif defined(__ANDROID__)
    char release[PROP_VALUE_MAX] = "", maker[PROP_VALUE_MAX] = "", model[PROP_VALUE_MAX] = "";
    __system_property_get("ro.build.version.release", release);
    __system_property_get("ro.product.manufacturer", maker);
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.soc.model", cpu);
    snprintf(os, sizeof os, "Android %s, %s %s", release, maker, model);
#else
    char name[96];
    text_value("/etc/os-release", "PRETTY_NAME=", '=', name, sizeof name);
    snprintf(os, sizeof os, "%s (Linux %s)", *name ? name : "Linux", u.release);
    text_value("/proc/cpuinfo", "model name", ':', cpu, sizeof cpu);
#endif
    snprintf(out, size, "%s, %s (%s)", os, *cpu ? cpu : "unknown CPU", u.machine);
}

/* ---- files (host_os.h): the C library's ---- */
FILE *host_os_fopen(const char *path, const char *mode) { return fopen(path, mode); }
