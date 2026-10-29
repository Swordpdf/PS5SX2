/* DiscTile.elf (test 6, 2026-10-12, AI-assisted): a tile on the PS5 home screen for each PS2 disc in a drive, the way the 5S2CD fork
 * (github.com/The7mirrors/5S2CD, discwatch, GPL-3.0) does it, and the system's own "Unsupported Disc" tile hidden while ours is up.
 *
 * When a PS2 disc goes in (/dev/cd0 = the PS5's own drive, /dev/cd1.. = USB drives) it makes a folder /data/homebrew/PPSA9923x:
 *   - PS5SX2's own eboot.bin (hard-linked: no extra space) and its sce_module,
 *   - disc-tile.txt naming the drive (PS5SX2 then starts that disc, or its copy when there is one, with no shelf),
 *   - sce_sys with param.json (the game's name), icon0.png (512x512) and pic0.dds (3840x2160 BC7) made from the game's art.
 * ShadowMount+ (its API on 127.0.0.1:10101) registers the folder as a title: that is what puts the tile on the home screen and makes the shell
 * draw it at once. The tile's title ID is taken from a ring of eight (the home screen keeps a title's picture by its ID, so a reused ID would
 * show an old one). The tile is added to /data/whitelist.txt so that PS5SX2's helper gives it root. When the disc comes out the tile is
 * uninstalled (ShadowMount+ again) and its folder removed. Nothing is started by itself: the click starts the game.
 *
 * The art: the game's cover from PS5SX2's cover cache (/data/PCSX2/cache/covers/<serial>.jpg), and with a SteamGridDB API key (one line in
 * /data/PCSX2/steamgriddb.key) its hero, logo and grid art, which tile.c (from the fork) puts together; without art, PS5SX2's own icon.
 *
 * The system's "Unsupported Disc" tile (and its Unreadable, PS5 and PS4 disc cousins) is one row of the PS5's app database for each console
 * user. While one of our tiles is up, the rows that say a disc is in (discStatus 1) get visible = 0, checked every 2 seconds; the system sets
 * them itself when the disc goes out. Flags (empty files in /data/PCSX2/flags/): disctile_nohide (leave that tile alone), disctile_nodownload
 * (no SteamGridDB), disctile_nonudge (do not touch the database's change counters).
 *
 * Log: klog "[disctile]" and /data/PCSX2/disc-tiles/disctile.log. Needs the ELF loader's root rights.
 * Copyright (C) 2026 swordpdf; the art code is from 5S2CD (tile.c). SPDX-License-Identifier: GPL-3.0-or-later */
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include <sqlite3.h>

#include "tile.h"
#include "disc_switch.h"
#include "kp.h"
#include "disc_compat.h"    /* 171k: next to the PS5SX3 helper: a non-PS2 disc's drive is left to it (yield), published in ps5sx2-helper.txt */
#include "disc_compat_db.h" /* 171k: a removed tile's concept rows stay when another title uses the concept */
#ifdef DISC_HOST_THREAD
#include "progress.h" /* 171f: payload_progress() of the helper's host (main.c / main-nomount.c): the revision lands in payload-init.log although no log file is written */
#endif

#define APP_SRC    "/data/homebrew/PPSA99203"
#define HB         "/data/homebrew"
#define WORK       "/data/PCSX2/disc-tiles"
#define ART_DIR    WORK "/art"
#define COVERS     "/data/PCSX2/cache/covers"
#define GAMEDB     "/data/PCSX2/resources/GameIndex.yaml"
#define WHITELIST  "/data/whitelist.txt"
#define KEY_FILE   "/data/PCSX2/steamgriddb.key"
#define KEY_FILE2  WORK "/steamgriddb-key.txt"
#define FLAGS_DIR  "/data/PCSX2/flags"
#define LOG_FILE   WORK "/disctile.log"
#define PID_FILE   WORK "/disctile.pid"
static void write_helper_status(void); /* 179: forward decl; defined lower, used at payload start */
#define APP_DB     "/system_data/priv/mms/app.db"
#define SMP_PORT   10101
#define MAX_DRIVES 4
#define NSLOTS     8
#define POLL_US    250000
#define PAYLOAD_REV "171k" /* written in the log's first line, so that a log says which payload made it: 171k = 171j + next to the PS5SX3 helper (disc_compat.h: a drive with a disc that is not a PS2 disc is left alone until the disc goes - not opened, not asked, not pinned to 8x - and ps5sx2-helper.txt says so (compat=1, cd0..cd3=, usb=), so that the PS5SX3 helper takes the drive without stopping this one; the system's disc tile put back at a stop only for a PS2 disc; a removed tile's concept rows that another title uses stay; flag disc_noyield = 171j's way); 171j = 171i + DiscSpeed8 SceShellCore 8x patch and 60 s drive pin (disc_speed8.c; flag disc_nospeed8), game tiles ON by default (gametile_off switches off); 171i = 171h + the flag kp_log looked at every 10 s (not only at the start); 171h = disc speed (the copies of disc_launch.c read in bigger pieces, a drive the watcher reads is not opened here, every 3 s while PS5SX2 copies), 171 = the first reload build, 171b = cover/art refresh on reload, clean slots first,
                            * tile-gone timing; 171c = the Reload tile kept next to the disc tile, the uninstall on its own thread asked every 0.4 s, half a second of debounce at eject;
                            * 171d = a tile's game closed when its disc goes out, the leftover database rows of a removed tile deleted, concept rows for the order, a title card when there is no cover;
                            * 171e = kp.c: quiet while the console shuts down or rests (no drive, file, database, ShadowMount+ or notification access), the log files off in a KP test build,
                            * the flags kp_nowatcher / kp_nodrive / kp_stub9020 / kp_off_quiesce / kp_log for the kernel-panic test;
                            * 171f = the configuration the tester ran without a kernel panic is the default: no log files (the flag kp_log brings them back), quiet at shutdown and the
                            * jb/mount patches on; game quiet (kp.c: while PS5SX2 plays from a drive each drive is looked at once per 3 s; flag gamequiet_off); the game of a tile whose
                            * disc is out is still closed when the log files are off (ShadowMount+'s log is asked through its API); the tile code is as in 171e;
                            * 171g = game tiles (gametile.c, hosted builds only, off unless the flag gametiles exists); the disc code is as in 171f */

/* 2.05: the "Reload PS2 disc" tile, a permanent one next to the disc tiles (PPSA99231..38). Pressing it starts PS5SX2's eboot (build 171+), which
 * only leaves a note (RELOAD_REQ) and closes at once; this payload then cleans up, asks ShadowMount+ to scan again and makes the tiles afresh. */
#define RELOAD_TID  "PPSA99230"
#define RELOAD_DIR  HB "/" RELOAD_TID
#define RELOAD_MARK WORK "/reload-tile.ok"
#define RELOAD_REV  "1" /* raised when the tile's pictures or files change: its folder is then made again */
#define RELOAD_REQ  "/data/PCSX2/reload.req"
#define SANDBOX_DIR "/mnt/sandbox"

static const char *const kSlots[NSLOTS] = {"PPSA99231", "PPSA99232", "PPSA99233", "PPSA99234",
                                           "PPSA99235", "PPSA99236", "PPSA99237", "PPSA99238"};

/* ---------------------------------------------------------------- log, notification, small helpers */
typedef struct { char unused[45]; char message[3075]; } notify_req_t;
int sceKernelSendNotificationRequest(int32_t device, void *req, size_t size, int32_t blocking);

static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char buf[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog_printf("[disctile] %s\n", buf);
    if (!kp_log_files() || kp_quiet()) return; /* 171e: a KP test build writes no log file; and none while the console shuts down */
    pthread_mutex_lock(&g_log_mu);
    struct stat lst;
    if (stat(LOG_FILE, &lst) == 0 && lst.st_size > (4 << 20)) rename(LOG_FILE, LOG_FILE ".1"); /* rolled over at 4 MB */
    FILE *f = fopen(LOG_FILE, "a");
    if (f) { fprintf(f, "%lld %s\n", (long long)time(NULL), buf); fclose(f); }
    pthread_mutex_unlock(&g_log_mu);
}

static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...) {
    static notify_req_t req; /* one buffer for all threads: used under a lock (three threads may notify at once) */
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    char text[600];
    if (kp_quiet()) return; /* 171e: no request to the shell while the console shuts down */
    pthread_mutex_lock(&mu);
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    snprintf(text, sizeof(text), "%s", req.message);
    pthread_mutex_unlock(&mu);
    say("notify: %s", text);
}

/* A notification about the DISC TILES (a disc went in, a tile is ready, a tile comes later, "no tile yet", the start-up line, the clean-up steps): shown
 * only when /data/PCSX2/switch.txt says 1 (disc_switch.h); the log has the line either way. The result of a reload (the reload tile says "reloaded") and
 * "PS5SX2 is not installed" stay on notify(), and so does every notification of the rest of the payload and of the helper. */
static void notify_tile(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify_tile(const char *fmt, ...) {
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (switch_tile_notices_on()) notify("%s", msg);
    else say("notify (about the tiles, not shown: switch.txt is 0): %s", msg);
}

static int flag(const char *name) {
    char p[200];
    struct stat st;
    snprintf(p, sizeof(p), FLAGS_DIR "/%s", name);
    return stat(p, &st) == 0;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static char *read_file(const char *path, size_t *n_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)(n > 0 ? n : 0) + 1);
    if (!b) { fclose(f); return NULL; }
    const size_t got = n > 0 ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    b[got] = '\0';
    if (n_out) *n_out = got;
    return b;
}

static int write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int ok = fwrite(data, 1, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

/* A line for the log that is not written again while it is the same as the last one and under a minute old (a build that fails the same way every
 * two seconds filled a tester's log with hundreds of lines and said nothing about why). */
static void say_dedupe(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say_dedupe(const char *fmt, ...) {
    static char last[400];
    static time_t last_at;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&mu);
    const int same = strcmp(msg, last) == 0 && time(NULL) - last_at < 60;
    if (!same) { snprintf(last, sizeof(last), "%s", msg); last_at = time(NULL); }
    pthread_mutex_unlock(&mu);
    if (!same) say("%s", msg);
}

static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) { say_dedupe("copy %s: cannot be read (errno %d)", from, errno); return -1; }
    FILE *out = fopen(to, "wb");
    if (!out) { const int e = errno; fclose(in); say_dedupe("copy to %s: cannot be written (errno %d)", to, e); return -1; }
    /* The buffer belongs to this call. It was one static buffer for all of them until 171d: the staging thread copying the 37 MB eboot of the next slot and the
     * tile thread copying PS5SX2's own icon and picture (the cover-less case) used it at the same time, and the eboot of that staged tile came out mixed with
     * bytes of the picture: a tester's tile crashed twice at its first instructions ("privileged instruction fault" in std::string::append) and the Reload
     * tile's new copy ran fine. */
    char *buf = (char *)malloc(1 << 16);
    if (!buf) { fclose(in); fclose(out); say_dedupe("copy to %s: out of memory", to); return -1; }
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, 1 << 16, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
        if (kp_quiet()) { ok = 0; errno = ECANCELED; break; } /* 171e: the 37 MB copy of a staged eboot stops when the console starts to shut down */
    }
    fclose(in);
    free(buf);
    if (!ok) say_dedupe("copy to %s: a write failed (errno %d)", to, errno);
    return (fclose(out) == 0 && ok) ? 0 : -1;
}

/* Are the two files the same, byte for byte? The copy of the eboot that a tile runs is read back and compared with PS5SX2's own: a tile's app once crashed
 * twice at its first instructions ("privileged instruction fault" inside std::string::append, in the log of a tester), and the Reload tile's new copy
 * ran fine, which is what a damaged copy looks like. */
static int files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    char *ba = (char *)malloc(1 << 16), *bb = (char *)malloc(1 << 16);
    int eq = fa && fb && ba && bb;
    while (eq) {
        const size_t na = fread(ba, 1, 1 << 16, fa), nb = fread(bb, 1, 1 << 16, fb);
        if (na != nb || memcmp(ba, bb, na) != 0) eq = 0;
        else if (na == 0) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    free(ba);
    free(bb);
    return eq;
}

static void age(const char *path);

/* A file the tile has to run: by default a real copy with every execute bit set (the shell's launch of a tile died when it started the process,
 * errno 13, EACCES: a copy made by fopen has no execute bit, and a hard link is not looked at the same way by the kernel's launch). The flag
 * disctile_link makes the hard link, as the first builds did. `how` says what was done. */
static int put_exec_file(const char *from, const char *to, char *how, size_t howmax) {
    char note[96] = "copied";
    if (flag("disctile_link")) {
        if (link(from, to) == 0) {
            chmod(to, 0777);
            if (how) snprintf(how, howmax, "hard-linked");
            return 0;
        }
        snprintf(note, sizeof(note), "link failed (errno %d), copied", errno);
    }
    int rc = copy_file(from, to);
    if (rc == 0 && !files_equal(from, to)) { /* not the same as the original: once more, and if it differs again the tile is not made with it */
        say("copy %s -> %s differs from the original: copied again", from, to);
        unlink(to);
        rc = copy_file(from, to);
        if (rc == 0 && !files_equal(from, to)) { say("copy %s -> %s differs again: not used", from, to); unlink(to); rc = -1; }
    }
    if (rc == 0) {
        chmod(to, 0777);
        age(to);
    }
    if (how) snprintf(how, howmax, "%s", note);
    return rc;
}

static void rm_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char c[600];
                snprintf(c, sizeof(c), "%s/%s", path, e->d_name);
                rm_tree(c);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* a file or folder given an old modification time: ShadowMount+ takes up what has been still for a while */
static void age(const char *path) {
    struct timeval tv[2];
    gettimeofday(&tv[0], NULL);
    tv[0].tv_sec -= 3600;
    tv[1] = tv[0];
    utimes(path, tv);
}

/* ---------------------------------------------------------------- the disc: its serial and name */
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static int read_lba(int fd, uint32_t lba, uint8_t *out) { return pread(fd, out, 2048, (off_t)lba * 2048) == 2048; }

/* "BOOT2 = cdrom0:\SLUS_213.51;1" in SYSTEM.CNF -> "SLUS-21351" */
static int read_serial(int fd, char *out) {
    uint8_t pvd[2048];
    if (!read_lba(fd, 16, pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return 0;
    const uint32_t root_lba = le32(pvd + 156 + 2), root_size = le32(pvd + 156 + 10);
    const uint32_t secs = (root_size + 2047) / 2048 < 64 ? (root_size + 2047) / 2048 : 64;
    uint8_t dir[2048];
    for (uint32_t ds = 0; ds < secs; ds++) {
        if (!read_lba(fd, root_lba + ds, dir)) continue;
        for (int off = 0; off < 2048;) {
            const int rlen = dir[off];
            if (rlen == 0 || off + rlen > 2048 || rlen < 34) break;
            const int nlen = dir[off + 32];
            if (nlen >= 10 && strncasecmp((char *)(dir + off + 33), "SYSTEM.CNF", 10) == 0) {
                const uint32_t clba = le32(dir + off + 2);
                uint32_t csz = le32(dir + off + 10);
                if (csz > 4096) csz = 4096;
                char cnf[4097];
                memset(cnf, 0, sizeof(cnf));
                uint8_t sec[2048];
                size_t got = 0;
                for (uint32_t k = 0; k < (csz + 2047) / 2048 && got < csz; k++) {
                    if (!read_lba(fd, clba + k, sec)) break;
                    const size_t take = csz - got < 2048 ? csz - got : 2048;
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
                    if (bs[i] == '_') out[ni++] = '-';
                    else if (bs[i] != '.') out[ni++] = (char)toupper((unsigned char)bs[i]);
                }
                out[ni] = '\0';
                return ni >= 9 && ni <= 12;
            }
            off += rlen;
        }
    }
    return 0;
}

/* The game's name from GameIndex.yaml ("name-en" first, else "name"); the serial itself when it has none. */
static void game_title(const char *serial, char *out, size_t max) {
    snprintf(out, max, "%s", serial);
    FILE *f = fopen(GAMEDB, "r");
    if (!f) return;
    char line[1024], key[40], name[600] = "", name_en[600] = "";
    snprintf(key, sizeof(key), "%s:", serial);
    int in = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!in) { if (strncmp(line, key, strlen(key)) == 0) in = 1; continue; }
        if (line[0] != ' ' && line[0] != '\t') break;
        const char *p = strstr(line, "name-en:");
        char *dst = name_en;
        if (p) p += 8;
        else if ((p = strstr(line, "name:")) != NULL && (p == line || p[-1] == ' ')) { p += 5; dst = name; }
        else continue;
        while (*p == ' ') p++;
        size_t n = strlen(p);
        while (n && (p[n - 1] == '\n' || p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
        if (n >= 2 && p[0] == '"' && p[n - 1] == '"') { p++; n -= 2; }
        if (n >= 600) n = 599;
        memcpy(dst, p, n);
        dst[n] = '\0';
        for (char *q = dst; *q; q++)
            if (q[0] == '\\' && (q[1] == '"' || q[1] == '\\')) memmove(q, q + 1, strlen(q));
    }
    fclose(f);
    if (name_en[0]) snprintf(out, max, "%s", name_en);
    else if (name[0]) snprintf(out, max, "%s", name);
}

/* ---------------------------------------------------------------- ShadowMount+'s API (127.0.0.1:10101) */
static int smp_call(const char *method, const char *route, const char *body, char *reply, size_t reply_size) {
    if (reply_size) reply[0] = '\0';
    if (kp_quiet()) return -1; /* 171e: ShadowMount+ is not asked while the console shuts down (it stops by itself then) */
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SMP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        static time_t last_say;
        if (time(NULL) - last_say >= 30) { last_say = time(NULL); say("ShadowMount+ API not reachable (errno %d): is ShadowMount+ loaded?", errno); }
        close(fd);
        return -1;
    }
    char req[1200];
    const int n = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                           method, route, SMP_PORT, strlen(body), body);
    send(fd, req, (size_t)n, 0);
    char buf[2048];
    size_t got = 0;
    ssize_t r;
    while (got < sizeof(buf) - 1 && (r = recv(fd, buf + got, sizeof(buf) - 1 - got, 0)) > 0) got += (size_t)r;
    buf[got] = '\0';
    close(fd);
    int code = 0;
    if (sscanf(buf, "HTTP/1.%*d %d", &code) != 1) code = 0;
    const char *json = strstr(buf, "\r\n\r\n");
    if (reply_size) snprintf(reply, reply_size, "%s", json ? json + 4 : "");
    return code;
}

static int smp_post(const char *route, const char *body, char *reply, size_t reply_size) { return smp_call("POST", route, body, reply, reply_size); }

/* Is ShadowMount+ holding a title active (after a game, until that title's sandbox is gone): scans are put off, uninstall and mount answer 409
 * "Device busy"? Its answer to a scan does not say ({"queued":true} either way), so it is asked with an uninstall of a title that does not exist: the
 * busy check comes before the lookup (the uninstalls of tiles it never took up, from the tester's log, answered 409 too). 1 busy, 0 free, -1 not there. */
static int smp_busy(void) {
    char reply[200];
    const int code = smp_post("/api/v1/games/uninstall", "{\"title_id\":\"PPSA99299\"}", reply, sizeof(reply));
    if (code <= 0) return -1;
    return code == 409 ? 1 : 0;
}

static void whitelist_add(const char *tid) {
    size_t n = 0;
    char *w = read_file(WHITELIST, &n);
    if (w && strstr(w, tid)) { free(w); return; }
    FILE *f = fopen(WHITELIST, "ab");
    if (f) {
        if (w && n && w[n - 1] != '\n') fputc('\n', f);
        fprintf(f, "%s\n", tid);
        fclose(f);
        say("%s added to " WHITELIST, tid);
    }
    free(w);
}

/* ---------------------------------------------------------------- the PS5's app database: hiding the system's disc tile */
static sqlite3 *open_db(void) {
    sqlite3 *db = NULL;
    if (kp_quiet()) return NULL; /* 171e: the shell's database is left alone while the console shuts down */
    if (sqlite3_open_v2(APP_DB, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        say("cannot open " APP_DB ": %s", db ? sqlite3_errmsg(db) : "no memory");
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 600); /* the main loop must not stand still for seconds when the shell holds the database (it was 5 s): the next look is a second away */
    return db;
}

static int exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    const int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        /* the same failure comes every 2 s while a disc is in (firmware 2.50 has no discStatus column): said once, then counted */
        static char last[200];
        static time_t last_at;
        static int repeats;
        char cur[200];
        snprintf(cur, sizeof(cur), "%d %s | %.100s", rc, err ? err : "?", sql);
        if (strcmp(cur, last) == 0 && time(NULL) - last_at < 600) {
            repeats++;
        } else {
            if (repeats) say("(that sql failure came %d more time(s))", repeats);
            repeats = 0;
            snprintf(last, sizeof(last), "%s", cur);
            last_at = time(NULL);
            say("sql failed (%d): %s | %.100s", rc, err ? err : "?", sql);
        }
        sqlite3_free(err);
        return 0;
    }
    return 1;
}

/* does the table have the column? (the PS5's database differs between firmwares) */
static int table_has_column(sqlite3 *db, const char *table, const char *col) {
    char sql[160];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    sqlite3_stmt *st = NULL;
    int has = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 1);
            if (name && strcasecmp(name, col) == 0) has = 1;
        }
        sqlite3_finalize(st);
    }
    return has;
}

/* the column names of a table, once, for the log (what this firmware's database looks like) */
static void log_columns_once(sqlite3 *db, const char *table) {
    static int done;
    if (done) return;
    done = 1;
    char sql[160], line[900];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    sqlite3_stmt *st = NULL;
    size_t k = 0;
    line[0] = '\0';
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 1);
            if (name && k + 40 < sizeof(line)) k += (size_t)snprintf(line + k, sizeof(line) - k, "%s ", name);
        }
        sqlite3_finalize(st);
    }
    say("database table %s has the columns: %s", table, line);
}

static int icon_tables(sqlite3 *db, char names[][64], int max) {
    sqlite3_stmt *st = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'tbl_iconinfo_%'", -1, &st, NULL) != SQLITE_OK) return 0;
    while (sqlite3_step(st) == SQLITE_ROW && n < max) { snprintf(names[n], 64, "%s", (const char *)sqlite3_column_text(st, 0)); n++; }
    sqlite3_finalize(st);
    return n;
}

/* The home screen reads a tile once; the database's change counters and the row's mTime are raised the way the shell's own writes do it, so
 * that it looks again at once (flag disctile_nonudge turns this off). */
static void nudge(sqlite3 *db) {
    if (flag("disctile_nonudge")) return;
    /* conceptinfo_sync_index too (171d): the Games row is made of CONCEPTS (tbl_concepticoninfo_*, one per game, homebrew included), not of the title rows alone */
    exec(db, "UPDATE tbl_version SET status=status+1 WHERE category IN ('appinfo_sync_index','table_access_index','conceptinfo_sync_index')");
}

#define DISC_TILE_IDS "('NPXS40144','NPXS40148','NPXS40149','NPXS40150')"

/* visible = 0 for the system's disc tiles that say a disc is in; returns how many rows changed. force_nudge (the reload tile): the database's change
 * counters are raised even when no row had to change, so that a tile the home screen still draws although its row says hidden is looked at again. */
static int hide_system_tile_ex(int force_nudge) {
    if (flag("disctile_nohide")) return 0;
    sqlite3 *db = open_db();
    if (!db) return 0;
    static char tabs[16][64];
    const int nt = icon_tables(db, tabs, 16);
    int changed = 0;
    if (nt > 0) log_columns_once(db, tabs[0]);
    /* Look first, read only: this runs every second while a PS2 disc is in, and "nothing to hide" (the usual answer) must not take the database's
     * write lock, which the shell wants too while it installs the tile. */
    int need = 0;
    char disc[16][24];
    for (int i = 0; i < nt; i++) {
        /* firmware 2.50's table has no discStatus column: the row is looked for by its title ID alone */
        snprintf(disc[i], sizeof(disc[i]), "%s", table_has_column(db, tabs[i], "discStatus") ? " AND discStatus=1" : "");
        char sql[300];
        snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE titleId IN " DISC_TILE_IDS " AND visible=1%s LIMIT 1", tabs[i], disc[i]);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) need = 1;
            sqlite3_finalize(st);
        }
    }
    if (!need && !force_nudge) { sqlite3_close(db); return 0; }
    if (exec(db, "BEGIN IMMEDIATE")) {
        for (int i = 0; i < nt; i++) {
            char sql[300];
            snprintf(sql, sizeof(sql), "UPDATE %s SET visible=0 WHERE titleId IN " DISC_TILE_IDS " AND visible=1%s", tabs[i], disc[i]);
            if (exec(db, sql)) changed += sqlite3_changes(db);
        }
        if (changed || force_nudge) nudge(db);
        exec(db, "COMMIT");
    }
    sqlite3_close(db);
    if (changed) say("the system's disc tile hidden (%d row(s))", changed);
    else if (force_nudge) say("the system's disc tile: no row to hide (it is hidden already); the database was nudged so that the home screen looks again");
    return changed;
}

/* One of OUR tiles (PPSA9923x) hidden from the home screen at once (visible = 0, the way the system's disc tile is), or shown again. Used when the disc
 * comes out: the shell's own removal after the uninstall takes seconds (a tester timed 20 s), the row goes first. A tile made again under the same ID
 * (the slots are a ring) is shown again when it is ready. Returns how many rows changed. Main loop only (SQLite). */
static int set_tile_row_visible(const char *tid, int visible) {
    sqlite3 *db = open_db();
    if (!db) return 0;
    static char tabs[16][64];
    const int nt = icon_tables(db, tabs, 16);
    int need = 0;
    for (int i = 0; i < nt; i++) {
        char sql[240];
        snprintf(sql, sizeof(sql), "SELECT 1 FROM %s WHERE titleId='%s' AND visible=%d LIMIT 1", tabs[i], tid, visible ? 0 : 1);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) need = 1;
            sqlite3_finalize(st);
        }
    }
    int changed = 0;
    if (need && exec(db, "BEGIN IMMEDIATE")) {
        for (int i = 0; i < nt; i++) {
            char sql[240];
            snprintf(sql, sizeof(sql), "UPDATE %s SET visible=%d WHERE titleId='%s' AND visible=%d", tabs[i], visible ? 1 : 0, tid, visible ? 0 : 1);
            if (exec(db, sql)) changed += sqlite3_changes(db);
        }
        if (changed) nudge(db);
        exec(db, "COMMIT");
    }
    sqlite3_close(db);
    return changed;
}

static int hide_system_tile(void) { return hide_system_tile_ex(0); }

/* Is there a row for the title in the home screen's tables (the tile is known to the shell)? Read only. -1: the database could not be read (a busy answer is
 * not "no row": a tile is not taken for gone because the shell held the database for a moment). */
static int db_has_title(const char *tid) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(APP_DB, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) { if (db) sqlite3_close(db); return -1; }
    sqlite3_busy_timeout(db, 400);
    static char tabs[16][64];
    const int nt = icon_tables(db, tabs, 16);
    if (nt <= 0) { sqlite3_close(db); return -1; } /* the list of tables could not be read (a busy database): no answer, not "no row" */
    int rows = 0, failed = 0;
    for (int i = 0; i < nt; i++) {
        char sql[200];
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE titleId='%s'", tabs[i], tid);
        sqlite3_stmt *st = NULL;
        int ok = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) { rows += sqlite3_column_int(st, 0); ok = 1; }
            sqlite3_finalize(st);
        }
        if (!ok) failed = 1;
    }
    sqlite3_close(db);
    return (failed && rows == 0) ? -1 : rows;
}

/* ---------------------------------------------------------------- the Reload tile next to the disc tile
 * The home screen lists the games by last use: the newest first, and a title that was just installed counts as used now (lastAccessIndex in the user's
 * tbl_iconinfo_* table, one counter for all titles in tbl_version 'access_index', and lastAccessTime). The Reload tile was installed once and sank behind everything
 * that was used since (a tester's screenshot: twelfth in the row, far from the disc). So, when a PS2 disc is read and before its tile is installed, the
 * Reload tile's row gets the newest place: the number after the highest one and a time a millisecond after the latest, the counter raised to match. The disc
 * tile that the shell installs a few seconds later gets the next number and stands first, the Reload tile second. Afterwards (order_after_install) the log says
 * what the list looks like, and if the shell numbered the disc tile no higher than the Reload tile, the disc tile is put above it. Only these two rows and the
 * counter are written, with values of the kind the shell writes itself. Main loop only (SQLite). Flag disctile_noorder turns it off. */
#define DISC_SLOT_IDS "('PPSA99231','PPSA99232','PPSA99233','PPSA99234','PPSA99235','PPSA99236','PPSA99237','PPSA99238')"

/* one number out of a query: dflt when there is no row, 0 for a NULL */
static long long db_int(sqlite3 *db, const char *sql, long long dflt) {
    long long v = dflt;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_type(st, 0) == SQLITE_NULL ? 0 : sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

/* The concept table that goes with a user's title table: tbl_iconinfo_<n> -> tbl_concepticoninfo_<n> (the Games row is made of concepts: a tile's place and
 * its being there at all are decided by its concept row as well as by its title row). 0 when the database has none. */
static int concept_table_for(sqlite3 *db, const char *icon_table, char *out, size_t max) {
    snprintf(out, max, "tbl_concepticoninfo_%s", icon_table + strlen("tbl_iconinfo_"));
    char sql[200];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='%s'", out);
    return db_int(db, sql, 0) > 0;
}

static int table_exists(sqlite3 *db, const char *name) {
    char sql[200];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='%s'", name);
    return db_int(db, sql, 0) > 0;
}

static int reload_row_front(const char *why) {
    if (flag("disctile_noorder") || flag("disctile_noreload")) return 0;
    sqlite3 *db = open_db();
    if (!db) return 0;
    static char tabs[16][64];
    const int nt = icon_tables(db, tabs, 16);
    long long counter = db_int(db, "SELECT status FROM tbl_version WHERE category='access_index'", 0);
    int moved = 0;
    if (nt > 0 && exec(db, "BEGIN IMMEDIATE")) {
        for (int i = 0; i < nt; i++) {
            char sql[700];
            snprintf(sql, sizeof(sql), "SELECT lastAccessIndex FROM %s WHERE titleId='" RELOAD_TID "'", tabs[i]);
            const long long cur = db_int(db, sql, -2); /* -2: this user's table has no such row */
            if (cur == -2) continue;
            snprintf(sql, sizeof(sql), "SELECT MAX(lastAccessIndex) FROM %s WHERE titleId<>'" RELOAD_TID "' AND titleId NOT IN " DISC_SLOT_IDS, tabs[i]);
            const long long others = db_int(db, sql, 0);
            if (cur > others) continue; /* newer than everything else but the disc tiles already */
            snprintf(sql, sizeof(sql), "SELECT MAX(lastAccessIndex) FROM %s", tabs[i]);
            const long long all = db_int(db, sql, 0);
            const long long next = (all > counter ? all : counter) + 1;
            snprintf(sql, sizeof(sql),
                     "UPDATE %s SET lastAccessIndex=%lld, lastAccessTime=COALESCE((SELECT strftime('%%Y-%%m-%%d %%H:%%M:%%f', MAX(lastAccessTime), '+0.001 seconds') FROM %s), lastAccessTime) "
                     "WHERE titleId='" RELOAD_TID "'",
                     tabs[i], next, tabs[i]);
            if (exec(db, sql) && sqlite3_changes(db) > 0) {
                moved++;
                counter = next;
                say("the Reload tile is moved to the front of the home screen's list (%s): number %lld -> %lld in %s, the newest of the others is %lld", why, cur, next, tabs[i], others);
                char ct[80];
                if (concept_table_for(db, tabs[i], ct, sizeof(ct))) { /* the Games row is made of concepts: the same number and time there */
                    snprintf(sql, sizeof(sql),
                             "UPDATE %s SET lastAccessIndex=%lld, lastInteractedTime=(SELECT lastAccessTime FROM %s WHERE titleId='" RELOAD_TID "') WHERE primaryTitleId='" RELOAD_TID "'",
                             ct, next, tabs[i]);
                    exec(db, sql);
                }
            }
        }
        if (moved) {
            char sql[200];
            snprintf(sql, sizeof(sql), "UPDATE tbl_version SET status=%lld WHERE category='access_index' AND status<%lld", counter, counter);
            exec(db, sql);
            nudge(db);
        }
        exec(db, "COMMIT");
    }
    sqlite3_close(db);
    return moved;
}

/* When a disc tile is ready: the log gets the tile's row (platform, badge, place) and the top of the list; a disc tile that the shell numbered no higher than
 * the Reload tile is put above it. */
static void order_after_install(const char *tid) {
    sqlite3 *db = open_db();
    if (!db) return;
    static char tabs[16][64];
    const int nt = icon_tables(db, tabs, 16);
    long long counter = db_int(db, "SELECT status FROM tbl_version WHERE category='access_index'", 0);
    int fixed = 0;
    if (nt > 0 && exec(db, "BEGIN IMMEDIATE")) {
        for (int i = 0; i < nt; i++) {
            char sql[700];
            snprintf(sql, sizeof(sql), "SELECT lastAccessIndex FROM %s WHERE titleId='%s'", tabs[i], tid);
            const long long d = db_int(db, sql, -2);
            if (d == -2) continue;
            snprintf(sql, sizeof(sql), "SELECT lastAccessIndex FROM %s WHERE titleId='" RELOAD_TID "'", tabs[i]);
            const long long r = db_int(db, sql, -2);
            char row[200] = "", top[300] = "";
            snprintf(sql, sizeof(sql), "SELECT platform, contentBadgeType, dispLocation, primaryTitleSort, visible FROM %s WHERE titleId='%s'", tabs[i], tid);
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                if (sqlite3_step(st) == SQLITE_ROW)
                    snprintf(row, sizeof(row), "platform %d, badge %d, place %d, sort %lld, visible %d", sqlite3_column_int(st, 0), sqlite3_column_int(st, 1), sqlite3_column_int(st, 2),
                             (long long)sqlite3_column_int64(st, 3), sqlite3_column_int(st, 4));
                sqlite3_finalize(st);
            }
            snprintf(sql, sizeof(sql), "SELECT titleId, dispLocation, lastAccessIndex FROM %s WHERE visible=1 ORDER BY lastAccessIndex DESC LIMIT 6", tabs[i]);
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                size_t k = 0;
                while (sqlite3_step(st) == SQLITE_ROW && k + 40 < sizeof(top))
                    k += (size_t)snprintf(top + k, sizeof(top) - k, "%s/%d/%d ", (const char *)sqlite3_column_text(st, 0), sqlite3_column_int(st, 1), sqlite3_column_int(st, 2));
                sqlite3_finalize(st);
            }
            char crow[160] = "none";
            char ct0[80];
            if (concept_table_for(db, tabs[i], ct0, sizeof(ct0))) {
                snprintf(sql, sizeof(sql), "SELECT lastAccessIndex, dispLocation, validFlag FROM %s WHERE primaryTitleId='%s'", ct0, tid);
                if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                    if (sqlite3_step(st) == SQLITE_ROW) snprintf(crow, sizeof(crow), "number %d, place %d, valid %d", sqlite3_column_int(st, 0), sqlite3_column_int(st, 1), sqlite3_column_int(st, 2));
                    sqlite3_finalize(st);
                }
            }
            say("home screen list (%s): %s number %lld, Reload tile number %lld; its row: %s; its concept row: %s; top of the list (title/place/number): %s", tabs[i], tid, d,
                r == -2 ? -1LL : r, row, crow, top);
            if (r != -2 && d <= r && !flag("disctile_noorder")) {
                const long long next = (r > counter ? r : counter) + 1;
                snprintf(sql, sizeof(sql),
                         "UPDATE %s SET lastAccessIndex=%lld, lastAccessTime=COALESCE((SELECT strftime('%%Y-%%m-%%d %%H:%%M:%%f', MAX(lastAccessTime), '+0.001 seconds') FROM %s), lastAccessTime) "
                         "WHERE titleId='%s'",
                         tabs[i], next, tabs[i], tid);
                if (exec(db, sql) && sqlite3_changes(db) > 0) {
                    fixed++;
                    counter = next;
                    say("the disc tile %s was numbered %lld, not above the Reload tile (%lld): it is put above it (%lld)", tid, d, r, next);
                    char ct[80];
                    if (concept_table_for(db, tabs[i], ct, sizeof(ct))) {
                        snprintf(sql, sizeof(sql),
                                 "UPDATE %s SET lastAccessIndex=%lld, lastInteractedTime=(SELECT lastAccessTime FROM %s WHERE titleId='%s') WHERE primaryTitleId='%s'",
                                 ct, next, tabs[i], tid, tid);
                        exec(db, sql);
                    }
                }
            }
        }
        if (fixed) {
            char sql[200];
            snprintf(sql, sizeof(sql), "UPDATE tbl_version SET status=%lld WHERE category='access_index' AND status<%lld", counter, counter);
            exec(db, sql);
            nudge(db);
        }
        exec(db, "COMMIT");
    }
    sqlite3_close(db);
}

/* ---------------------------------------------------------------- the rows a removed tile leaves behind
 * After its uninstall went through, a tile's rows STAY in the shell's database: a tester's log showed the title list of ShadowMount+ (tbl_contentinfo) not
 * shrinking by one after any of 25 uninstalls (92, 93, 94 ... 98, and then 90 only after the console was restarted), our own look finding the title's two rows
 * (one per user) 90 s after the uninstall was accepted, and the home screen still loading the removed tile's picture from /user/app/<ID>/sce_sys 40 s
 * after. The Games row is made of concepts, and the tile stays there as long as its rows do. A slot used again then also gets no fresh registration
 * ShadowMount+ finds the title in tbl_contentinfo, logs "Skip (already present in app.db)", and the tile keeps the previous disc's name and cover (the
 * "reload did not refresh the cover" report). So when the tile's folder and its /user/app folder are gone, its rows are deleted: the title row and the concept
 * row of every user, the title's row in tbl_contentinfo and its concept's row in tbl_conceptmetadata. Only for one of OUR tile IDs (the caller), never for a
 * tile that is installed. Returns the number of rows deleted (-1: the database could not be opened). Main loop only (SQLite). Flag disctile_nopurge. */
static int safe_db_word(const char *s) {
    if (!s || !s[0]) return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == ':' || *s == '_' || *s == '-')) return 0;
    return 1;
}

static int purge_title_rows(const char *tid) {
    if (flag("disctile_nopurge") || !safe_db_word(tid)) return 0;
    sqlite3 *db = open_db();
    if (!db) return -1;
    static char tabs[16][64];
    const int nt = icon_tables(db, tabs, 16);
    int n_icon = 0, n_concept = 0, n_content = 0, n_meta = 0;
    if (nt > 0 && exec(db, "BEGIN IMMEDIATE")) {
        char cids[256] = ""; /* 'cid:scp:...','cid:scp:...' : the concepts of the title, as an SQL list */
        size_t cl = 0;
        for (int i = 0; i < nt; i++) {
            char sql[300];
            snprintf(sql, sizeof(sql), "SELECT localConceptId FROM %s WHERE titleId='%s'", tabs[i], tid);
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW) {
                    const char *c = (const char *)sqlite3_column_text(st, 0);
                    if (c && safe_db_word(c) && !strstr(cids, c) && cl + strlen(c) + 4 < sizeof(cids))
                        cl += (size_t)snprintf(cids + cl, sizeof(cids) - cl, "%s'%s'", cl ? "," : "", c);
                }
                sqlite3_finalize(st);
            }
        }
        { /* 171k: a concept that a title outside PS5SX2's IDs uses too (a PS5SX3 tile's, a game's) keeps its rows (disc_compat_db.h) */
            static char ctabs[16][80];
            for (int i = 0; i < nt; i++)
                if (!concept_table_for(db, tabs[i], ctabs[i], sizeof(ctabs[i]))) ctabs[i][0] = '\0';
            char kept[200];
            if (cids[0] && dc_db_keep_shared_concepts(db, tabs, ctabs, nt, cids, sizeof(cids), kept, sizeof(kept)) > 0)
                say("tile %s: its concept(s) %s are used by a title that is not PS5SX2's too: their rows stay", tid, kept);
        }
        for (int i = 0; i < nt; i++) {
            char sql[500], ct[80];
            snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE titleId='%s'", tabs[i], tid);
            if (exec(db, sql)) n_icon += sqlite3_changes(db);
            if (concept_table_for(db, tabs[i], ct, sizeof(ct))) {
                snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE primaryTitleId='%s'%s%s%s", ct, tid, cids[0] ? " OR localConceptId IN (" : "", cids, cids[0] ? ")" : "");
                if (exec(db, sql)) n_concept += sqlite3_changes(db);
            }
        }
        if (table_exists(db, "tbl_contentinfo")) {
            char sql[300];
            snprintf(sql, sizeof(sql), "DELETE FROM tbl_contentinfo WHERE titleId='%s'", tid);
            if (exec(db, sql)) n_content += sqlite3_changes(db);
        }
        if (cids[0] && table_exists(db, "tbl_conceptmetadata")) {
            char sql[400];
            snprintf(sql, sizeof(sql), "DELETE FROM tbl_conceptmetadata WHERE localConceptId IN (%s)", cids);
            if (exec(db, sql)) n_meta += sqlite3_changes(db);
        }
        if (n_icon + n_concept + n_content + n_meta > 0) nudge(db);
        exec(db, "COMMIT");
    }
    sqlite3_close(db);
    const int total = n_icon + n_concept + n_content + n_meta;
    if (total > 0) say("the leftover rows of the removed tile %s are deleted: %d title row(s), %d concept row(s), %d content row(s), %d concept metadata row(s)", tid, n_icon, n_concept, n_content, n_meta);
    return total;
}

/* Is the tile's title mounted where the shell starts apps from? Looked for by the mount table (whatever the path), and by the usual folder. */
static int title_mounted(const char *tid, char *where, size_t max) {
    where[0] = '\0';
    struct statfs *m = NULL;
    const int n = getmntinfo(&m, MNT_NOWAIT);
    for (int i = 0; i < n; i++) {
        if (strstr(m[i].f_mntonname, tid) || strstr(m[i].f_mntfromname, tid)) { snprintf(where, max, "%s", m[i].f_mntonname); return 1; }
    }
    char p[160];
    snprintf(p, sizeof(p), "/system_ex/app/%s/eboot.bin", tid);
    if (exists(p)) { snprintf(where, max, "/system_ex/app/%s", tid); return 1; }
    return 0;
}

static int whitelisted(const char *tid) {
    char *w = read_file(WHITELIST, NULL);
    const int r = w && strstr(w, tid) != NULL;
    free(w);
    return r;
}

/* Puts them back (only for a disc that is still in: the system hides them itself when a disc goes out). */
static void show_system_tile(int disc_present) {
    sqlite3 *db = open_db();
    if (!db) return;
    static char tabs[16][64];
    const int nt = icon_tables(db, tabs, 16);
    int changed = 0;
    if (exec(db, "BEGIN IMMEDIATE")) {
        for (int i = 0; i < nt; i++) {
            char sql[300];
            const int has_disc = table_has_column(db, tabs[i], "discStatus");
            if (!has_disc && !disc_present) continue; /* no discStatus to tell: with no disc in, the hidden row is the system's own state */
            snprintf(sql, sizeof(sql), "UPDATE %s SET visible=1 WHERE titleId IN " DISC_TILE_IDS " AND visible=0%s", tabs[i], has_disc ? " AND discStatus=1" : "");
            if (exec(db, sql)) changed += sqlite3_changes(db);
        }
        if (changed) nudge(db);
        exec(db, "COMMIT");
    }
    sqlite3_close(db);
    if (changed) say("the system's disc tile shown again (%d row(s))", changed);
}

/* ---------------------------------------------------------------- the art */
int sgdb_fetch_art(const char *key, const char *name, const char *hero, const char *logo, const char *grid, char *msg, size_t msgmax);

static int read_key(char *out, size_t max) {
    const char *paths[] = {KEY_FILE, KEY_FILE2};
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        out[0] = '\0';
        if (fgets(out, (int)max, f)) out[strcspn(out, "\r\n \t")] = '\0';
        fclose(f);
        if (out[0]) return 1;
    }
    return 0;
}

static void mkdirs(const char *path) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0777); *p = '/'; }
    mkdir(tmp, 0777);
}

/* hero/logo/grid for a disc, kept in art/<serial>/ (".none" = SteamGridDB had nothing, not asked again for a day); "" for the ones missing.
 * again (a reload): all three are asked for afresh, saved beside the old ones and put in their place only when they came (the old ones stay otherwise),
 * and an earlier "nothing" is forgotten. */
static void sgdb_art(const char *serial, const char *title, char *hero, char *logo, char *grid, size_t size, int again) {
    hero[0] = logo[0] = grid[0] = '\0';
    if (flag("disctile_nodownload")) return;
    char dir[200], h[240], l[240], g[240], none[240];
    snprintf(dir, sizeof(dir), ART_DIR "/%s", serial);
    mkdirs(dir);
    snprintf(h, sizeof(h), "%s/hero.img", dir);
    snprintf(l, sizeof(l), "%s/logo.img", dir);
    snprintf(g, sizeof(g), "%s/grid.img", dir);
    snprintf(none, sizeof(none), "%s/.none", dir);
    struct stat st;
    if (again) unlink(none);
    if (again || (!(exists(h) && exists(l) && exists(g)) && !(stat(none, &st) == 0 && time(NULL) - st.st_mtime < 24 * 3600))) {
        char key[200];
        if (!read_key(key, sizeof(key))) {
            static char said[32];
            if (strcmp(said, serial) != 0) { snprintf(said, sizeof(said), "%s", serial); say("no SteamGridDB key (one line in " KEY_FILE "): the tile uses the cover only"); }
        } else {
            char msg[400], nh[250], nl[250], ng[250];
            snprintf(nh, sizeof(nh), "%s.new", h);
            snprintf(nl, sizeof(nl), "%s.new", l);
            snprintf(ng, sizeof(ng), "%s.new", g);
            const int mask = sgdb_fetch_art(key, title, again ? nh : (exists(h) ? NULL : h), again ? nl : (exists(l) ? NULL : l), again ? ng : (exists(g) ? NULL : g),
                                            msg, sizeof(msg));
            say("SteamGridDB for %s%s: %s", serial, again ? " (again)" : "", msg);
            if (again) {
                const char *const from[3] = {nh, nl, ng}, *const to[3] = {h, l, g};
                for (int k = 0; k < 3; k++) {
                    if (mask > 0 && (mask & (1 << k)) && exists(from[k]) && rename(from[k], to[k]) == 0) continue;
                    unlink(from[k]); /* not fetched, or not put in place: the old copy (if any) stays */
                }
            }
            if (mask <= 0 && !(exists(h) || exists(l) || exists(g))) write_file(none, msg, strlen(msg));
        }
    }
    if (exists(h)) snprintf(hero, size, "%s", h);
    if (exists(l)) snprintf(logo, size, "%s", l);
    if (exists(g)) snprintf(grid, size, "%s", g);
}

int cover_fetch(const char *serial, const char *dest, char *msg, size_t msgmax);

/* Serials whose cover is asked for again (the reload tile marks the discs in the drives): cover_get then downloads a fresh copy even when one is on the
 * console, and forgets an earlier "there is none" (404) answer. The old copy is kept when the new download fails. */
static char g_cover_force[MAX_DRIVES][16];
static pthread_mutex_t g_cover_mu = PTHREAD_MUTEX_INITIALIZER;

static void cover_force_mark(const char *serial) {
    pthread_mutex_lock(&g_cover_mu);
    int free_i = -1, have = 0;
    for (int i = 0; i < MAX_DRIVES; i++) {
        if (!strcmp(g_cover_force[i], serial)) have = 1;
        else if (!g_cover_force[i][0] && free_i < 0) free_i = i;
    }
    if (!have && free_i >= 0) snprintf(g_cover_force[free_i], sizeof(g_cover_force[free_i]), "%s", serial);
    pthread_mutex_unlock(&g_cover_mu);
}

static int cover_force_take(const char *serial) {
    int took = 0;
    pthread_mutex_lock(&g_cover_mu);
    for (int i = 0; i < MAX_DRIVES; i++)
        if (!strcmp(g_cover_force[i], serial)) { g_cover_force[i][0] = '\0'; took = 1; }
    pthread_mutex_unlock(&g_cover_mu);
    return took;
}

/* The game's cover from the public set PS5SX2's own shelf uses (no key needed), kept in the shelf's cover folder; after a miss it is not asked
 * again for a day (not after a reload: again = 1 downloads it afresh even when one is there). A tile made without a SteamGridDB key then still has the
 * game's cover, not PS5SX2's icon. */
static void cover_get(const char *serial, const char *dest, int again) {
    if (flag("disctile_nodownload")) return;
    if (!again && exists(dest)) return;
    char none[240];
    snprintf(none, sizeof(none), ART_DIR "/%s-nocover", serial);
    struct stat st;
    if (again) unlink(none);
    else if (stat(none, &st) == 0 && time(NULL) - st.st_mtime < 24 * 3600) return;
    mkdirs(COVERS);
    char msg[240], fresh[300];
    snprintf(fresh, sizeof(fresh), "%s.new", dest);
    int r = cover_fetch(serial, again ? fresh : dest, msg, sizeof(msg));
    /* A failed lookup (the console's DNS answers now and then not: a tester's log) is asked once more a second later, but not when the last attempt failed
     * less than a minute ago: a console without network is not made to wait for every tile. */
    static time_t net_failed_at;
    if (r == -1) {
        if (time(NULL) - net_failed_at >= 60) {
            usleep(1000000);
            r = cover_fetch(serial, again ? fresh : dest, msg, sizeof(msg));
        }
        if (r == -1) net_failed_at = time(NULL);
    }
    if (r == 0) net_failed_at = 0;
    if (r == 0) {
        if (again) {
            /* the copy that was there stays beside the new one as <serial>.jpg.old (a cover the person put there himself is not lost) */
            char old[310];
            snprintf(old, sizeof(old), "%s.old", dest);
            const int had = exists(dest);
            if (had) rename(dest, old);
            if (rename(fresh, dest) != 0) {
                say("cover for %s: the fresh copy could not replace the old one (errno %d)", serial, errno);
                if (had) rename(old, dest);
                unlink(fresh);
                return;
            }
        }
        say("cover for %s %s (%s)", serial, again ? "downloaded again" : "downloaded", msg);
    } else {
        if (again) unlink(fresh);
        say("no cover for %s: %s%s", serial, msg, again && exists(dest) ? " (the copy already on the console stays)" : "");
        if (r == 1 && !exists(dest)) write_file(none, msg, strlen(msg));
    }
}

/* ---------------------------------------------------------------- ShadowMount+'s own "Installing: ..." popup
 * For every title it takes up (our tiles too) ShadowMount+ shows a popup "Installing: <title> (<ID>)..." (it comes as a "Debug:" notification). Its setting
 * quiet_mode=1 in /data/shadowmount/config.ini keeps only its start-up popup and its errors: in its source the informational popups go through
 * notify_system_info*(), which that setting gates, while the shell's rich "Installed." toast is sent by sceNotificationSendById and is NOT gated: it stays,
 * which is what was asked (the popup on the left of the tester's screenshot goes, the "Installed." toast on the right stays). ShadowMount+ watches
 * config.ini and applies a change at once. What the file had is written to disc-tiles/smp-quiet-was.txt the first time ("absent", "0"), so that the flag
 * disctile_smnotify (leave ShadowMount+'s popups alone) can put it back. Only the one line is touched; every other line of the file stays as it is. */
#define SMP_DIR       "/data/shadowmount"
#define SMP_CONFIG    SMP_DIR "/config.ini"
#define SMP_QUIET_WAS WORK "/smp-quiet-was.txt"

/* The LAST uncommented "quiet_mode = value" line of an ini text (the last assignment is the one that counts): its start and its end (after the newline). Returns
 * 1 when its value is true (1, true, yes, on), 0 for any other value (ShadowMount+ ignores an invalid one), -1 when there is no such line. */
static int ini_quiet_line(const char *txt, size_t *start, size_t *end) {
    int val = -1;
    const size_t n = strlen(txt);
    size_t pos = 0;
    while (pos < n) {
        size_t eol = pos;
        while (eol < n && txt[eol] != '\n') eol++;
        const size_t next = eol < n ? eol + 1 : eol;
        const char *p = txt + pos, *e = txt + eol;
        while (p < e && (*p == ' ' || *p == '\t')) p++;
        if (e - p > 10 && *p != '#' && *p != ';' && strncasecmp(p, "quiet_mode", 10) == 0) {
            const char *q = p + 10;
            while (q < e && (*q == ' ' || *q == '\t')) q++;
            if (q < e && *q == '=') {
                q++;
                while (q < e && (*q == ' ' || *q == '\t')) q++;
                char v[16];
                size_t k = 0;
                while (q < e && *q != ' ' && *q != '\t' && *q != '\r' && *q != '#' && *q != ';' && k < sizeof(v) - 1) v[k++] = (char)tolower((unsigned char)*q++);
                v[k] = '\0';
                val = (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes") || !strcmp(v, "on")) ? 1 : 0;
                *start = pos;
                *end = next;
            }
        }
        pos = next;
    }
    return val;
}

/* config.ini rewritten whole and atomically (a temporary file, then rename: what ShadowMount+'s own settings update does, and what its watcher is made for) */
static int smp_config_write(const char *txt, size_t n) {
    mkdir(SMP_DIR, 0777);
    const char *tmp = SMP_CONFIG ".disctile";
    if (write_file(tmp, txt, n) != 0) return -1;
    chmod(tmp, 0666);
    if (rename(tmp, SMP_CONFIG) != 0) { unlink(tmp); return -1; }
    return 0;
}

static void smp_quiet_ensure(void) {
    const int keep = flag("disctile_smnotify");
    size_t n = 0, ls = 0, le = 0;
    char *cfg = read_file(SMP_CONFIG, &n);
    const int cur = cfg ? ini_quiet_line(cfg, &ls, &le) : -1;
    char *out = (char *)malloc(n + 64);
    if (!out) { free(cfg); return; }
    size_t o = 0;
    if (!keep) {
        if (cur == 1) { free(cfg); free(out); return; } /* already quiet: nothing to do */
        if (!exists(SMP_QUIET_WAS)) write_file(SMP_QUIET_WAS, cur < 0 ? "absent\n" : "0\n", cur < 0 ? 7 : 2);
        if (cur >= 0) {
            memcpy(out, cfg, ls);
            o = ls;
            o += (size_t)sprintf(out + o, "quiet_mode=1\n");
            memcpy(out + o, cfg + le, n - le);
            o += n - le;
        } else {
            if (cfg && n) { memcpy(out, cfg, n); o = n; if (cfg[n - 1] != '\n') out[o++] = '\n'; }
            o += (size_t)sprintf(out + o, "quiet_mode=1\n");
        }
        say("ShadowMount+: quiet_mode=1 written to " SMP_CONFIG " (%s): its \"Installing: ...\" popup stays away, the shell's \"Installed.\" toast stays "
            "(flag disctile_smnotify puts it back)", cur < 0 ? "the line was not there" : "it was 0");
    } else {
        char *was = read_file(SMP_QUIET_WAS, NULL);
        if (!was) { free(cfg); free(out); return; } /* never changed by this payload: nothing to put back */
        if (cur == 1) {
            memcpy(out, cfg, ls);
            o = ls;
            if (strncmp(was, "absent", 6) != 0) o += (size_t)sprintf(out + o, "quiet_mode=0\n"); /* it said 0 before; "absent": the line goes */
            memcpy(out + o, cfg + le, n - le);
            o += n - le;
            say("ShadowMount+: quiet_mode put back as it was (%s): flag disctile_smnotify", strncmp(was, "absent", 6) != 0 ? "0" : "no line");
        } else {
            free(cfg); free(out); free(was);
            unlink(SMP_QUIET_WAS);
            return;
        }
        free(was);
        unlink(SMP_QUIET_WAS);
    }
    out[o] = '\0';
    if (smp_config_write(out, o) != 0) say("ShadowMount+: " SMP_CONFIG " could not be written (errno %d)", errno);
    free(cfg);
    free(out);
}

/* ---------------------------------------------------------------- the tiles */
typedef struct {
    int present;       /* a disc was readable last time */
    off_t media;       /* its size, to notice a swap without reading */
    char serial[16];   /* "" = no PS2 serial (yet, or not a PS2 disc) */
    int tries;         /* serial reads since it went in */
    int slot;          /* its tile, -1 = none */
    int gone;          /* polls with no disc */
    double t_gone0;    /* when the first of them was (monotonic seconds) */
    volatile int building; /* the tile is being made on its own thread */
    int ready;         /* the tile is registered AND mounted: only then the system's disc tile is hidden and the person is told */
    time_t ready_t0;   /* when the scan was taken (the wait for the mount is counted from here), 0 = not yet */
    int diag;          /* how many of the state lines were written while waiting */
    int kinded;        /* 171k: the disc's kind (disc_compat.h, its root folder) was asked once */
    char title[96];    /* for the "ready" notification */
    double t_disc, t_serial, t_built, t_scan; /* when (monotonic seconds): the disc went in, its serial was read, the tile was built, the scan was asked for */
    int warm;          /* the tile came from a staged folder */
} Drive;

static Drive g_drv[MAX_DRIVES];
static int g_slot_busy[NSLOTS];  /* drive index + 1, 0 = free */
static int g_slot_stale[NSLOTS]; /* the uninstall is still to be done (it was refused as busy, or is being asked for): the slot is not used meanwhile */
static volatile int g_scan_pending;    /* the scan that registers new tiles did not happen (ShadowMount+ down, or a game was active) */
static time_t g_scan_next;
static int g_scan_tries;
static int g_next_slot;
static pthread_mutex_t g_state_mu = PTHREAD_MUTEX_INITIALIZER;

static void json_escape(char *dst, size_t size, const char *src) {
    size_t k = 0;
    for (const char *p = src; *p && k + 8 < size; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { dst[k++] = '\\'; dst[k++] = (char)c; }
        else if (c < 0x20) k += (size_t)snprintf(dst + k, size - k, "\\u%04x", c);
        else dst[k++] = (char)c;
    }
    dst[k] = '\0';
}

static char *replace_all(const char *s, const char *from, const char *to) {
    const size_t fl = strlen(from), tl = strlen(to);
    size_t count = 0;
    for (const char *p = s; (p = strstr(p, from)) != NULL; p += fl) count++;
    char *out = (char *)malloc(strlen(s) + count * (tl > fl ? tl - fl : 0) + 1);
    if (!out) return NULL;
    char *o = out;
    for (const char *p = s;;) {
        const char *q = strstr(p, from);
        if (!q) { strcpy(o, p); break; }
        memcpy(o, p, (size_t)(q - p));
        o += q - p;
        memcpy(o, to, tl);
        o += tl;
        p = q + fl;
    }
    return out;
}

/* Cleans up the leftover sandboxes of our titles at once (it is called when an uninstall was refused as busy): defined with the sandbox code below. */
static int heal_sandboxes(int min_age, const char *why);
static double mono_s(void);

/* ---------------------------------------------------------------- taking a tile down
 * A tile goes when its disc goes (testers timed 18 to 20 s from the eject to the tile leaving the home screen). ShadowMount+'s uninstall call is synchronous
 * (the shell's own uninstall runs inside it) and is refused for as long as ShadowMount+ holds a title active or is scanning (409), so it does not run on the
 * main loop, which can be held up by the database or by ShadowMount+'s other answers: one thread per slot does it, asking every 0.4 s at first, and cleaning up
 * the leftover sandboxes of the tile's game when it is refused. The main loop only follows (gone_tick): the tile's row is kept hidden, the home screen is
 * nudged to look again, and the log says when the shell's list lost the title and how the seconds were spent. g_slot_stale keeps the slot from being used again
 * until the uninstall went through. */
static volatile sig_atomic_t g_stop;
typedef struct { int active; int accepted; int purges; int rows_deleted; double t_out, t_acc, t_purge; } GoneTrack; /* when the media went out, when the uninstall was accepted (monotonic seconds), the purges of the leftover rows */
static GoneTrack g_gone[NSLOTS];
static volatile int g_rm_running[NSLOTS]; /* a removal thread works on the slot */
static double g_remove_t0;                /* set by poll_drive: when the media went away (0 = now) */

/* ---- closing the game of a tile whose disc went out
 * ShadowMount+ refuses the uninstall of a title whose game is still running ("a game is active or awaiting exit", error_reason game_active): a tester went
 * to the home screen with the game still open, took the disc out, and the tile stayed for the six minutes until ShadowMount+ was loaded again. Without its
 * disc the game cannot go on, so it is closed: the pid comes from ShadowMount+'s log ("[GAME] started: <ID> pid=N app_id=0x..."), which is kept in a file here.
 * Not while the game is in front (the last "SceShellCoreUtilAppFocus" line names its app id): then the person is looking at it. */
static int tile_game_from_log(const char *text, const char *tid, int *pid_out, unsigned *app_out, int *front_out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "[GAME] started: %s pid=", tid);
    const char *last = NULL;
    for (const char *p = text; (p = strstr(p, needle)) != NULL; p += strlen(needle)) last = p;
    if (!last) return 0;
    const int pid = atoi(last + strlen(needle));
    const char *ap = strstr(last, "app_id=0x");
    const unsigned app = ap ? (unsigned)strtoul(ap + strlen("app_id=0x"), NULL, 16) : 0;
    if (pid <= 1) return 0;
    char gone1[96], gone2[96];
    snprintf(gone1, sizeof(gone1), "game stopped: pid=%d title=%s", pid, tid);
    snprintf(gone2, sizeof(gone2), "awaiting same-app handoff: %s pid=%d", tid, pid);
    if (strstr(last + 1, gone1) || strstr(last + 1, gone2)) return 0; /* it stopped after it started */
    const char *fnd = "SceShellCoreUtilAppFocus value=0x";
    const char *foc = NULL;
    for (const char *p = text; (p = strstr(p, fnd)) != NULL; p += strlen(fnd)) foc = p;
    *pid_out = pid;
    *app_out = app;
    *front_out = foc && app && (unsigned)strtoull(foc + strlen(fnd), NULL, 16) == app;
    return 1;
}

/* is the process with this pid an app process (eboot.bin) that is still there? */
static int proc_is_app(int pid) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0) return 0;
    char *buf = (char *)calloc(1, len + 4096);
    if (!buf) return 0;
    len += 4096;
    int is = 0;
    if (sysctl(mib, 4, buf, &len, NULL, 0) == 0 && len >= offsetof(struct kinfo_proc, ki_comm) + sizeof(((struct kinfo_proc *)0)->ki_comm)) {
        const struct kinfo_proc *kp = (const struct kinfo_proc *)buf;
        is = kp->ki_pid == pid && strcmp(kp->ki_comm, "eboot.bin") == 0 && kp->ki_stat != 5 /* SZOMB */;
    }
    free(buf);
    return is;
}

/* does the tile have an app sandbox (/mnt/sandbox/<ID>_NNN)? Its game cannot be running without one: a pid from the log is not trusted without it */
static int tile_sandbox_present(const char *tid) {
    DIR *d = opendir(SANDBOX_DIR);
    if (!d) return 0;
    const size_t l = strlen(tid);
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strncmp(e->d_name, tid, l) == 0 && e->d_name[l] == '_') { found = 1; break; }
    closedir(d);
    return found;
}

/* ShadowMount+'s debug log as text: the file this payload keeps (follow_logs, only with log files on) when it is there, else the log asked from ShadowMount+'s
 * API (171f: the default build writes no log files, and without this close_tile_game would never find a game). NULL when neither works. The caller frees it. */
static int smp_fetch(const char *route, const char *body, char **out, size_t *outlen);
static size_t json_field_text(const char *json, const char *key, char *dst, size_t max);
static char *shadowmount_debug_text(size_t *n) {
    if (kp_log_files()) {
        char *t = read_file(WORK "/shadowmount-debug.log", n);
        if (t) return t;
    }
    char *body = NULL;
    size_t bl = 0;
    const int code = smp_fetch("/api/v1/debug-log", "{\"max_bytes\":262144}", &body, &bl);
    if (code != 200 || !body) { free(body); return NULL; }
    char *txt = (char *)malloc(bl + 1);
    if (!txt) { free(body); return NULL; }
    *n = json_field_text(body, "content", txt, bl + 1);
    free(body);
    if (*n == 0) { free(txt); return NULL; }
    return txt;
}

/* 1 when a game was closed */
static int close_tile_game(int slot) {
    const char *tid = kSlots[slot];
    size_t n = 0;
    char *text = shadowmount_debug_text(&n);
    if (!text) { say("tile %s: the log of ShadowMount+ is not there: its game cannot be looked for", tid); return 0; }
    char *tail = text;
    if (n > (400u << 10)) tail = text + (n - (400u << 10)); /* the last 400 KB: a game started long ago is not looked for */
    int pid = 0, front = 0;
    unsigned app = 0;
    int closed = 0;
    if (!tile_game_from_log(tail, tid, &pid, &app, &front)) {
        say("tile %s: no running game of it in ShadowMount+'s log", tid);
    } else if (front) {
        say_dedupe("tile %s: its game (pid %d, app 0x%x) is in front: not closed", tid, pid, app);
    } else if (!proc_is_app(pid) || !tile_sandbox_present(tid)) {
        say("tile %s: its game (pid %d) is not a running app any more (or its sandbox is gone): not closed", tid, pid);
    } else if (kill(pid, SIGKILL) == 0) {
        say("tile %s: its disc is out, so its game (pid %d, app 0x%x, not in front) was closed", tid, pid, app);
        closed = 1;
    } else {
        say("tile %s: its game (pid %d) could not be closed (errno %d)", tid, pid, errno);
    }
    free(text);
    return closed;
}

/* One pass: the tile's folder removed (before the uninstall, so that no scan of ShadowMount+ takes it up again meanwhile), the uninstall asked for. A 409 (busy)
 * cleans up the leftover sandboxes of our titles: a title stays held active until its sandbox is gone, and ShadowMount+ lets go about a second after that.
 * Returns the HTTP code (0 or less: ShadowMount+ did not answer). */
static int remove_pass(int slot, char *reply, size_t reply_size) {
    const char *tid = kSlots[slot];
    char dir[128], body[80];
    snprintf(dir, sizeof(dir), HB "/%s", tid);
    rm_tree(dir);
    snprintf(body, sizeof(body), "{\"title_id\":\"%s\"}", tid);
    const int code = smp_post("/api/v1/games/uninstall", body, reply, reply_size);
    if (code == 409) heal_sandboxes(2, "an uninstall was refused as busy");
    return code;
}

/* 200 (asked for), 404 (not installed: nothing to do) and the other answers that will not change are final; 409, no answer and a failed uninstall (5xx) are tried again */
static int remove_final(int code) { return code > 0 && code != 409 && code < 500; }

typedef struct { int slot; char why[48]; } rm_job_t;

static void *remove_thread(void *arg) {
    rm_job_t job = *(rm_job_t *)arg;
    free(arg);
    const int slot = job.slot;
    const char *tid = kSlots[slot];
    char reply[512] = "";
    int code = 0, tries = 0, closes = 0;
    double t_pass = mono_s();
    while (!g_stop) {
        if (kp_quiet()) break; /* 171e: the console shuts down: the removal is not asked again (nothing is left stale in a console that restarts) */
        t_pass = mono_s();
        code = remove_pass(slot, reply, sizeof(reply));
        tries++;
        if (remove_final(code) || (code >= 500 && tries >= 40)) break;
        if (code == 409 && strstr(reply, "game_active") && closes < 2 && (tries == 1 || tries == 12)) { /* the tile's game is still there: it goes with its disc */
            closes++;
            if (close_tile_game(slot)) usleep(300000);
        }
        if (tries <= 2 || tries % 25 == 0)
            say("tile %s (%s): uninstall HTTP %d %s (try %d, %.1f s after the media went out; asked again every %s)", tid, job.why, code, code > 0 ? reply : "", tries,
                mono_s() - g_gone[slot].t_out, tries < 50 ? "0.4 s" : tries < 70 ? "2 s" : tries < 100 ? "5 s" : "20 s");
        usleep(tries < 50 ? 400000 : tries < 70 ? 2000000 : tries < 100 ? 5000000 : 20000000);
    }
    if (remove_final(code) || code >= 500) {
        const double t_acc = mono_s();
        pthread_mutex_lock(&g_state_mu);
        g_slot_stale[slot] = 0;
        pthread_mutex_unlock(&g_state_mu);
        g_gone[slot].t_acc = t_acc;
        g_gone[slot].accepted = 1;
        say("tile %s removed (%s): media gone %.1f s ago, uninstall HTTP %d %s after %d pass(es) (the last one took %.1f s)", tid, job.why, t_acc - g_gone[slot].t_out, code, reply,
            tries, t_acc - t_pass);
    }
    g_rm_running[slot] = 0;
    return NULL;
}

static void remove_tile(int slot, const char *why) {
    const double t_out = g_remove_t0 > 0 ? g_remove_t0 : mono_s();
    g_remove_t0 = 0;
    pthread_mutex_lock(&g_state_mu);
    g_slot_stale[slot] = 1;
    const int running = g_rm_running[slot];
    if (!running) g_rm_running[slot] = 1;
    pthread_mutex_unlock(&g_state_mu);
    if (running) return; /* its thread is at it already */
    g_gone[slot].t_out = t_out;
    g_gone[slot].t_acc = 0;
    g_gone[slot].accepted = 0;
    g_gone[slot].purges = 0;
    g_gone[slot].rows_deleted = 0;
    g_gone[slot].active = 1;
    rm_job_t *job = (rm_job_t *)malloc(sizeof(*job));
    int started = 0;
    if (job) {
        job->slot = slot;
        snprintf(job->why, sizeof(job->why), "%s", why);
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1 << 20);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        started = pthread_create(&th, &at, remove_thread, job) == 0;
        pthread_attr_destroy(&at);
        if (!started) free(job);
    }
    if (!started) { /* no thread: one pass here; the main loop asks again for a slot that stays stale */
        char reply[512] = "";
        const int code = remove_pass(slot, reply, sizeof(reply));
        say("tile %s (%s): no thread for the removal, asked here: uninstall HTTP %d %s", kSlots[slot], why, code, code > 0 ? reply : "");
        if (remove_final(code)) {
            g_gone[slot].t_acc = mono_s();
            g_gone[slot].accepted = 1;
            pthread_mutex_lock(&g_state_mu);
            g_slot_stale[slot] = 0;
            pthread_mutex_unlock(&g_state_mu);
        }
        g_rm_running[slot] = 0;
    }
}

/* The rows that ghosts of an earlier run (or of a tile whose removal was not followed) have left for slots that are free now: deleted before a new tile is made, so
 * that ShadowMount+ registers it afresh (a title found in tbl_contentinfo is "Skip (already present in app.db)": its tile keeps the previous disc's name and
 * cover) and the old tile is not shown again. A slot counts as free when it has no folder, no /user/app folder, no tile on it and no removal going on. */
static int purge_free_slots(const char *why) {
    int total = 0;
    for (int s = 0; s < NSLOTS; s++) {
        const char *tid = kSlots[s];
        pthread_mutex_lock(&g_state_mu);
        const int busy = g_slot_busy[s] || g_slot_stale[s] || g_rm_running[s];
        pthread_mutex_unlock(&g_state_mu);
        char p[120];
        snprintf(p, sizeof(p), HB "/%s", tid);
        if (busy || exists(p)) continue;
        snprintf(p, sizeof(p), "/user/app/%s", tid);
        if (exists(p)) continue;
        if (db_has_title(tid) > 0) {
            say("slot %s is free but the shell's database still has rows of it (%s): they are deleted", tid, why);
            const int d = purge_title_rows(tid);
            if (d > 0) total += d;
        }
    }
    return total;
}

typedef struct { int drive; char serial[16]; } tile_job_t;

/* ---------------------------------------------------------------- building a tile */
static double mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* PS5SX2's own sce_sys/param.json (build 171), kept here: a tile is made from it with its name, content ID, concept ID and title ID swapped in. When
 * the app's folder on the console has no sce_sys (a tester who replaced PPSA99203 with only the eboot: every tile then failed to build), this copy is used. */
static const char kParamFallback[] =
    "{\n"
    "  \"ageLevel\": {\n"
    "    \"default\": 0\n"
    "  },\n"
    "  \"applicationCategoryType\": 0,\n"
    "  \"applicationDrmType\": \"free\",\n"
    "  \"attribute\": 0,\n"
    "  \"attribute2\": 0,\n"
    "  \"attribute3\": 524352,\n"
    "  \"conceptId\": \"99203\",\n"
    "  \"contentBadgeType\": 1,\n"
    "  \"contentId\": \"UP9000-PPSA99203_00-PCSX2ORBIS00001\",\n"
    "  \"contentVersion\": \"02.030.000\",\n"
    "  \"downloadDataSize\": 256,\n"
    "  \"gameIntent\": {\n"
    "    \"permittedIntents\": [\n"
    "      {\n"
    "        \"intentType\": \"launchActivity\"\n"
    "      }\n"
    "    ]\n"
    "  },\n"
    "  \"localizedParameters\": {\n"
    "    \"defaultLanguage\": \"en-US\",\n"
    "    \"en-US\": {\n"
    "      \"titleName\": \"PS5SX2\"\n"
    "    }\n"
    "  },\n"
    "  \"masterVersion\": \"02.03\",\n"
    "  \"pubtools\": {\n"
    "    \"creationDate\": \"2026-09-05 00:00:00\",\n"
    "    \"loudnessSnd0\": \"-28.00\",\n"
    "    \"toolVersion\": \"2.00\"\n"
    "  },\n"
    "  \"requiredSystemSoftwareVersion\": \"0x0000000000000000\",\n"
    "  \"sdkVersion\": \"0x0000000000000000\",\n"
    "  \"titleId\": \"PPSA99203\",\n"
    "  \"versionFileUri\": \"\"\n"
    "}\n";

/* sce_sys/param.json for a slot: PS5SX2's own, with the tile's title, content ID, concept ID and title ID. Written over any file there. */
static int write_param_json(const char *sys_dir, int slot, const char *tid, const char *title) {
    char *pj = read_file(APP_SRC "/sce_sys/param.json", NULL);
    if (!pj) {
        static int said;
        if (!said) { said = 1; say("PS5SX2's " APP_SRC "/sce_sys/param.json is missing: the built-in copy is used for the tiles (copy the WHOLE PPSA99203 folder, not only eboot.bin)"); }
        pj = (char *)malloc(sizeof(kParamFallback));
        if (!pj) return 0;
        memcpy(pj, kParamFallback, sizeof(kParamFallback));
    }
    char esc[700], tname[760], content[64], concept[16], p[300];
    json_escape(esc, sizeof(esc), title);
    snprintf(tname, sizeof(tname), "\"titleName\": \"%s\"", esc);
    snprintf(content, sizeof(content), "UP9000-%s_00-PS5SX2DISCTILE%02d", tid, slot + 1);
    snprintf(concept, sizeof(concept), "\"%s\"", tid + 4);
    char *a = replace_all(pj, "\"titleName\": \"PS5SX2\"", tname);
    char *b = a ? replace_all(a, "UP9000-PPSA99203_00-PCSX2ORBIS00001", content) : NULL;
    char *c = b ? replace_all(b, "\"99203\"", concept) : NULL;
    char *e = c ? replace_all(c, "PPSA99203", tid) : NULL;
    snprintf(p, sizeof(p), "%s/param.json", sys_dir);
    const int ok = e && write_file(p, e, strlen(e)) == 0;
    free(a); free(b); free(c); free(e); free(pj);
    return ok;
}

/* icon0.png and pic0.dds for the disc, into sys_dir. The files are written over (fopen "wb" keeps the file, only its content goes), so that a
 * folder they are already in is not changed: ShadowMount+ judges a folder by its change time. */
static int make_art(const char *sys_dir, const char *serial, const char *title, char *used, size_t used_size) {
    char icon[260], pic[260], cover[240], hero[240], logo[240], grid[240];
    snprintf(icon, sizeof(icon), "%s/icon0.png", sys_dir);
    snprintf(pic, sizeof(pic), "%s/pic0.dds", sys_dir);
    snprintf(used, used_size, "PS5SX2's own");
    snprintf(cover, sizeof(cover), COVERS "/%s.jpg", serial);
    const int again = cover_force_take(serial); /* the reload tile marked it: the cover and the art are fetched afresh */
    cover_get(serial, cover, again);
    const int have_cover = exists(cover);
    sgdb_art(serial, title, hero, logo, grid, sizeof(hero), again);
    tile_set_icon_case(flag("disctile_iconcase")); /* the cover's middle fills the icon; the flag brings back the whole case over a blurred copy */
    const TileArt ta = {have_cover ? cover : NULL, hero[0] ? hero : NULL, logo[0] ? logo : NULL, grid[0] ? grid : NULL,
                        NULL, NULL, "/preinst/common/font/SST-Bold.otf", "PlayStation 2"};
    int art = -1, ok = 1;
    if (ta.cover || ta.hero || ta.grid) {
        art = tile_make_art(&ta, icon, pic, used, used_size);
        if (art != 0) { say("art for %s could not be made (%d): a title card instead", serial, art); snprintf(used, used_size, "PS5SX2's own"); }
    }
    if (art != 0) {
        /* No cover (a console without network or DNS, or a game the cover set does not know): a card drawn from the game's name, not PS5SX2's own picture */
        if (tile_make_title_art(icon, pic, ta.font, title) == 0) {
            art = 0;
            snprintf(used, used_size, "title card (no cover)");
        }
    }
    if (art != 0) {
        ok = copy_file(APP_SRC "/sce_sys/icon0.png", icon) == 0;
        if (exists(APP_SRC "/sce_sys/pic0.dds")) copy_file(APP_SRC "/sce_sys/pic0.dds", pic);
    }
    chmod(icon, 0777);
    chmod(pic, 0777);
    return ok;
}

/* what the tile's executable looks like next to PS5SX2's own (a tile's process once died on exec with errno 13) */
static void log_files(const char *tid, const char *how) {
    struct stat st_t, st_s;
    char eb[300];
    snprintf(eb, sizeof(eb), HB "/%s/eboot.bin", tid);
    const int a = stat(eb, &st_t) == 0, b = stat(APP_SRC "/eboot.bin", &st_s) == 0;
    say("tile %s files: eboot.bin %s; tile: mode %o nlink %u size %lld ino %llu | PS5SX2: mode %o nlink %u size %lld ino %llu", tid, how,
        a ? (unsigned)(st_t.st_mode & 07777) : 0u, a ? (unsigned)st_t.st_nlink : 0u, a ? (long long)st_t.st_size : -1LL, a ? (unsigned long long)st_t.st_ino : 0ULL,
        b ? (unsigned)(st_s.st_mode & 07777) : 0u, b ? (unsigned)st_s.st_nlink : 0u, b ? (long long)st_s.st_size : -1LL, b ? (unsigned long long)st_s.st_ino : 0ULL);
}

/* The slow way, the way of the first builds: the tile's folder is made where ShadowMount+ looks, sce_sys last (made beside, then renamed in).
 * ShadowMount+ then waits its 10 s (stability_wait_seconds) for the new sce_sys before it takes the tile. */
static int build_in_place(int slot, const char *serial, int drive, const char *title, char *used, size_t used_n, char *how, size_t how_n) {
    const char *tid = kSlots[slot];
    char dir[128], p[300], q[300];
    snprintf(dir, sizeof(dir), HB "/%s", tid);
    rm_tree(dir);
    mkdir(dir, 0777);
    chmod(dir, 0777);
    snprintf(p, sizeof(p), "%s/sce_module", dir);
    mkdir(p, 0777);
    chmod(p, 0777);
    snprintf(p, sizeof(p), "%s/eboot.bin", dir);
    int ok = put_exec_file(APP_SRC "/eboot.bin", p, how, how_n) == 0;
    DIR *dd = opendir(APP_SRC "/sce_module");
    if (dd) {
        struct dirent *e;
        while ((e = readdir(dd)) != NULL) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof(p), APP_SRC "/sce_module/%s", e->d_name);
            snprintf(q, sizeof(q), "%s/sce_module/%s", dir, e->d_name);
            ok = ok && put_exec_file(p, q, NULL, 0) == 0;
        }
        closedir(dd);
    }
    char devline[32];
    snprintf(devline, sizeof(devline), "/dev/cd%d\n", drive);
    snprintf(p, sizeof(p), "%s/disc-tile.txt", dir);
    ok = ok && write_file(p, devline, strlen(devline)) == 0;
    chmod(p, 0777);
    /* sce_sys is made beside and renamed in, so that ShadowMount+ never sees half of it */
    char sys_tmp[160], sys[160];
    snprintf(sys_tmp, sizeof(sys_tmp), "%s/sce_sys.new", dir);
    snprintf(sys, sizeof(sys), "%s/sce_sys", dir);
    mkdir(sys_tmp, 0777);
    ok = ok && write_param_json(sys_tmp, slot, tid, title);
    ok = ok && make_art(sys_tmp, serial, title, used, used_n);
    if (!ok) {
        say("tile %s for %s could not be written", tid, serial);
        rm_tree(dir);
        return 0;
    }
    rename(sys_tmp, sys);
    chmod(sys, 0777);
    return 1;
}

/* ---------------------------------------------------------------- staged tiles ("warm slots")
 * ShadowMount+ does not take a folder before its sce_sys has been left alone for 10 s. A tile made when the disc goes in therefore costs 10 s of
 * waiting. So the next slots' folders are made beforehand, in WORK/stage (outside everything ShadowMount+ looks at): the 37 MB eboot copy, the
 * libraries and a sce_sys with placeholders. When a disc goes in, the files in sce_sys are only WRITTEN OVER (the folder's change time stays old),
 * the marker is written, and the whole folder is MOVED into /data/homebrew; ShadowMount+ finds a sce_sys that is long stable and installs at once.
 * If anything about this does not work (the move across the folders, an eboot changed since, a build not done), the slow way is taken. */
#define STAGE WORK "/stage"
static int g_warm = -1;           /* -1 not looked at, 0 off, 1 on */
static int g_stage_state[NSLOTS]; /* 0 nothing, 1 being made, 2 ready, 3 taken by a tile (guarded by g_state_mu) */
static volatile int g_stage_running;

static void stage_marker(int slot, char *out, size_t max) { snprintf(out, max, STAGE "/%s.ok", kSlots[slot]); }

static int eboot_sig(char *out, size_t max) {
    struct stat st;
    if (stat(APP_SRC "/eboot.bin", &st) != 0) return 0;
    snprintf(out, max, "%lld %lld", (long long)st.st_size, (long long)st.st_mtime);
    return 1;
}

/* the staged folder is whole and was made from the eboot as it is now */
static int stage_valid(int slot) {
    char mk[200], want[64], have[64], eb[200];
    stage_marker(slot, mk, sizeof(mk));
    char *t = read_file(mk, NULL);
    if (!t) return 0;
    snprintf(have, sizeof(have), "%s", t);
    free(t);
    have[strcspn(have, "\r\n")] = '\0';
    snprintf(eb, sizeof(eb), STAGE "/%s/eboot.bin", kSlots[slot]);
    return eboot_sig(want, sizeof(want)) && strcmp(want, have) == 0 && exists(eb);
}

static char g_stage_why[160]; /* why the last stage_build failed (the one staging thread is the only writer) */
static volatile int g_stage_failed; /* the last staging failed: the next try comes later (it is not a thing that gets better in two seconds) */

static int stage_build(int slot) {
    const char *tid = kSlots[slot];
    char dir[160], sys[200], p[300], q[300], mk[200], sig[64];
    snprintf(dir, sizeof(dir), STAGE "/%s", tid);
    stage_marker(slot, mk, sizeof(mk));
    unlink(mk);
    rm_tree(dir);
    mkdirs(STAGE);
    mkdir(dir, 0777);
    chmod(dir, 0777);
    /* sce_sys first and with all three files, so that its clock starts at once and nothing is ever added to it later */
    snprintf(sys, sizeof(sys), "%s/sce_sys", dir);
    mkdir(sys, 0777);
    chmod(sys, 0777);
    const char *why = NULL; /* the first step that failed, for the log */
    int ok = write_param_json(sys, slot, tid, "PS2 disc");
    if (!ok) why = "sce_sys/param.json could not be written";
    snprintf(q, sizeof(q), "%s/icon0.png", sys);
    /* the picture of PS5SX2 is only a placeholder here (every disc's own picture is written over it); without one an empty file does as well */
    if (copy_file(APP_SRC "/sce_sys/icon0.png", q) != 0) write_file(q, "", 0);
    chmod(q, 0777);
    snprintf(q, sizeof(q), "%s/pic0.dds", sys);
    if (exists(APP_SRC "/sce_sys/pic0.dds")) copy_file(APP_SRC "/sce_sys/pic0.dds", q);
    else write_file(q, "", 0); /* an empty placeholder; taken out again when the art fails (see below) */
    chmod(q, 0777);
    snprintf(p, sizeof(p), "%s/sce_module", dir);
    mkdir(p, 0777);
    chmod(p, 0777);
    snprintf(p, sizeof(p), "%s/eboot.bin", dir);
    if (ok && put_exec_file(APP_SRC "/eboot.bin", p, NULL, 0) != 0) { ok = 0; why = "eboot.bin of " APP_SRC " could not be copied"; }
    DIR *dd = opendir(APP_SRC "/sce_module");
    if (dd) {
        struct dirent *e;
        while ((e = readdir(dd)) != NULL) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof(p), APP_SRC "/sce_module/%s", e->d_name);
            snprintf(q, sizeof(q), "%s/sce_module/%s", dir, e->d_name);
            if (ok && put_exec_file(p, q, NULL, 0) != 0) { ok = 0; why = "a file of " APP_SRC "/sce_module could not be copied"; }
        }
        closedir(dd);
    }
    snprintf(p, sizeof(p), "%s/disc-tile.txt", dir);
    if (ok && write_file(p, "/dev/cd0\n", 9) != 0) { ok = 0; why = "disc-tile.txt could not be written"; }
    chmod(p, 0777);
    if (ok && !eboot_sig(sig, sizeof(sig))) { ok = 0; why = APP_SRC "/eboot.bin cannot be looked at"; }
    if (!ok) {
        snprintf(g_stage_why, sizeof(g_stage_why), "%s", why ? why : "a step failed");
        rm_tree(dir);
        return 0;
    }
    write_file(mk, sig, strlen(sig)); /* the marker last: a folder without it was not finished */
    return 1;
}

/* Is the slot's title ID free of what the shell keeps of an installed title (/user/app/<ID> and /user/appmeta/<ID>, where its icon and name are kept)? The home
 * screen keeps a title's picture by its ID: a slot whose uninstall did not finish gave a new tile the PREVIOUS disc's cover and name (a tester saw a tile that
 * a reload did not refresh). Such a slot is not used while another is clean. */
static int slot_clean(int s) {
    char p[96];
    snprintf(p, sizeof(p), "/user/app/%s", kSlots[s]);
    if (exists(p)) return 0;
    snprintf(p, sizeof(p), "/user/appmeta/%s", kSlots[s]);
    return !exists(p);
}

/* what the shell still keeps of a slot's title ID, for the log ("" = nothing) */
static void slot_keeps(int s, char *out, size_t max) {
    char p[96];
    snprintf(p, sizeof(p), "/user/app/%s", kSlots[s]);
    const int a = exists(p);
    snprintf(p, sizeof(p), "/user/appmeta/%s", kSlots[s]);
    const int b = exists(p);
    snprintf(out, max, "%s%s%s", a ? "/user/app" : "", a && b ? " and " : "", b ? "/user/appmeta" : "");
}

/* The free slots in the order tile_thread takes them (at most max): the clean ones in ring order first, then the others in ring order. The staged folders
 * (stage_thread) are made for the first of them, so that a disc finds its folder ready even when the shell keeps something of every slot's title ID. */
static int pick_slots(int *out, int max) {
    int cand[NSLOTS], nc = 0;
    pthread_mutex_lock(&g_state_mu);
    for (int k = 0; k < NSLOTS; k++) {
        const int s = (g_next_slot + k) % NSLOTS;
        if (!g_slot_busy[s] && !g_slot_stale[s]) cand[nc++] = s;
    }
    pthread_mutex_unlock(&g_state_mu);
    int n = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nc && n < max; i++)
            if (slot_clean(cand[i]) == (pass == 0)) out[n++] = cand[i];
    return n;
}

static void *stage_thread(void *arg) {
    (void)arg;
    int cand[2];
    const int nc = pick_slots(cand, 2); /* the slots the next two discs will use (tile_thread takes them in the same order) */
    for (int k = 0; k < nc; k++) {
        pthread_mutex_lock(&g_state_mu);
        const int s = cand[k];
        char hb[128];
        snprintf(hb, sizeof(hb), HB "/%s", kSlots[s]);
        const int free_slot = !g_slot_busy[s] && !g_slot_stale[s] && g_stage_state[s] != 3 && g_stage_state[s] != 1 && !exists(hb);
        const int was = g_stage_state[s];
        if (free_slot && was != 2) g_stage_state[s] = 1;
        pthread_mutex_unlock(&g_state_mu);
        if (!free_slot || was == 2) continue;
        const double t0 = mono_s();
        g_stage_why[0] = '\0';
        const int ok = stage_build(s);
        pthread_mutex_lock(&g_state_mu);
        g_stage_state[s] = ok ? 2 : 0;
        pthread_mutex_unlock(&g_state_mu);
        g_stage_failed = !ok;
        if (ok) say("%s staged beforehand ok (%.1f s)", kSlots[s], mono_s() - t0);
        else say_dedupe("%s staged beforehand FAILED: %s (tried again every 10 s; the log says it once a minute)", kSlots[s], g_stage_why[0] ? g_stage_why : "a step failed");
        if (!ok) break; /* the same thing would fail for the next slot */
    }
    g_stage_running = 0;
    return NULL;
}

/* from the main loop: start making the next slots' folders when they are not there */
static void warm_kick(void) {
    if (g_warm != 1 || g_stage_running) return;
    static double last_kick;
    const double t_now = mono_s();
    if (t_now - last_kick < (g_stage_failed ? 10.0 : 2.0)) return; /* a slot that cannot be staged (its folder is still being taken down) is not retried four times a second */
    last_kick = t_now;
    int need = 0, cand[2];
    const int nc = pick_slots(cand, 2);
    pthread_mutex_lock(&g_state_mu);
    for (int k = 0; k < nc; k++)
        if (g_stage_state[cand[k]] == 0) need = 1;
    pthread_mutex_unlock(&g_state_mu);
    if (!need) return;
    g_stage_running = 1;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 1 << 20);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, stage_thread, NULL) != 0) g_stage_running = 0;
    pthread_attr_destroy(&at);
}

/* once at the start: can a folder be moved from WORK/stage to /data/homebrew at all (across mounts it cannot)? what is staged already? */
static void warm_init(void) {
    if (flag("disctile_nowarm")) { g_warm = 0; say("staged tiles are off (flag disctile_nowarm)"); return; }
    const char *a = STAGE "/.probe", *b = HB "/.disctile-probe";
    mkdirs(STAGE);
    rm_tree(a);
    rm_tree(b);
    if (mkdir(a, 0777) != 0) { g_warm = 0; say("staged tiles are off: cannot make %s (errno %d)", a, errno); return; }
    if (rename(a, b) != 0) {
        const int e = errno;
        rmdir(a);
        g_warm = 0;
        say("staged tiles are off: a folder cannot be moved from " STAGE " to " HB " (errno %d): the slow way is used", e);
        return;
    }
    rm_tree(b);
    g_warm = 1;
    int have = 0;
    for (int s = 0; s < NSLOTS; s++)
        if (stage_valid(s)) { g_stage_state[s] = 2; have++; }
    say("staged tiles are on (%d already there)", have);
}

/* a staged folder for the slot that is whole and current, taken for this tile; waits a few seconds for one that is being made */
static int take_stage(int slot) {
    for (int i = 0; i < 160; i++) { /* up to 8 s */
        pthread_mutex_lock(&g_state_mu);
        const int st = g_stage_state[slot];
        if (st != 1) {
            if (st == 2) g_stage_state[slot] = 3;
            pthread_mutex_unlock(&g_state_mu);
            if (st != 2) return 0;
            if (stage_valid(slot)) return 1;
            say("%s: the staged folder is out of date (the eboot changed): the slow way", kSlots[slot]);
            char dir[160];
            snprintf(dir, sizeof(dir), STAGE "/%s", kSlots[slot]);
            rm_tree(dir);
            pthread_mutex_lock(&g_state_mu);
            g_stage_state[slot] = 0;
            pthread_mutex_unlock(&g_state_mu);
            return 0;
        }
        pthread_mutex_unlock(&g_state_mu);
        usleep(50000);
    }
    return 0;
}

/* until the folder's change time is `secs` old (ShadowMount+ looks at the later of its mtime and ctime) */
static void wait_stable(const char *path, int secs) {
    struct stat st;
    if (stat(path, &st) != 0) return;
    const time_t last = st.st_ctime > st.st_mtime ? st.st_ctime : st.st_mtime;
    const double age_s = difftime(time(NULL), last);
    if (age_s >= secs) return;
    double w = (double)secs - age_s;
    if (w > 12.0) w = 12.0;
    usleep((useconds_t)(w * 1e6) + 300000);
}

/* The fast way: the staged folder gets this disc's files written over the placeholders and is moved into /data/homebrew. 1 when it is there. */
static int build_warm(int slot, const char *serial, int drive, const char *title, char *used, size_t used_n) {
    const char *tid = kSlots[slot];
    char work[160], sysd[200], p[260], dir[128], mk[200], devline[32];
    snprintf(work, sizeof(work), STAGE "/%s", tid);
    snprintf(sysd, sizeof(sysd), "%s/sce_sys", work);
    snprintf(dir, sizeof(dir), HB "/%s", tid);
    stage_marker(slot, mk, sizeof(mk));
    unlink(mk);
    snprintf(devline, sizeof(devline), "/dev/cd%d\n", drive);
    snprintf(p, sizeof(p), "%s/disc-tile.txt", work);
    int ok = write_param_json(sysd, slot, tid, title);
    ok = ok && make_art(sysd, serial, title, used, used_n);
    ok = ok && write_file(p, devline, strlen(devline)) == 0;
    snprintf(p, sizeof(p), "%s/pic0.dds", sysd);
    struct stat st;
    if (stat(p, &st) == 0 && st.st_size == 0) unlink(p); /* the placeholder, when no art came: no picture, as before (this one changes the folder) */
    if (!ok) { rm_tree(work); return 0; }
    wait_stable(sysd, 11);
    rm_tree(dir); /* nothing should be there */
    if (rename(work, dir) != 0) {
        const int e = errno;
        say("%s: moving the staged folder into place failed (errno %d): the slow way, and staged tiles are off", tid, e);
        g_warm = 0;
        rm_tree(work);
        return 0;
    }
    return 1;
}

static void *tile_thread(void *arg) {
    tile_job_t job = *(tile_job_t *)arg;
    free(arg);
    Drive *d = &g_drv[job.drive];
    int slot = -1, dirty = 0;
    /* A free slot whose title ID the shell has fully forgotten (see slot_clean), in ring order; only when there is none, any free slot. Claimed under the
     * mutex, one by one, so that two drives never get the same one. */
    for (int pass = 0; pass < 2 && slot < 0; pass++) {
        for (int k = 0; k < NSLOTS && slot < 0; k++) {
            const int s = (g_next_slot + k) % NSLOTS;
            if (pass == 0 && !slot_clean(s)) continue;
            pthread_mutex_lock(&g_state_mu);
            if (!g_slot_busy[s] && !g_slot_stale[s]) { g_slot_busy[s] = job.drive + 1; slot = s; dirty = pass; }
            pthread_mutex_unlock(&g_state_mu);
        }
    }
    if (slot >= 0) {
        pthread_mutex_lock(&g_state_mu);
        g_next_slot = (slot + 1) % NSLOTS; /* a ring: the home screen keeps a title's picture by its ID */
        char ns[16];
        snprintf(ns, sizeof(ns), "%d\n", g_next_slot);
        write_file(WORK "/next-slot.txt", ns, strlen(ns));
        pthread_mutex_unlock(&g_state_mu);
        if (dirty) {
            char keeps[64];
            slot_keeps(slot, keeps, sizeof(keeps));
            say("slot %s: no clean slot was free, so it is used although the shell still has %s of its title ID (its old picture may show)", kSlots[slot], keeps);
        }
    }
    if (slot < 0) { say("no free tile slot for /dev/cd%d", job.drive); d->building = 0; return NULL; }

    const char *tid = kSlots[slot];
    char title[300], used[96] = "PS5SX2's own", how[96] = "";
    game_title(job.serial, title, sizeof(title));
    const double t_start = mono_s();
    int ok = 0, warm = 0;
    if (g_warm == 1 && take_stage(slot)) {
        warm = build_warm(slot, job.serial, job.drive, title, used, sizeof(used));
        pthread_mutex_lock(&g_state_mu);
        g_stage_state[slot] = 0; /* the staged folder is gone: moved, or taken down */
        pthread_mutex_unlock(&g_state_mu);
        ok = warm;
        if (warm) snprintf(how, sizeof(how), "copied beforehand");
    }
    if (!ok) ok = build_in_place(slot, job.serial, job.drive, title, used, sizeof(used), how, sizeof(how));
    if (!ok) {
        pthread_mutex_lock(&g_state_mu);
        g_slot_busy[slot] = 0;
        pthread_mutex_unlock(&g_state_mu);
        d->building = 0;
        return NULL;
    }
    d->t_built = mono_s();
    d->warm = warm;
    log_files(tid, how);
    whitelist_add(tid);

    char reply[512];
    const int code = smp_post("/api/v1/scan", "{\"reset_attempts\":true}", reply, sizeof(reply));
    d->t_scan = mono_s();
    say("tile %s: \"%s\" (%s) from /dev/cd%d, art: %s; made in %.1f s (%s); scan HTTP %d %s", tid, title, job.serial, job.drive, used,
        d->t_built - t_start, warm ? "staged beforehand" : "slow way", code, reply);
    /* A stuck ShadowMount+ answers a scan with {"queued":true} too (the tester's log), so it is asked whether it is busy as well */
    const int busy = code > 0 ? smp_busy() : -1;
    if (code <= 0 || strstr(reply, "\"scan_deferred\":true") || busy == 1) {
        /* ShadowMount+ is down, or a game is active (it puts the scan off): ask again every 5 s until it takes the tile */
        if (busy == 1) say("tile %s: ShadowMount+ is BUSY (a title is held active, 409): the scan is put off until it is free", tid);
        g_scan_pending = 1; g_scan_next = time(NULL) + 5; g_scan_tries = 0;
    }
    const int nowait = flag("disctile_nowait"); /* the old way: no wait for the mount, the system's tile is hidden at once */
    if (code <= 0) notify_tile("PS2 disc: %s. No tile yet: ShadowMount+ did not answer (is it loaded?)", title);
    else if (g_scan_pending) notify_tile("PS2 disc: %s. Its tile comes when the running game is closed.", title);
    else if (nowait) notify_tile("PS2 disc: %s. Its tile is on the home screen in a moment.", title);
    pthread_mutex_lock(&g_state_mu);
    snprintf(d->title, sizeof(d->title), "%s", title);
    d->ready = nowait;
    d->ready_t0 = 0;
    d->diag = 0;
    d->slot = slot;
    pthread_mutex_unlock(&g_state_mu);
    d->building = 0;
    return NULL;
}

/* A tile is registered, but is it launchable yet? The scan only queues it: the title must also be mounted, or a press on the tile gives the
 * shell's "the game or app cannot be started". The wait is counted from the moment the scan was taken; what it saw is logged at 0, 3 and 8 s
 * (mount point, folder, whitelist, the home screen's rows), and after 15 s the tile is let through anyway (its state in the log). */
#define MOUNT_WAIT_S 15

/* ShadowMount+'s own logs, followed into files while a tile exists: its kernel-log route (the console's kernel log, so no klog server is
 * needed) and its debug-log route. The reply is {"status":0,"content":"<JSON-escaped text>",...}; each poll takes a tail, and only what is new
 * since the last one is appended. Flag disctile_nofollow turns this off. */
static int smp_fetch(const char *route, const char *body, char **out, size_t *outlen) {
    *out = NULL;
    *outlen = 0;
    if (kp_quiet()) return -1; /* 171f: ShadowMount+ is not asked while the console shuts down (the other callers of it check this already) */
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SMP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    struct timeval tv = {4, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    char req[400];
    const int n = snprintf(req, sizeof(req), "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                           route, SMP_PORT, strlen(body), body);
    send(fd, req, (size_t)n, 0);
    size_t cap = 1u << 16, got = 0;
    char *buf = (char *)malloc(cap + 1);
    if (!buf) { close(fd); return -1; }
    for (;;) {
        if (got == cap) {
            if (cap >= (1u << 20)) break;
            char *nb = (char *)realloc(buf, cap * 2 + 1);
            if (!nb) break;
            buf = nb;
            cap *= 2;
        }
        const ssize_t r = recv(fd, buf + got, cap - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    int code = 0;
    if (sscanf(buf, "HTTP/1.%*d %d", &code) != 1) code = 0;
    char *he = strstr(buf, "\r\n\r\n");
    if (!he) { free(buf); return code; }
    *he = '\0';
    const int chunked = strcasestr(buf, "transfer-encoding: chunked") != NULL;
    char *p = he + 4;
    size_t len = got - (size_t)(p - buf);
    if (chunked) {
        char *dst = p;
        const char *s = p;
        const char *end = buf + got;
        len = 0;
        while (s < end) {
            char *e = NULL;
            const unsigned long sz = strtoul(s, &e, 16);
            if (!e || sz == 0) break;
            const char *data = strstr(e, "\r\n");
            if (!data) break;
            data += 2;
            if (data + sz > end) break;
            memmove(dst + len, data, sz);
            len += sz;
            s = data + sz + 2;
        }
    }
    memmove(buf, p, len);
    buf[len] = '\0';
    *out = buf;
    *outlen = len;
    return code;
}

/* the text of "key":"..." with its JSON escapes undone (UTF-8); dst must hold as many bytes as the JSON text has */
static size_t json_field_text(const char *json, const char *key, char *dst, size_t max) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    size_t k = 0;
    if (!p || max == 0) { if (max) dst[0] = '\0'; return 0; }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == ':') p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') { dst[0] = '\0'; return 0; }
    p++;
    while (*p && *p != '"' && k + 4 < max) {
        if (*p != '\\') { dst[k++] = *p++; continue; }
        p++;
        switch (*p) {
        case 'n': dst[k++] = '\n'; p++; break;
        case 'r': dst[k++] = '\r'; p++; break;
        case 't': dst[k++] = '\t'; p++; break;
        case 'b': dst[k++] = '\b'; p++; break;
        case 'f': dst[k++] = '\f'; p++; break;
        case 'u': {
            unsigned cp = 0;
            for (int i = 1; i <= 4 && p[i]; i++) cp = cp * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower((unsigned char)p[i]) - 'a' + 10) & 15);
            p += 5;
            if (cp >= 0xD800 && cp < 0xDC00 && p[0] == '\\' && p[1] == 'u') { /* a surrogate pair */
                unsigned lo = 0;
                for (int i = 2; i <= 5 && p[i]; i++) lo = lo * 16 + (unsigned)(isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower((unsigned char)p[i]) - 'a' + 10) & 15);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
            if (cp < 0x80) dst[k++] = (char)cp;
            else if (cp < 0x800) { dst[k++] = (char)(0xC0 | (cp >> 6)); dst[k++] = (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { dst[k++] = (char)(0xE0 | (cp >> 12)); dst[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[k++] = (char)(0x80 | (cp & 0x3F)); }
            else { dst[k++] = (char)(0xF0 | (cp >> 18)); dst[k++] = (char)(0x80 | ((cp >> 12) & 0x3F)); dst[k++] = (char)(0x80 | ((cp >> 6) & 0x3F)); dst[k++] = (char)(0x80 | (cp & 0x3F)); }
            break;
        }
        case '\0': break;
        default: dst[k++] = *p++; break; /* \" \\ \/ */
        }
    }
    dst[k] = '\0';
    return k;
}

typedef struct { char tail[256]; size_t tail_len; int started; } Follow;

/* the SDK's libc has no memmem */
static const char *mem_find(const char *hay, size_t hn, const char *needle, size_t nn) {
    if (nn == 0 || hn < nn) return NULL;
    const char first = needle[0];
    for (size_t i = 0; i + nn <= hn; i++)
        if (hay[i] == first && memcmp(hay + i, needle, nn) == 0) return hay + i;
    return NULL;
}

static void follow_log(const char *route, int max_bytes, const char *file, Follow *f) {
    char rq[64];
    snprintf(rq, sizeof(rq), "{\"max_bytes\":%d}", max_bytes);
    char *body = NULL;
    size_t bl = 0;
    const int code = smp_fetch(route, rq, &body, &bl);
    if (code != 200 || !body) { free(body); return; }
    char *txt = (char *)malloc(bl + 1);
    if (!txt) { free(body); return; }
    const size_t n = json_field_text(body, "content", txt, bl + 1);
    free(body);
    size_t from = 0;
    const char *marker = NULL;
    if (!f->started) {
        marker = "--- first snapshot ---\n";
    } else if (f->tail_len) {
        const char *hit = NULL;
        const char *p = txt;
        while (n && (size_t)(p - txt) < n && (p = mem_find(p, n - (size_t)(p - txt), f->tail, f->tail_len)) != NULL) { hit = p; p++; }
        if (hit) from = (size_t)(hit - txt) + f->tail_len;
        else marker = "--- gap: lines may be missing ---\n";
    }
    if (marker || n > from) {
        struct stat st;
        if (stat(file, &st) == 0 && st.st_size > (6 << 20)) {
            char old[200];
            snprintf(old, sizeof(old), "%s.1", file);
            rename(file, old);
        }
        FILE *o = fopen(file, "ab");
        if (o) {
            if (marker) { fprintf(o, "%lld %s", (long long)time(NULL), marker); }
            if (n > from) fwrite(txt + from, 1, n - from, o);
            fclose(o);
        }
    }
    const size_t keep = n < sizeof(f->tail) ? n : sizeof(f->tail);
    memcpy(f->tail, txt + n - keep, keep);
    f->tail_len = keep;
    f->started = 1;
    free(txt);
}

static Follow g_follow_kernel, g_follow_debug;
static time_t g_follow_next;
static int g_follow_was_active;

static void follow_logs(int active) {
    if (!kp_log_files() || kp_quiet()) return; /* 171e: the copies of ShadowMount+'s logs are files too */
    if (flag("disctile_nofollow")) return;
    const time_t now = time(NULL);
    if (active && now >= g_follow_next) {
        follow_log("/api/v1/kernel-log", 65536, WORK "/kernel-log.txt", &g_follow_kernel);
        follow_log("/api/v1/debug-log", 32768, WORK "/shadowmount-debug.log", &g_follow_debug);
        g_follow_next = now + 2;
    } else if (!active && g_follow_was_active) { /* the last bit. (Not "started afresh" any more: that put the whole of ShadowMount+'s log in again at
                                                  * each later tile, so the files had the same hours over and over. The kept tail finds the place.) */
        follow_log("/api/v1/kernel-log", 65536, WORK "/kernel-log.txt", &g_follow_kernel);
        follow_log("/api/v1/debug-log", 32768, WORK "/shadowmount-debug.log", &g_follow_debug);
    }
    g_follow_was_active = active;
}

/* What the console looks like around a title (for the log; nothing is changed): the mount table's entries for apps, the folders the shell
 * and the installer use, how the working PS5SX2 (PPSA99203) looks next to the tile, and what ShadowMount+'s API says. */
static void list_ppsa(const char *dir, char *out, size_t max) {
    out[0] = '\0';
    DIR *d = opendir(dir);
    if (!d) { snprintf(out, max, "(cannot open, errno %d)", errno); return; }
    struct dirent *e;
    int n = 0;
    size_t k = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "PPSA99", 6) != 0) continue;
        if (n < 12 && k + 14 < max) k += (size_t)snprintf(out + k, max - k, "%s ", e->d_name);
        n++;
    }
    closedir(d);
    if (n == 0) snprintf(out, max, "(no PPSA99* entries)");
}

static void dump_env(const char *tid, const char *when) {
    char where[160], ref[160], l1[300], l2[300], p[200];
    const int tm = title_mounted(tid, where, sizeof(where));
    const int rm = title_mounted("PPSA99203", ref, sizeof(ref));
    say("env %s: tile %s mounted %s %s | PS5SX2 (PPSA99203) mounted %s %s", when, tid, tm ? "yes" : "no", where, rm ? "yes" : "no", ref);
    struct statfs *m = NULL;
    const int n = getmntinfo(&m, MNT_NOWAIT);
    int shown = 0;
    for (int i = 0; i < n && shown < 12; i++) {
        if (strstr(m[i].f_mntonname, "PPSA") || strstr(m[i].f_mntfromname, "PPSA") || strstr(m[i].f_mntonname, "homebrew") ||
            strstr(m[i].f_mntfromname, "homebrew")) {
            say("env %s: mount %s on %s (%s)", when, m[i].f_mntfromname, m[i].f_mntonname, m[i].f_fstypename);
            shown++;
        }
    }
    say("env %s: %d mounts in all, %d about apps", when, n, shown);
    list_ppsa("/system_ex/app", l1, sizeof(l1));
    list_ppsa("/user/app", l2, sizeof(l2));
    say("env %s: /system_ex/app: %s | /user/app: %s", when, l1, l2);
    snprintf(p, sizeof(p), "/system_ex/app/%s", tid);
    const int a = exists(p);
    snprintf(p, sizeof(p), "/user/app/%s", tid);
    const int b = exists(p);
    snprintf(p, sizeof(p), "/data/homebrew/%s/sce_sys/param.json", tid);
    const int c = exists(p);
    say("env %s: tile folders: /system_ex/app/%s %s, /user/app/%s %s, homebrew param.json %s", when, tid, a ? "yes" : "no", tid, b ? "yes" : "no", c ? "yes" : "NO");
    /* what ShadowMount+ itself says about the tile and about the working PS5SX2 (its own web page asks this with {"title_id": ...}) */
    const char *const ids[2] = {tid, "PPSA99203"};
    for (int i = 0; i < 2; i++) {
        char body[80], reply[900];
        snprintf(body, sizeof(body), "{\"title_id\":\"%s\"}", ids[i]);
        const int code = smp_call("POST", "/api/v1/games/info", body, reply, sizeof(reply));
        say("env %s: ShadowMount+ info %s -> HTTP %d %.600s", when, ids[i], code, code > 0 ? reply : "");
    }
}

static void tile_state_line(const char *tid, int *mounted_out, int *row_out, char *buf, size_t max) {
    char where[160];
    const int mounted = title_mounted(tid, where, sizeof(where));
    char hb[160];
    snprintf(hb, sizeof(hb), HB "/%s/eboot.bin", tid);
    const int row = db_has_title(tid);
    snprintf(buf, max, "mounted %s%s%s, files %s, whitelist %s, home screen rows %d", mounted ? "yes" : "no", mounted ? " at " : "", where,
             exists(hb) ? "yes" : "NO", whitelisted(tid) ? "yes" : "NO", row);
    *mounted_out = mounted;
    *row_out = row;
}

static void check_ready(void) {
    if (g_scan_pending) return;
    static double last_check;
    const double t_now = mono_s();
    if (t_now - last_check < 0.5) return; /* twice a second is plenty (the main loop runs four times) */
    last_check = t_now;
    const time_t now = time(NULL);
    for (int i = 0; i < MAX_DRIVES; i++) {
        Drive *d = &g_drv[i];
        if (d->slot < 0 || d->ready || d->building) continue;
        const char *tid = kSlots[d->slot];
        if (d->ready_t0 == 0) {
            d->ready_t0 = now;
            if (flag("disctile_mount")) { /* an experiment (an empty file in the flags folder): ask ShadowMount+ to mount the tile's title itself */
                char body[80], reply[600];
                snprintf(body, sizeof(body), "{\"title_id\":\"%s\"}", tid);
                const int mc = smp_call("POST", "/api/v1/games/mount", body, reply, sizeof(reply));
                say("tile %s: asked ShadowMount+ to mount it -> HTTP %d %.300s", tid, mc, mc > 0 ? reply : "");
            }
        }
        const int waited = (int)(now - d->ready_t0);
        int mounted = 0, row = 0, forced = 0;
        char st[400];
        tile_state_line(tid, &mounted, &row, st, sizeof(st));
        const int diag_on = flag("disctile_diag"); /* the state lines and the env dumps: for a hunt, not for every tile */
        const int want_diag = diag_on && ((d->diag == 0) || (d->diag == 1 && waited >= 3) || (d->diag == 2 && waited >= 8));
        if (want_diag) { if (d->diag == 0) dump_env(tid, "start"); d->diag++; say("tile %s after %d s: %s", tid, waited, st); }
        /* Ready = the home screen has the tile AND ShadowMount+ knows the title. A folder title is probably mounted only when it is launched,
         * so "mounted" is logged but not waited for. */
        int info = 0;
        if (row > 0) {
            char ib[80], ir[300];
            snprintf(ib, sizeof(ib), "{\"title_id\":\"%s\"}", tid);
            info = smp_call("POST", "/api/v1/games/info", ib, ir, sizeof(ir));
        }
        if (row > 0 && info == 200) {
            if (diag_on) say("tile %s is ready after %d s (%s; ShadowMount+ info HTTP %d)", tid, waited, st, info);
            d->ready = 1;
            const double now_s = mono_s();
            say("tile %s ready %.1f s after the disc went in: serial %.1f s, tile made %.1f s (%s), ShadowMount+ and the home screen %.1f s", tid,
                now_s - d->t_disc, d->t_serial - d->t_disc, d->t_built - d->t_serial, d->warm ? "staged beforehand" : "slow way", now_s - d->t_built);
        } else if (waited >= MOUNT_WAIT_S) {
            say("tile %s: NOT ready after %d s, letting it through (%s): pressing it may fail", tid, waited, st);
            const int busy = smp_busy();
            say("tile %s: ShadowMount+ is %s", tid, busy == 1 ? "BUSY (a title is held active: 409)" : busy == 0 ? "free (no 409)" : "not reachable");
            d->ready = 1;
            forced = 1;
        }
        if (d->ready) {
            if (diag_on) dump_env(tid, "end");
            set_tile_row_visible(tid, 1); /* a slot used before may have been left hidden by an uninstall that did not go through */
            hide_system_tile();
            order_after_install(tid); /* the log: the tile's row and the top of the list; the disc tile above the Reload tile if the shell numbered it lower */
            if (forced) notify_tile("PS2 disc: %s. Its tile is on the home screen, but the console did not confirm it is ready: if it does not start, tell us.", d->title);
            else notify_tile("PS2 disc: %s. Its tile is ready on the home screen.", d->title);
        }
    }
}

static void start_tile(int drive, const char *serial) {
    tile_job_t *job = (tile_job_t *)malloc(sizeof(*job));
    if (!job) return;
    job->drive = drive;
    snprintf(job->serial, sizeof(job->serial), "%s", serial);
    g_drv[drive].building = 1;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4 << 20); /* the art is put together on this stack's thread */
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, tile_thread, job) != 0) { say("cannot start the tile thread"); g_drv[drive].building = 0; free(job); }
    pthread_attr_destroy(&at);
}

/* ---------------------------------------------------------------- the apps' sandboxes: stale ones block ShadowMount+
 * Every app the shell starts gets a folder /mnt/sandbox/<TITLEID>_NNN. PS5SX2's helper mounts /data into the ones of PS5SX2 and of the tiles
 * (/mnt/sandbox/<TITLEID>_NNN/data: the "data mounted" lines of its payload-init.log), and when the app ends the shell cannot take the sandbox
 * away: the folder stays (the next start of PS5SX2 then got ..._001). ShadowMount+ holds a title "active" (scans put off, uninstall and mount answer
 * 409 Device busy) until that title's sandbox is gone; after the first game no new tile was taken up any more ("the disc does not start after two
 * inserts"), and loading ShadowMount+ again cleared it. So the stale sandboxes of OUR titles (PS5SX2 and the tiles) are cleaned up here when no app
 * is running: the mounts inside are taken off (deepest first), then what is left is removed. Nothing outside those folders is touched, nothing on
 * another filesystem is removed (a mount that will not come off is left alone, and said), and nothing at all while any eboot.bin process exists.
 * Flag disctile_noheal: not by itself (the reload tile still does it). */
typedef struct { char name[32]; time_t age; } Sbx;           /* a sandbox folder and the seconds since it last changed */
typedef struct { char on[160]; char from[96]; char fs[16]; } Mnt;

static int our_sandbox_name(const char *name) {
    if (strlen(name) < 11 || name[9] != '_') return 0; /* PPSA99203_000 */
    for (const char *p = name + 10; *p; p++) if (!isalnum((unsigned char)*p)) return 0;
    if (strncmp(name, "PPSA99203", 9) == 0 || strncmp(name, RELOAD_TID, 9) == 0) return 1;
    for (int s = 0; s < NSLOTS; s++) if (strncmp(name, kSlots[s], 9) == 0) return 1;
    return 0;
}

static int list_sandboxes(Sbx *out, int max) {
    DIR *d = opendir(SANDBOX_DIR);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (n >= max || !our_sandbox_name(e->d_name)) continue;
        char p[160];
        snprintf(p, sizeof(p), SANDBOX_DIR "/%s", e->d_name);
        struct stat st;
        time_t age = 0;
        if (stat(p, &st) == 0) {
            const time_t last = st.st_ctime > st.st_mtime ? st.st_ctime : st.st_mtime;
            age = time(NULL) - last;
            if (age < 0) age = 0;
        }
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        out[n].age = age;
        n++;
    }
    closedir(d);
    return n;
}

/* how many app processes (eboot.bin: PS5SX2, its tiles, any game) there are; -1 when the process table cannot be read. The PS5's kinfo_proc is
 * larger than the SDK's (1096 against 1088 bytes), so the records are stepped by their own ki_structsize. */
static int app_processes(void) {
    int mib[3] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC};
    size_t len = 0;
    if (sysctl(mib, 3, NULL, &len, NULL, 0) != 0 || len == 0) return -1;
    len += len / 4 + 8192; /* room for the ones started meanwhile */
    char *buf = (char *)malloc(len);
    if (!buf) return -1;
    if (sysctl(mib, 3, buf, &len, NULL, 0) != 0) { free(buf); return -1; }
    int n = 0, total = 0, saw_self = 0;
    size_t off = 0;
    while (off + sizeof(int) <= len) {
        const struct kinfo_proc *kp = (const struct kinfo_proc *)(buf + off);
        const size_t sz = kp->ki_structsize > 0 ? (size_t)kp->ki_structsize : 0;
        if (sz < offsetof(struct kinfo_proc, ki_comm) + sizeof(kp->ki_comm) || off + sz > len) break;
        total++;
        if (kp->ki_pid == getpid()) saw_self = 1;
        if (strcmp(kp->ki_comm, "eboot.bin") == 0 && kp->ki_stat != 5 /* SZOMB: ended, not yet collected by its parent */) n++;
        off += sz;
    }
    free(buf);
    /* a table that does not even show this payload (or hardly anything) is not to be believed: "no app" must never come from a table that hides them */
    if (!saw_self || total < 10) {
        static time_t said;
        if (time(NULL) - said >= 60) { said = time(NULL); say("the process table looks wrong (%d entries, this payload %s): taken as unknown", total, saw_self ? "seen" : "NOT seen"); }
        return -1;
    }
    return n;
}

/* a private copy of the mount table (getmntinfo keeps one static buffer, and threads of ours look at it); free() it */
static struct statfs *mount_table(int *n_out) {
    *n_out = 0;
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0) return NULL;
    struct statfs *m = (struct statfs *)malloc((size_t)(n + 8) * sizeof(*m));
    if (!m) return NULL;
    n = getfsstat(m, (long)((size_t)(n + 8) * sizeof(*m)), MNT_NOWAIT);
    if (n <= 0) { free(m); return NULL; }
    *n_out = n;
    return m;
}

/* the mounts at or below dir, the deepest first */
static int mounts_below(const char *dir, Mnt *out, int max) {
    int n = 0, k = 0;
    struct statfs *m = mount_table(&n);
    const size_t dl = strlen(dir);
    for (int i = 0; i < n && k < max; i++) {
        const char *p = m[i].f_mntonname;
        if (strncmp(p, dir, dl) != 0 || (p[dl] != '\0' && p[dl] != '/')) continue;
        snprintf(out[k].on, sizeof(out[k].on), "%s", p);
        snprintf(out[k].from, sizeof(out[k].from), "%s", m[i].f_mntfromname);
        snprintf(out[k].fs, sizeof(out[k].fs), "%s", m[i].f_fstypename);
        k++;
    }
    free(m);
    for (int a = 1; a < k; a++)
        for (int b = a; b > 0 && strlen(out[b].on) > strlen(out[b - 1].on); b--) { Mnt t = out[b]; out[b] = out[b - 1]; out[b - 1] = t; }
    return k;
}

/* what a sandbox holds, for the log: its mounts and the names in it */
static void log_sandbox(const char *what, const char *name) {
    char p[160], ml[480] = "", el[240] = "";
    snprintf(p, sizeof(p), SANDBOX_DIR "/%s", name);
    Mnt mm[16];
    const int nm = mounts_below(p, mm, 16);
    size_t k = 0;
    for (int i = 0; i < nm && k + 130 < sizeof(ml); i++)
        k += (size_t)snprintf(ml + k, sizeof(ml) - k, "%s%s%s (%s from %s)", i ? ", " : "", mm[i].on + strlen(p), mm[i].on[strlen(p)] ? "" : "<itself>", mm[i].fs, mm[i].from);
    DIR *d = opendir(p);
    size_t q = 0;
    int ne = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (q + 40 < sizeof(el)) q += (size_t)snprintf(el + q, sizeof(el) - q, "%s ", e->d_name);
            ne++;
        }
        closedir(d);
    } else {
        snprintf(el, sizeof(el), "(cannot open, errno %d)", errno);
    }
    say("sandbox %s %s: %d mount(s) [%s]; %d entr%s: %s", name, what, nm, ml, ne, ne == 1 ? "y" : "ies", el);
}

/* Removes what lies below path (files, links, folders) and then path itself, never leaving the filesystem it started on (a mount point differs in
 * its device number AND its filesystem id: either one stops the walk, so that not even a mount the table did not show can be walked into: the
 * nullfs mount of /data is the real /data), never following a link, at most *budget entries. 0 when it is all gone. */
static int rm_same_fs(const char *path, dev_t dev, const fsid_t *fsid, int depth, int *budget) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (st.st_dev != dev || depth > 8 || *budget <= 0) return -1;
    struct statfs sf;
    if (statfs(path, &sf) != 0 || sf.f_fsid.val[0] != fsid->val[0] || sf.f_fsid.val[1] != fsid->val[1]) return -1;
    (*budget)--;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 ? 0 : -1;
    int blocked = 0;
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[400];
            snprintf(c, sizeof(c), "%s/%s", path, e->d_name);
            if (rm_same_fs(c, dev, fsid, depth + 1, budget) != 0) blocked = 1;
        }
        closedir(d);
    } else {
        blocked = 1;
    }
    if (blocked) return -1;
    return rmdir(path) == 0 ? 0 : -1;
}

/* one sandbox: mounts off, then removed. 1 when it is gone. */
static int heal_one(const char *name) {
    char p[160];
    snprintf(p, sizeof(p), SANDBOX_DIR "/%s", name);
    log_sandbox("before the clean-up", name);
    struct stat st;
    if (lstat(p, &st) != 0) return 1;
    struct statfs root;
    if (statfs(p, &root) != 0) { say("sandbox %s: statfs failed (errno %d): nothing removed", name, errno); return 0; }
    /* The helper's mount thread puts /data back into any app sandbox that lacks it within 50 ms (in the merged helper it is a thread of this very
     * process), so nothing is looked at or logged between taking the mounts off and removing the folders: the mounts are listed first, taken off in
     * one go (the deepest first), and the walk that removes the folders follows at once. It never goes into a mount: one that would not come off
     * stops it (and is said below). */
    Mnt mm[16];
    const int nm = mounts_below(p, mm, 16);
    char res[480] = "";
    size_t k = 0;
    for (int i = 0; i < nm; i++) {
        int rc = unmount(mm[i].on, 0);
        int e = errno;
        const char *how = "";
        if (rc != 0) { rc = unmount(mm[i].on, MNT_FORCE); how = " forced"; if (rc != 0) e = errno; }
        if (rc == 0) rmdir(mm[i].on); /* the empty mount point, at once (an empty folder: rmdir never takes anything else) */
        if (k + 140 < sizeof(res)) k += (size_t)snprintf(res + k, sizeof(res) - k, "%s%s%s: %s", i ? "; " : "", mm[i].on + strlen(p), how, rc == 0 ? "ok" : strerror(e));
    }
    int budget = 400;
    const int rc = rm_same_fs(p, st.st_dev, &root.f_fsid, 0, &budget);
    const int gone = !exists(p);
    say("sandbox %s: %d mount(s) taken off [%s]; folders removed: %s (%s, %d of 400 entries looked at)", name, nm, res, gone ? "yes" : "NO",
        rc == 0 ? "clean" : "something is left", 400 - budget);
    if (!gone) {
        Mnt left[4];
        const int nl = mounts_below(p, left, 4);
        if (nl > 0) say("sandbox %s: %s is mounted again or would not come off (the helper's mount thread puts /data back within 50 ms): tried again later", name, left[0].on);
    }
    return gone;
}

static pthread_mutex_t g_heal_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile time_t g_follow_until; /* the logs of ShadowMount+ are followed until then, tiles or not (after a clean-up or a reload) */

/* The sandboxes of our titles that last changed at least min_age seconds ago (a start in progress is left alone), when no app runs. How many are gone. */
static int heal_sandboxes(int min_age, const char *why) {
    pthread_mutex_lock(&g_heal_mu);
    Sbx sb[16];
    const int n = list_sandboxes(sb, 16);
    int healed = 0;
    if (n > 0) {
        const int apps = app_processes();
        if (apps != 0) {
            say("sandboxes (%s): %d of ours, but %s: not cleaned up", why, n, apps < 0 ? "the process table cannot be read" : "an app is running");
        } else {
            for (int i = 0; i < n; i++) {
                if (sb[i].age < min_age) { say("sandbox %s: changed %lld s ago, too new to touch (%s)", sb[i].name, (long long)sb[i].age, why); continue; }
                if (heal_one(sb[i].name)) healed++;
            }
        }
    }
    pthread_mutex_unlock(&g_heal_mu);
    if (healed) g_follow_until = time(NULL) + 30;
    return healed;
}

/* from the main loop, once a second: say when the set of our sandboxes changes (the log is the evidence of what happens to them), and clean up
 * those that stay after the app ended: no app process for 6 s, and the sandbox itself unchanged for 10 s. A failed try is repeated every 20 s. */
static void heal_tick(time_t now) {
    static time_t next, stale_since, last_try, check_at;
    static int fails; /* clean-ups in a row that took nothing away: tried less and less often */
    static char last_sig[256];
    if (now < next) return;
    next = now + 1;
    if (check_at && now >= check_at) { /* the evidence: was ShadowMount+ released by the clean-up? */
        check_at = 0;
        const int busy = smp_busy();
        say("ShadowMount+ a few seconds after the clean-up: %s", busy == 1 ? "STILL BUSY (409)" : busy == 0 ? "free (no 409)" : "not reachable");
    }
    Sbx sb[16];
    const int n = list_sandboxes(sb, 16);
    char sig[256] = "";
    size_t k = 0;
    for (int i = 0; i < n && k + 40 < sizeof(sig); i++) k += (size_t)snprintf(sig + k, sizeof(sig) - k, "%s ", sb[i].name);
    if (strcmp(sig, last_sig) != 0) {
        say("our app sandboxes in " SANDBOX_DIR ": %s", n > 0 ? sig : n < 0 ? "(cannot be listed)" : "(none)");
        for (int i = 0; i < n; i++) if (!strstr(last_sig, sb[i].name)) log_sandbox("appeared", sb[i].name);
        snprintf(last_sig, sizeof(last_sig), "%s", sig);
    }
    if (n <= 0) { stale_since = 0; fails = 0; return; }
    if (flag("disctile_noheal")) return;
    if (app_processes() != 0) { stale_since = 0; return; } /* an app runs, or the table cannot be read */
    if (stale_since == 0) stale_since = now;
    if (now - stale_since < 6 || now - last_try < (fails < 4 ? 20 : 120)) return;
    last_try = now;
    const int busy0 = smp_busy();
    say("stale app sandbox(es) with no app running for %lld s; ShadowMount+ before the clean-up: %s", (long long)(now - stale_since),
        busy0 == 1 ? "BUSY (409)" : busy0 == 0 ? "free (no 409)" : "not reachable");
    const int healed = heal_sandboxes(10, "no app is running");
    fails = healed ? 0 : fails + 1;
    if (healed) {
        check_at = now + 4;
        say("cleaned up %d stale app sandbox(es): ShadowMount+ should take scans and uninstalls again", healed);
        notify_tile("PS2 disc: cleaned up %d stale app folder(s) of the last game; ShadowMount+ is free again", healed);
        if (g_scan_pending) g_scan_next = now;
    }
}

/* From the main loop, twice a second, for every tile that is being taken down (see GoneTrack): its title row is kept hidden until the uninstall went through (a
 * read first: it is written only when the row says visible); then, once the tile's folders are gone, the rows it leaves in the shell's database are deleted
 * (purge_title_rows: 1.5 s after the uninstall was accepted, again after 10 s and 30 s when they are still there), the home screen is nudged to look at the
 * list again, and the log says when the list lost the title: how long after the media went out, how much of that was ours (up to the accepted uninstall) and
 * how much came after. A slot taken by another disc is not followed. */
static void gone_tick(void) {
    static double last, last_nudge;
    const double t_now = mono_s();
    if (t_now - last < 0.5) return;
    last = t_now;
    for (int s = 0; s < NSLOTS; s++) {
        GoneTrack *g = &g_gone[s];
        if (!g->active) continue;
        if (g_slot_busy[s]) { g->active = 0; continue; }
        const char *tid = kSlots[s];
        int rows = db_has_title(tid);
        if (rows < 0) continue; /* the database could not be read this time */
        if (rows > 0 && g->accepted) {
            const double age = t_now - g->t_acc;
            const int due = (g->purges == 0 && age >= 1.5) || (g->purges == 1 && age >= 10.0) || (g->purges == 2 && age >= 30.0);
            char p[120];
            snprintf(p, sizeof(p), HB "/%s", tid);
            const int folder_gone = !exists(p);
            snprintf(p, sizeof(p), "/user/app/%s", tid);
            if (due && folder_gone && (!exists(p) || age >= 8.0)) { /* the shell has taken its files away (or has had its time) */
                g->purges++;
                const int deleted = purge_title_rows(tid);
                if (deleted > 0) g->rows_deleted += deleted;
                rows = db_has_title(tid);
                if (rows < 0) continue;
            }
        }
        if (rows == 0) {
            if (g->accepted)
                say("tile %s: gone from the home screen's list %.1f s after the media went out (ours up to the accepted uninstall %.1f s, %.1f s more after it; %d leftover row(s) deleted)",
                    tid, t_now - g->t_out, g->t_acc - g->t_out, t_now - g->t_acc, g->rows_deleted);
            else
                say("tile %s: gone from the home screen's list %.1f s after the media went out, before ShadowMount+ answered the uninstall", tid, t_now - g->t_out);
            g->active = 0;
            continue;
        }
        set_tile_row_visible(tid, 0);
        if (g->accepted && t_now - last_nudge >= 1.5) {
            last_nudge = t_now;
            sqlite3 *db = open_db();
            if (db) { nudge(db); sqlite3_close(db); }
        }
        if (g->accepted && t_now - g->t_acc > 90) {
            say("tile %s: STILL in the home screen's list %.0f s after the uninstall was accepted (rows %d, purged %d time(s))", tid, t_now - g->t_acc, rows, g->purges);
            g->active = 0;
        } else if (!g->accepted && t_now - g->t_out > 600) {
            g->active = 0;
        }
    }
}

/* ---------------------------------------------------------------- the reload tile
 * A permanent tile PPSA99230 ("Reload PS2 Disc"). Its app is PS5SX2's own eboot (build 171+), told by disc-tile.txt ("reload") to write RELOAD_REQ
 * and close at once. This payload takes it from there: waits for the app to be gone, cleans up the stale sandboxes, asks ShadowMount+ to scan
 * again (reset_attempts), makes the reload tile itself if it is missing, and has the tiles of the discs in the drives made afresh (the disc
 * watcher starts over too). The press is noticed by RELOAD_REQ, or when the tile's sandbox appears (an eboot that could not write the note). */
#ifdef DISC_MERGED
extern void disc_auto_request_reset(void);
#endif

static volatile int g_reload_running;
static volatile int g_reload_stage;     /* 0 idle; 1 the reload thread works; 2 it is done and the main loop finishes (the discs' tiles, the system's disc tile, the message) */
static volatile time_t g_reload_last;   /* when the last reload ended */
static char g_reload_msg[360];          /* the result for the person, made by the thread, sent by the main loop */
static volatile int g_reload_ok;        /* the reload went through (ShadowMount+ took the scan) */
static volatile time_t g_reload_scan_at; /* the reload tile is looked for on the home screen from then; 0 = no more */
static int g_reload_scan_tries;

static int reload_tile_current(void) {
    char want[96], exp[160];
    if (!eboot_sig(want, sizeof(want))) return 0;
    char *t = read_file(RELOAD_MARK, NULL);
    if (!t) return 0;
    t[strcspn(t, "\r\n")] = '\0';
    snprintf(exp, sizeof(exp), "%s rev " RELOAD_REV, want);
    const int same = strcmp(exp, t) == 0;
    free(t);
    return same && exists(RELOAD_DIR "/eboot.bin") && exists(RELOAD_DIR "/sce_sys/param.json") && exists(RELOAD_DIR "/sce_sys/icon0.png") &&
           exists(RELOAD_DIR "/disc-tile.txt");
}

static int reload_tile_build(void) {
    char how[96] = "", p[300], q[300];
    const char *dir = RELOAD_DIR;
    unlink(RELOAD_MARK);
    rm_tree(dir);
    mkdir(dir, 0777);
    chmod(dir, 0777);
    snprintf(p, sizeof(p), "%s/sce_module", dir);
    mkdir(p, 0777);
    chmod(p, 0777);
    snprintf(p, sizeof(p), "%s/eboot.bin", dir);
    const char *why = NULL; /* the first step that failed, for the log */
    int ok = put_exec_file(APP_SRC "/eboot.bin", p, how, sizeof(how)) == 0;
    if (!ok) why = "eboot.bin of " APP_SRC " could not be copied";
    DIR *dd = opendir(APP_SRC "/sce_module");
    if (dd) {
        struct dirent *e;
        while ((e = readdir(dd)) != NULL) {
            if (e->d_name[0] == '.') continue;
            snprintf(p, sizeof(p), APP_SRC "/sce_module/%s", e->d_name);
            snprintf(q, sizeof(q), "%s/sce_module/%s", dir, e->d_name);
            if (ok && put_exec_file(p, q, NULL, 0) != 0) { ok = 0; why = "a file of " APP_SRC "/sce_module could not be copied"; }
        }
        closedir(dd);
    }
    snprintf(p, sizeof(p), "%s/disc-tile.txt", dir);
    if (ok && write_file(p, "reload\n", 7) != 0) { ok = 0; why = "disc-tile.txt could not be written"; }
    chmod(p, 0777);
    char sys_tmp[160], sys[160];
    snprintf(sys_tmp, sizeof(sys_tmp), "%s/sce_sys.new", dir);
    snprintf(sys, sizeof(sys), "%s/sce_sys", dir);
    mkdir(sys_tmp, 0777);
    if (ok && !write_param_json(sys_tmp, NSLOTS, RELOAD_TID, "Reload PS2 Disc")) { ok = 0; why = "sce_sys/param.json could not be written"; }
    if (ok) {
        char icon[200], pic[200];
        snprintf(icon, sizeof(icon), "%s/icon0.png", sys_tmp);
        snprintf(pic, sizeof(pic), "%s/pic0.dds", sys_tmp);
        const int art = tile_make_reload_art(icon, pic, "/preinst/common/font/SST-Bold.otf", "RELOAD PS2 DISC");
        if (art != 0) {
            say("the reload tile's art could not be made (%d): PS5SX2's own instead", art);
            copy_file(APP_SRC "/sce_sys/icon0.png", icon);
            if (exists(APP_SRC "/sce_sys/pic0.dds")) copy_file(APP_SRC "/sce_sys/pic0.dds", pic);
        }
        chmod(icon, 0777);
        chmod(pic, 0777);
    }
    if (!ok) {
        say_dedupe("the reload tile " RELOAD_TID " could not be written: %s", why ? why : "a step failed");
        rm_tree(dir);
        return 0;
    }
    rename(sys_tmp, sys);
    chmod(sys, 0777);
    char sig[96], mark[200];
    if (eboot_sig(sig, sizeof(sig))) {
        snprintf(mark, sizeof(mark), "%s rev " RELOAD_REV "\n", sig);
        write_file(RELOAD_MARK, mark, strlen(mark));
    }
    whitelist_add(RELOAD_TID);
    say("the reload tile " RELOAD_TID " is made (eboot.bin %s)", how);
    return 1;
}

/* at the start, and from a reload: the folder is there and whole (else it is made), and the home screen is looked at in a moment (no sqlite here:
 * this may run on the reload thread). Flag disctile_noreload: no reload tile (an earlier one is taken away). */
static void reload_tile_ensure(void) {
    if (flag("disctile_noreload")) {
        if (exists(RELOAD_DIR)) {
            char reply[300];
            rm_tree(RELOAD_DIR);
            unlink(RELOAD_MARK);
            const int code = smp_post("/api/v1/games/uninstall", "{\"title_id\":\"" RELOAD_TID "\"}", reply, sizeof(reply));
            say("the reload tile is off (flag disctile_noreload): taken away, uninstall HTTP %d %.80s", code, code > 0 ? reply : "");
        }
        return;
    }
    whitelist_add(RELOAD_TID);
    if (reload_tile_current()) say("the reload tile " RELOAD_TID " is there");
    else if (!reload_tile_build()) return;
    g_reload_scan_tries = 0;
    g_reload_scan_at = time(NULL) + 12; /* ShadowMount+ takes a folder whose sce_sys has been still for 10 s */
}

/* from the main loop, every 30 s while the Reload tile is missing or incomplete (its build failed at the start of a tester's run, because the app's folder
 * was not whole yet, and nothing tried again: there was no tile to press): make it again. No sqlite here. */
static void reload_tile_recheck(time_t now) {
    static time_t next;
    if (now < next || g_reload_running) return;
    next = now + 30;
    if (flag("disctile_noreload") || reload_tile_current()) return;
    reload_tile_ensure();
}

/* from the main loop: is the reload tile on the home screen yet? When not, ShadowMount+ is asked to scan (it also scans by itself every 15 s). */
static void reload_tile_tick(time_t now) {
    if (g_reload_scan_at == 0 || now < g_reload_scan_at) return;
    const int rows = db_has_title(RELOAD_TID);
    if (rows > 0) {
        say("the reload tile is on the home screen (rows %d)", rows);
        g_reload_scan_at = 0;
        return;
    }
    char reply[300];
    const int code = smp_post("/api/v1/scan", "{\"reset_attempts\":true}", reply, sizeof(reply));
    g_reload_scan_tries++;
    say("the reload tile is not on the home screen yet (rows %d): scan HTTP %d %.100s (try %d)", rows, code, code > 0 ? reply : "", g_reload_scan_tries);
    g_reload_scan_at = now + (g_reload_scan_tries < 6 ? 8 : 30);
    if (g_reload_scan_tries >= 20) {
        say("the reload tile did not appear: given up (is ShadowMount+ loaded?)");
        g_reload_scan_at = 0;
    }
}

static int g_reload_sandbox_seen = -1; /* -1: not looked at yet */

/* Was the reload tile pressed? Bit 1: its app wrote RELOAD_REQ (it has run its marker code and is closing). Bit 2: its sandbox appeared (the app is only
 * starting; what was there at the start does not count). 0: no. */
static int reload_pressed(void) {
    int hit = 0;
    struct stat st;
    if (stat(RELOAD_REQ, &st) == 0) {
        unlink(RELOAD_REQ);
        say("reload tile: the app left its note");
        hit |= 1;
    }
    int has = 0;
    DIR *d = opendir(SANDBOX_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
            if (strncmp(e->d_name, RELOAD_TID "_", 10) == 0) { has = 1; break; }
        closedir(d);
    }
    if (g_reload_sandbox_seen >= 0 && has && !g_reload_sandbox_seen) {
        if (!hit) say("reload tile: its sandbox appeared");
        hit |= 2;
    }
    g_reload_sandbox_seen = has;
    return hit;
}

static void *reload_thread(void *arg) {
    const int how = (int)(intptr_t)arg; /* what noticed the press (reload_pressed) */
    const double t0 = mono_s();
    g_follow_until = time(NULL) + 90;
    say("reload: started");
    notify_tile("Reload PS2 disc: started");
    /* The app of the tile has to have had its say and be gone before anything is done. Started by its sandbox alone, the app is only starting: its
     * note, or a process that comes and goes, is waited for (12 s at the most). Then no app process for three looks in a row (25 s at the most). */
    if (!(how & 1)) {
        int seen = 0;
        for (int i = 0; i < 48; i++) {
            struct stat st;
            if (stat(RELOAD_REQ, &st) == 0) { unlink(RELOAD_REQ); say("reload: the app left its note"); break; }
            const int n = app_processes();
            if (n > 0) seen = 1;
            if (seen && n == 0) break;
            usleep(250000);
        }
    }
    int apps = -1, quiet = 0;
    for (int i = 0; i < 100; i++) {
        apps = app_processes();
        if (apps == 0) { if (++quiet >= 3) break; } else quiet = 0;
        usleep(250000);
    }
    if (apps != 0) {
        say("reload: %s after 25 s: nothing done", apps < 0 ? "the process table cannot be read" : "an app is still running");
        notify("Reload PS2 disc failed: an app is still open. Close it, then press Reload again.");
        g_reload_last = time(NULL);
        g_reload_running = 0;
        return NULL;
    }
    smp_quiet_ensure(); /* in case ShadowMount+'s config was written again meanwhile */
    int healed = heal_sandboxes(2, "reload");
    notify_tile("Reload PS2 disc: %d stale app folder(s) cleaned up", healed);
    /* ShadowMount+ must not be holding a title active (it then puts the scan off): up to about 30 s, cleaning up again in between */
    char reply[400] = "";
    int code = 0, taken = 0, busy = smp_busy();
    say("reload: ShadowMount+ is %s after the clean-up", busy == 1 ? "BUSY (a title is held active)" : busy == 0 ? "free" : "not reachable");
    for (int i = 0; i < 15 && busy == 1; i++) {
        sleep(2);
        healed += heal_sandboxes(2, "reload, again");
        busy = smp_busy();
    }
    if (busy == 1) {
        code = 409;
        snprintf(reply, sizeof(reply), "still busy");
    } else {
        code = smp_post("/api/v1/scan", "{\"reset_attempts\":true}", reply, sizeof(reply)); /* -1 = not there: the scan says so too */
        taken = code > 0 && !strstr(reply, "\"scan_deferred\":true");
    }
    say("reload: ShadowMount+ scan %s (HTTP %d %.100s), %d sandbox(es) cleaned up, %.1f s", taken ? "taken" : "NOT taken", code, code > 0 ? reply : "", healed, mono_s() - t0);
    reload_tile_ensure(); /* the reload tile itself, if it is missing or out of date */
#ifdef DISC_MERGED
    disc_auto_request_reset(); /* the disc watcher looks at the discs afresh */
#endif
    int discs = 0;
    for (int i = 0; i < MAX_DRIVES; i++) if (g_drv[i].present && g_drv[i].serial[0]) discs++;
    /* What the reload tile says: just "reloaded" (switch.txt = 1 adds what was done); a failure says why. Nothing else about the discs and tiles says anything. */
    g_reload_ok = taken;
    if (taken) {
        size_t k = (size_t)snprintf(g_reload_msg, sizeof(g_reload_msg), "PS2 disc reloaded. ShadowMount+ scanned again.");
        if (healed) k += (size_t)snprintf(g_reload_msg + k, sizeof(g_reload_msg) - k, " %d stale app folder(s) cleaned up.", healed);
        if (discs) snprintf(g_reload_msg + k, sizeof(g_reload_msg) - k, " The disc's tile is being made again.");
    } else if (code <= 0) {
        snprintf(g_reload_msg, sizeof(g_reload_msg), "Reload PS2 disc failed: ShadowMount+ did not answer. Is it loaded? Load it, then press Reload again.");
    } else {
        snprintf(g_reload_msg, sizeof(g_reload_msg), "Reload PS2 disc failed: ShadowMount+ is still busy with the last app. Load ShadowMount+ again, then press Reload once more.");
    }
    g_reload_stage = 2; /* the main loop finishes: the discs' tiles afresh, the system's disc tile hidden (its sqlite is the main loop's alone), the message */
    return NULL;        /* g_reload_running stays 1 until then */
}

static void start_reload(int how) {
    if (g_reload_running) return;
    g_reload_running = 1;
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 4 << 20); /* the reload tile's pictures are drawn on it when it has to be made again */
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, reload_thread, (void *)(intptr_t)how) != 0) { say("cannot start the reload thread"); g_reload_running = 0; }
    pthread_attr_destroy(&at);
}

/* From the main loop, when the reload thread is done (stage 2):
 *  - the discs in the drives are met as new ones (their tiles taken down and made again);
 *  - the system's "Unsupported Disc" tile is hidden now, whether our tile is ready or not, and the database nudged so that the home screen looks
 *    again even if the row said hidden already (the main loop keeps hiding it every second while a PS2 disc is known: it is not up to this
 *    function). Only for a disc whose PS2 serial we read: a disc that is not a PS2 game keeps its system tile.
 *  - the result is told to the person. */
static void reload_finish_tick(time_t now) {
    if (g_reload_stage != 2) return;
    for (int i = 0; i < MAX_DRIVES; i++) if (g_drv[i].building) return; /* wait for the tile threads */
    int ps2 = 0;
    for (int i = 0; i < MAX_DRIVES; i++) {
        Drive *d = &g_drv[i];
        if (!d->present) continue;
        if (d->serial[0]) ps2++;
        say("reload: /dev/cd%d (%s): its tile is made again, with the cover and the art fetched afresh", i, d->serial[0] ? d->serial : "no PS2 serial");
        if (d->serial[0]) cover_force_mark(d->serial);
        if (d->slot >= 0) { remove_tile(d->slot, "reload"); g_slot_busy[d->slot] = 0; d->slot = -1; }
        d->present = 0;
        d->media = 0;
        d->serial[0] = '\0';
        d->tries = 0;
        d->ready = 0;
        d->ready_t0 = 0;
        d->gone = 0;
        d->diag = 0;
    }
    int hidden = 0;
    if (ps2 > 0) hidden = hide_system_tile_ex(1);
    char msg[420];
    if (g_reload_ok && !switch_tile_notices_on()) snprintf(msg, sizeof(msg), "PS2 disc reloaded.");
    else snprintf(msg, sizeof(msg), "%s%s", g_reload_msg, (g_reload_ok && hidden) ? " The \"Unsupported Disc\" tile was hidden." : "");
    notify("%s", msg);
    g_reload_last = now;
    g_reload_stage = 0;
    g_reload_running = 0;
}

#ifdef DISC_HOST_THREAD
#include "gametile.c"
#endif

/* ---------------------------------------------------------------- 171k: next to the PS5SX3 helper (disc_compat.h has the rules)
 * The tile thread classifies /dev/cd0..cd3 (g_dc[0..3]); the disc watcher (disc_launch.c) keeps the USB drive's claim itself and tells
 * which drive holds a PS2 disc it reads (a PS2 CD, which /dev/cd0 does not show); DiscSpeed8 pins the drive only for a PS2 disc. */
static dc_drive g_dc[DC_NDRIVES];
static int g_dc_pub[DC_NDRIVES] = {DC_EMPTY, DC_EMPTY, DC_EMPTY, DC_EMPTY, DC_NONE}; /* what each drive is, as published (atomic) */
static int g_dc_watch_ps2[DC_NDRIVES]; /* the watcher holds a PS2 disc in that drive */
static volatile int g_dc_dirty;        /* a published claim changed: ps5sx2-helper.txt is written at once */
static int g_dc_off;                   /* flag disc_noyield (read at the start): 171j's way, no drive is ever yielded */

/* The published claim of a drive (a PS2 disc the watcher holds wins). Asked by the disc watcher and DiscSpeed8. */
int dc_claim_of(int drive);
int dc_claim_of(int drive) {
    if (drive < 0 || drive >= DC_NDRIVES) return DC_NONE;
    if (__atomic_load_n(&g_dc_watch_ps2[drive], __ATOMIC_RELAXED)) return DC_PS2;
    return __atomic_load_n(&g_dc_pub[drive], __ATOMIC_RELAXED);
}
void dc_publish(int drive, int claim);
void dc_publish(int drive, int claim) {
    if (drive < 0 || drive >= DC_NDRIVES) return;
    if (__atomic_exchange_n(&g_dc_pub[drive], claim, __ATOMIC_RELAXED) != claim) g_dc_dirty = 1;
}
void dc_watch_ps2(int drive, int on);
void dc_watch_ps2(int drive, int on) {
    if (drive < 0 || drive >= DC_NDRIVES) return;
    const int v = on ? 1 : 0;
    if (__atomic_exchange_n(&g_dc_watch_ps2[drive], v, __ATOMIC_RELAXED) != v) g_dc_dirty = 1;
}
/* 1 when the drive is left to the other helper (or the system) now: the watcher does not open it */
int dc_hands_off(int drive);
int dc_hands_off(int drive) { return !g_dc_off && dc_claim_of(drive) == DC_YIELD; }
/* 0 with the flag disc_noyield: nothing is ever yielded (the watcher asks it for the USB drive) */
int dc_on(void);
int dc_on(void) { return !g_dc_off; }

static int dc_fs_exists(void *u, const char *p) { (void)u; return exists(p); }
/* the disc mounts of the mount table (from /dev/cdN), read at most every 250 ms (tile thread only: getmntinfo's buffer is not the thread's own) */
static int dc_fs_mounts(void *u, dc_mnt *out, int max) {
    (void)u;
    static dc_mnt cache[16];
    static int ncache;
    static double at = -1;
    const double now = mono_s();
    if (at < 0 || now - at >= 0.25 || now < at) {
        at = now;
        ncache = 0;
        struct statfs *m = NULL;
        const int n = getmntinfo(&m, MNT_NOWAIT);
        for (int i = 0; i < n && m && ncache < 16; i++) {
            if (strncmp(m[i].f_mntfromname, "/dev/cd", 7) != 0) continue;
            snprintf(cache[ncache].from, sizeof(cache[ncache].from), "%s", m[i].f_mntfromname);
            snprintf(cache[ncache].on, sizeof(cache[ncache].on), "%s", m[i].f_mntonname);
            ncache++;
        }
    }
    const int k = ncache < max ? ncache : max;
    memcpy(out, cache, (size_t)k * sizeof(dc_mnt));
    return k;
}
static int dc_fd_sector(void *ctx, uint32_t lba, uint8_t *out) { return read_lba(*(int *)ctx, lba, out); }

static int g_kp_nodrive; /* flag kp_nodrive (read at the start): the drives are never opened, so no tile is made: for the kernel-panic test */

static void poll_drive_look(int i) {
    Drive *d = &g_drv[i];
    char dev[16];
    snprintf(dev, sizeof(dev), "/dev/cd%d", i);
    off_t media = 0;
    int fd = exists(dev) ? open(dev, O_RDONLY) : -1;
    if (fd >= 0 && (ioctl(fd, DIOCGMEDIASIZE, &media) != 0 || media <= 0)) media = 0;
    if (g_dc[i].claim == DC_YIELD) { /* 171k: the yielded drive's one look per DC_LOOK_S: its size only, nothing read */
        if (!dc_after_look(&g_dc[i], (long long)media, mono_s())) { if (fd >= 0) close(fd); return; }
        say("%s: the disc that is not a PS2 disc %s: the drive is looked at again%s", dev, media > 0 ? "was replaced by another" : "went",
            media > 0 ? "" : " (after a 5 s rest)");
        if (media <= 0) { if (fd >= 0) close(fd); return; } /* (the disc-out bookkeeping below happens after the rest) */
    }
    if (media > 0) {
        d->gone = 0;
        if (!d->present || media != d->media) { /* a disc, or another one */
            if (d->present && d->slot >= 0 && !d->building) { remove_tile(d->slot, "another disc"); g_slot_busy[d->slot] = 0; d->slot = -1; }
            d->present = 1;
            d->media = media;
            d->serial[0] = '\0';
            d->tries = 0;
            d->ready = 0;
            d->ready_t0 = 0;
            d->diag = 0;
            d->kinded = 0;
            d->t_disc = mono_s();
            say("%s: a disc is in (%lld bytes)", dev, (long long)media);
            dc_note(&g_dc[i], DC_KIND_UNKNOWN, (long long)media, mono_s());
        }
        if (!d->serial[0] && d->tries < (int)(5000000 / POLL_US) && !d->building) {
            d->tries++;
            char serial[16] = "";
            if (read_serial(fd, serial)) {
                snprintf(d->serial, sizeof(d->serial), "%s", serial);
                d->t_serial = mono_s();
                say("%s: PS2 disc %s", dev, serial);
                dc_note(&g_dc[i], DC_KIND_PS2, (long long)media, mono_s());
                if (flag("disctile_nowait")) hide_system_tile(); /* the old way; otherwise it is hidden when OUR tile is launchable (check_ready) */
                purge_free_slots("a PS2 disc was read"); /* the ghosts of removed tiles go before a new one is made */
                reload_row_front("a PS2 disc was read"); /* before the disc tile is installed: it then stands first and the Reload tile right behind it */
                start_tile(i, serial);
            } else {
                /* 171k: the first failed read asks the root folder what the disc is: a PS3 (or PS1) disc's drive is left alone at once */
                int k = DC_KIND_UNKNOWN;
                if (!d->kinded && !g_dc_off) {
                    d->kinded = 1;
                    k = dc_iso_kind(dc_fd_sector, &fd);
                    if (k == DC_KIND_UNKNOWN) d->kinded = 0; /* (sector 16 did not read: asked again at the next try) */
                }
                if (k == DC_KIND_PS3 || k == DC_KIND_PS1) {
                    dc_note(&g_dc[i], k, (long long)media, mono_s());
                    say("%s: %s (its root folder), not a PS2 disc: no tile, and the drive is left to %s until the disc goes (one look at its size every %.0f s)",
                        dev, dc_kind_name(k), k == DC_KIND_PS3 ? "the PS5SX3 helper" : "the system", DC_LOOK_S);
                    d->tries = (int)(5000000 / POLL_US) + 1;
                } else if (d->tries == (int)(5000000 / POLL_US)) {
                    say("%s: no PS2 serial on this disc (not a PS2 DVD, or not readable): no tile%s", dev,
                        g_dc_off ? "" : "; the drive is left alone until the disc goes (one look at its size every 5 s)");
                    if (!g_dc_off) dc_note(&g_dc[i], DC_KIND_OTHER, (long long)media, mono_s());
                }
            }
        }
    } else if (d->present) {
        if (++d->gone == 1) d->t_gone0 = mono_s(); /* the media is gone: since when */
        if (d->gone >= (int)(500000 / POLL_US) && !d->building) { /* a disc out (half a second of no media: it was two seconds, then one; a tile that stays after the eject was noticed) */
            say("%s: the disc is out", dev);
            dc_note(&g_dc[i], DC_KIND_NONE, 0, mono_s());
            g_remove_t0 = d->t_gone0;
            if (d->slot >= 0) { remove_tile(d->slot, "disc out"); g_slot_busy[d->slot] = 0; d->slot = -1; }
            d->present = 0;
            d->media = 0;
            d->serial[0] = '\0';
            d->tries = 0;
            d->ready = 0;
            d->ready_t0 = 0;
        }
    }
    if (fd >= 0) close(fd);
}

/* 171k: the drive looked at only when disc_compat.h's gate says so (a PS3 disc's files on the PS5's mount: never; a yielded disc: its size
 * every DC_LOOK_S; the 5 s after a yielded disc went: not at all); what the drive is now published for the PS5SX3 helper. */
static void poll_drive(int i) {
    if (g_kp_nodrive || kp_quiet()) return; /* 171e: no open() of /dev/cdN while the console resets its drive for a restart */
    if (g_dc_off) { poll_drive_look(i); return; }
    const int was = dc_published(&g_dc[i], mono_s());
    char where[160] = "";
    const dc_fs fs = {dc_fs_exists, dc_fs_mounts, NULL};
    const int mount = dc_mount_ps3(&fs, i, where, sizeof(where));
    int changed = 0;
    const int may = dc_gate(&g_dc[i], mount, mono_s(), &changed);
    if (changed && g_dc[i].claim == DC_YIELD)
        say("/dev/cd%d: a PS3 disc (its files on the PS5's mount %s): the drive is left to the PS5SX3 helper until they go (not opened, not asked)", i, where);
    else if (changed)
        say("/dev/cd%d: the PS3 disc's files are gone from the PS5's mount: the drive is looked at again after a %.0f s rest", i, DC_GRACE_S);
    if (may) poll_drive_look(i);
    const int now_pub = dc_published(&g_dc[i], mono_s());
    if (now_pub != was) say("/dev/cd%d: %s -> %s (published in ps5sx2-helper.txt)", i, dc_claim_name(was), dc_claim_name(now_pub));
    dc_publish(i, now_pub);
}

static void __attribute__((unused)) on_term(int s) { (void)s; g_stop = 1; }

/* The whole payload. hosted = 0: a process of its own (PS5SX2-Disc.elf): it takes its credentials, its signals, the single-instance rule and the
 * whitelist entries itself. hosted = 1 (build with -DDISC_HOST_THREAD): a resident thread of a host process, the merged PS5SXHelper.elf, which already
 * carries the credentials (authid 0x4800000000010003, full caps), the signals and its own jailbreak list: those are left out, the loop never stops
 * (the host's process ends it), and the entry is disc_payload_thread() below. */
static int payload_run(int hosted) {
    mkdirs(WORK);
    mkdirs(ART_DIR);
    mkdir(FLAGS_DIR, 0777);
    switch_ensure_file(); /* /data/PCSX2/switch.txt: 0 (the default) or 1, the notifications about the tiles */
    smp_quiet_ensure(); /* ShadowMount+'s own "Installing: ..." popup off (its quiet_mode); flag disctile_smnotify leaves it alone */
    say("DiscTile payload %s (pid %d%s), notifications about the tiles %s (switch.txt)", PAYLOAD_REV, (int)getpid(), hosted ? ", a thread of the helper" : "", switch_tile_notices_on() ? "ON" : "off");
    klog_printf("[kp] DiscTile payload %s: the klog line that tells which build runs\n", PAYLOAD_REV);
#ifdef DISC_HOST_THREAD
    payload_progress("disc payload " PAYLOAD_REV); /* 171f: payload-init.log (fsync'ed) says which payload runs, with the log files off too */
#endif
    g_kp_nodrive = kp_flag("kp_nodrive");
    g_dc_off = flag("disc_noyield"); /* 171k: no drive is ever left to the PS5SX3 helper (171j's way) */
    say("next to the PS5SX3 helper: %s", g_dc_off ? "OFF (flag disc_noyield): every drive is looked at as in 171j" : "a drive whose disc is not a PS2 disc is left alone until it goes; ps5sx2-helper.txt says compat=1 and what each drive is");
    kp_start(); /* 171e: the shutdown watch's report, and the :9020 answerer when asked */
    signal(SIGPIPE, SIG_IGN); /* a ShadowMount+ that closes early must not end the whole process (a host that has not done it yet) */
    if (!hosted) {
        {
            uint8_t all[16];
            memset(all, 0xff, sizeof(all));
            kernel_set_ucred_authid(getpid(), 0x4800000000010003ull);
            kernel_set_ucred_caps(getpid(), all);
        }
        signal(SIGTERM, on_term);
        signal(SIGINT, on_term);
        {
            FILE *f = fopen(PID_FILE, "r");
            int old = 0;
            if (f) { if (fscanf(f, "%d", &old) != 1) old = 0; fclose(f); }
            if (old > 1 && old != (int)getpid() && kill(old, 0) == 0) { say("another copy (pid %d) is running: stopping it", old); kill(old, SIGTERM); sleep(3); }
            f = fopen(PID_FILE, "w");
            if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
        }
    }
    write_helper_status(); /* 179: let the app know a helper (rev PAYLOAD_REV) is here from the first moment */
    if (!exists(APP_SRC "/eboot.bin")) { notify("DiscTile: PS5SX2 is not at " APP_SRC); return 1; }
    for (int i = 0; i < MAX_DRIVES; i++) g_drv[i].slot = -1;
    {
        char *ns = read_file(WORK "/next-slot.txt", NULL);
        if (ns) { g_next_slot = atoi(ns) % NSLOTS; free(ns); }
    }
    /* tiles left by an earlier run are removed first */
    for (int s = 0; s < NSLOTS; s++) {
        char dir[128];
        snprintf(dir, sizeof(dir), HB "/%s", kSlots[s]);
        if (exists(dir)) remove_tile(s, "left from an earlier run");
    }
    purge_free_slots("payload start"); /* rows of removed tiles that an earlier run left in the shell's database */
    /* All the tile IDs go into the whitelist now, before any helper is started after us: a helper that reads the file only at its start
     * then knows them (load DiscTile first, then the helper). A hosting helper has the eight slots built in; the reload tile's ID is added in any case. */
    if (!hosted) for (int s = 0; s < NSLOTS; s++) whitelist_add(kSlots[s]);
    if (!flag("disctile_noreload")) whitelist_add(RELOAD_TID);
    warm_init(); /* can folders be staged beforehand? (see the staged tiles above) */
    reload_tile_ensure(); /* the "Reload PS2 Disc" tile: made when it is missing or out of date */
    { /* what is in the app sandboxes at the start: the log shows whether a stale one is there */
        Sbx sb[16];
        const int n = list_sandboxes(sb, 16);
        say("start: %d app sandbox(es) of ours in " SANDBOX_DIR "%s", n < 0 ? 0 : n, n < 0 ? " (cannot be listed)" : "");
        for (int i = 0; i < n; i++) log_sandbox("at the start", sb[i].name);
    }
    { /* what the shell keeps of the tile slots' title IDs (their picture and name are kept by ID): the log shows whether such leftovers exist at all */
        char all[400] = "";
        size_t k = 0;
        for (int s = 0; s < NSLOTS && k + 80 < sizeof(all); s++) {
            char keeps[64];
            slot_keeps(s, keeps, sizeof(keeps));
            if (keeps[0]) k += (size_t)snprintf(all + k, sizeof(all) - k, "%s: %s; ", kSlots[s], keeps);
        }
        say("start: what the shell keeps of the tile slots' title IDs: %s", k ? all : "nothing (every slot is clean)");
    }
#ifdef DISC_MERGED
    /* One payload for both: the disc watcher (disc_launch.c, built with -DDISC_AUTO_EMBEDDED) runs on a thread of its own. This process already has
     * its credentials and signals; the watcher's single-instance rule is applied once (an older DiscLaunch.elf or merged copy is stopped). */
    {
        extern void disc_auto_take_over(void);
        extern void *disc_auto_thread(void *);
        if (!hosted) {
            signal(SIGCHLD, SIG_IGN);
            disc_auto_take_over();
        }
        pthread_t dth;
        pthread_attr_t dat;
        pthread_attr_init(&dat);
        pthread_attr_setstacksize(&dat, 2 << 20);
        pthread_attr_setdetachstate(&dat, PTHREAD_CREATE_DETACHED);
        if (kp_flag("kp_nowatcher")) say("the disc watcher is not started (flag kp_nowatcher): for the kernel-panic test");
        else if (pthread_create(&dth, &dat, disc_auto_thread, NULL) != 0) say("the disc watcher thread could not be started");
        else say("the disc watcher runs inside this payload (one ELF for the watcher and the tiles)");
        pthread_attr_destroy(&dat);
    }
    notify_tile("PS5SX2 disc payload running: a PS2 disc gets a tile on the home screen"); /* the watcher says its own line too (always shown) */
#else
    notify_tile("DiscTile test running: a PS2 disc gets a tile on the home screen");
#endif

    time_t next_hide = 0;
    while (!g_stop) {
        if (kp_quiet()) { usleep(POLL_US); continue; } /* 171e: the console shuts down or rests: nothing is opened, written or asked until it works again */
        const int gq = kp_game_quiet(); /* 171f: a game plays from a drive: each drive is looked at once per 3 s, not four times a second */
        int any = 0, ps2 = 0;
        for (int i = 0; i < MAX_DRIVES; i++) {
#ifdef DISC_MERGED
            /* 171h: the disc watcher (disc_launch.c) reads this drive itself (a PS2 CD or USB disc copy): not opened at all; PS5SX2 copies a disc:
             * every drive once per 3 s, as in game quiet. Each open of an idle /dev/cdN is a lock of the tray and a few commands to the drive. */
            extern int disc_auto_drive_busy(int drive);
            const int busy = disc_auto_drive_busy(i);
            static double busy_due[MAX_DRIVES];
            if (busy == 2) {
                /* left alone */
            } else if (gq) {
                if (kp_game_quiet_due(i)) poll_drive(i);
            } else if (busy == 1) {
                const double t = mono_s();
                if (t >= busy_due[i] || busy_due[i] - t > 3.0) { busy_due[i] = t + 3.0; poll_drive(i); }
            } else {
                poll_drive(i);
            }
#else
            if (!gq || kp_game_quiet_due(i)) poll_drive(i);
#endif
            if (g_drv[i].present && g_drv[i].serial[0]) ps2 = 1;
            if (g_drv[i].present && g_drv[i].serial[0] && g_drv[i].ready) any = 1; /* the system's tile goes only when ours is launchable */
        }
        check_ready();
        warm_kick();
        {
            int active = time(NULL) < g_follow_until; /* after a clean-up or during a reload too */
            for (int i = 0; i < MAX_DRIVES; i++) if (g_drv[i].building || g_drv[i].slot >= 0) active = 1;
            follow_logs(active);
        }
        const time_t now = time(NULL);
        /* The system's "Unsupported Disc" tile is hidden from the moment a PS2 disc is known, not when our tile is ready: the shell redraws its tile row when
         * our tile is installed, and a tester's video showed the system's tile still beside the new one when it came. The system sets the row again at
         * times, so it is looked at every second (a read only look: nothing to hide takes no lock). */
        if ((any || ps2) && now >= next_hide) { hide_system_tile(); next_hide = now + 1; }
        for (int s = 0; s < NSLOTS; s++) { /* a removal whose thread could not be started (or is gone): asked again here, every 2 s */
            static time_t again_at[NSLOTS];
            if (g_slot_stale[s] && !g_rm_running[s] && !g_slot_busy[s] && now >= again_at[s]) { again_at[s] = now + 2; remove_tile(s, "retry"); }
        }
        if (g_scan_pending && now >= g_scan_next) {
            char reply[512] = "";
            int tiles = 0;
            for (int i = 0; i < MAX_DRIVES; i++) if (g_drv[i].slot >= 0) tiles = 1;
            /* not while ShadowMount+ holds a title active: a scan then only queues (and its reset_attempts would be wasted) */
            const int busy = tiles ? smp_busy() : 0;
            const int code = !tiles ? 200 : busy == 1 ? 409 : smp_post("/api/v1/scan", "{\"reset_attempts\":true}", reply, sizeof(reply));
            g_scan_tries++;
            if (!tiles || (busy != 1 && code > 0 && !strstr(reply, "\"scan_deferred\":true"))) {
                if (tiles) say("scan taken after %d tries: HTTP %d", g_scan_tries, code);
                g_scan_pending = 0;
            } else {
                g_scan_next = now + (g_scan_tries < 12 ? 5 : 30);
                if (g_scan_tries == 1 || g_scan_tries % 10 == 0)
                    say("scan again later (try %d): ShadowMount+ %s, HTTP %d %.80s", g_scan_tries, busy == 1 ? "BUSY (409)" : "did not take it", code, code > 0 ? reply : "");
            }
        }
        reload_tile_tick(now);
        reload_tile_recheck(now);
        heal_tick(now);
        gone_tick();
        reload_finish_tick(now);
        {
            const int pressed = reload_pressed(); /* always looked at: it also takes the note away */
            if (pressed && !g_reload_running && now - g_reload_last >= 8) start_reload(pressed);
        }
#ifdef DISC_HOST_THREAD
        gt_tick(now);
#endif
        { static time_t hs_last; if (now != hs_last || g_dc_dirty) { hs_last = now; g_dc_dirty = 0; write_helper_status(); } } /* 179: refresh ps5sx2-helper.txt (the app's auto-load heartbeat) */
        usleep(POLL_US);
    }
    say("stopping: removing the tiles");
    for (int i = 0; i < MAX_DRIVES; i++)
        if (g_drv[i].slot >= 0) { /* here and now, the process ends: one pass each, no thread */
            char reply[512] = "";
            const int code = remove_pass(g_drv[i].slot, reply, sizeof(reply));
            say("tile %s removed (stopping): uninstall HTTP %d %s", kSlots[g_drv[i].slot], code, code > 0 ? reply : "");
        }
    {
        /* 171k: put back only when this payload hid it, for a PS2 disc; with another disc in (a PS3 one) the PS5SX3 helper may keep it hidden */
        int ps2_in = 0;
        for (int i = 0; i < MAX_DRIVES; i++) if (g_drv[i].present && g_drv[i].serial[0]) ps2_in = 1;
        if (ps2_in) show_system_tile(1);
        else say("stopping: no PS2 disc is in: the system's disc tile is left as it is");
    }
    unlink(PID_FILE);
    unlink("/data/PCSX2/logs/ps5sx2-helper.txt"); /* 179: the app sees the helper is gone */
    return 0;
}

/* 179: the PS5SX2 app reads this to decide whether to auto-load the helper (OrbisHelperAutoload.cpp): the helper's pid, a
 * heartbeat time() and PAYLOAD_REV. Written whole (temp file then rename) so the app never reads half of it. */
static void write_helper_status(void) {
    const char *path = "/data/PCSX2/logs/ps5sx2-helper.txt";
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "pid=%d\ntime=%lld\nrev=%s\n", (int)getpid(), (long long)time(NULL), PAYLOAD_REV);
    if (!g_dc_off) { /* 171k: next to the PS5SX3 helper (disc_compat.h): what each drive is */
        int claims[DC_NDRIVES];
        for (int i = 0; i < DC_NDRIVES; i++) claims[i] = dc_claim_of(i);
        char lines[160];
        dc_status_lines(claims, lines, sizeof(lines));
        fputs(lines, f);
    }
    fclose(f);
    rename(tmp, path);
}

#ifdef DISC_HOST_THREAD
/* The host helper (PS5SXHelper.elf: the /data mount, the jailbreak watcher and this payload in one process) starts this on a thread of its own
 * (4 MB stack: the pictures are drawn on it). It returns only when PS5SX2 is not installed. */
void *disc_payload_thread(void *arg);
void *disc_payload_thread(void *arg) {
    (void)arg;
    payload_run(1);
    return NULL;
}
#else
int main(void) { return payload_run(0); }
#endif
