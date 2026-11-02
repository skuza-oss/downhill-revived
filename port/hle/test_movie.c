/* Plays one movie of the player's disc through movie.c without the game, on a simulated field
   clock (each field waits for the decoder), and writes chosen pictures as BMP and the sound as a
   WAV: a check of the demuxer, libmpeg2, the colour conversion and the sound's timing (finding
   055). Output is game data: keep it under build/.

   build: clang --target=x86_64-pc-windows-msvc -O2 -DMOVIE_TEST -I port/runtime -I <SDL3 include>
          -I port/third_party/libmpeg2 port/hle/test_movie.c port/hle/movie.c
          port/third_party/libmpeg2/{alloc,cpu_accel,cpu_state,decode,header,idct,motion_comp,slice}.c
          <sdl3.py's SDL3-static.lib and its SYSTEM_LIBS>
   usage: test_movie ISO LSN SIZE AUDIO OUT_PREFIX [FIELD...]   (fields of 59.94 Hz) */
#include "hle.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

int movie_open(const char *iso, uint32_t lsn, uint32_t size, int audio, const char *name);
void movie_close(void);
int movie_over(void);
void movie_audio(int32_t *mix, int frames);
const uint32_t *movie_picture(unsigned *width, unsigned *height, unsigned *rows);
int movie_test_ready(void);

void hle_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void host_thread_name(const char *name) { (void)name; }
FILE *host_os_fopen(const char *path, const char *mode) { return fopen(path, mode); }  /* ASCII paths here */
int cdvd_find(const char *path, uint32_t *lsn, uint32_t *size) { return 0; }

static void bmp(const char *path, const uint32_t *image, unsigned w, unsigned h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    BITMAPFILEHEADER file = {0};
    BITMAPINFOHEADER info = {0};
    file.bfType = 0x4D42; file.bfOffBits = sizeof file + sizeof info; file.bfSize = file.bfOffBits + w * h * 4;
    info.biSize = sizeof info; info.biWidth = w; info.biHeight = -(LONG)h; info.biPlanes = 1; info.biBitCount = 32;
    fwrite(&file, sizeof file, 1, f); fwrite(&info, sizeof info, 1, f);
    for (unsigned i = 0; i < w * h; i++) {
        uint32_t v = image[i], bgra = (v & 0xFF00FF00) | (v & 255) << 16 | (v >> 16 & 255);
        fwrite(&bgra, 4, 1, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage: test_movie ISO LSN SIZE AUDIO OUT_PREFIX [FIELD...]\n"); return 2; }
    char path[1024];
    snprintf(path, sizeof path, "%s.wav", argv[5]);
    FILE *wav = fopen(path, "wb");
    uint8_t header[44] = {0};
    fwrite(header, 1, 44, wav);
    uint32_t samples = 0;
    double start = (double)GetTickCount64();
    if (!movie_open(argv[1], (uint32_t)strtoul(argv[2], 0, 0), (uint32_t)strtoul(argv[3], 0, 0), atoi(argv[4]), argv[5])) {
        fprintf(stderr, "movie_open failed\n");
        return 1;
    }
    uint64_t field = 0, rendered = 0;
    uint32_t last_hash = 0;
    unsigned changes = 0;
    while (!movie_over()) {
        while (!movie_test_ready()) Sleep(1);
        uint64_t due = (field + 1) * 4004 / 5 - rendered;
        int32_t mix[1024 * 2] = {0};
        movie_audio(mix, (int)due);
        for (uint64_t i = 0; i < due * 2; i++) {
            int16_t s = (int16_t)(mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i]);
            fwrite(&s, 2, 1, wav);
        }
        rendered += due; samples += (uint32_t)due;
        unsigned w, h, rows;
        const uint32_t *p = movie_picture(&w, &h, &rows);
        uint32_t hash = 2166136261u;
        for (unsigned i = 0; i < w * h; i += 97) hash = (hash ^ p[i]) * 16777619u;
        if (hash != last_hash) changes++, last_hash = hash;
        for (int a = 6; a < argc; a++)
            if ((uint64_t)atoi(argv[a]) == field) {
                snprintf(path, sizeof path, "%s-f%05llu.bmp", argv[5], (unsigned long long)field);
                bmp(path, p, w, h);
            }
        field++;
    }
    movie_close();
    uint32_t bytes = samples * 4;
    memcpy(header, "RIFF", 4); *(uint32_t *)(header + 4) = 36 + bytes; memcpy(header + 8, "WAVEfmt ", 8);
    *(uint32_t *)(header + 16) = 16; *(uint16_t *)(header + 20) = 1; *(uint16_t *)(header + 22) = 2;
    *(uint32_t *)(header + 24) = 48000; *(uint32_t *)(header + 28) = 48000 * 4; *(uint16_t *)(header + 32) = 4;
    *(uint16_t *)(header + 34) = 16; memcpy(header + 36, "data", 4); *(uint32_t *)(header + 40) = bytes;
    fseek(wav, 0, SEEK_SET); fwrite(header, 1, 44, wav); fclose(wav);
    printf("fields %llu (%.2f s at 59.94), picture changes %u, sound %.2f s, wall %.2f s\n", (unsigned long long)field,
           field * 1001 / 60000.0, changes, samples / 48000.0, ((double)GetTickCount64() - start) / 1000);
    return 0;
}
