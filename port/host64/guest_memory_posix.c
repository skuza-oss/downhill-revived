/* POSIX mapping for guest_memory.c: one PROT_NONE reservation, a memfd RAM
   section (macOS: an unlinked shared memory object) aliased with MAP_FIXED|MAP_SHARED, commit-on-demand with mprotect
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
#include <unistd.h>
#include "core_host.h"  /* guest_base, for the fault bounds check */
#include "guest_memory_os.h"

#define GUEST_SIZE 0x200000000ull
#define RAM_SIZE 0x2000000ull

static int ram_fd=-1;
static stack_t altstack;
static struct sigaction old_segv, old_bus;
static int installed;

void os_fail(const char *what) {
    fprintf(stderr,"host64 memory: %s (%d: %s)\n",what,errno,strerror(errno)); abort();
}
uintptr_t os_reserve(void) {
#ifdef __APPLE__  /* no memfd: a POSIX shared memory object, unlinked at once */
    char name[32]; snprintf(name,sizeof name,"/dhd-guest-ram-%d",(int)getpid());
    ram_fd=shm_open(name,O_RDWR|O_CREAT|O_EXCL,0600);
    if (ram_fd>=0) shm_unlink(name);
#else
    ram_fd=memfd_create("dhd-guest-ram",MFD_CLOEXEC);
#endif
    if (ram_fd<0) return 0;
    if (ftruncate(ram_fd,RAM_SIZE)<0) { close(ram_fd); ram_fd=-1; return 0; }
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
static void fault(int sig,siginfo_t *info,void *ctx) {
    (void)ctx;
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
    static const char tail[]="\n";
    (void)write(STDERR_FILENO,head,sizeof head-1);
    write_hex(STDERR_FILENO,(unsigned long long)(host-guest_base),9);
    (void)write(STDERR_FILENO,tail,sizeof tail-1);
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
