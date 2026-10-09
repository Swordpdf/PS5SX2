/* PS5SX2 disc-auto daemon (vk-285-160d, AI-assisted):
 * Polls /dev/cd0 every 3 s.  When a PS2 disc is inserted:
 *   – ISO already in /data/PCSX2/games/: writes disc-launch.txt (ISO path) and
 *     disc-launch-exec.txt so PS5SX2 self-execs, comes to the foreground, and
 *     starts the game directly (skipping the shelf).
 *   – ISO not yet dumped: writes disc-launch-exec.txt only so PS5SX2 self-execs
 *     to the front; its disc watcher dumps the disc and then starts the game.
 *   In either case, if PS5SX2 is not already running (disc-launch-exec.txt is
 *   still present after 5 s because the disc watcher didn't pick it up),
 *   sceSystemServiceLoadExec is called from a child process (fork) so the
 *   daemon keeps running.
 * When the disc is removed: deletes disc-autostart.txt (so a reinserted disc
 * auto-starts again and the user can reach the shelf freely) and cleans up any
 * unconsumed signal files.
 *
 * Send to the console's ELF loader (port 9021).  The ELF keeps running until
 * the loader process is killed.
 *
 * vk-285-160b: dlopen sceSystemServiceLoadExec; privilege escalation via
 *   kernel_set_ucred_authid so notifications and /dev/cd0 work from any loader.
 * vk-285-160c: foreground fix — write disc-launch-exec.txt; PS5SX2's disc
 *   watcher self-execs from within its own process (same as TryReForeground).
 * vk-285-160d: daemon loop; auto-dump path; disc-removal cleanup;
 *   fork() + LoadExec fallback to wake PS5SX2 when not running.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

/* ------------------------------------------------------------------ */
/* Notification                                                          */
/* ------------------------------------------------------------------ */
typedef struct { char unused[45]; char message[3075]; } notify_req_t;
int sceKernelSendNotificationRequest(int32_t device, void *req, size_t size, int32_t blocking);
static void notify(const char *msg) {
    static notify_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* ------------------------------------------------------------------ */
/* Logging                                                               */
/* ------------------------------------------------------------------ */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog_printf("[disc-auto] %s\n", buf);
    printf("[disc-auto] %s\n", buf);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* PS2 serial from a disc or ISO file                                    */
/* ------------------------------------------------------------------ */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int read_lba(int fd, uint32_t lba, uint8_t *out) {
    return pread(fd, out, 2048, (off_t)lba * 2048) == 2048;
}

static int read_serial(int fd, char *out) {
    uint8_t pvd[2048];
    if (!read_lba(fd, 16, pvd)) return 0;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return 0;

    const uint32_t root_lba  = le32(pvd + 156 + 2);
    const uint32_t root_size = le32(pvd + 156 + 10);
    const uint32_t dir_secs  = (root_size + 2047) / 2048 < 64 ? (root_size + 2047) / 2048 : 64;

    uint8_t dir[2048];
    for (uint32_t ds = 0; ds < dir_secs; ds++) {
        if (!read_lba(fd, root_lba + ds, dir)) continue;
        for (int off = 0; off < 2048; ) {
            const int rlen = dir[off];
            if (rlen == 0) { off = (off / 2048 + 1) * 2048; continue; }
            if (off + rlen > 2048 || rlen < 34) break;
            const int nlen = dir[off + 32];
            if (nlen >= 10 && strncasecmp((char *)(dir + off + 33), "SYSTEM.CNF", 10) == 0) {
                const uint32_t clba = le32(dir + off + 2);
                uint32_t csz = le32(dir + off + 10);
                if (csz > 4096) csz = 4096;
                char cnf[4096 + 1];
                memset(cnf, 0, sizeof(cnf));
                const uint32_t nsecs = (csz + 2047) / 2048;
                uint8_t sec[2048];
                size_t got = 0;
                for (uint32_t k = 0; k < nsecs && got < csz; k++) {
                    if (!read_lba(fd, clba + k, sec)) break;
                    size_t take = 2048;
                    if (got + take > csz) take = csz - got;
                    memcpy(cnf + got, sec, take);
                    got += take;
                }
                const char *b2 = strstr(cnf, "BOOT2");
                if (!b2) break;
                const char *bs = strchr(b2, '\\');
                if (!bs) break;
                bs++;
                char raw[17];
                int ri = 0;
                while (ri < 16 && bs[ri] && bs[ri] != ';' && bs[ri] != '\r' && bs[ri] != '\n' && bs[ri] != ' ') {
                    raw[ri] = bs[ri];
                    ri++;
                }
                raw[ri] = '\0';
                int ni = 0;
                for (int i = 0; raw[i] && ni < 16; i++) {
                    if (raw[i] == '_')      out[ni++] = '-';
                    else if (raw[i] == '.') /* skip dot */;
                    else                    out[ni++] = (char)toupper((unsigned char)raw[i]);
                }
                out[ni] = '\0';
                return ni >= 9 && ni <= 12;
            }
            off += rlen;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* games/ scanner                                                        */
/* ------------------------------------------------------------------ */

static int name_starts_with_serial(const char *name, const char *serial) {
    const size_t slen = strlen(serial);
    if (strncasecmp(name, serial, slen) != 0) return 0;
    return name[slen] == '\0' || name[slen] == '.' || name[slen] == '-' || name[slen] == '_';
}

static int find_iso(const char *games_dir, const char *serial, char *out_path, size_t max) {
    DIR *d = opendir(games_dir);
    if (!d) { say("opendir(%s) failed: errno %d", games_dir, errno); return 0; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        const size_t nlen = strlen(n);
        if (nlen < 5 || strcasecmp(n + nlen - 4, ".iso") != 0) continue;
        if (name_starts_with_serial(n, serial)) {
            snprintf(out_path, max, "%s/%s", games_dir, n);
            closedir(d);
            return 1;
        }
    }

    rewinddir(d);
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        const size_t nlen = strlen(n);
        if (nlen < 5 || strcasecmp(n + nlen - 4, ".iso") != 0) continue;
        if (name_starts_with_serial(n, serial)) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", games_dir, n);
        const int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char found[17];
        const int ok = read_serial(fd, found);
        close(fd);
        if (ok && strcasecmp(found, serial) == 0) {
            snprintf(out_path, max, "%s", path);
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Signal file paths                                                     */
/* ------------------------------------------------------------------ */
#define LOGS_DIR  "/data/PCSX2/logs"
#define GAMES_DIR "/data/PCSX2/games"
#define LAUNCH_TXT LOGS_DIR "/disc-launch.txt"
#define EXEC_TXT   LOGS_DIR "/disc-launch-exec.txt"
#define AUTO_TXT   LOGS_DIR "/disc-autostart.txt"

/* ------------------------------------------------------------------ */
/* sceSystemServiceLoadExec via dlopen (no hard NEEDED entry)           */
/* ------------------------------------------------------------------ */
typedef int (*LoadExec_t)(const char *, const char **);

static LoadExec_t find_load_exec(void) {
    void *h = dlopen("libSceSystemService.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { say("dlopen(libSceSystemService.sprx) failed: %s", dlerror()); return NULL; }
    LoadExec_t fn = (LoadExec_t)dlsym(h, "sceSystemServiceLoadExec");
    if (!fn) { say("dlsym(sceSystemServiceLoadExec) failed"); }
    return fn;
}

static const char *eboot_path(void) {
    struct stat st;
    const char *p = "/data/homebrew/PPSA99203/eboot.bin";
    return (stat(p, &st) == 0) ? p : "/app0/eboot.bin";
}

/* ------------------------------------------------------------------ */
/* trigger: write signal files; fallback-start PS5SX2 if not running   */
/* ------------------------------------------------------------------ */

/* Write exec.txt (and disc-launch.txt when iso != NULL).
 * Sleep 5 s so a running PS5SX2's disc watcher can consume exec.txt.
 * If exec.txt is still present afterwards, PS5SX2 wasn't running —
 * fork() and have the child call LoadExec (child is replaced by PS5SX2;
 * parent daemon keeps running). */
static void trigger(const char *serial, const char *iso) {
    /* disc-launch.txt (ISO path) — only when the ISO already exists */
    if (iso) {
        FILE *f = fopen(LAUNCH_TXT, "w");
        if (!f) { say("fopen(" LAUNCH_TXT "): errno %d", errno); }
        else { fprintf(f, "%s\n", iso); fclose(f); say("wrote " LAUNCH_TXT); }
    }

    /* disc-launch-exec.txt (self-exec signal) */
    {
        FILE *f = fopen(EXEC_TXT, "w");
        if (!f) { say("fopen(" EXEC_TXT "): errno %d", errno); return; }
        fclose(f);
        say("wrote " EXEC_TXT);
    }

    /* notify user */
    {
        char msg[220];
        if (iso)
            snprintf(msg, sizeof(msg), "PS5SX2: disc game ready (%s) — switching to it now", serial);
        else
            snprintf(msg, sizeof(msg), "PS5SX2: dumping disc (%s) — starting PS5SX2", serial);
        notify(msg);
    }

    /* Give the disc watcher up to 5 s to consume exec.txt. */
    sleep(5);

    struct stat est;
    if (stat(EXEC_TXT, &est) != 0) {
        /* exec.txt gone — the disc watcher picked it up; PS5SX2 was running */
        say("exec.txt consumed by disc watcher; PS5SX2 is self-exec-ing");
        return;
    }

    /* exec.txt still present: PS5SX2 was not running.
     * Fork so the daemon survives; child calls LoadExec (its process is
     * replaced by PS5SX2, which will cold-boot, see exec.txt, self-exec
     * to the foreground, then read disc-launch.txt if present). */
    say("exec.txt still present — PS5SX2 not running; forking to call LoadExec");
    LoadExec_t le = find_load_exec();
    if (!le) return;

    const pid_t child = fork();
    if (child == 0) {
        /* child: call LoadExec — replaces this child process with PS5SX2 */
        le(eboot_path(), NULL);
        _exit(1);
    }
    if (child < 0)
        say("fork: errno %d", errno);
    else
        say("forked pid %d to call LoadExec(%s)", (int)child, eboot_path());
    /* parent continues the polling loop */
}

/* ------------------------------------------------------------------ */
/* cleanup: remove signal files and autostart mark on disc removal      */
/* ------------------------------------------------------------------ */
static void cleanup(const char *serial) {
    say("disc out (%s): clearing autostart + signal files", serial);
    unlink(EXEC_TXT);
    unlink(LAUNCH_TXT);
    unlink(AUTO_TXT);
    /* reap any child that finished */
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */
int main(void) {
    say("disc-auto daemon v1 (vk-285-160d)");

    /* Privilege escalation: /dev/cd0 access + notifications from any loader uid */
    {
        const pid_t pid = getpid();
        uint8_t all[16];
        memset(all, 0xff, sizeof(all));
        kernel_set_ucred_authid(pid, 0x4800000000010003ull);
        kernel_set_ucred_caps(pid, all);
    }

    char last_serial[17] = {};

    for (;;) {
        const int fd = open("/dev/cd0", O_RDONLY);
        if (fd >= 0) {
            char serial[17] = {};
            const int ok = read_serial(fd, serial);
            close(fd);

            if (ok) {
                /* Valid PS2 disc */
                if (strcmp(serial, last_serial) != 0) {
                    /* New disc (or first detection) */
                    memcpy(last_serial, serial, sizeof(last_serial));
                    say("disc: %s", serial);

                    char iso[1024] = {};
                    if (find_iso(GAMES_DIR, serial, iso, sizeof(iso))) {
                        say("ISO found: %s — signalling launch", iso);
                        trigger(serial, iso);
                    } else {
                        say("no ISO for %s — signalling PS5SX2 to dump", serial);
                        trigger(serial, NULL);
                    }
                }
                /* else: same disc still in — nothing to do this iteration */
            } else {
                /* /dev/cd0 opened but not a PS2 disc (e.g. BD movie, or non-PS2 DVD) */
                if (last_serial[0]) {
                    cleanup(last_serial);
                    last_serial[0] = '\0';
                }
            }
        } else {
            /* /dev/cd0 not available: no disc in drive */
            if (last_serial[0]) {
                cleanup(last_serial);
                last_serial[0] = '\0';
            }
        }

        sleep(3);
    }
    return 0;
}
