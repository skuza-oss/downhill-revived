#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "../host/backtrace_posix.h"  /* <execinfo.h>, or Android's own (API < 33) */
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exports_gen.h>
#include "bridge.h"
#include "core_host.h"
#include "memory_layout.h"
#include "wasm-rt-impl.h"
static w2c_core core;
#ifndef HOST64_FAKE_MODULE
/* WASM_RT_TRAP_HANDLER (build_host64.py): a trap can come from any EE fiber,
   whose native stack is not main's, so it cannot longjmp back. Report the
   trap and end the process with 0x7700 + the trap code. */
void host64_trap(wasm_rt_trap_t code) {
#ifdef _WIN32
    void *frames[48]; char text[160]; DWORD written;
    USHORT n=RtlCaptureStackBackTrace(1,48,frames,NULL);
    HANDLE err=GetStdHandle(STD_ERROR_HANDLE);
    uintptr_t base=(uintptr_t)GetModuleHandleW(NULL);
    int k=snprintf(text,sizeof text,"host64 trap: %s (EE fiber %d, wasm depth %u)\n",
                   wasm_rt_strerror(code),host64_fiber_id(),wasm_rt_call_stack_depth);
    WriteFile(err,text,(DWORD)k,&written,NULL);
    for (USHORT i=0;i<n;i++) {
        k=snprintf(text,sizeof text,"  at rva 0x%llx\n",(unsigned long long)((uintptr_t)frames[i]-base));
        WriteFile(err,text,(DWORD)k,&written,NULL);
    }
    TerminateProcess(GetCurrentProcess(),0x7700u+(UINT)code);
#else
    void *frames[48]; int n=backtrace(frames,48);
    dprintf(STDERR_FILENO,"host64 trap: %s (EE fiber %d, wasm depth %u)\n",
            wasm_rt_strerror(code),host64_fiber_id(),wasm_rt_call_stack_depth);
    backtrace_symbols_fd(frames,n,STDERR_FILENO);
    {
        void host_log_drain(void) __attribute__((weak));
        if (host_log_drain) host_log_drain();
    }
    _exit(0x7700u+(unsigned)code);
#endif
    for (;;) {}
}
#endif
typedef struct { int argc; char **argv; } Args;
/* The game: the module instantiated and run. On a thread of its own (sdl_host.c: the process's
   main thread runs SDL's window and events); the fixture build (--fake) runs it on main. */
static int game(void *start) {
    Args *args=start; int argc=args->argc; char **argv=args->argv;
    struct w2c_env env;
    wasm_rt_init(); host64_exports_bind(&env,&core);
    wasm2c_core_instantiate(&core,&env);
    host64_bridge_bind(w2c_core_memory(&core));
    host64_fiber_bind(w2c_core_0x5F_stack_pointer(&core));
    if (!host64_memory_commit_stack(*w2c_core_0x5F_stack_pointer(&core))) {
        fprintf(stderr,"host64: module shadow stack at 0x%08x is neither below the low RAM view"
                " nor above 0x%llx\n",*w2c_core_0x5F_stack_pointer(&core),HOST64_MODULE_BASE);
        return 3;
    }
    extern int hle_verbose;
    hle_verbose=getenv("DHD_LOG")!=NULL;
#ifdef HOST64_FAKE_MODULE
    if (argc==3 && !strcmp(argv[1],"--negative")) {
        unsigned test=(unsigned)strtoul(argv[2],NULL,0);
        WASM_RT_SAVE_STACK_DEPTH();
        wasm_rt_trap_t trap=WASM_RT_SETJMP(g_wasm_rt_jmp_buf);
        if (!trap) {
            (void)env.exports.w2c_core_boundary_negative(&core,test);
            fprintf(stderr,"FAIL: boundary negative %u did not trap\n",test); return 1;
        }
        wasm_rt_trap_t expected=(test==1 || test==5 || test==6 || test==8 || test==9 || test==10 || test==11) ? WASM_RT_TRAP_OOB : WASM_RT_TRAP_CALL_INDIRECT;
        if (trap!=expected) { fprintf(stderr,"FAIL: wrong boundary trap %d\n",trap); return 1; }
        printf("PASS: boundary negative %u caught %s\n",test,wasm_rt_strerror(trap));
        wasm2c_core_free(&core); wasm_rt_free(); return 0;
    }
#else
    (void)argc; (void)argv;
#endif
    int code=host64_module_main(&env);
    if (host_fiber_current()) host64_fiber_shutdown();
    wasm2c_core_free(&core); wasm_rt_free();
    printf("host64 module exit: %d\n",code);
    return code;
}
int sdl_host_main(int (*game)(void *),void *arg);  /* port/host/sdl_host.c */
void touch_host_args(int argc,char **argv);  /* port/host/touch_host.c: --touch-editor (finding 226) */
#ifndef HOST64_FAKE_MODULE
#include "../host/host_end.h"
#endif
int main(int argc,char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
#endif
    Args args={argc,argv};
#ifdef HOST64_FAKE_MODULE
    return game(&args);  /* the boundary fixture: no window, no SDL */
#else
    touch_host_args(argc,argv);
    int code=sdl_host_main(game,&args);
#ifdef __ANDROID__
    return code;  /* android_main.c logs it and ends the same way */
#else
    host_end(code);  /* the game returned: the same end as EXIT GAME (finding 210) */
#endif
#endif
}
