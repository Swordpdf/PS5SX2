/* PS5SX2 disc-auto daemon (vk-285-160e, AI-assisted).
 * Send once to the console's ELF loader (port 9021) and leave it running.
 * Put a PS2 DVD in:
 *   - already dumped  -> PS5SX2 comes to the front and starts that game.
 *   - not dumped yet  -> PS5SX2 (started if needed) dumps it in the background,
 *                        then comes to the front and starts the game.
 * Take the disc out   -> the autostart guard and any leftover signal files are
 *                        removed, so opening PS5SX2 lands on the normal shelf.
 *
 * How it talks to PS5SX2 (all in /data/PCSX2/logs/):
 *   disc-launch.txt       line 1 ISO path, line 2 serial; main-boot starts it directly.
 *   disc-launch-exec.txt  PS5SX2's disc watcher sees it within ~3 s and re-execs its own
 *                         eboot. Only a self-exec brings the app to the front (vk-285-159
 *                         console results); a LoadExec from this payload leaves it behind
 *                         the shell (160b).
 *   ps5sx2-alive.txt      PS5SX2 stamps time() every 2 s (its own thread, so a dump in
 *                         progress doesn't make it look dead).
 *   disc-auto-alive.txt   this daemon stamps time() each loop; PS5SX2 then skips its own
 *                         disc_refg bounce.
 *
 * The shell pulls the user to the home screen ("disc not supported") when any PS2 disc
 * goes in. 159 found the bounce back must come ~3 s after that or the shell lands last,
 * so a freshly inserted disc is left to settle kSettleSec before anything happens.
 *
 * vk-285-160b: dlopen for sceSystemServiceLoadExec (no hard libSceSystemService NEEDED).
 * vk-285-160c: foreground via disc-launch-exec.txt self-exec.
 * vk-285-160d: daemon loop, dump path, disc-out cleanup.
 * vk-285-160e: heartbeats instead of "exec.txt not consumed in 5 s" (that misread a PS5SX2
 *   busy dumping as not running and restarted it mid-dump); settle delay; serial on line 2;
 *   media-size check so the disc isn't re-read every loop during a dump; "(SERIAL)" names.
 * Needs proper testing on a console with a disc drive.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#define LOGS_DIR    "/data/PCSX2/logs"
#define GAMES_DIR   "/data/PCSX2/games"
#define LAUNCH_TXT  LOGS_DIR "/disc-launch.txt"
#define EXEC_TXT    LOGS_DIR "/disc-launch-exec.txt"
#define AUTO_TXT    LOGS_DIR "/disc-autostart.txt"
#define EMU_ALIVE   LOGS_DIR "/ps5sx2-alive.txt"
#define SELF_ALIVE  LOGS_DIR "/disc-auto-alive.txt"

enum { kPollSec = 2, kSettleSec = 4, kAliveWithin = 8, kRestartAfter = 45 };

/* ------------------------------------------------------------------ */
/* Notification / logging                                                */
/* ------------------------------------------------------------------ */
typedef struct { char unused[45]; char message[3075]; } notify_req_t;
int sceKernelSendNotificationRequest(int32_t device, void *req, size_t size, int32_t blocking);

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

static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...) {
    static notify_req_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    const int rc = sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    say("notify rc=%d: %s", rc, req.message);
}

/* ------------------------------------------------------------------ */
/* PS2 serial from a disc or ISO file                                    */
/* ------------------------------------------------------------------ */
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Whole 2048-byte sectors only: the drive refuses anything else (vk-285-146). */
static int read_lba(int fd, uint32_t lba, uint8_t *out) {
    return pread(fd, out, 2048, (off_t)lba * 2048) == 2048;
}

/* "SLUS_213.51" in SYSTEM.CNF's BOOT2 -> "SLUS-21351" (same normalisation as fe_games). */
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
            if (rlen == 0) break; /* rest of this sector is padding */
            if (off + rlen > 2048 || rlen < 34) break;
            const int nlen = dir[off + 32];
            if (nlen >= 10 && strncasecmp((char *)(dir + off + 33), "SYSTEM.CNF", 10) == 0) {
                const uint32_t clba = le32(dir + off + 2);
                uint32_t csz = le32(dir + off + 10);
                if (csz > 4096) csz = 4096;
                char cnf[4096 + 1];
                memset(cnf, 0, sizeof(cnf));
                uint8_t sec[2048];
                size_t got = 0;
                for (uint32_t k = 0; k < (csz + 2047) / 2048 && got < csz; k++) {
                    if (!read_lba(fd, clba + k, sec)) break;
                    size_t take = csz - got < 2048 ? csz - got : 2048;
                    memcpy(cnf + got, sec, take);
                    got += take;
                }
                const char *b2 = strstr(cnf, "BOOT2");
                if (!b2) return 0;
                const char *bs = strchr(b2, '\\');
                if (!bs) bs = strchr(b2, ':');
                if (!bs) return 0;
                bs++;
                int ni = 0;
                for (int i = 0; i < 16 && bs[i] && bs[i] != ';' && bs[i] != '\r' && bs[i] != '\n' && bs[i] != ' '; i++) {
                    if (bs[i] == '_')      out[ni++] = '-';
                    else if (bs[i] == '.') ;
                    else                   out[ni++] = (char)toupper((unsigned char)bs[i]);
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
static int has_iso_ext(const char *n) {
    const size_t l = strlen(n);
    return l >= 5 && strcasecmp(n + l - 4, ".iso") == 0;
}

/* "SLUS-21351.whatever.iso" or "Title (SLUS-21351).iso" (PS5SX2's own dump names). */
static int name_has_serial(const char *n, const char *serial) {
    const size_t sl = strlen(serial);
    if (strncasecmp(n, serial, sl) == 0 && (n[sl] == '.' || n[sl] == ' ' || n[sl] == '_' || n[sl] == '-'))
        return 1;
    for (const char *p = n; *p; p++)
        if (*p == '(' && strncasecmp(p + 1, serial, sl) == 0 && p[1 + sl] == ')')
            return 1;
    return 0;
}

/* Fast pass on names, then SYSTEM.CNF of each other .iso (renamed files). */
static int find_iso(const char *serial, char *out, size_t max, int deep) {
    DIR *d = opendir(GAMES_DIR);
    if (!d) { say("opendir(" GAMES_DIR "): errno %d", errno); return 0; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (has_iso_ext(e->d_name) && name_has_serial(e->d_name, serial)) {
            snprintf(out, max, GAMES_DIR "/%s", e->d_name);
            closedir(d);
            return 1;
        }
    }
    if (deep) {
        rewinddir(d);
        while ((e = readdir(d)) != NULL) {
            if (!has_iso_ext(e->d_name) || name_has_serial(e->d_name, serial)) continue;
            char path[1024];
            snprintf(path, sizeof(path), GAMES_DIR "/%s", e->d_name);
            const int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            char found[17] = {0};
            const int ok = read_serial(fd, found);
            close(fd);
            if (ok && strcasecmp(found, serial) == 0) {
                snprintf(out, max, "%s", path);
                closedir(d);
                return 1;
            }
        }
    }
    closedir(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Heartbeats                                                            */
/* ------------------------------------------------------------------ */
static int stamp_fresh(const char *path, int within) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    long long t = 0;
    const int got = fscanf(f, "%lld", &t);
    fclose(f);
    const long long now = (long long)time(NULL);
    return got == 1 && now >= t && now - t <= within;
}
static int emu_alive(void) { return stamp_fresh(EMU_ALIVE, kAliveWithin); }
static void stamp_self(void) {
    FILE *f = fopen(SELF_ALIVE, "w");
    if (f) { fprintf(f, "%lld\n", (long long)time(NULL)); fclose(f); }
}

/* ------------------------------------------------------------------ */
/* Start PS5SX2 when it isn't running                                    */
/* ------------------------------------------------------------------ */
typedef int (*LoadExec_t)(const char *, const char **);

static const char *eboot_path(void) {
    struct stat st;
    const char *p = "/data/homebrew/PPSA99203/eboot.bin";
    return stat(p, &st) == 0 ? p : "/app0/eboot.bin";
}

/* From this payload LoadExec starts PS5SX2 behind the shell (160b); whatever it then
 * needs to show, it brings itself to the front with a self-exec. Called in a child so the
 * daemon survives whether or not LoadExec replaces the calling process; a direct call if
 * fork isn't allowed here. */
static void start_emu(void) {
    void *h = dlopen("libSceSystemService.sprx", RTLD_NOW | RTLD_GLOBAL);
    LoadExec_t le = h ? (LoadExec_t)dlsym(h, "sceSystemServiceLoadExec") : NULL;
    if (!le) { say("sceSystemServiceLoadExec not found (dlopen %p)", h); return; }
    const char *path = eboot_path();
    const pid_t child = fork();
    if (child == 0) {
        const int rc = le(path, NULL);
        klog_printf("[disc-auto] child: LoadExec(%s) = 0x%08x\n", path, (unsigned)rc);
        _exit(rc == 0 ? 0 : 1);
    }
    if (child > 0) {
        say("starting PS5SX2: forked %d for LoadExec(%s)", (int)child, path);
        return;
    }
    say("fork errno %d; calling LoadExec(%s) directly", errno, path);
    const int rc = le(path, NULL);
    say("LoadExec = 0x%08x", (unsigned)rc);
}

/* ------------------------------------------------------------------ */
/* Actions                                                               */
/* ------------------------------------------------------------------ */
static time_t g_last_start;

static void launch(const char *serial, const char *iso) {
    FILE *f = fopen(LAUNCH_TXT, "w");
    if (!f) { say("fopen(" LAUNCH_TXT "): errno %d", errno); notify("PS5SX2 disc: can't write disc-launch.txt"); return; }
    fprintf(f, "%s\n%s\n", iso, serial);
    fclose(f);
    f = fopen(EXEC_TXT, "w");
    if (f) fclose(f);
    else say("fopen(" EXEC_TXT "): errno %d", errno);
    say("launch %s: %s", serial, iso);
    notify("PS5SX2: starting %s", serial);
    /* Running: its watcher self-execs into the game (front). Not running: start it; main-boot
     * sees exec.txt, holds disc-launch.txt, the watcher self-execs, and that boot starts it. */
    if (!emu_alive()) {
        say("PS5SX2 not running (no fresh ps5sx2-alive.txt): starting it");
        start_emu();
        g_last_start = time(NULL);
    }
}

static void cleanup(const char *serial) {
    say("disc out (%s): removing autostart guard and signal files", serial);
    unlink(AUTO_TXT);
    unlink(LAUNCH_TXT);
    unlink(EXEC_TXT);
}

/* ------------------------------------------------------------------ */
/* main                                                                  */
/* ------------------------------------------------------------------ */
enum state { S_NONE, S_SETTLE, S_DUMPING, S_DONE };

int main(void) {
    say("disc-auto daemon (vk-285-160e) pid %d", (int)getpid());

    /* /dev/cd0 and notifications whatever uid the loader gave us (160b). */
    {
        uint8_t all[16];
        memset(all, 0xff, sizeof(all));
        kernel_set_ucred_authid(getpid(), 0x4800000000010003ull);
        kernel_set_ucred_caps(getpid(), all);
    }
    signal(SIGCHLD, SIG_IGN); /* no zombies from start_emu's child */

    notify("PS5SX2 disc-auto running: put a PS2 DVD in");
    say("PS5SX2 %s", emu_alive() ? "is running" : "is not running");

    char serial[17] = {0};
    off_t media = 0;
    time_t seen = 0;
    enum state st = S_NONE;
    int startup = 1;

    for (;;) {
        stamp_self();

        /* Is a disc in, and is it the same one? DIOCGMEDIASIZE doesn't touch the disc, so a
         * dump in progress isn't slowed by us re-reading its directory every loop. */
        off_t size = 0;
        const int fd = open("/dev/cd0", O_RDONLY);
        if (fd >= 0 && ioctl(fd, DIOCGMEDIASIZE, &size) != 0)
            size = 0;

        if (fd < 0 || size <= 0) {
            if (fd >= 0) close(fd);
            if (serial[0]) cleanup(serial);
            serial[0] = '\0';
            media = 0;
            st = S_NONE;
        } else {
            if (size != media) {
                char s[17] = {0};
                const int ok = read_serial(fd, s);
                if (serial[0] && (!ok || strcmp(s, serial) != 0)) cleanup(serial);
                media = size;
                if (ok) {
                    memcpy(serial, s, sizeof(serial));
                    seen = time(NULL);
                    st = S_SETTLE;
                    say("PS2 disc %s (%lld bytes)%s", serial, (long long)size, startup ? ", already in at start" : "");
                    if (startup) seen -= kSettleSec; /* no shell takeover to wait out */
                } else {
                    serial[0] = '\0';
                    st = S_NONE;
                    say("disc in (%lld bytes) but no PS2 SYSTEM.CNF: ignoring (PS2 CD games can't be read by this drive)", (long long)size);
                }
            }
            close(fd);
        }
        startup = 0;

        char iso[1024];
        switch (st) {
        case S_SETTLE:
            if (time(NULL) - seen < kSettleSec) break;
            if (find_iso(serial, iso, sizeof(iso), 1)) {
                char inc[1100];
                snprintf(inc, sizeof(inc), "%s.incomplete", iso);
                struct stat ist;
                if (stat(inc, &ist) == 0) {
                    say("%s: dump has unreadable sectors (%s): not auto-started", serial, inc);
                    notify("PS5SX2: %s was dumped with read errors; pick it from the shelf to try it", serial);
                } else {
                    launch(serial, iso);
                }
                st = S_DONE;
            } else {
                say("%s: no ISO in " GAMES_DIR " yet: PS5SX2 dumps it, then it starts", serial);
                notify("PS5SX2: dumping %s, it starts when the copy is done", serial);
                if (!emu_alive()) {
                    say("PS5SX2 not running: starting it to dump");
                    start_emu();
                    g_last_start = time(NULL);
                }
                st = S_DUMPING;
            }
            break;
        case S_DUMPING:
            /* The .part file is renamed to .iso only when the copy is finished. Names only:
             * PS5SX2 names its dumps "Title (SERIAL).iso". */
            if (find_iso(serial, iso, sizeof(iso), 0)) {
                say("%s: dump finished", serial);
                st = S_SETTLE;
                seen = time(NULL) - kSettleSec;
            } else if (!emu_alive() && time(NULL) - g_last_start > kRestartAfter) {
                say("%s: PS5SX2 stopped before the dump finished: starting it again", serial);
                start_emu();
                g_last_start = time(NULL);
            }
            break;
        default:
            break;
        }

        sleep(kPollSec);
    }
    return 0;
}
