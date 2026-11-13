/* ios_main.m - Native iOS Platform Host Implementation for Downhill Revived
   Provides sandbox paths, lifecycle management, ISO validation, and game thread startup.
   GPL-3.0-or-later. */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include "ios_main.h"
#include "metal_bridge.h"
#include "touch_input.h"

#define DISC_FILENAME "SCUS-97177.iso"

/* Real game core declarations from libdhd_ios.a */
extern void host_console_init(void);
extern int dhd_settings_save(const char *path, const char *key, const char *value);
extern int dhd_settings_read(const char *path, const char *key, char *out, size_t size);
extern int dhd_main(int argc, char **argv);

static char s_documents_dir[1024] = {0};
static char s_settings_path[1100] = {0};
static char s_iso_path[1100] = {0};
static _Atomic(bool) s_game_started = false;
static _Atomic(bool) s_game_active = true;
static pthread_once_t s_directories_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t s_disc_operation = PTHREAD_MUTEX_INITIALIZER;

static void initialize_directories(void) {
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *docDir = paths.firstObject;
    if (!docDir) docDir = NSTemporaryDirectory();

    snprintf(s_documents_dir, sizeof s_documents_dir, "%s/", [docDir UTF8String]);
    snprintf(s_settings_path, sizeof s_settings_path, "%sdhd.ini", s_documents_dir);

    /* Create saves folder */
    char saves[1200];
    snprintf(saves, sizeof saves, "%ssaves/card1", s_documents_dir);
    [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:saves]
                              withIntermediateDirectories:YES attributes:nil error:nil];

    /* Use the engine's parser, including whitespace, aliases and comments. */
    dhd_settings_read(s_settings_path, "iso", s_iso_path, sizeof s_iso_path);
}

static void ensure_directories(void) {
    pthread_once(&s_directories_once, initialize_directories);
}

const char *ios_documents_dir(void) {
    ensure_directories();
    return s_documents_dir;
}

const char *ios_settings_path(void) {
    ensure_directories();
    return s_settings_path;
}

int ios_setting_save(const char *key, const char *value) {
    ensure_directories();
    if (!key || !value) return 0;
    return dhd_settings_save(s_settings_path, key, value);
}

int ios_setting_read(const char *key, char *out, int out_size) {
    ensure_directories();
    if (!key || !out || out_size <= 0) return 0;
    return dhd_settings_read(s_settings_path, key, out, (size_t)out_size);
}

const char *ios_iso_path(void) {
    ensure_directories();
    return s_iso_path;
}

uint64_t ios_available_disk_space(void) {
    ensure_directories();
    NSError *error = nil;
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:s_documents_dir]];
    NSDictionary *results = [url resourceValuesForKeys:@[NSURLVolumeAvailableCapacityForImportantUsageKey] error:&error];
    if (results && results[NSURLVolumeAvailableCapacityForImportantUsageKey]) {
        return [results[NSURLVolumeAvailableCapacityForImportantUsageKey] unsignedLongLongValue];
    }
    NSDictionary *attrs = [[NSFileManager defaultManager] attributesOfFileSystemForPath:[NSString stringWithUTF8String:s_documents_dir] error:&error];
    if (attrs && attrs[NSFileSystemFreeSize]) {
        return [attrs[NSFileSystemFreeSize] unsignedLongLongValue];
    }
    return 0;
}

static inline uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *ios_disc_validate(const char *iso_path) {
    if (!iso_path || !*iso_path) return "No file path provided.";
    struct stat st;
    if (stat(iso_path, &st) != 0) return "File does not exist or cannot be accessed.";
    if (st.st_size < 100000000ull) return "File size is too small for a Downhill Domination DVD image.";

    FILE *f = fopen(iso_path, "rb");
    if (!f) return "Could not open ISO file for reading.";

    enum { SECTOR = 2048 };
    uint8_t sector[SECTOR];

    /* 1. Try standard ISO 9660 Volume Descriptor at LSN 16 */
    if (fseek(f, 16ull * SECTOR, SEEK_SET) == 0 && fread(sector, 1, SECTOR, f) == SECTOR) {
        if (sector[0] == 1 && memcmp(sector + 1, "CD001", 5) == 0) {
            /* Valid ISO 9660 PVD. Root Directory record is at offset 156 */
            uint32_t dir_lsn = le32(sector + 156 + 2);
            uint32_t dir_size = le32(sector + 156 + 10);
            uint32_t cnf_lsn = 0;
            uint32_t cnf_size = 0;

            for (uint32_t off = 0; off < dir_size && off < 64 * SECTOR && !cnf_lsn; off += SECTOR) {
                if (fseek(f, (uint64_t)(dir_lsn + off / SECTOR) * SECTOR, SEEK_SET) != 0) break;
                if (fread(sector, 1, SECTOR, f) != SECTOR) break;
                for (unsigned i = 0; i + 33 < SECTOR && sector[i]; i += sector[i]) {
                    const uint8_t *r = sector + i;
                    uint8_t name_len = r[32];
                    if (name_len >= 10 && i + 33 + name_len <= SECTOR) {
                        const char *name = (const char *)r + 33;
                        if (strncasecmp(name, "SYSTEM.CNF", 10) == 0 && (name_len == 10 || name[10] == ';')) {
                            cnf_lsn = le32(r + 2);
                            cnf_size = le32(r + 10);
                            break;
                        }
                    }
                }
            }

            if (cnf_lsn > 0 && fseek(f, (uint64_t)cnf_lsn * SECTOR, SEEK_SET) == 0) {
                char cnf_text[SECTOR + 1];
                size_t n = fread(cnf_text, 1, cnf_size < SECTOR ? cnf_size : SECTOR, f);
                cnf_text[n] = '\0';

                if (strstr(cnf_text, "SCUS_971.77") || strstr(cnf_text, "SCUS-97177") ||
                    strstr(cnf_text, "scus_971.77") || strstr(cnf_text, "scus-97177")) {
                    fclose(f);
                    return NULL; /* Match verified! */
                }

                /* Extract other PS2 game id to provide clear user diagnostic */
                const char *boot = strstr(cnf_text, "BOOT2");
                if (boot) {
                    static char err_msg[300];
                    char disc_id[64] = {0};
                    const char *p = strpbrk(boot, "\\:");
                    if (p) {
                        while (*p == '\\' || *p == ':') p++;
                        size_t len = strcspn(p, ";\r\n ");
                        if (len > 0 && len < sizeof(disc_id) - 1) {
                            strncpy(disc_id, p, len);
                            disc_id[len] = '\0';
                        }
                    }
                    fclose(f);
                    if (disc_id[0]) {
                        NSString *disc = [NSString stringWithUTF8String:disc_id] ?: @"?";
                        NSString *message = [NSString localizedStringWithFormat:
                            NSLocalizedString(@"Detected PS2 disc %@. Downhill Revived requires Downhill Domination USA (SCUS-97177).", @"Wrong disc"), disc];
                        snprintf(err_msg, sizeof err_msg, "%s", message.UTF8String);
                        return err_msg;
                    }
                }
            }
        }
    }

    /* 2. Fallback: Stream first 16 MB in 64 KB overlapping blocks (handles raw/bin or offset PVDs) */
    fseek(f, 0, SEEK_SET);
    static uint8_t chunk[65536];
    size_t overlap = 0;
    for (int b = 0; b < 256; b++) { /* 256 * 64KB = 16 MB */
        size_t rd = fread(chunk + overlap, 1, sizeof(chunk) - overlap, f);
        if (rd == 0) break;
        size_t total_buf = overlap + rd;
        for (size_t k = 0; k + 11 <= total_buf; k++) {
            if (strncasecmp((const char *)chunk + k, "SCUS_971.77", 11) == 0 ||
                strncasecmp((const char *)chunk + k, "SCUS-97177", 10) == 0) {
                fclose(f);
                return NULL; /* Match verified! */
            }
        }
        overlap = total_buf > 32 ? 32 : total_buf;
        memmove(chunk, chunk + total_buf - overlap, overlap);
    }

    fclose(f);
    return "Disc image is not the USA Downhill Domination release (SCUS-97177).";
}

bool ios_disc_auto_detect(void) {
    ensure_directories();

    /* 1. If currently registered s_iso_path exists and is valid, keep it */
    if (s_iso_path[0] && ios_disc_validate(s_iso_path) == NULL) {
        return true;
    }

    /* 2. Check standard SCUS-97177.iso in Documents */
    char std_path[1100];
    snprintf(std_path, sizeof std_path, "%s%s", s_documents_dir, DISC_FILENAME);
    if (ios_disc_validate(std_path) == NULL) {
        snprintf(s_iso_path, sizeof s_iso_path, "%s", std_path);
        dhd_settings_save(s_settings_path, "iso", s_iso_path);
        return true;
    }

    /* 3. Scan Documents directory for any .iso, .ISO, .bin, .BIN, .img */
    NSString *docPath = [NSString stringWithUTF8String:s_documents_dir];
    NSFileManager *fm = [NSFileManager defaultManager];
    NSError *err = nil;
    NSArray *files = [fm contentsOfDirectoryAtPath:docPath error:&err];
    if (files) {
        for (NSString *filename in files) {
            NSString *ext = [filename pathExtension].lowercaseString;
            if ([ext isEqualToString:@"iso"] || [ext isEqualToString:@"bin"] || [ext isEqualToString:@"img"]) {
                NSString *fullPath = [docPath stringByAppendingPathComponent:filename];
                if (ios_disc_validate([fullPath UTF8String]) == NULL) {
                    snprintf(s_iso_path, sizeof s_iso_path, "%s", [fullPath UTF8String]);
                    dhd_settings_save(s_settings_path, "iso", s_iso_path);
                    return true;
                }
            }
        }
    }

    return false;
}

bool ios_disc_is_ready(void) {
    return ios_disc_auto_detect();
}

bool ios_disc_get_info(char *name_buf, size_t name_size, uint64_t *size_bytes) {
    ensure_directories();
    if (!ios_disc_auto_detect()) return false;
    struct stat st;
    if (stat(s_iso_path, &st) != 0) return false;
    if (size_bytes) *size_bytes = (uint64_t)st.st_size;
    if (name_buf && name_size > 0) {
        const char *slash = strrchr(s_iso_path, '/');
        const char *fname = slash ? slash + 1 : s_iso_path;
        snprintf(name_buf, name_size, "%s", fname);
    }
    return true;
}

bool ios_disc_delete(void) {
    ensure_directories();
    if (pthread_mutex_trylock(&s_disc_operation) != 0) return false;
    bool removed = false;
    if (!atomic_load(&s_game_started) && s_iso_path[0] &&
        (unlink(s_iso_path) == 0 || errno == ENOENT)) {
        s_iso_path[0] = '\0';
        dhd_settings_save(s_settings_path, "iso", "");
        removed = true;
    }
    pthread_mutex_unlock(&s_disc_operation);
    return removed;
}

static bool disc_import_from_url(const char *source_url_path, iOSImportProgressCallback progress_cb, char *err_msg, size_t err_size) {
    ensure_directories();
    if (!source_url_path || !*source_url_path) {
        if (err_msg) snprintf(err_msg, err_size, "Empty source URL.");
        return false;
    }

    struct stat st;
    if (stat(source_url_path, &st) != 0) {
        if (err_msg) snprintf(err_msg, err_size, "Source file could not be read.");
        return false;
    }

    /* If the file is already inside the Documents directory, validate in-place without redundant copy */
    if (strncmp(source_url_path, s_documents_dir, strlen(s_documents_dir)) == 0) {
        const char *problem = ios_disc_validate(source_url_path);
        if (problem) {
            if (err_msg) snprintf(err_msg, err_size, "%s", problem);
            return false;
        }
        snprintf(s_iso_path, sizeof s_iso_path, "%s", source_url_path);
        dhd_settings_save(s_settings_path, "iso", s_iso_path);
        return true;
    }

    uint64_t needed = (uint64_t)st.st_size + (100ull << 20); /* ISO size + 100MB margin */
    uint64_t available = ios_available_disk_space();
    if (available > 0 && available < needed) {
        if (err_msg) {
            NSString *message = [NSString localizedStringWithFormat:
                NSLocalizedString(@"Insufficient storage. Available: %llu MB, needed: %llu MB.", @"ISO import"),
                (unsigned long long)(available >> 20), (unsigned long long)(needed >> 20)];
            snprintf(err_msg, err_size, "%s", message.UTF8String);
        }
        return false;
    }

    char target_final_path[1100];
    snprintf(target_final_path, sizeof target_final_path, "%s%s", s_documents_dir, DISC_FILENAME);

    char part_path[1200];
    snprintf(part_path, sizeof part_path, "%s.part", target_final_path);
    unlink(part_path);

    FILE *in = fopen(source_url_path, "rb");
    if (!in) {
        if (err_msg) snprintf(err_msg, err_size, "Failed to open source file for reading.");
        return false;
    }

    FILE *out = fopen(part_path, "wb");
    if (!out) {
        fclose(in);
        if (err_msg) snprintf(err_msg, err_size, "Failed to create destination file in sandbox.");
        return false;
    }

    static char chunk[1024 * 1024]; /* 1 MB buffer */
    uint64_t total = (uint64_t)st.st_size;
    uint64_t copied = 0;
    size_t n;
    bool ok = true;

    while ((n = fread(chunk, 1, sizeof chunk, in)) > 0) {
        if (fwrite(chunk, 1, n, out) != n) {
            ok = false;
            break;
        }
        copied += n;
        if (progress_cb) {
            double prog = total > 0 ? (double)copied / (double)total : 0.0;
            progress_cb(prog, copied, total);
        }
    }

    fclose(in);
    ok &= (fclose(out) == 0);

    if (!ok || copied < total) {
        unlink(part_path);
        if (err_msg) snprintf(err_msg, err_size, "Copy interrupted or storage write error.");
        return false;
    }

    /* Validate the copied file before finalizing */
    const char *problem = ios_disc_validate(part_path);
    if (problem) {
        unlink(part_path);
        if (err_msg) snprintf(err_msg, err_size, "%s", problem);
        return false;
    }

    /* POSIX rename replaces the destination atomically. Keep the previous disc on failure. */
    if (rename(part_path, target_final_path) != 0) {
        unlink(part_path);
        if (err_msg) snprintf(err_msg, err_size, "Failed to finalize imported ISO file.");
        return false;
    }

    snprintf(s_iso_path, sizeof s_iso_path, "%s", target_final_path);
    dhd_settings_save(s_settings_path, "iso", s_iso_path);
    return true;
}

bool ios_disc_import_from_url(const char *path, iOSImportProgressCallback progress, char *error, size_t size) {
    if (pthread_mutex_trylock(&s_disc_operation) != 0) {
        if (error && size) snprintf(error, size, "Another disc operation is still running.");
        return false;
    }
    bool ok = false;
    if (atomic_load(&s_game_started)) {
        if (error && size) snprintf(error, size, "Close the game before importing a disc.");
    } else {
        ok = disc_import_from_url(path, progress, error, size);
    }
    pthread_mutex_unlock(&s_disc_operation);
    return ok;
}

/* Returns 0 after the game ends normally, nonzero when it cannot run. */
int ios_start_game(int argc, char **argv) {
    ensure_directories();
    if (pthread_mutex_trylock(&s_disc_operation) != 0) return 5;
    bool expected = false;
    int problem = atomic_load(&s_game_started) ? 4 : !ios_disc_auto_detect() ? 3 : 0;
    if (!problem && !atomic_compare_exchange_strong(&s_game_started, &expected, true)) problem = 4;
    pthread_mutex_unlock(&s_disc_operation);
    if (problem) return problem;
    setenv("DHD_ISO", s_iso_path, 1);
    setenv("DHD_SETTINGS", s_settings_path, 1);
    setenv("DHD_DATA_DIR", s_documents_dir, 1);
    setenv("DHD_IOS_DISPLAY_AUTO", "1", 1);
    ios_metal_set_game_active(atomic_load(&s_game_active));
    host_console_init();
    char *default_argv[] = {(char *)"dhd64", NULL};
    if (!argv || argc <= 0) {
        argv = default_argv;
        argc = 1;
    }
    int rc = dhd_main(argc, argv);
    atomic_store(&s_game_started, false);
    ios_metal_set_game_active(false);
    return rc;
}

void ios_request_exit(void) {
    /* Use the engine's close path; never mark a live core as stopped without ending it. */
    void gs_window_close(void);
    gs_window_close();
}

bool ios_is_game_running(void) {
    return atomic_load(&s_game_started);
}

bool ios_game_is_active(void) { return atomic_load(&s_game_active); }

void ios_lifecycle_will_resign_active(void) {
    atomic_store(&s_game_active, false);
    touch_input_reset();
    ios_metal_set_game_active(false);
}
void ios_lifecycle_did_enter_background(void) { ios_lifecycle_will_resign_active(); }
void ios_lifecycle_will_enter_foreground(void) {}
void ios_lifecycle_did_become_active(void) {
    atomic_store(&s_game_active, true);
    ios_metal_set_game_active(atomic_load(&s_game_started));
}
void ios_lifecycle_memory_warning(void) {}
