/* Diagnostic replay of our legacy GS captures, without running the EE/VU. */
#include <windows.h>
#include "gpu.h"
#include "present.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *input;
static void read_bytes(void *p, size_t size)
{
    if (fread(p, 1, size, input) != size) { fprintf(stderr, "Truncated GS dump\n"); exit(1); }
}
static uint32_t read32(void) { uint32_t v; read_bytes(&v, 4); return v; }
static uint64_t read64(void) { uint64_t v; read_bytes(&v, 8); return v; }

static void registers(void)
{
    uint8_t priv[8192]; read_bytes(priv, sizeof priv);
    for (unsigned i = 0; i < sizeof gs.privileged / sizeof *gs.privileged; i++)
        memcpy(gs.privileged + i, priv + i * 16, 8);
}

static void freeze(void)
{
    static const unsigned env[] = {GS_PRIM, GS_PRMODECONT, GS_TEXCLUT, GS_SCANMSK,
        GS_TEXA, GS_FOGCOL, GS_DIMX, GS_DTHE, GS_COLCLAMP, GS_PABE, GS_BITBLTBUF,
        GS_TRXDIR, GS_TRXPOS, GS_TRXREG, GS_TRXREG};
    static const unsigned context[] = {GS_XYOFFSET_1, GS_TEX0_1, GS_TEX1_1, GS_CLAMP_1,
        GS_MIPTBP1_1, GS_MIPTBP2_1, GS_SCISSOR_1, GS_ALPHA_1, GS_TEST_1,
        GS_FBA_1, GS_FRAME_1, GS_ZBUF_1};
    uint32_t crc = read32(), size = read32(), version = read32(); (void)crc;
    if (size != 4194752 || version != 8) { fprintf(stderr, "Expected native version-8 capture\n"); exit(1); }
    for (unsigned i = 0; i < sizeof env / sizeof *env; i++) gs.reg[env[i]] = read64();
    for (unsigned c = 0; c < 2; c++) for (unsigned i = 0; i < sizeof context / sizeof *context; i++)
        gs.reg[context[i] + c] = read64();
    uint64_t v = read64(); gs.vertex.rgba = (uint32_t)v;
    uint32_t q = (uint32_t)(v >> 32); memcpy(&gs.vertex.q, &q, 4);
    read_bytes(&gs.vertex.s, 8); v = read32(); gs.vertex.u = v & 16383; gs.vertex.v = v >> 16 & 16383;
    gs.vertex.fog = read32(); v = read64();
    gs.vertex.x = v & 65535; gs.vertex.y = v >> 16 & 65535; gs.vertex.z = (uint32_t)(v >> 32);
    read64(); read32(); read32(); read_bytes(gs.vram, GS_VRAM_SIZE);
    for (unsigned i = 0; i < 4; i++) {
        uint64_t tag = read64(), regs = read64(); unsigned reg = read32();
        if (i < 3) {
            GsPath *p = gs.path + i; p->left = tag & 32767; p->eop = tag >> 15 & 1;
            p->mode = tag >> 58 & 3; p->nreg = tag >> 60; if (!p->nreg) p->nreg = 16;
            p->regs = regs; p->reg = reg;
        }
    }
    read32(); registers();
    for (unsigned c = 0; c < 2; c++) gs_write(&gs, GS_TEX0_1 + c, gs.reg[GS_TEX0_1 + c]);
}

/* DHD_GS_REPLAY_VRAM=1: each field's whole VRAM as a 64-bit FNV-1a hash in prefix-vram.txt, and
   the last field's VRAM in prefix-vram.bin, to compare with the goldens (gs_golden.py compare replay). */
static FILE *vram_log;
static int no_pictures; /* DHD_GS_REPLAY_TIME: scan out each field but read nothing back */
static void vram_hash(unsigned field)
{
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t *p = (const uint8_t *)gs.vram;
    if (!vram_log) return;
    gs.sync(&gs);
    for (size_t i = 0; i < GS_VRAM_SIZE; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    fprintf(vram_log, "%u %016llX\n", field, (unsigned long long)h);
}

/* DHD_RENDER=native: each field goes to the native renderer (gs_native_vsync) and its picture,
   upscaled with DHD_SCALE, is read back (gs_native_picture), as present.c does for screenshots. */
static void image(const char *prefix, unsigned field)
{
    static uint32_t storage[1024 * 1024]; unsigned w, h;
    uint32_t *pixels = storage;
    vram_hash(field);
    if (gs_gpu_native(&gs)) {
        gs_native_vsync(&gs, &w, &h, 0);
        if (!(pixels = gs_native_picture(&gs, &w, &h))) return;
    } else if (!gs_gpu_display(&gs, &w, &h) || no_pictures || !gs_gpu_capture(&gs, pixels)) return;
    char name[1024]; snprintf(name, sizeof name, "%s-%03u.ppm", prefix, field);
    FILE *f = fopen(name, "wb"); if (!f) { perror(name); exit(1); }
    fprintf(f, "P6\n%u %u\n255\n", w,h);
    for (unsigned i = 0; i < w*h; i++) {
        uint8_t rgb[3] = {(uint8_t)pixels[i], (uint8_t)(pixels[i] >> 8), (uint8_t)(pixels[i] >> 16)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "Usage: replay.exe capture.gs output-prefix\n"); return 1; }
    input = fopen(argv[1], "rb"); if (!input) { perror(argv[1]); return 1; }
    gs_reset(&gs); freeze();
    if (!gs_gpu_init(&gs, NULL)) return 1;  /* the GPU (DXGI's first adapter, gpu_rust.c) */
    char vram_name[1024];
    if (getenv("DHD_GS_REPLAY_VRAM")) {
        snprintf(vram_name, sizeof vram_name, "%s-vram.txt", argv[2]);
        if (!(vram_log = fopen(vram_name, "w"))) { perror(vram_name); return 1; }
    }
    /* DHD_GS_REPLAY_TIME=1: time the replay from the first record until the GPU has finished
       (a whole-VRAM sync), without reading pictures back. */
    no_pictures = getenv("DHD_GS_REPLAY_TIME") != NULL;
    LARGE_INTEGER started, ended, frequency;
    QueryPerformanceCounter(&started);
    unsigned fields = 0; int type;
    while ((type = fgetc(input)) != EOF) {
        if (type == 0) {
            unsigned path = fgetc(input); uint32_t size = read32();
            if (path < 1 || path > 3 || (size & 15)) return 1;
            uint32_t *packet = malloc(size); if (!packet) return 1;
            read_bytes(packet, size); gs_feed(&gs, path, packet, size / 4); free(packet);
        } else if (type == 1) {
            unsigned field = fgetc(input);
            gs.privileged[GS_CSR / 16] = (gs.privileged[GS_CSR / 16] & ~(UINT64_C(1) << 13)) | (uint64_t)field << 13;
            image(argv[2], fields++);
        } else if (type == 2) {
            unsigned count = read32(); uint8_t *data = malloc((size_t)count * 16);
            if (!data) return 1; gs_read_image(&gs, data, count * 16); free(data);
        } else if (type == 3) registers();
        else { fprintf(stderr, "Unknown GS record %d\n", type); return 1; }
    }
    fclose(input);
    if (no_pictures) {
        gs.sync(&gs);
        QueryPerformanceCounter(&ended); QueryPerformanceFrequency(&frequency);
        printf("Replay time %.2f ms\n", (ended.QuadPart - started.QuadPart) * 1000.0 / frequency.QuadPart);
    }
    const char *backend = gs_gpu_backend_name(&gs);
    if (vram_log) {
        gs.sync(&gs);
        fclose(vram_log);
        snprintf(vram_name, sizeof vram_name, "%s-vram.bin", argv[2]);
        FILE *f = fopen(vram_name, "wb");
        if (!f || fwrite(gs.vram, 1, GS_VRAM_SIZE, f) != GS_VRAM_SIZE) { perror(vram_name); return 1; }
        fclose(f);
    }
    gs_gpu_destroy(&gs);
    printf("Replayed %u fields (%s)\n", fields, backend); return 0;
}
