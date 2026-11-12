/* POSIX mapping for guest_memory.c: one PROT_NONE reservation, a memfd RAM
   section (macOS: an unlinked shared memory object; Android 10: ashmem) aliased with MAP_FIXED|MAP_SHARED, commit-on-demand with mprotect
   (which preserves contents, like MEM_COMMIT), and a SIGSEGV/SIGBUS handler
   on an alternate stack that reports the guest address and re-raises the
   signal. Policy (which regions are mirrors, what is committed) stays shared.
   Written for this task; no third-party code. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include "../host/backtrace_posix.h"  /* <execinfo.h>, or Android's own (API < 33) */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include "core_host.h"  /* guest_base, for the fault bounds check */
#include "guest_memory_os.h"

#define GUEST_SIZE 0x200000000ull
#define RAM_SIZE 0x2000000ull

static int ram_fd=-1;
static stack_t altstack;
static struct sigaction old_segv, old_bus;
static int installed;

#ifdef __ANDROID__
#include <android/sharedmem.h>
#include <dlfcn.h>
/* The guest RAM's file (finding 191). The APK's minimum is Android 10 (API 29), whose libc has no
   memfd_create (API 30); there the app sandbox's system call filter is not counted on either. So
   memfd_create is looked up in libc at run time (Android 11 and later: as before), else the RAM is
   an ashmem region (ASharedMemory_create, API 26), made at its size, which takes the same
   MAP_SHARED mappings at any offset. */
static int sized;
static int android_ram_fd(void) {
    int (*memfd)(const char *,unsigned)=(int (*)(const char *,unsigned))dlsym(RTLD_DEFAULT,"memfd_create");
    if (memfd) return memfd("dhd-guest-ram",MFD_CLOEXEC);
    int fd=ASharedMemory_create("dhd-guest-ram",RAM_SIZE);
    sized=fd>=0;
    return fd;
}
#else
enum { sized=0 };
#endif

void os_fail(const char *what) {
    fprintf(stderr,"host64 memory: %s (%d: %s)\n",what,errno,strerror(errno)); abort();
}
uintptr_t os_reserve(void) {
#ifdef __APPLE__  /* no memfd: a POSIX shared memory object, unlinked at once */
    char name[32]; snprintf(name,sizeof name,"/dhd-guest-ram-%d",(int)getpid());
    ram_fd=shm_open(name,O_RDWR|O_CREAT|O_EXCL,0600);
    if (ram_fd>=0) shm_unlink(name);
#elif defined(__ANDROID__)
    ram_fd=android_ram_fd();
#else
    ram_fd=memfd_create("dhd-guest-ram",MFD_CLOEXEC);
#endif
    if (ram_fd<0) return 0;
    if (!sized && ftruncate(ram_fd,RAM_SIZE)<0) { close(ram_fd); ram_fd=-1; return 0; }
    /* MAP_NORESERVE: the 8 GiB reservation must not charge swap; committed
       pages are charged when mprotect commits them. */
    void *p=mmap(NULL,GUEST_SIZE,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
    if (p==MAP_FAILED) { close(ram_fd); ram_fd=-1; return 0; }
    return (uintptr_t)p;
}
/* No placeholder splitting on POSIX: MAP_FIXED overlays the reservation. */
int os_split(uintptr_t at,uint64_t size) {
    (void)at; (void)size; return 1;
}
int os_map_ram(uintptr_t at,uint64_t size,uint64_t file_offset) {
    return mmap((void *)at,size,PROT_READ|PROT_WRITE,
        MAP_SHARED|MAP_FIXED,ram_fd,(off_t)file_offset)!=MAP_FAILED;
}
/* Already a private PROT_NONE reservation from os_reserve. */
int os_private(uintptr_t at,uint64_t size) {
    (void)at; (void)size; return 1;
}
int os_commit(uintptr_t at,uint64_t size) {
    return mprotect((void *)at,(size_t)size,PROT_READ|PROT_WRITE)==0;
}
void os_decommit(const void *at,uint64_t size) {
    /* Release the backing like MEM_DECOMMIT; the reservation stays. */
    madvise((void *)at,(size_t)size,MADV_DONTNEED);
    mprotect((void *)at,(size_t)size,PROT_NONE);
}
int os_unmap_view(void *at,uint64_t size) {
    return munmap(at,(size_t)size)==0;
}
int os_release(void *at,uint64_t size) {
    return munmap(at,(size_t)size)==0;
}
void os_backing_close(void) {
    close(ram_fd); ram_fd=-1;
}
static void write_hex(int fd,unsigned long long v,int width) {
    char digits[16]; int i=0;
    do { digits[i++]="0123456789abcdef"[v&15]; v>>=4; } while (v);
    while (i<width) digits[i++]='0';
    while (i--) {
        char c=digits[i];
        (void)write(fd,&c,1);
    }
}
/* Whether the faulting access wrote: 1 a write, 0 a read, -1 not known. From the CPU's own fault
   record, as Windows gives it in ExceptionInformation[0] (finding 208): the data abort's syndrome
   (ESR, its WnR bit) on arm64, the page-fault error code (its W bit) on x86-64. */
static int fault_wrote(const void *ctx) {
    const ucontext_t *uc=ctx;
    if (!uc) return -1;
#if defined(__APPLE__) && defined(__aarch64__)
    return (int)(uc->uc_mcontext->__es.__esr>>6&1);
#elif defined(__APPLE__) && defined(__x86_64__)
    return (int)(uc->uc_mcontext->__es.__err>>1&1);
#elif defined(__aarch64__)
    /* Linux and Android: the signal frame's records after the registers; ESR_MAGIC's holds it. */
    const uint8_t *p=(const uint8_t *)uc->uc_mcontext.__reserved;
    const uint8_t *end=p+sizeof uc->uc_mcontext.__reserved;
    while (p+8<=end) {
        uint32_t magic, size;
        memcpy(&magic,p,4); memcpy(&size,p+4,4);
        if (!magic || size<8 || size>(size_t)(end-p)) break;
        if (magic==0x45535201u && size>=16) {  /* ESR_MAGIC: struct esr_context { head; __u64 esr; } */
            uint64_t esr; memcpy(&esr,p+8,8);
            uint32_t ec=(uint32_t)(esr>>26&0x3f);
            return ec==0x24 || ec==0x25 ? (int)(esr>>6&1) : -1;  /* data aborts only */
        }
        p+=size;
    }
    return -1;
#elif defined(__x86_64__) && defined(REG_ERR)
    return (int)(uc->uc_mcontext.gregs[REG_ERR]>>1&1);
#else
    return -1;
#endif
}
static void fault(int sig,siginfo_t *info,void *ctx) {
    uintptr_t host=(uintptr_t)info->si_addr;
    /* Not ours: restore the default and re-raise. */
    struct sigaction dfl; memset(&dfl,0,sizeof dfl);
    dfl.sa_handler=SIG_DFL; sigemptyset(&dfl.sa_mask);
    if (!guest_base || host<guest_base || host-guest_base>=GUEST_SIZE) {
#ifdef __ANDROID__
        /* Not ours: the handler before ours (the system's crash dump) gets it, as below, so the
           crash still ends in a tombstone (finding 152). */
        sigaction(sig,sig==SIGBUS ? &old_bus : &old_segv,NULL);
        if (info->si_code<=0) raise(sig);
        return;
#else
        sigaction(sig,&dfl,NULL); raise(sig); return;
#endif
    }
    static const char head[]="host64 memory: access of uncommitted guest address 0x";
    static const char tail[]="\n", read_tail[]=" (a read)\n", write_tail[]=" (a write)\n";
    int wrote=fault_wrote(ctx);
    (void)write(STDERR_FILENO,head,sizeof head-1);
    write_hex(STDERR_FILENO,(unsigned long long)(host-guest_base),9);
    if (wrote==1) (void)write(STDERR_FILENO,write_tail,sizeof write_tail-1);
    else if (wrote==0) (void)write(STDERR_FILENO,read_tail,sizeof read_tail-1);
    else (void)write(STDERR_FILENO,tail,sizeof tail-1);
    /* Raw frames; names need -rdynamic, which the self-test link passes. */
    void *frames[24];
    int n=backtrace(frames,24);
    backtrace_symbols_fd(frames,n,STDERR_FILENO);
    /* Report and exit with the signal, as an unhandled fault would. Android: the handler that
       was there (the system's crash dump, debuggerd) gets the fault when it happens again. */
    {   /* the report reaches dhd.log (and logcat) first: host.c's log, absent in the self-test */
        void host_log_drain(void) __attribute__((weak));
        if (host_log_drain) host_log_drain();
    }
#ifdef __ANDROID__
    sigaction(sig,sig==SIGBUS ? &old_bus : &old_segv,NULL);
    return;
#else
    sigaction(sig,&dfl,NULL);
    raise(sig);
#endif
}
int os_fault_install(void) {
    if (installed) return 1;
    altstack.ss_sp=malloc(SIGSTKSZ);
    if (!altstack.ss_sp) return 0;
    altstack.ss_size=SIGSTKSZ; altstack.ss_flags=0;
    if (sigaltstack(&altstack,NULL)<0) { free(altstack.ss_sp); return 0; }
    struct sigaction sa; memset(&sa,0,sizeof sa);
    sa.sa_sigaction=fault; sigemptyset(&sa.sa_mask);
    sa.sa_flags=SA_SIGINFO|SA_ONSTACK;
    if (sigaction(SIGSEGV,&sa,&old_segv)<0 || sigaction(SIGBUS,&sa,&old_bus)<0)
        return 0;
    installed=1; return 1;
}
void os_fault_remove(void) {
    if (!installed) return;
    sigaction(SIGSEGV,&old_segv,NULL); sigaction(SIGBUS,&old_bus,NULL);
    altstack.ss_flags=SS_DISABLE; sigaltstack(&altstack,NULL);
    free(altstack.ss_sp); altstack.ss_sp=NULL; installed=0;
}
