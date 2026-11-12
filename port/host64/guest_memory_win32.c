/* Windows mapping for guest_memory.c: placeholder reservation, a paging-file
   section for the aliased RAM views, and a vectored report-and-exit handler.
   Policy (which regions are mirrors, what is committed) stays shared. */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "guest_memory_os.h"
#include "../runtime/disc_recent.h"

static HANDLE ram;
static PVOID reporter;
void os_fail(const char *what) {
    fprintf(stderr,"host64 memory: %s (%lu)\n",what,GetLastError()); abort();
}
uintptr_t os_reserve(void) {
    ram=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,0x2000000,NULL);
    if (!ram) return 0;
    uintptr_t base=(uintptr_t)VirtualAlloc2(NULL,NULL,0x200000000ull,
        MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0);
    if (!base) { CloseHandle(ram); ram=NULL; }
    return base;
}
int os_split(uintptr_t at,uint64_t size) {
    return VirtualFree((void *)at,(SIZE_T)size,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER);
}
int os_map_ram(uintptr_t at,uint64_t size,uint64_t file_offset) {
    return MapViewOfFile3(ram,GetCurrentProcess(),(void *)at,file_offset,
        (SIZE_T)size,MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,NULL,0)!=NULL;
}
int os_private(uintptr_t at,uint64_t size) {
    return VirtualAlloc2(NULL,(void *)at,(SIZE_T)size,
        MEM_RESERVE|MEM_REPLACE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)!=NULL;
}
int os_commit(uintptr_t at,uint64_t size) {
    return VirtualAlloc((void *)at,(SIZE_T)size,MEM_COMMIT,PAGE_READWRITE)!=NULL;
}
void os_decommit(const void *at,uint64_t size) {
    VirtualFree((void *)at,(SIZE_T)size,MEM_DECOMMIT);
}
int os_unmap_view(void *at,uint64_t size) {
    (void)size; return UnmapViewOfFile(at);
}
int os_release(void *at,uint64_t size) {
    (void)size; return VirtualFree(at,0,MEM_RELEASE);
}
void os_backing_close(void) {
    CloseHandle(ram); ram=NULL;
}
/* Last vectored handler: the fault guard (graphics.c) runs first. */
static LONG CALLBACK report_fault(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r=e->ExceptionRecord;
    uintptr_t host=r->NumberParameters>=2 ? (uintptr_t)r->ExceptionInformation[1] : 0;
    extern uintptr_t guest_base;
    if (r->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || !guest_base || host<guest_base ||
        host-guest_base>=0x200000000ull) return EXCEPTION_CONTINUE_SEARCH;
    char text[256]; DWORD written;
    int n=snprintf(text,sizeof text,"host64 memory: %s of uncommitted guest address 0x%09llx "
        "at rip %p (image base %p)\n",r->ExceptionInformation[0]==1?"write":"read",
        (unsigned long long)(host-guest_base),r->ExceptionAddress,(void *)GetModuleHandleW(NULL));
    WriteFile(GetStdHandle(STD_ERROR_HANDLE),text,(DWORD)n,&written,NULL);
    /* The callers, as RVAs (host64_rva.py): walk from the faulting context. */
    CONTEXT c=*e->ContextRecord; uintptr_t base=(uintptr_t)GetModuleHandleW(NULL);
    for (int i=0;i<24 && c.Rip;i++) {
        DWORD64 image; PRUNTIME_FUNCTION f=RtlLookupFunctionEntry(c.Rip,&image,NULL);
        n=snprintf(text,sizeof text,"  at rva 0x%llx\n",(unsigned long long)(c.Rip-base));
        WriteFile(GetStdHandle(STD_ERROR_HANDLE),text,(DWORD)n,&written,NULL);
        if (!f) break;
        void *data; DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER,image,c.Rip,f,&c,&data,&frame,NULL);
    }
    {   /* the disc files the game looked up last (finding 208): this handler ends the process
           before the crash report (host.c), which prints the same line on the other hosts */
        const uint8_t *at=(const uint8_t *)(guest_base+PS2_DISC_RECENT);
        MEMORY_BASIC_INFORMATION m; char line[PS2_DISC_RECENT_SIZE+64];
        if (VirtualQuery(at,&m,sizeof m)==sizeof m && m.State==MEM_COMMIT &&
            (uintptr_t)m.BaseAddress+m.RegionSize>=(uintptr_t)at+PS2_DISC_RECENT_SIZE &&
            (n=disc_recent_line(at,line,sizeof line))>0)
            WriteFile(GetStdHandle(STD_ERROR_HANDLE),line,(DWORD)n,&written,NULL);
    }
    TerminateProcess(GetCurrentProcess(),(UINT)EXCEPTION_ACCESS_VIOLATION);
    return EXCEPTION_CONTINUE_SEARCH;
}
int os_fault_install(void) {
    if (reporter) return 1;
    reporter=AddVectoredExceptionHandler(0,report_fault);
    return reporter!=NULL;
}
void os_fault_remove(void) {
    if (reporter) RemoveVectoredExceptionHandler(reporter);
    reporter=NULL;
}
