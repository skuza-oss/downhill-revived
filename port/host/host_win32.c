/* The Windows host of host_api.h (port/host): what has no portable answer (host_sdl.c has the
   rest, on SDL3 and the C library): guest memory protection and the access-fault guard (a vectored
   handler), the FP control word, the cycle counter, the program's path (UTF-8), code range and
   memory regions, the environment's writes (the C library's, which getenv reads), the heartbeat's
   sample of another thread and the process's end. The launcher, window, input and the rest of the
   Windows shell stay in host.c. Fibers, guest RAM, the FP mode, the guest-safe callback contracts
   and the thread name live in the host64 glue (fiber*.c, guest_memory*.c, fp_mode.c, the
   generated imports_gen.c, host.c). The module sees only host_api.h. */
#include "host_api.h"
#include "host_os.h"
#include "../include/guest.h"

#include <fcntl.h>
#include <intrin.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xmmintrin.h>

/* On 64-bit hosts guest_base (guest.h) is defined by port/host64/guest_memory.c, which maps
   guest memory. */

/* ---- environment, errors and the program ---- */
unsigned long host_last_error(void) { return GetLastError(); }

/* The program's full path in UTF-8 (SDL's paths are). */
unsigned host_exe_path(char *out, unsigned size)
{
    WCHAR wide[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, wide, sizeof wide / sizeof *wide);
    int k = n && n < sizeof wide / sizeof *wide && size
                ? WideCharToMultiByte(CP_UTF8, 0, wide, (int)n + 1, out, (int)size, NULL, NULL) : 0;
    if (k <= 0) {
        if (size) out[0] = 0;
        return 0;
    }
    return (unsigned)k - 1;
}

void host_exit(int code) { ExitProcess((UINT)code); }

unsigned host_fp_csr(void) { return _mm_getcsr(); }
void host_fp_set_csr(unsigned csr) { _mm_setcsr(csr); }
uint64_t host_cycles(void) { return __rdtsc(); }

void host_putenv(const char *text) { _putenv(text); }
void host_putenv_s(const char *name, const char *value) { _putenv_s(name, value); }

int host_mem_protect(uint32_t at, uint32_t size, int protection)
{
    DWORD old;
    return VirtualProtect(GUEST(at), size, protection == HOST_MEM_NOACCESS ? PAGE_NOACCESS : PAGE_READWRITE, &old) != 0;
}

/* The access-fault guard: one vectored handler, for the thread that installed it. */
static int (*guard_on_fault)(uint32_t address);
static DWORD guard_thread;

static LONG CALLBACK guard_fault(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        GetCurrentThreadId() != guard_thread || !guard_on_fault(GADDR(r->ExceptionInformation[1])))
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}

int host_fault_guard(int (*on_fault)(uint32_t address))
{
    guard_on_fault = on_fault;
    guard_thread = GetCurrentThreadId();
    return AddVectoredExceptionHandler(1, guard_fault) != NULL;
}

int host_fault_guard_here(void) { return guard_thread && GetCurrentThreadId() == guard_thread; }

/* dhd.exe's .text: the section of the image that holds the code, by its PE header. */
void host_code_range(uintptr_t *lo, uintptr_t *hi)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    *lo = *hi = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (memcmp(sec[i].Name, ".text\0\0\0", 8) == 0) {
            *lo = (uintptr_t)base + sec[i].VirtualAddress;
            *hi = *lo + sec[i].Misc.VirtualSize;
        }
}

/* ---- the heartbeat (autopad.c) ---- */
static HANDLE heartbeat_thread;  /* a duplicate of the thread that started the heartbeat */
static void (*heartbeat_beat)(void);

static DWORD WINAPI heartbeat_run(void *unused)
{
    (void)unused;
    for (;;) {
        Sleep(30000);
        heartbeat_beat();
    }
}

void host_heartbeat_start(void (*beat)(void))
{
    HANDLE self;
    heartbeat_beat = beat;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    heartbeat_thread = self;
    CloseHandle(CreateThread(NULL, 0, heartbeat_run, NULL, 0, NULL));
}

uintptr_t host_heartbeat_eip(void)
{
    CONTEXT c;
    uintptr_t eip = 0;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL;
    if (SuspendThread(heartbeat_thread) != (DWORD)-1) {
        if (GetThreadContext(heartbeat_thread, &c)) eip = (uintptr_t)c.Rip;
        ResumeThread(heartbeat_thread);
    }
    return eip;
}

/* ---- the system (host_os.h) ----
   ntdll's RtlGetVersion, since GetVersionEx says 6.2 to a program without a manifest; the release
   ("24H2") from the registry; Wine (Proton, for Linux and Mac players) by its wine_get_version;
   the CPU's name from CPUID's brand string. */
void host_os_system(char *out, unsigned size)
{
    RTL_OSVERSIONINFOW v = {sizeof v};
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    LONG(WINAPI *get_version)(RTL_OSVERSIONINFOW *) = (void *)GetProcAddress(ntdll, "RtlGetVersion");
    const char *(CDECL *wine_version)(void) = (void *)GetProcAddress(ntdll, "wine_get_version");
    if (get_version) get_version(&v);
    char release[32] = "", windows[32], wine[48] = "", brand[49] = "", *cpu = brand;
    DWORD n = sizeof release;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion",
                     RRF_RT_REG_SZ, NULL, release, &n) != ERROR_SUCCESS)
        release[0] = 0;
    if (v.dwMajorVersion == 10) snprintf(windows, sizeof windows, "%s", v.dwBuildNumber >= 22000 ? "11" : "10");
    else snprintf(windows, sizeof windows, "%lu.%lu", v.dwMajorVersion, v.dwMinorVersion);
    if (wine_version) snprintf(wine, sizeof wine, " on Wine %s", wine_version());
    int r[4];
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004)
        for (int i = 0; i < 3; i++) __cpuid(r, 0x80000002 + i), memcpy(brand + 16 * i, r, 16);
    while (*cpu == ' ') cpu++;
    for (size_t k = strlen(cpu); k && cpu[k - 1] == ' '; k--) cpu[k - 1] = 0;
    snprintf(out, size, "Windows %s%s%s (build %lu)%s, %s", windows, *release ? " " : "", release, v.dwBuildNumber, wine,
             *cpu ? cpu : "unknown CPU");
}

/* ---- files (host_os.h) ----
   The UTF-8 path as Windows' own; a read opens the file with write and delete sharing as well,
   the way the autopad script reader always has, so a driver can replace the script while it is
   being read. */
FILE *host_os_fopen(const char *path, const char *mode)
{
    WCHAR wide[MAX_PATH * 4], wmode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, sizeof wide / sizeof *wide) ||
        !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, sizeof wmode / sizeof *wmode))
        return NULL;
    if (mode[0] != 'r' || strchr(mode, '+')) return _wfopen(wide, wmode);
    HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    int binary = strchr(mode, 'b') != NULL;
    int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | (binary ? _O_BINARY : _O_TEXT));  /* the FILE closes the handle */
    if (fd < 0) {
        CloseHandle(h);
        return NULL;
    }
    FILE *f = _fdopen(fd, binary ? "rb" : "r");
    if (!f) _close(fd);
    return f;
}
