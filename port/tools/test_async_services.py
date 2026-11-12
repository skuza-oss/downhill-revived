"""Async host services regression (finding 082 task): the disc check and the lobby list.

Builds a small native harness against port/host/host_sdl.c (with SDL3, sdl3.py's x64 build),
port/net/* and the Rust library (dhd_disc, dhd_net), then runs it: the disc check hashes a small
ISO 9660 image's files (busy -> done, the files hash, then iso-check.txt's answer, failure paths;
finding 098) and the lobby service fetches from a local HTTP stub (busy -> rows, correct parse,
truncation, failure).
No game data, no visible windows; all output stays under build/.
"""
import hashlib
import http.server
import struct
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build' / 'port' / 'async-test'
LLVM = (Path.home() / 'scoop/apps/llvm/current/bin')
TARGET = 'x86_64-pc-windows-msvc'

LOBBIES = {
    "lobbies": [
        {"code": "ABC123", "roomName": "ALPINE CUP", "version": "v9-test",
         "hostName": "HOST1", "track": 3, "players": 2, "maxPlayers": 4},
        {"code": "XYZ789", "roomName": "MOAB DASH", "version": "v9-test",
         "hostName": "HOST2", "track": 1, "players": 4, "maxPlayers": 4},
    ]
}

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "host_api.h"
#include "net.h"
#include "guest.h"

uintptr_t guest_base;
/* host_sdl.c's display reads go through the SDL thread (sdl_host.c): none here */
int sdl_host_run(void (*fn)(void *), void *arg) { (void)fn; (void)arg; return 0; }
/* host_win32.c's UTF-8 open (host_os.h), for the hashed file's ASCII path here */
FILE *host_os_fopen(const char *path, const char *mode) { return fopen(path, mode); }

#define DIGEST_AT 0x1000u
#define ROWS_AT 0x2000u

static int failures;
#define CHECK(cond, label) \
    do { \
        if (cond) printf("PASS %s\n", label); \
        else { printf("FAIL %s\n", label); failures++; } \
    } while (0)

#define NOTE_AT 0x1100u

static int poll_disc(void)
{
    return host_service_disc_check_poll(DIGEST_AT, NOTE_AT, 256);
}

/* Copies a host string into the guest arena; the services take guest addresses. */
static uint32_t put_string(const char *s)
{
    static uint32_t next = 0x100u;
    size_t n = strlen(s) + 1;
    char *at = (char *)GUEST(next);
    memcpy(at, s, n);
    next += (uint32_t)n;
    return next - (uint32_t)n;
}

static int wait_disc(void)
{
    int r, waited = 0;
    while ((r = poll_disc()) == 0 && waited < 120000) {
        Sleep(5);
        waited += 5;
    }
    return r;
}

static void disc_tests(const char *image, const char *expected, const char *cache)
{
    char missing[1024];
    uint8_t *digest = (uint8_t *)GUEST(DIGEST_AT);
    const char *note = (const char *)GUEST(NOTE_AT);
    uint32_t image_at, missing_at, cache_at;
    int r, saw_busy;
    snprintf(missing, sizeof missing, "%s-no-such-file", image);
    image_at = put_string(image);
    missing_at = put_string(missing);
    cache_at = put_string(cache);
    CHECK(host_service_disc_check_start(missing_at, cache_at) == 1, "disc-missing-start");
    CHECK(wait_disc() == -1 && note[0], "disc-missing-failed");
    CHECK(poll_disc() == -1, "disc-idle-failed");
    CHECK(host_service_disc_check_start(image_at, cache_at) == 1, "disc-start");
    r = poll_disc();
    saw_busy = r == 0;
    if (saw_busy) CHECK(host_service_disc_check_start(image_at, cache_at) == 0, "disc-start-while-busy");
    if (r == 0) r = wait_disc();
    CHECK(saw_busy, "disc-saw-busy");
    CHECK(r == 1, "disc-hashed");
    if (r == 1) {
        char got[65];
        for (int i = 0; i < 32; i++) snprintf(got + 2 * i, 3, "%02x", digest[i]);
        printf("NOTE %s\n", note);
        CHECK(!strcmp(got, expected), "disc-files-hash");
        CHECK(!strcmp(note, "BIG.BIN isn't the USA disc's; 2 files, the USA disc has 2338"), "disc-note");
    }
    CHECK(poll_disc() == -1, "disc-idle-after-done");
    memset(digest, 0, 32);
    CHECK(host_service_disc_check_start(image_at, cache_at) == 1, "disc-restart");
    CHECK(wait_disc() == 2, "disc-from-cache");
    {
        char got[65];
        for (int i = 0; i < 32; i++) snprintf(got + 2 * i, 3, "%02x", digest[i]);
        CHECK(!strcmp(got, expected), "disc-cached-hash");
    }
}

static int poll_lobbies(uint32_t at, int max)
{
    return host_service_net_list_lobbies_poll(at, max);
}

static void lobby_tests(const char *base, const char *closed_base)
{
    NetLobbyInfo *rows = (NetLobbyInfo *)GUEST(ROWS_AT);
    int r, waited, saw_busy;
    CHECK(poll_lobbies(ROWS_AT, 8) == -1, "lobby-idle-failed");
    CHECK(host_service_net_list_lobbies_start(0) == 0, "lobby-start-zero");
    CHECK(host_service_net_list_lobbies_start(1000) == 0, "lobby-start-huge");
    net_init(base, "TESTER", "v9-test",
             "0123456789012345678901234567890123456789012345678901234567890123");
    CHECK(host_service_net_list_lobbies_start(8) == 1, "lobby-start");
    r = poll_lobbies(ROWS_AT, 8);
    saw_busy = (r == -2);
    if (r == -2)
        CHECK(host_service_net_list_lobbies_start(8) == 0, "lobby-start-while-busy");
    waited = 0;
    while (r == -2 && waited < 60000) {
        Sleep(10);
        waited += 10;
        r = poll_lobbies(ROWS_AT, 8);
    }
    CHECK(saw_busy, "lobby-saw-busy");
    CHECK(r == 2, "lobby-count");
    if (r == 2) {
        printf("ROW %s|%s|%s|%s|%d|%d|%d\n", rows[0].code, rows[0].room_name, rows[0].version,
               rows[0].host_name, rows[0].track, rows[0].players, rows[0].max_players);
        printf("ROW %s|%s|%s|%s|%d|%d|%d\n", rows[1].code, rows[1].room_name, rows[1].version,
               rows[1].host_name, rows[1].track, rows[1].players, rows[1].max_players);
        CHECK(!strcmp(rows[0].code, "ABC123") && !strcmp(rows[0].room_name, "ALPINE CUP") &&
              !strcmp(rows[0].version, "v9-test") && !strcmp(rows[0].host_name, "HOST1") &&
              rows[0].track == 3 && rows[0].players == 2 && rows[0].max_players == 4,
              "lobby-row0");
        CHECK(!strcmp(rows[1].code, "XYZ789") && !strcmp(rows[1].room_name, "MOAB DASH") &&
              !strcmp(rows[1].version, "v9-test") && !strcmp(rows[1].host_name, "HOST2") &&
              rows[1].track == 1 && rows[1].players == 4 && rows[1].max_players == 4,
              "lobby-row1");
    }
    CHECK(host_service_net_list_lobbies_start(1) == 1, "lobby-restart-1");
    waited = 0;
    r = -2;
    while (r == -2 && waited < 60000) {
        Sleep(10);
        waited += 10;
        r = poll_lobbies(ROWS_AT, 1);
    }
    CHECK(r == 1, "lobby-truncated");
    if (r == 1) CHECK(!strcmp(rows[0].code, "ABC123"), "lobby-truncated-row");
    net_init(closed_base, "TESTER", "v9-test",
             "0123456789012345678901234567890123456789012345678901234567890123");
    CHECK(host_service_net_list_lobbies_start(8) == 1, "lobby-fail-start");
    waited = 0;
    r = -2;
    while (r == -2 && waited < 60000) {
        Sleep(10);
        waited += 10;
        r = poll_lobbies(ROWS_AT, 8);
    }
    CHECK(r == -1, "lobby-fail-count");
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <image> <expected-hex> <base-url> <closed-base-url>\n", argv[0]);
        return 2;
    }
    guest_base = (uintptr_t)calloc(1, 1 << 20);
    if (!guest_base) {
        fprintf(stderr, "no guest arena\n");
        return 2;
    }
    {
        char cache[1100];
        snprintf(cache, sizeof cache, "%s-iso-check.txt", argv[1]);
        remove(cache);
        disc_tests(argv[1], argv[2], cache);
    }
    lobby_tests(argv[3], argv[4]);
    printf(failures ? "RESULT FAIL %d\n" : "RESULT PASS\n", failures);
    return failures != 0;
}
'''


class Stub(http.server.BaseHTTPRequestHandler):
    delay = 2.0
    body = b'{}'

    def do_GET(self):
        if self.path != '/v1/lobbies':
            self.send_response(404)
            self.end_headers()
            return
        time.sleep(self.delay)
        data = self.body
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def iso_image(big):
    """A small ISO 9660 image, SYSTEM.CNF and BIG.BIN in the root, and its game files hash by the
    rule (dhd_disc's hash: each file's path, 0, size as 8 bytes little-endian and contents'
    SHA-256, in path order, all hashed)."""
    cnf = b'BOOT2 = cdrom0:\\SCUS_971.77;1\r\n'
    files = [('SYSTEM.CNF', 20, cnf), ('BIG.BIN', 21, big)]
    img = bytearray(21 * 2048)
    img[16 * 2048:16 * 2048 + 6] = b'\x01CD001'
    struct.pack_into('<I', img, 16 * 2048 + 158, 18)
    struct.pack_into('<I', img, 16 * 2048 + 166, 2048)
    records = b''
    for name, at, data in [('\0', 18, None), ('\1', 18, None)] + files:
        n = name.encode() + (b';1' if data is not None else b'')
        r = bytearray(33 + len(n) + (len(n) + 1) % 2)
        r[0], r[25], r[32] = len(r), 2 if data is None else 0, len(n)
        struct.pack_into('<I', r, 2, at)
        struct.pack_into('<I', r, 10, 2048 if data is None else len(data))
        r[33:33 + len(n)] = n
        records += r
    img[18 * 2048:18 * 2048 + len(records)] = records
    img[20 * 2048:20 * 2048 + len(cnf)] = cnf
    img += big + bytes(2048)
    rows = sorted(p.encode() + b'\0' + struct.pack('<Q', len(d)) + hashlib.sha256(d).digest() for p, _, d in files)
    return bytes(img), hashlib.sha256(b''.join(rows)).hexdigest()


def main():
    import json as jsonlib
    OUT.mkdir(parents=True, exist_ok=True)
    data = OUT / 'async-disc.iso'
    image, expected = iso_image(os.urandom(40 << 20))
    data.write_bytes(image)
    Stub.body = jsonlib.dumps(LOBBIES).encode()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Stub)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    closed = free_port()
    harness = OUT / 'async-harness.c'
    harness.write_text(HARNESS, newline='')
    exe = OUT / 'async-harness.exe'
    # Inherit the environment (explicit env dicts break spawning here): extend
    # it in place instead.
    os.environ['PATH'] = str(LLVM) + os.pathsep + os.environ.get('PATH', '')
    os.environ.update(TMP=str(OUT), TEMP=str(OUT), TMPDIR=str(OUT))
    sys.path.insert(0, str(ROOT / 'port' / 'tools'))
    import sdl3
    sdl_lib, sdl_include = sdl3.build()
    sources = [harness, ROOT / 'port/host/host_sdl.c', ROOT / 'port/host/test_host_end_stub.c', ROOT / 'port/net/net.c', ROOT / 'port/net/client.c', ROOT / 'port/net/json.c',
               ROOT / 'port/net/hash.c']
    # the WebSocket/HTTP client (ws.h), dhd_net, and the disc check, dhd_disc: inside the Rust
    # library rustgs.py builds
    import rustgs
    rust = rustgs.build(rustgs.X64)
    cmd = ['clang', '--target=' + TARGET, '-O1', '-w', '-fms-runtime-lib=static',
           '-I' + str(ROOT / 'port' / 'include'), '-I' + str(ROOT / 'port' / 'runtime'),
           '-I' + str(ROOT / 'port' / 'host'), '-I' + str(ROOT / 'port' / 'net'), '-I' + str(sdl_include),
           *[str(s) for s in sources], '-o', str(exe), str(sdl_lib), rust['lib'],
           *['-l' + x[:-4] for x in dict.fromkeys(sdl3.SYSTEM_LIBS + ['user32.lib'] +
                                                  [l for l in rust['native_static_libs'] if l.endswith('.lib')])]]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8',
                       errors='replace', timeout=300)
    if r.returncode:
        raise SystemExit('harness build failed:\n' + r.stdout + r.stderr)
    base = f'http://127.0.0.1:{port}'
    args = [str(exe), str(data), expected, base, f'http://127.0.0.1:{closed}/']
    r = subprocess.run(args, capture_output=True, text=True, encoding='utf-8',
                       errors='replace', timeout=300)
    print(r.stdout, end='')
    print(r.stderr, end='', file=sys.stderr)
    server.shutdown()
    if r.returncode:
        raise SystemExit('harness failed')
    if '\nFAIL' in ('\n' + r.stdout) or 'RESULT FAIL' in r.stdout:
        raise SystemExit('harness reported failures')
    if 'RESULT PASS' not in r.stdout:
        raise SystemExit('harness did not report PASS')
    print(f'PASS: async services against {data.name} and {base}')


if __name__ == '__main__':
    main()
