/* The CD/DVD drive (IOP module cdvdfsv's RPC servers), reading the user's own disc image: the
   ISO named by the DHD_ISO environment variable (finding 020). Command numbers and buffers as
   Sony's libcdvd sends them (ps2sdk's ee/rpc/cdvd documents the same protocol). */
#include "hle.h"
#include "../runtime/portable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR 2048

static HostFile iso = HOST_FILE_NONE;

static int read_sectors(uint32_t lsn, uint32_t count, void *dst)
{
    if (iso == HOST_FILE_NONE) {
        const char *path = host_getenv("DHD_ISO");
        if (!path || (iso = host_file_open(path, HOST_FILE_READ)) == HOST_FILE_NONE) {
            hle_log("cdvd: set DHD_ISO to the path of your Downhill Domination (USA) disc image\n");
            exit(2);
        }
    }
    if (host_file_seek(iso, (uint64_t)lsn * SECTOR) != 0)
        return 0;
    graphics_settle();  /* the OS writes dst: no guarded pages */
    return host_file_read(iso, dst, (size_t)SECTOR * count) == (int64_t)SECTOR * count;
}

/* ---- ISO 9660: sceCdSearchFile's lookup ---- */
typedef struct {               /* sceCdlFILE */
    uint32_t lsn, size;
    char name[16];
    uint8_t date[8];
    uint32_t flag;
} CdFile;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Finds `name` (no version) in the directory at lsn/size; fills lsn/size, nonzero if found. */
static int find_entry(uint32_t *lsn, uint32_t *size, const char *name, size_t len, int *is_dir, uint8_t *date)
{
    uint8_t sec[SECTOR];
    for (uint32_t off = 0; off < *size; off += SECTOR) {
        if (!read_sectors(*lsn + off / SECTOR, 1, sec))
            return 0;
        for (uint32_t i = 0; i < SECTOR && sec[i];) {
            const uint8_t *r = sec + i;
            size_t n = r[32];
            const char *id = (const char *)r + 33;
            size_t idlen = n;
            for (size_t k = 0; k < n; k++)
                if (id[k] == ';') {
                    idlen = k;
                    break;
                }
            if (idlen && id[idlen - 1] == '.')  /* "NAME." for a name without extension */
                idlen--;
            if (idlen == len && dhd_strnicmp(id, name, len) == 0) {
                *lsn = le32(r + 2);
                *size = le32(r + 10);
                *is_dir = (r[25] & 2) != 0;
                if (date)
                    memcpy(date, r + 18, 7);
                return 1;
            }
            i += r[0];
        }
    }
    return 0;
}

/* "\\DIR\\FILE.EXT;1" -> its first sector and size. */
static int search_file(const char *path, CdFile *out)
{
    uint8_t pvd[SECTOR];
    if (!read_sectors(16, 1, pvd))
        return 0;
    uint32_t lsn = le32(pvd + 156 + 2), size = le32(pvd + 156 + 10);
    int is_dir = 1;
    uint8_t date[8] = {0};
    const char *p = path;
    while (*p == '\\' || *p == '/')
        p++;
    while (*p) {
        size_t len = strcspn(p, "\\/;");
        if (!is_dir || !find_entry(&lsn, &size, p, len, &is_dir, date))
            return 0;
        p += len;
        if (*p == ';')
            break;
        while (*p == '\\' || *p == '/')
            p++;
    }
    out->lsn = lsn;
    out->size = size;
    const char *base = strrchr(path, '\\');
    snprintf(out->name, sizeof out->name, "%s", base ? base + 1 : path);
    memcpy(out->date, date, sizeof out->date);
    out->flag = 0;
    return 1;
}

/* ---- servers ---- */
void rpc_cd_init(int fno, void *send, int ssize, void *recv, int rsize)
{
    (void)fno, (void)send, (void)ssize;
    memset(recv, 0, (size_t)rsize);
}

enum { SCECdComplete = 2 };

void rpc_cd_diskready(int fno, void *send, int ssize, void *recv, int rsize)
{
    (void)fno, (void)send, (void)ssize;
    memset(recv, 0, (size_t)rsize);
    *(int32_t *)recv = SCECdComplete;
}

/* S-commands: status queries; all succeed. */
enum { SCMD_GETDISKTYPE = 3, SCMD_GETERROR = 4, SCMD_STATUS = 12 };

void rpc_cd_scmd(int fno, void *send, int ssize, void *recv, int rsize)
{
    int32_t *r = recv;
    memset(recv, 0, (size_t)rsize);
    switch (fno) {
    case SCMD_GETDISKTYPE:
        r[0] = 0x14;  /* PS2 DVD */
        break;
    case SCMD_GETERROR:
        r[0] = 0;
        break;
    case SCMD_STATUS:
        r[0] = 0x0A;  /* spinning, paused */
        break;
    default:
        if (hle_verbose)
            hle_log("cdvd S-command %d (send %d)\n", fno, ssize);
        r[0] = 1;
        if (send && ssize >= 4 && rsize >= 8)
            r[1] = 0;
    }
}

/* N-commands: reads and the stream. */
enum { NCMD_READ = 1, NCMD_DVDREAD = 3, NCMD_SEEK = 5, NCMD_STANDBY, NCMD_STOP, NCMD_PAUSE, NCMD_STREAM,
       NCMD_DISKREADY = 14 };
enum { ST_START = 1, ST_READ, ST_STOP, ST_SEEK, ST_INIT, ST_STAT, ST_PAUSE, ST_RESUME, ST_SEEKF };

static uint32_t stream_lsn;
static int streaming;

static uint32_t stream(const uint32_t *a)
{
    switch (a[3]) {
    case ST_START:
    case ST_SEEK:
    case ST_SEEKF:
        stream_lsn = a[0];
        streaming = 1;
        return 1;
    case ST_READ: {
        uint32_t n = a[1];
        if (!streaming || !read_sectors(stream_lsn, n, (void *)(uintptr_t)a[2]))
            return 0;
        stream_lsn += n;
        return n;
    }
    case ST_STOP:
    case ST_PAUSE:
        streaming = 0;
        return 1;
    case ST_RESUME:
        streaming = 1;
        return 1;
    case ST_INIT:
        return 1;
    case ST_STAT:
        return streaming ? 0x7FFF : 0;  /* sectors buffered */
    }
    return 0;
}

void rpc_cd_ncmd(int fno, void *send, int ssize, void *recv, int rsize)
{
    const uint32_t *a = send;
    if (recv && rsize > 0)
        memset(recv, 0, (size_t)rsize);
    switch (fno) {
    case NCMD_READ:
    case NCMD_DVDREAD: {
        uint32_t pattern = a[3] >> 16 & 0xFF;
        if (pattern != 0)
            hle_log("cdvd read: sector size pattern %u not supported\n", pattern);
        if (hle_verbose)
            hle_log("cdvd read %u sectors at %u to %08X\n", a[1], a[0], a[2]);
        read_sectors(a[0], a[1], (void *)(uintptr_t)a[2]);
        if (ssize >= 24 && a[5])
            *(uint32_t *)(uintptr_t)a[5] = a[1] * SECTOR;  /* the read position */
        break;
    }
    case NCMD_STREAM:
        if (recv)
            *(uint32_t *)recv = stream(a);
        break;
    case NCMD_DISKREADY:
        if (recv)
            *(int32_t *)recv = SCECdComplete;
        break;
    case NCMD_SEEK:
    case NCMD_STANDBY:
    case NCMD_STOP:
    case NCMD_PAUSE:
        break;
    default:
        rpc_unknown("cdvd N-command", fno, send, ssize, recv, rsize);
    }
}

/* The disc files the game looked up last, in the HLE window (memory.h PS2_DISC_RECENT), for the
   host's crash report: a crash while a screen loads names the files it was reading, e.g. the Bike
   Shop's rider and bike (finding 208). From the lookups here (sceCdSearchFile, fileio's cdrom0:)
   and from the game's own loader, which finds most files in its own table of the disc
   (func_0020D958, patched in port/patches.py). A file not on the disc ends in " (not found)". */
void hle_disc_lookup(const char *path, int found)
{
    uint32_t *count = (uint32_t *)(uintptr_t)PS2_DISC_RECENT;
    char *slot = (char *)(uintptr_t)(PS2_DISC_RECENT + 8 + *count % PS2_DISC_RECENT_SLOTS * PS2_DISC_RECENT_PATH);
    snprintf(slot, PS2_DISC_RECENT_PATH, "%s%s", path, found ? "" : " (not found)");
    ++*count;
}

/* sceCdSearchFile: the send buffer has the path and is also where the IOP writes the result
   (sceCdlFILE, 32 bytes, at its start); the reply word is 1 if found. */
void rpc_cd_search(int fno, void *send, int ssize, void *recv, int rsize)
{
    (void)fno;
    char *pkt = send;
    const char *name = pkt + sizeof(CdFile);  /* after the result area (func_00246FA0) */
    CdFile f = {0};  /* the reply copies all of it: no host stack bytes after the name's NUL */
    int found = search_file(name, &f);
    hle_disc_lookup(name, found);
    if (hle_verbose || !found)
        hle_log("sceCdSearchFile(%s): %s (lsn %u, %u bytes)\n", name, found ? "found" : "not found",
                found ? f.lsn : 0, found ? f.size : 0);
    if (found)
        memcpy(pkt, &f, sizeof f);
    (void)ssize;
    if (recv && rsize >= 4)
        *(int32_t *)recv = found;
}

/* For the other servers: a file of the disc by path (fileio's cdrom0:), whole. */
int cdvd_find(const char *path, uint32_t *lsn, uint32_t *size)
{
    CdFile f;
    int found = search_file(path, &f);
    hle_disc_lookup(path, found);
    if (!found)
        return 0;
    *lsn = f.lsn;
    *size = f.size;
    return 1;
}

int cdvd_read(uint32_t lsn, uint32_t count, void *dst) { return read_sectors(lsn, count, dst); }

/* Module-side entry points used by patched game code. No native decoder calls
   back into this module to walk the ISO directory. */
void host_movie_start(const char *name, int audio)
{
    uint32_t lsn=0,size=0;
    if ((uintptr_t)name<0x02000000u) name=GUEST(0x08000000u+(uint32_t)(uintptr_t)name);
    const char *iso=host_getenv("DHD_ISO");
    host_service_movie_prepare();
    int found=iso && cdvd_find(name,&lsn,&size);
    host_service_movie_start(GADDR(name),lsn,size,audio,found);
}
int host_movie_update(int keep) { return host_service_movie_update(keep); }
