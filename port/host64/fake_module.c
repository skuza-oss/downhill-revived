/* Synthetic integration fixture: no game code or data. Built with wasi-sdk. */
#include "../host/host_api.h"
#include "../gs/gs.h"
#define EXPORT(name) __attribute__((export_name(name)))
static HostFiber root;
static volatile unsigned stage;
static void thread(void *arg) {
    volatile unsigned local=0x13579bdf;
    if ((uintptr_t)&local<0x06000000 || (uintptr_t)&local>=0x07000000) { stage=90; return; }
    if ((uintptr_t)arg!=0x12345678) { stage=91; return; }
    stage=1; host_fiber_switch(root);
    if (local!=0x13579bdf) { stage=92; return; }
    stage=2; host_fiber_switch(root);
}
static volatile unsigned async_done;
static void async_job(void *arg) {
    volatile unsigned local=0xabcdef12;
    if ((uintptr_t)&local<0x06000000 || (uintptr_t)&local>=0x07000000 ||
        host_fiber_current()==root || (uintptr_t)arg!=0x1234) { async_done=99; return; }
    /* Reentrant drains cannot enter another callback on this wasm stack. */
    host_callbacks_drain();
    if (local!=0xabcdef12) { async_done=98; return; }
    async_done++;
}
static int wrong_callback(double d) { return (int)d; }
EXPORT("boundary_negative") int boundary_negative(unsigned test) {
    if (test==1) { host_file_read(0,(void *)0x7fffffff,2); return 1; }
    if (test==2) { host_callback_fiber(0xffffffff,0); return 1; }
    if (test==3) { host_callback_fiber(0,0); return 1; }
    if (test==4) { host_callback_fiber(HOST_CALLBACK(wrong_callback),0); return 1; }
    if (test==5) { host_console_write(0x7fffffff,2); return 1; }
    if (test==7) { host_callback_detach(HOST_CALLBACK(wrong_callback),0); return 1; }
    if (test==8) { host_service_text_input_edit(0x7fffffff,2,(uint32_t)(uintptr_t)"AB"); return 1; }
    if (test==9) { host_service_disc_check_start(0x80000010,0x80000010); return 1; }
    if (test==10) { host_service_disc_check_poll(0x7fffffff,0,0); return 1; }
    if (test==11) { host_service_net_list_lobbies_poll(0x7fffffff,2); return 1; }
    return 2;
}
EXPORT("main") int fake_main(void) {
    char bytes[16]={0};
    HostFile f=host_file_open("fixture.txt",HOST_FILE_READ);
    if (!f || host_file_size(f)!=12 || host_file_read(f,bytes,12)!=12) return 1;
    host_file_close(f);
    const char *want="host64-proof";
    for (int i=0;i<12;i++) if (bytes[i]!=want[i]) return 2;
    double before=host_wall_seconds(); host_sleep_ms(2);
    if (host_wall_seconds()<before) return 3;
    if (!host_ram_create(0x02000000) || !host_ram_view(0x28000000,0,0x02000000,0) ||
        !host_mem_commit(0x70000000,0x10000) || !host_mem_reserved(0x06000000,0x01000000) ||
        /* the windows outside the RAM views that this fixture touches */
        !host_mem_commit(0x003ff000,0x1000) || !host_mem_commit(0x02000000,0x1000) ||
        !host_mem_commit(0x13000000,(uint32_t)offsetof(Gs,sync))) return 4;
    *(volatile unsigned *)0x08001000=0xcafef00d;
    if (*(volatile unsigned *)0x28001000!=0xcafef00d || *(volatile unsigned *)0x38001000!=0xcafef00d) return 5;
    if (!host_ram_view(0x00400000,0x00400000,0x01c00000,1) ||
        !host_ram_view(0x20400000,0x00400000,0x01c00000,0) ||
        !host_ram_view(0x30400000,0x00400000,0x01c00000,0)) return 12;
    *(volatile unsigned *)0x08401000=0x1234beef;
    if (*(volatile unsigned *)0x00401000!=0x1234beef ||
        *(volatile unsigned *)0x20401000!=0x1234beef ||
        *(volatile unsigned *)0x30401000!=0x1234beef) return 13;
    *(volatile unsigned *)0x003ffffc=0x11223344;
    *(volatile unsigned *)0x08400000=0xaabbccdd;
    if (*(volatile uint64_t *)0x003ffffc!=UINT64_C(0xaabbccdd11223344)) return 14;
    *(volatile unsigned *)0x09fffffc=0x55667788;
    *(volatile unsigned *)0x02000000=0x99aabbcc;
    if (*(volatile uint64_t *)0x01fffffc!=UINT64_C(0x99aabbcc55667788)) return 15;
    root=host_fiber_main(); HostFiber child=host_callback_fiber(HOST_CALLBACK(thread),0x12345678);
    if (!child) return 6;
    host_fiber_switch(child); if (stage!=1) return 7;
    uint32_t ranges[4][2]; int n=host_stack_ranges_copy((uint32_t)(uintptr_t)ranges,4);
    if (n!=1 || ranges[0][0]!=0x06000000 || ranges[0][1]!=0x06040000) return 8;
    host_fiber_switch(child); if (stage!=2) return 9;
    host_fiber_delete(child);
    if (host_fiber_current()!=root) return 10;
    if (!host_callback_detach(HOST_CALLBACK(async_job),0x1234)) return 16;
    /* A producer may post, but cannot run module code before the owner drains. */
    host_sleep_ms(10); if (async_done) return 17;
    for (unsigned i=0;i<1000 && !async_done;i++) { host_callbacks_drain(); host_sleep_ms(1); }
    if (async_done!=1 || host_fiber_current()!=root) return 18;
    host_service_gs_reset();
    host_service_gs_priv_write(GS_PMODE,0x1234);
    Gs *snapshot=(Gs *)(uintptr_t)0x13000000;
    host_service_gs_snapshot((uint32_t)(uintptr_t)snapshot,(uint32_t)offsetof(Gs,sync));
    if (snapshot->privileged[0]!=0x1234 || snapshot->reg[GS_TRXREG]!=host_service_gs_register(GS_TRXREG)) return 20;
    char edited[8]="A";
    host_service_text_input_begin();
    if (host_service_text_input_edit((uint32_t)(uintptr_t)edited,sizeof edited,(uint32_t)(uintptr_t)"ABC") ||
        edited[0]!='A' || edited[1]) return 21;
    host_service_text_input_end();
    static const int16_t pcm[4]={123,-123,456,-456};
    host_sound_reset(); host_sound_write(pcm,2); host_sound_tick();
    /* The async services: the blocking part runs on a host thread, only the
       answer and rows cross. The fixture has no lobby server, so the fetch is
       expected to fail; fixture.txt has no ISO 9660 file system, so the disc
       check answers -2 and says why. */
    if (!host_service_disc_check_start((uint32_t)(uintptr_t)"fixture.txt",(uint32_t)(uintptr_t)"fixture-check.txt")) return 22;
    if (host_service_disc_check_start((uint32_t)(uintptr_t)"fixture.txt",(uint32_t)(uintptr_t)"fixture-check.txt")) return 23;
    static uint8_t digest[32];
    static char note[128];
    unsigned waited=0;
    int checked;
    while (!(checked=host_service_disc_check_poll((uint32_t)(uintptr_t)digest,(uint32_t)(uintptr_t)note,sizeof note))) {
        if (++waited>10000) return 24;
        host_sleep_ms(1);
    }
    if (host_service_disc_check_poll((uint32_t)(uintptr_t)digest,(uint32_t)(uintptr_t)note,sizeof note)!=-1) return 25;
    static const char why[]="it isn't a disc image (no ISO 9660 volume in it)";
    if (checked!=-2) return 26;
    for (unsigned i=0;i<sizeof why;i++) if (note[i]!=why[i]) return 26;
    if (host_service_net_list_lobbies_poll((uint32_t)(uintptr_t)digest,2)!=-1) return 27;
    if (!host_service_net_list_lobbies_start(8)) return 28;
    waited=0;
    int lobbies;
    while ((lobbies=host_service_net_list_lobbies_poll((uint32_t)(uintptr_t)digest,2))==-2) {
        if (++waited>10000) return 29;
        host_sleep_ms(1);
    }
    if (lobbies!=-1) return 30;
    const char *value=host_getenv("DHD_HEADLESS");
    if (!value || value[0]!='1') return 11;
    return 0;
}

/* Synthetic reverse-export shape; no retail disc or validation data. */
EXPORT("port_disc_validate") uint32_t fixture_disc_validate(uint32_t path,uint32_t error,uint32_t size) {
    (void)path;
    if (size) *(char *)(uintptr_t)error=0;
    return 0;
}
