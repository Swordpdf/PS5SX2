/* PS5SX2 disc-auto daemon (vk-285-160k, AI-assisted).
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
 * vk-285-160f: 160e on the console (WRC, SCES-50139): dump, launch and self-exec all worked, but a self-exec
 *   keeps the focus the app had. After the disc goes in the shell owns focus (home screen), so the game came
 *   up behind it. Now, once PS5SX2 has re-executed, the daemon hands it focus (sceLncUtilSetAppFocus, then
 *   sceLncUtilLaunchApp on the running title as clicking its tile does) at +2/+6/+12 s. A new daemon also
 *   kills the previous one (logs/disc-auto.pid) so a resend doesn't leave two running.
 * vk-285-160g: a self-exec keeps PS5SX2's app id (0x8017 before and after on the console), so 160f's "new app
 *   id" trigger never fired. Focus is now timed from the moment PS5SX2 consumes disc-launch-exec.txt. The
 *   pid-file lookup no longer insists on the full kinfo_proc size (it failed silently and left 160e running),
 *   and the file may list several pids.
 * vk-285-160h: SetAppFocus(id, 0) returned 0 but the console logged SetControllerFocus(-1), its "take focus away"
 *   line: 0 is likely "unfocus". Focus attempts are now SetAppFocus(id, 1), then LaunchApp on the running title,
 *   then SetAppFocus(id, 1) again, each logged. The PS5's kinfo_proc is bigger than the SDK's (ENOMEM), so the
 *   pid lookup reads into a large buffer.
 * vk-285-160i: 160h on the console: sceLncUtilLaunchApp on the running PS5SX2 answered 0x8094000c (already
 *   running) yet the shell ran its LaunchFlow and switched to it (FG 7 -> 0x8017, controller focus to it): that
 *   is the call. SetAppFocus logged SetControllerFocus(-1) each time (it takes the pad away), so it is gone.
 *   LaunchApp at +3 and +8 s after the exec signal is consumed.
 * vk-285-160j: 160i's cold start KERNEL-PANICKED the console right after "forked N for LoadExec": fork() in a
 *   payload process, then LoadExec from the child. No fork and no LoadExec any more. PS5SX2 is started with
 *   sceLncUtilLaunchApp (the call that already brought it to the front, the launcher's way): for a launch it
 *   gets disc-launch.txt only (no exec signal, nothing to re-exec), so its first boot starts the game; LaunchApp
 *   again at +6/+11 s keeps it in front. For a dump it is simply launched.
 * vk-285-160k: 160j passed all four cases on swordpdf's phat (dump, cold start, running, disc out -> shelf).
 *   Each extra LaunchApp on an app already in front replays its splash for a moment, so one follow-up only
 *   (+3 s after the exec signal is consumed; +6 s after a cold launch, in case the shell's disc screen lands
 *   last), and the settle after insert is 2 s (was 4) to shorten the home-screen detour.
 * live-12: the start of a disc dumper for PS1, PS2 and PS3 discs, PS2 for now, after the disc_detect payload its author
 *   sent swordpdf on 2026-10-10 (written anew here; what it showed: the drive's GET CONFIGURATION profile tells Sony's own
 *   disc kinds apart, 0xFF50 PS1 CD, 0xFF60 PS2 CD, 0xFF61 PS2 DVD, 0xFF70/0xFF71 PS3; a CD is read raw with READ CD,
 *   2352-byte sectors, into a .bin with a .cue; CRC-32, MD5 and SHA-1 as Redump lists them). Each disc put in logs its
 *   profile. A PS2 DVD goes the way it went (PS5SX2 dumps it). A PS2 CD, which /dev/cd0 can't give as a PS2 disc, is
 *   dumped by this daemon through the drive's pass device to games/<Title> (<SERIAL>).bin + .cue, its checksums in
 *   <Title> (<SERIAL>).hashes.txt, then started like a DVD. PS1 and PS3 discs are only named in the log for now.
 *   Needs proper testing on a console with a PS2 CD.
 * live-14: live-12 on the console: the drive reports a PS2 DVD as plain 0x0010 DVD-ROM (Sony's 0xFF6x profiles are
 *   ShellCore's own, not the drive's on a PS5), so a PS2 CD is looked for on any CD profile (0x08/0x09/0x0A) by its
 *   SYSTEM.CNF's BOOT2, through READ CD.
 * live-5: USB probe: which standard read commands return data (READ 6/10/12/16, READ CD, capacities, TOC, ...).
 * live-4: USB diagnostics: raw CCB status / SCSI status / resid, GET CONFIGURATION, and the device queue's freeze
 *   count, released (only as many as it reads) in case the cd driver's failed attach left it frozen.
 * live-3: USB serial read says why it failed (sector 16 result, capacity), READ(12) if READ(10) is refused, 5 tries.
 * live-2: USB DVD drives through their pass device (the CD device doesn't attach on the 4.03 phat), dumped by
 *   the daemon itself; see usb_dump.
 * live-1: builds are now named live-N (swordpdf, 2026-10-09); live-1 = vk-285-160k daemon + 160l eboot.
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
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "disc_hash.h"

#define LOGS_DIR    "/data/PCSX2/logs"
#define GAMES_DIR   "/data/PCSX2/games"
#define LAUNCH_TXT  LOGS_DIR "/disc-launch.txt"
#define EXEC_TXT    LOGS_DIR "/disc-launch-exec.txt"
#define AUTO_TXT    LOGS_DIR "/disc-autostart.txt"
#define EMU_ALIVE   LOGS_DIR "/ps5sx2-alive.txt"
#define SELF_ALIVE  LOGS_DIR "/disc-auto-alive.txt"
#define PID_FILE    LOGS_DIR "/disc-auto.pid"
#define TITLE_ID    "PPSA99203"

enum { kPollSec = 1, kSettleSec = 2, kAliveWithin = 8, kRestartAfter = 45 };

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
typedef int (*sector_reader)(void *ctx, uint32_t lba, uint8_t *out);
static int read_serial_with(sector_reader rd, void *ctx, char *out) {
    uint8_t pvd[2048];
    if (!rd(ctx, 16, pvd)) return 0;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return 0;

    const uint32_t root_lba  = le32(pvd + 156 + 2);
    const uint32_t root_size = le32(pvd + 156 + 10);
    const uint32_t dir_secs  = (root_size + 2047) / 2048 < 64 ? (root_size + 2047) / 2048 : 64;

    uint8_t dir[2048];
    for (uint32_t ds = 0; ds < dir_secs; ds++) {
        if (!rd(ctx, root_lba + ds, dir)) continue;
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
                    if (!rd(ctx, clba + k, sec)) break;
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

static int fd_sector(void *ctx, uint32_t lba, uint8_t *out) { return read_lba(*(int *)ctx, lba, out); }
static int read_serial(int fd, char *out) { return read_serial_with(fd_sector, &fd, out); }

/* ------------------------------------------------------------------ */
/* games/ scanner                                                        */
/* ------------------------------------------------------------------ */
/* live-12: .bin too (a PS2 CD's raw copy). */
static int has_iso_ext(const char *n) {
    const size_t l = strlen(n);
    return l >= 5 && (strcasecmp(n + l - 4, ".iso") == 0 || strcasecmp(n + l - 4, ".bin") == 0);
}

/* live-12: a raw CD image's sectors as 2048-byte ones: 2352-byte sectors with the data 24 bytes in (mode 2, the PS2's
 * CDs) or 16 (mode 1); a file without the CD sync pattern at its start is read as a plain ISO. */
typedef struct { int fd; uint32_t size, off; } image_ctx;
static const uint8_t k_cd_sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
static void image_open(image_ctx *c, int fd) {
    uint8_t h[16] = {0};
    c->fd = fd; c->size = 2048; c->off = 0;
    if (pread(fd, h, 16, 0) == 16 && memcmp(h, k_cd_sync, 12) == 0) { c->size = 2352; c->off = h[15] == 1 ? 16 : 24; }
}
static int image_sector(void *ctx, uint32_t lba, uint8_t *out) {
    const image_ctx *c = ctx;
    return pread(c->fd, out, 2048, (off_t)lba * c->size + c->off) == 2048;
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
            image_ctx ic;
            image_open(&ic, fd);
            const int ok = read_serial_with(image_sector, &ic, found);
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
/* ------------------------------------------------------------------ */
/* Focus: bring the running PS5SX2 to the front (vk-285-160f)            */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t size; int user_id; uint32_t app_opt; uint64_t crash_report; uint64_t check_flag; } lnc_app_param_t;
typedef uint32_t (*GetBigApp_t)(void);
typedef int (*GetTitle_t)(uint32_t, char *);
typedef int (*SetFocus_t)(uint32_t, int);
typedef int (*LaunchApp_t)(const char *, const char **, lnc_app_param_t *);
typedef int (*UsrInit_t)(void *);
typedef int (*UsrFg_t)(int *);

static GetBigApp_t p_bigapp;
static GetTitle_t  p_title;
static SetFocus_t  p_focus;
static LaunchApp_t p_launch;
static UsrFg_t     p_fguser;

static void load_lnc(void) {
    void *h = dlopen("libSceSystemService.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (h) {
        p_bigapp = (GetBigApp_t)dlsym(h, "sceLncUtilGetAppIdOfRunningBigApp");
        p_title  = (GetTitle_t)dlsym(h, "sceLncUtilGetAppTitleId");
        p_focus  = (SetFocus_t)dlsym(h, "sceLncUtilSetAppFocus");
        p_launch = (LaunchApp_t)dlsym(h, "sceLncUtilLaunchApp");
    }
    void *u = dlopen("libSceUserService.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (u) {
        UsrInit_t init = (UsrInit_t)dlsym(u, "sceUserServiceInitialize");
        p_fguser = (UsrFg_t)dlsym(u, "sceUserServiceGetForegroundUser");
        if (init) { const int rc = init(NULL); say("sceUserServiceInitialize = 0x%08x", (unsigned)rc); }
    }
    say("lnc: bigapp %p title %p focus %p launch %p fguser %p", (void *)p_bigapp, (void *)p_title,
        (void *)p_focus, (void *)p_launch, (void *)p_fguser);
}

/* PS5SX2's app id when it is the running big app, else 0. */
static uint32_t emu_appid(void) {
    if (!p_bigapp || !p_title) return 0;
    const uint32_t id = p_bigapp();
    if (id == 0 || id == 0xffffffffu) return 0;
    char t[32] = {0};
    if (p_title(id, t) != 0) return 0;
    return strncmp(t, TITLE_ID, 9) == 0 ? id : 0;
}

/* Launching the title that is already running is what clicking its tile does: the shell answers
 * "already running" (0x8094000c) and switches to it. */
static void give_focus(uint32_t id, int attempt) {
    if (!p_launch) { say("focus #%d: no sceLncUtilLaunchApp", attempt); return; }
    int user = -1;
    if (p_fguser) p_fguser(&user);
    lnc_app_param_t prm;
    memset(&prm, 0, sizeof(prm));
    prm.size = sizeof(prm);
    prm.user_id = user;
    const int rc = p_launch(TITLE_ID, NULL, &prm);
    say("focus #%d (app 0x%x): sceLncUtilLaunchApp(" TITLE_ID ", user %d) = 0x%08x", attempt, (unsigned)id, user, (unsigned)rc);
}

/* ------------------------------------------------------------------ */
/* One daemon at a time                                                  */
/* ------------------------------------------------------------------ */
static int proc_comm(pid_t pid, char *out, size_t max) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
    static unsigned char buf[8192];
    size_t len = sizeof(buf);
    memset(buf, 0, sizeof(buf));
    if (sysctl(mib, 4, buf, &len, NULL, 0) != 0) { say("sysctl(proc %d): errno %d", (int)pid, errno); return 0; }
    if (len == 0) return 0;
    const struct kinfo_proc *kp = (const struct kinfo_proc *)buf;
    if (kp->ki_pid != pid) { say("sysctl(proc %d): len %zu, ki_pid %d", (int)pid, len, (int)kp->ki_pid); return 0; }
    snprintf(out, max, "%s", kp->ki_comm);
    say("pid %d: comm '%s' (kinfo %zu bytes, SDK %zu)", (int)pid, out, len, sizeof(*kp));
    return 1;
}

static void take_over(void) {
    FILE *f = fopen(PID_FILE, "r");
    if (f) {
        int old = 0;
        while (fscanf(f, "%d", &old) == 1) {
            if (old <= 0 || old == (int)getpid()) continue;
            char comm[32] = {0};
            if (!proc_comm((pid_t)old, comm, sizeof(comm))) { say("previous daemon pid %d: not running", old); continue; }
            if (strstr(comm, "payload") || strstr(comm, "disc") || strstr(comm, "Disc")) {
                const int rc = kill((pid_t)old, SIGKILL);
                say("previous daemon pid %d (%s): SIGKILL rc=%d", old, comm, rc);
            } else {
                say("pid %d in " PID_FILE " is now '%s', not a daemon: left alone", old, comm);
            }
        }
        fclose(f);
    }
    f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
}

/* Another daemon has taken over when the pid file no longer names us. */
static int superseded(void) {
    FILE *f = fopen(PID_FILE, "r");
    if (!f) return 0;
    int p = 0;
    const int got = fscanf(f, "%d", &p);
    fclose(f);
    return got == 1 && p > 0 && p != (int)getpid();
}

/* ------------------------------------------------------------------ */
/* Actions                                                               */
/* ------------------------------------------------------------------ */
/* vk-285-160j: start PS5SX2 the way its tile does (sceLncUtilLaunchApp). Never fork here: fork + LoadExec
 * from the child panicked the kernel (160i). */
static void launch_title(const char *why) {
    if (!p_launch) { say("%s: no sceLncUtilLaunchApp", why); return; }
    int user = -1;
    if (p_fguser) p_fguser(&user);
    lnc_app_param_t prm;
    memset(&prm, 0, sizeof(prm));
    prm.size = sizeof(prm);
    prm.user_id = user;
    const int rc = p_launch(TITLE_ID, NULL, &prm);
    say("%s: sceLncUtilLaunchApp(" TITLE_ID ", user %d) = 0x%08x", why, user, (unsigned)rc);
}

static time_t g_last_start;
static time_t g_focus_armed;   /* when; 0 = no focus watch */
static time_t g_focus_seen;    /* exec signal consumed (or cold launch); 0 = not yet */

static void launch(const char *serial, const char *iso) {
    const int alive = emu_alive();
    FILE *f = fopen(LAUNCH_TXT, "w");
    if (!f) { say("fopen(" LAUNCH_TXT "): errno %d", errno); notify("PS5SX2 disc: can't write disc-launch.txt"); return; }
    fprintf(f, "%s\n%s\n", iso, serial);
    fclose(f);
    say("launch %s: %s (PS5SX2 %s)", serial, iso, alive ? "running" : "not running");
    notify("PS5SX2: starting %s", serial);
    if (alive) {
        /* Its disc watcher sees the exec signal and re-execs into the game; then LaunchApp brings it to the
         * front (the re-exec keeps the shell's focus where it was). */
        f = fopen(EXEC_TXT, "w");
        if (f) fclose(f);
        else say("fopen(" EXEC_TXT "): errno %d", errno);
        g_focus_armed = time(NULL);
        g_focus_seen = 0;
    } else {
        /* Not running: launch it; its first boot reads disc-launch.txt and starts the game. */
        unlink(EXEC_TXT);
        launch_title("start PS5SX2 into the game");
        g_last_start = time(NULL);
        g_focus_armed = time(NULL);
        g_focus_seen = time(NULL) + 3; /* LaunchApp once more at +6 s */
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
/* ------------------------------------------------------------------ */
/* USB DVD drives (live-2)                                               */
/* ------------------------------------------------------------------ */
/* swordpdf's Verbatim BD RW (PIONEER BDR-UD03) on the 4.03 phat: the kernel sees it (umass0, /dev/pass1, INQUIRY
 * type 5) but its CD device never attaches ("(cd1:umass-sim1...): got CAM status 0x50 ... failed to attach to
 * device", every plug). So PS5SX2's watcher (cdN only) never sees it. The daemon reads it itself through the pass
 * device with plain MMC commands: TEST UNIT READY to see a disc (no read), READ CAPACITY, READ(10) in 64 KiB
 * pieces, SET CD SPEED to the drive's maximum first (standard; a PC drive takes it, and no Sony vendor command
 * ever goes to it). The copy goes to games/<SERIAL>.iso(.part); then the usual launch takes over.
 * Needs proper testing on a console with a USB DVD drive. */
#define USB_CHUNK_SECTORS 32u
#define USB_MAX_BAD       20000u

static uint32_t g_raw_status, g_raw_resid; /* live-4: what the last command really returned */
static uint8_t g_raw_scsi;
static uint32_t g_raw_sense; /* live-12: key << 16 | ASC << 8 | ASCQ of the last failed command, 0 when none came */

static int scsi(int pass, const uint8_t *cdb, int cdb_len, uint32_t dir, uint8_t *data, uint32_t len, uint32_t timeout_ms,
                char *why, size_t whylen) {
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    cam_fill_csio(&ccb.csio, 1, NULL, dir | CAM_DEV_QFRZDIS, MSG_SIMPLE_Q_TAG, data, len, SSD_FULL_SIZE,
                  (uint8_t)cdb_len, timeout_ms);
    memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, (size_t)cdb_len);
    g_raw_status = 0xffffffffu; g_raw_scsi = 0xff; g_raw_resid = 0xffffffffu; g_raw_sense = 0;
    if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0) {
        if (why) snprintf(why, whylen, "errno %d", errno);
        return -1;
    }
    g_raw_status = ccb.ccb_h.status; g_raw_scsi = ccb.csio.scsi_status; g_raw_resid = ccb.csio.resid;
    if ((ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP) return 0;
    if (ccb.ccb_h.status & CAM_AUTOSNS_VALID) {
        const uint8_t *sd = (const uint8_t *)&ccb.csio.sense_data;
        g_raw_sense = (uint32_t)(sd[2] & 15) << 16 | (uint32_t)sd[12] << 8 | sd[13];
    }
    if (why) {
        const uint8_t *sd = (const uint8_t *)&ccb.csio.sense_data;
        snprintf(why, whylen, "sense %X/%02X/%02X (cam %#x)", sd[2] & 15, sd[12], sd[13], ccb.ccb_h.status & CAM_STATUS_MASK);
    }
    return -1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* An optical drive that isn't the PS5's own: its pass device path and INQUIRY text. */
static int usb_find(char *path, size_t pmax, char *what, size_t wmax) {
    DIR *d = opendir("/dev");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "pass", 4) != 0) continue;
        char p[64];
        snprintf(p, sizeof(p), "/dev/%s", e->d_name);
        const int fd = open(p, O_RDWR);
        if (fd < 0) continue;
        uint8_t inq[36] = {0};
        const uint8_t c[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
        if (scsi(fd, c, 6, CAM_DIR_IN, inq, sizeof(inq), 5000, NULL, 0) == 0 && (inq[0] & 0x1f) == 5) {
            char w[64];
            snprintf(w, sizeof(w), "%.8s | %.16s | %.4s", inq + 8, inq + 16, inq + 32);
            if (!strstr(w, "PS-SYSTEM")) {
                snprintf(path, pmax, "%s", p);
                snprintf(what, wmax, "%s", w);
                found = 1;
            }
        }
        close(fd);
    }
    closedir(d);
    return found;
}

/* live-4: the device queue's freeze count (XPT_REL_SIMQ with CAM_DEV_QFREEZE only reads it), and releasing
 * exactly that many. The cd driver's failed attach (CAM status 0x50 = AUTOSENSE_FAIL | DEV_QFRZN) may have left the
 * queue frozen. Never releases more than the count it read. -1 when the query isn't taken. */
static int usb_qfrozen(int pass) {
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_REL_SIMQ;
    ccb.ccb_h.flags = CAM_DEV_QFREEZE; /* count only */
    if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0) { say("usb: queue count: errno %d", errno); return -1; }
    return (int)ccb.crs.qfrozen_cnt;
}
static void usb_release(int pass) {
    const int n = usb_qfrozen(pass);
    say("usb: device queue frozen count %d", n);
    for (int i = 0; i < n && i < 8; i++) {
        union ccb ccb;
        memset(&ccb, 0, sizeof(ccb));
        ccb.ccb_h.func_code = XPT_REL_SIMQ;
        ccb.ccb_h.flags = 0;
        ccb.crs.release_flags = 0;
        const int rc = ioctl(pass, CAMIOCOMMAND, &ccb);
        say("usb: release %d: rc %d status %#x, count now %d", i + 1, rc, ccb.ccb_h.status, usb_qfrozen(pass));
    }
}

/* live-5: which standard read commands give this drive's data through the PS5 (live-4: READ(10) and READ CAPACITY
 * "complete" with resid = everything, while GET CONFIGURATION and INQUIRY return data). Read-only MMC/SBC commands,
 * once per disc. Returns the opcode that read sector 16 with data, or 0. */
static uint8_t usb_probe_reads(int pass) {
    struct { const char *name; uint8_t cdb[16]; int len; uint32_t bytes; int is_s16; } t[] = {
        {"READ(10) s16",           {0x28, 0, 0, 0, 0, 16, 0, 0, 1, 0}, 10, 2048, 1},
        {"READ(12) s16",           {0xA8, 0, 0, 0, 0, 16, 0, 0, 0, 1, 0, 0}, 12, 2048, 1},
        {"READ(16) s16",           {0x88, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 1, 0, 0}, 16, 2048, 1},
        {"READ CD s16 user data",  {0xBE, 0x00, 0, 0, 0, 16, 0, 0, 1, 0x10, 0, 0}, 12, 2048, 1},
        {"READ(6) s16",            {0x08, 0, 0, 16, 1, 0}, 6, 2048, 1},
        {"READ CAPACITY(10)",      {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 10, 8, 0},
        {"READ CAPACITY(16)",      {0x9E, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0}, 16, 32, 0},
        {"READ TOC",               {0x43, 0, 0, 0, 0, 0, 0, 0, 12, 0}, 10, 12, 0},
        {"READ DISC INFORMATION",  {0x51, 0, 0, 0, 0, 0, 0, 0, 34, 0}, 10, 34, 0},
        {"READ DISC STRUCTURE 0",  {0xAD, 0, 0, 0, 0, 0, 0, 0, 0, 24, 0, 0}, 12, 24, 0},
        {"MODE SENSE(10) 2A",      {0x5A, 0x08, 0x2A, 0, 0, 0, 0, 0, 64, 0}, 10, 64, 0},
        {"REQUEST SENSE",          {0x03, 0, 0, 0, 18, 0}, 6, 18, 0},
    };
    uint8_t op = 0;
    static uint8_t buf[2048];
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        memset(buf, 0, sizeof(buf));
        char w[64] = "";
        const int r = scsi(pass, t[i].cdb, t[i].len, CAM_DIR_IN, buf, t[i].bytes, 15000, w, sizeof(w));
        const uint32_t got = g_raw_resid <= t[i].bytes ? t[i].bytes - g_raw_resid : 0;
        int nz = 0;
        for (uint32_t k = 0; k < t[i].bytes; k++) nz += buf[k] != 0;
        say("usb probe: %-22s rc %d %s ccb %#x scsi %#x got %u/%u bytes, %d non-zero; %02x %02x %02x %02x %02x %02x %02x %02x",
            t[i].name, r, w, g_raw_status, g_raw_scsi, got, t[i].bytes, nz, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
        if (t[i].is_s16 && r == 0 && buf[0] == 1 && memcmp(buf + 1, "CD001", 5) == 0 && !op) op = t[i].cdb[0];
    }
    say("usb probe: sector 16 readable with %s", op ? "the opcode above" : "none of these");
    return op;
}

static int usb_ready(int pass) {
    const uint8_t c[6] = {0x00, 0, 0, 0, 0, 0};
    return scsi(pass, c, 6, CAM_DIR_NONE, NULL, 0, 5000, NULL, 0) == 0;
}

/* Sectors on the disc (2048-byte blocks), 0 if unknown. */
static uint32_t usb_capacity(int pass) {
    uint8_t r[8] = {0};
    const uint8_t c[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    char why[64];
    if (scsi(pass, c, 10, CAM_DIR_IN, r, sizeof(r), 10000, why, sizeof(why)) != 0) {
        say("usb: READ CAPACITY %s", why);
        return 0;
    }
    if (be32(r + 4) != 2048) { say("usb: block length %u, not 2048", be32(r + 4)); return 0; }
    return be32(r) + 1;
}

static int usb_read(int pass, uint32_t lba, uint32_t count, uint8_t *buf, char *why, size_t wmax) {
    static int use12 = 0; /* READ(12) once READ(10) has been refused as a command */
    if (!use12) {
        const uint8_t c[10] = {0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                               0, (uint8_t)(count >> 8), (uint8_t)count, 0};
        char w[64] = "";
        if (scsi(pass, c, 10, CAM_DIR_IN, buf, count * 2048u, 30000, w, sizeof(w)) == 0) return 1;
        if (why) snprintf(why, wmax, "READ(10) %s", w);
        if (!strstr(w, "5/20/")) return 0; /* not "invalid command": a real read error */
        say("usb: READ(10) refused (%s): using READ(12)", w);
        use12 = 1;
    }
    const uint8_t c12[12] = {0xA8, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                             0, 0, (uint8_t)(count >> 8), (uint8_t)count, 0, 0};
    char w[64] = "";
    if (scsi(pass, c12, 12, CAM_DIR_IN, buf, count * 2048u, 30000, w, sizeof(w)) == 0) return 1;
    if (why) snprintf(why, wmax, "READ(12) %s", w);
    return 0;
}

static int usb_sector(void *ctx, uint32_t lba, uint8_t *out) { return usb_read(*(int *)ctx, lba, 1, out, NULL, 0); }

static double mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Copies the disc in the USB drive to games/<serial>.iso. 1 when the .iso is there in full (bad sectors, if any,
 * written as zeros and marked with <iso>.incomplete), 0 when it stopped. */
static int usb_dump(int pass, const char *serial, uint32_t sectors) {
    char iso[512], part[520];
    snprintf(iso, sizeof(iso), GAMES_DIR "/%s.iso", serial);
    snprintf(part, sizeof(part), "%s.part", iso);
    mkdir(GAMES_DIR, 0777);

    char why[64];
    uint8_t perf[40] = {0};
    const uint8_t c_perf[12] = {0xAC, 0x10, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0};
    if (scsi(pass, c_perf, 12, CAM_DIR_IN, perf, sizeof(perf), 10000, why, sizeof(why)) == 0)
        say("usb: GET PERFORMANCE %u..%u kB/s", be32(perf + 12), be32(perf + 20));
    const uint8_t c_bb[12] = {0xBB, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    if (scsi(pass, c_bb, 12, CAM_DIR_NONE, NULL, 0, 10000, why, sizeof(why)) == 0) say("usb: SET CD SPEED max: ok");
    else say("usb: SET CD SPEED max: %s", why);

    const int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { say("usb: open %s: errno %d", part, errno); notify("USB disc: can't write to " GAMES_DIR); return 0; }
    static uint8_t buf[USB_CHUNK_SECTORS * 2048u];
    const double t0 = mono_s();
    double tlast = t0;
    uint32_t lba_last = 0, bad = 0, next_pct = 5;
    const uint64_t total = (uint64_t)sectors * 2048u;
    notify("USB disc: copying %s (%.1f GB)", serial, total / 1e9);
    say("usb: copying %s: %u sectors (%llu bytes) to %s", serial, sectors, (unsigned long long)total, part);

    for (uint32_t lba = 0; lba < sectors;) {
        const uint32_t n = sectors - lba < USB_CHUNK_SECTORS ? sectors - lba : USB_CHUNK_SECTORS;
        int ok = 0;
        for (int tries = 0; tries < 3 && !ok; tries++) ok = usb_read(pass, lba, n, buf, why, sizeof(why));
        if (!ok) {
            if (!usb_ready(pass)) { say("usb: drive not ready at sector %u (%s): disc out? stopping", lba, why); close(out); unlink(part); return 0; }
            /* sector by sector; what stays unreadable becomes zeros */
            for (uint32_t i = 0; i < n; i++) {
                int one = 0;
                for (int tries = 0; tries < 2 && !one; tries++) one = usb_read(pass, lba + i, 1, buf + i * 2048u, NULL, 0);
                if (!one) { memset(buf + i * 2048u, 0, 2048); bad++; }
            }
            say("usb: sectors %u..%u: %s; read one by one, %u unreadable so far", lba, lba + n - 1, why, bad);
            if (bad > USB_MAX_BAD) { say("usb: too many unreadable sectors: stopping"); notify("USB disc: too many read errors, stopped"); close(out); unlink(part); return 0; }
        }
        const ssize_t w = pwrite(out, buf, (size_t)n * 2048u, (off_t)lba * 2048);
        if (w != (ssize_t)n * 2048) { say("usb: write errno %d at sector %u", errno, lba); notify("USB disc: write failed (disk full?)"); close(out); unlink(part); return 0; }
        lba += n;
        const uint32_t pct = (uint32_t)((uint64_t)lba * 100 / sectors);
        if (pct >= next_pct || lba == sectors) {
            const double t = mono_s();
            const double now_mb = (double)(lba - lba_last) * 2048 / (t - tlast > 0.001 ? t - tlast : 0.001) / 1e6;
            const double avg_mb = (double)lba * 2048 / (t - t0 > 0.001 ? t - t0 : 0.001) / 1e6;
            say("usb: %u%% (%llu MB, %.1f MB/s = %.1fx DVD now, %.1fx average; %u unreadable)", pct,
                (unsigned long long)((uint64_t)lba * 2048 / 1000000), now_mb, now_mb / 1.385, avg_mb / 1.385, bad);
            notify("USB disc %s: %u%% | %.1f MB/s (%.1fx)", serial, pct, now_mb, now_mb / 1.385);
            tlast = t; lba_last = lba;
            while (next_pct <= pct) next_pct += 5;
            stamp_self();
        }
    }
    fsync(out);
    close(out);
    if (rename(part, iso) != 0) { say("usb: rename errno %d", errno); return 0; }
    char inc[540];
    snprintf(inc, sizeof(inc), "%s.incomplete", iso);
    if (bad) { FILE *f = fopen(inc, "w"); if (f) { fprintf(f, "%u unreadable sectors\n", bad); fclose(f); } }
    else unlink(inc);
    const double secs = mono_s() - t0;
    say("usb: %s copied in %.0f s (%.1f MB/s = %.1fx DVD average), %u unreadable sectors", iso, secs, total / secs / 1e6,
        total / secs / 1e6 / 1.385, bad);
    return 1;
}

/* ------------------------------------------------------------------ */
/* live-12: the PS5's own drive through its pass device: Sony's disc    */
/* profiles, and PS2 CDs read raw                                       */
/* ------------------------------------------------------------------ */
enum {
    PROFILE_PS1_CD = 0xFF50, PROFILE_PS2_CD = 0xFF60, PROFILE_PS2_DVD = 0xFF61,
    PROFILE_PS3_DVD = 0xFF70, PROFILE_PS3_BD = 0xFF71, PROFILE_PS4_BD = 0xFF80, PROFILE_PS5_BD = 0xFF90,
    PROFILE_NONE = -1,     /* sense 02/3A: no disc */
    PROFILE_UNKNOWN = -2,  /* not ready, or the command failed */
};
#define CD_RAW        2352u
#define CD_CHUNK      16u      /* sectors a READ CD asks for (37,632 bytes) */
#define CD_MAX_BAD    2000u
#define CD_MAX_TRACKS 99

/* live-14: on the PS5 the drive reports the standard MMC profile for a PS disc (live-12 on swordpdf's phat: a PS2 DVD,
 * SLES-53415, came back as 0x0010 DVD-ROM, not Sony's 0xFF61, which ShellCore keeps for itself), so any CD profile is a
 * candidate; a PS2 CD is one whose SYSTEM.CNF has BOOT2 (read_serial_with), which a PS1 CD's (BOOT) doesn't. */
static int is_cd_profile(int p) { return p == 0x0008 || p == 0x0009 || p == 0x000A || p == PROFILE_PS1_CD || p == PROFILE_PS2_CD; }

static const char *profile_name(int p) {
    switch (p) {
    case PROFILE_PS1_CD: return "PS1 CD";
    case PROFILE_PS2_CD: return "PS2 CD";
    case PROFILE_PS2_DVD: return "PS2 DVD";
    case PROFILE_PS3_DVD: return "PS3 DVD";
    case PROFILE_PS3_BD: return "PS3 BD";
    case PROFILE_PS4_BD: return "PS4 BD";
    case PROFILE_PS5_BD: return "PS5 BD";
    case 0x0008: return "CD-ROM";
    case 0x0009: return "CD-R";
    case 0x000A: return "CD-RW";
    case 0x0010: return "DVD-ROM";
    case 0x0040: return "BD-ROM";
    case 0x0000: return "no current profile";
    case PROFILE_NONE: return "no disc";
    case 0xFFFF: return "incompatible disc";
    default: return "other";
    }
}

/* The internal drive's pass device: the pass node whose INQUIRY says PS-SYSTEM (SONY PS-SYSTEM 503R on swordpdf's
 * phat). Opened once and kept. */
static int g_drive = -1;
static int drive_pass(void) {
    if (g_drive >= 0) return g_drive;
    DIR *d = opendir("/dev");
    if (!d) return -1;
    struct dirent *e;
    while (g_drive < 0 && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "pass", 4) != 0) continue;
        char p[64];
        snprintf(p, sizeof(p), "/dev/%s", e->d_name);
        const int fd = open(p, O_RDWR);
        if (fd < 0) continue;
        uint8_t inq[36] = {0};
        const uint8_t c[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
        char id[25] = {0}; /* vendor + product, as text (no memmem in the payload's libc) */
        if (scsi(fd, c, 6, CAM_DIR_IN, inq, sizeof(inq), 5000, NULL, 0) == 0 && (inq[0] & 0x1f) == 5 &&
            (memcpy(id, inq + 8, 24), strstr(id, "PS-SYSTEM") != NULL)) {
            g_drive = fd;
            say("drive: %s is the PS5's own drive (%.8s %.16s %.4s)", p, inq + 8, inq + 16, inq + 32);
        } else {
            close(fd);
        }
    }
    closedir(d);
    return g_drive;
}

/* GET CONFIGURATION's current profile. A PS disc reports Sony's own profile (0xFF50 ... 0xFF90). */
static int drive_profile(void) {
    const int pass = drive_pass();
    if (pass < 0) return PROFILE_UNKNOWN;
    uint8_t h[8] = {0};
    const uint8_t c[10] = {0x46, 0x00, 0, 0, 0, 0, 0, 0, sizeof(h), 0};
    if (scsi(pass, c, 10, CAM_DIR_IN, h, sizeof(h), 10000, NULL, 0) != 0)
        return (g_raw_sense & 0xFFFF00u) == 0x023A00u ? PROFILE_NONE : PROFILE_UNKNOWN;
    return h[6] << 8 | h[7];
}

/* READ CD: raw (sync, header, data, EDC/ECC: 2352 bytes) for data tracks, the 2352 samples for audio, or the user
 * data only (2048 bytes of a mode 1 or mode 2 form 1 sector). */
enum { CD_USER, CD_RAW_DATA, CD_RAW_AUDIO };
static int cd_read(int pass, uint32_t lba, uint32_t count, int how, uint8_t *buf, char *why, size_t wmax) {
    const uint8_t c[12] = {0xBE, 0x00, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                           (uint8_t)(count >> 16), (uint8_t)(count >> 8), (uint8_t)count,
                           (uint8_t)(how == CD_RAW_DATA ? 0xF8 : 0x10), 0, 0};
    const uint32_t each = how == CD_USER ? 2048u : CD_RAW;
    return scsi(pass, c, 12, CAM_DIR_IN, buf, count * each, 30000, why, wmax) == 0;
}
static int cd_user_sector(void *ctx, uint32_t lba, uint8_t *out) { return cd_read(*(int *)ctx, lba, 1, CD_USER, out, NULL, 0); }

typedef struct { uint8_t number, data, mode; uint32_t lba; } cd_track;
typedef struct { cd_track t[CD_MAX_TRACKS]; unsigned count; uint32_t leadout; } cd_toc;

/* READ TOC/PMA/ATIP, format 0, LBA addresses. */
static int cd_read_toc(int pass, cd_toc *toc, char *why, size_t wmax) {
    static uint8_t d[2048];
    const uint8_t c[10] = {0x43, 0x00, 0, 0, 0, 0, 0, (uint8_t)(sizeof(d) >> 8), (uint8_t)sizeof(d), 0};
    memset(toc, 0, sizeof(*toc));
    memset(d, 0, sizeof(d));
    if (scsi(pass, c, 10, CAM_DIR_IN, d, sizeof(d), 10000, why, wmax) != 0) return 0;
    unsigned total = ((unsigned)d[0] << 8 | d[1]) + 2;
    if (total > sizeof(d)) total = sizeof(d);
    int leadout = 0;
    for (unsigned i = 4; i + 8 <= total; i += 8) {
        const uint8_t num = d[i + 2];
        const uint32_t lba = be32(d + i + 4);
        if (num == 0xAA) { toc->leadout = lba; leadout = 1; break; }
        if (num == 0 || toc->count >= CD_MAX_TRACKS) continue;
        cd_track *t = &toc->t[toc->count++];
        t->number = num; t->lba = lba; t->data = (d[i + 1] & 0x04) != 0; t->mode = 2;
    }
    if (!leadout || !toc->count) { if (why) snprintf(why, wmax, "no lead-out or no track in the TOC"); return 0; }
    for (unsigned i = 0; i < toc->count; i++) {
        const uint32_t next = i + 1 < toc->count ? toc->t[i + 1].lba : toc->leadout;
        if (next <= toc->t[i].lba) { if (why) snprintf(why, wmax, "track %u ends before it starts", toc->t[i].number); return 0; }
    }
    return 1;
}

/* Clean a game's name for a file name the way PS5SX2's dumper does (": " -> " - ", no / \ : * ? " < > |). */
static void clean_name(const char *in, char *out, size_t max) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < max; i++) {
        if (in[i] == ':' && in[i + 1] == ' ') { out[o++] = ' '; out[o++] = '-'; continue; }
        const char ch = in[i];
        out[o++] = (strchr("/\\:*?\"<>|", ch) || (unsigned char)ch < 0x20) ? ' ' : ch;
    }
    out[o] = '\0';
    /* squeeze double spaces, trim trailing spaces and dots */
    size_t w = 0;
    for (size_t r = 0; out[r]; r++)
        if (!(out[r] == ' ' && w > 0 && out[w - 1] == ' ')) out[w++] = out[r];
    while (w > 0 && (out[w - 1] == ' ' || out[w - 1] == '.')) w--;
    out[w] = '\0';
}

/* The game's name from PCSX2's GameIndex.yaml (name-en when it has one), for the dump's file name. */
static int gameindex_title(const char *serial, char *out, size_t max) {
    FILE *f = fopen("/data/PCSX2/resources/GameIndex.yaml", "r");
    if (!f) return 0;
    static char line[4096];
    char name[256] = "", name_en[256] = "";
    const size_t sl = strlen(serial);
    int in = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] != ' ' && line[0] != '\n' && line[0] != '#') {
            if (in) break;
            in = strncasecmp(line, serial, sl) == 0 && line[sl] == ':';
            continue;
        }
        if (!in) continue;
        char *v = NULL, *dst = NULL;
        if (strncmp(line, "  name: ", 8) == 0) { v = line + 8; dst = name; }
        else if (strncmp(line, "  name-en: ", 11) == 0) { v = line + 11; dst = name_en; }
        if (!v) continue;
        while (*v == ' ') v++;
        if (*v == '"') v++;
        size_t n = strcspn(v, "\"\r\n");
        if (n > 255) n = 255;
        memcpy(dst, v, n);
        dst[n] = '\0';
    }
    fclose(f);
    const char *t = name_en[0] ? name_en : name;
    if (!t[0]) return 0;
    clean_name(t, out, max);
    return out[0] != '\0';
}

/* Copies the PS2 CD in the drive to games/<stem>.bin (+ .cue, + .hashes.txt). 1 when the .bin is there in full
 * (unreadable sectors, if any, written as zeros and marked with <bin>.incomplete), 0 when it stopped. */
static int cd_dump(int pass, const char *serial) {
    char why[96] = "";
    cd_toc toc;
    if (!cd_read_toc(pass, &toc, why, sizeof(why))) { say("cd: READ TOC failed: %s", why); notify("PS2 CD %s: can't read its table of contents", serial); return 0; }
    /* each data track's mode from its first sector's header */
    for (unsigned i = 0; i < toc.count; i++) {
        static uint8_t s1[CD_RAW];
        if (toc.t[i].data && cd_read(pass, toc.t[i].lba, 1, CD_RAW_DATA, s1, NULL, 0) && memcmp(s1, k_cd_sync, 12) == 0)
            toc.t[i].mode = s1[15] == 1 ? 1 : 2;
        say("cd: track %u at %u, %s", toc.t[i].number, toc.t[i].lba, toc.t[i].data ? (toc.t[i].mode == 1 ? "data, mode 1" : "data, mode 2") : "audio");
    }
    const uint32_t first = toc.t[0].lba, sectors = toc.leadout - first;
    const uint64_t total = (uint64_t)sectors * CD_RAW;

    char title[200] = "", stem[300], bin[512], part[520], cue[512], sums[512];
    if (gameindex_title(serial, title, sizeof(title))) snprintf(stem, sizeof(stem), "%s (%s)", title, serial);
    else snprintf(stem, sizeof(stem), "%s", serial);
    snprintf(bin, sizeof(bin), GAMES_DIR "/%s.bin", stem);
    snprintf(part, sizeof(part), "%s.part", bin);
    snprintf(cue, sizeof(cue), GAMES_DIR "/%s.cue", stem);
    snprintf(sums, sizeof(sums), GAMES_DIR "/%s.hashes.txt", stem);
    mkdir(GAMES_DIR, 0777);

    /* the standard speed request (SET CD SPEED to the drive's maximum); logged whether it takes it */
    const uint8_t c_bb[12] = {0xBB, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    if (scsi(pass, c_bb, 12, CAM_DIR_NONE, NULL, 0, 10000, why, sizeof(why)) == 0) say("cd: SET CD SPEED max: ok");
    else say("cd: SET CD SPEED max: %s", why);

    const int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { say("cd: open %s: errno %d", part, errno); notify("PS2 CD: can't write to " GAMES_DIR); return 0; }
    static uint8_t buf[CD_CHUNK * CD_RAW];
    disc_hash_ctx hc;
    disc_hash_init(&hc);
    const double t0 = mono_s();
    double tlast = t0;
    uint32_t done = 0, done_last = 0, bad = 0, next_pct = 5;
    notify("PS2 CD: copying %s (%u MB)", serial, (unsigned)(total / 1000000));
    say("cd: copying %s: %u sectors from %u (%llu bytes) to %s", serial, sectors, first, (unsigned long long)total, part);

    for (unsigned ti = 0; ti < toc.count; ti++) {
        const uint32_t end = ti + 1 < toc.count ? toc.t[ti + 1].lba : toc.leadout;
        const int how = toc.t[ti].data ? CD_RAW_DATA : CD_RAW_AUDIO;
        for (uint32_t lba = toc.t[ti].lba; lba < end;) {
            const uint32_t n = end - lba < CD_CHUNK ? end - lba : CD_CHUNK;
            int ok = 0;
            for (int tries = 0; tries < 3 && !ok; tries++) ok = cd_read(pass, lba, n, how, buf, why, sizeof(why));
            if (!ok) {
                if (!is_cd_profile(drive_profile())) { say("cd: the disc went at sector %u (%s): stopping", lba, why); close(out); unlink(part); return 0; }
                for (uint32_t i = 0; i < n; i++) {
                    int one = 0;
                    for (int tries = 0; tries < 2 && !one; tries++) one = cd_read(pass, lba + i, 1, how, buf + i * CD_RAW, NULL, 0);
                    if (!one) { memset(buf + i * CD_RAW, 0, CD_RAW); bad++; }
                }
                say("cd: sectors %u..%u: %s; read one by one, %u unreadable so far", lba, lba + n - 1, why, bad);
                if (bad > CD_MAX_BAD) { say("cd: too many unreadable sectors: stopping"); notify("PS2 CD: too many read errors, stopped"); close(out); unlink(part); return 0; }
            }
            const size_t bytes = (size_t)n * CD_RAW;
            if (pwrite(out, buf, bytes, (off_t)done * CD_RAW) != (ssize_t)bytes) {
                say("cd: write errno %d at sector %u", errno, lba); notify("PS2 CD: write failed (disk full?)"); close(out); unlink(part); return 0;
            }
            disc_hash_update(&hc, buf, bytes);
            lba += n;
            done += n;
            const uint32_t pct = (uint32_t)((uint64_t)done * 100 / sectors);
            if (pct >= next_pct || done == sectors) {
                const double t = mono_s();
                const double now_kb = (double)(done - done_last) * CD_RAW / (t - tlast > 0.001 ? t - tlast : 0.001) / 1000.0;
                say("cd: %u%% (%llu MB, %.0f kB/s = %.1fx CD; %u unreadable)", pct, (unsigned long long)((uint64_t)done * CD_RAW / 1000000),
                    now_kb, now_kb / 176.4, bad);
                notify("PS2 CD %s: %u%% | %.1fx", serial, pct, now_kb / 176.4);
                tlast = t; done_last = done;
                while (next_pct <= pct) next_pct += 5;
                stamp_self();
            }
        }
    }
    fsync(out);
    close(out);
    if (rename(part, bin) != 0) { say("cd: rename errno %d", errno); return 0; }

    /* the cue sheet: one .bin, each track's start relative to the first track (the .bin's start) */
    FILE *cf = fopen(cue, "w");
    if (cf) {
        fprintf(cf, "FILE \"%s.bin\" BINARY\n", stem);
        for (unsigned i = 0; i < toc.count; i++) {
            const uint32_t rel = toc.t[i].lba - first;
            if (toc.t[i].data) fprintf(cf, "  TRACK %02u MODE%u/2352\n", toc.t[i].number, toc.t[i].mode);
            else fprintf(cf, "  TRACK %02u AUDIO\n", toc.t[i].number);
            fprintf(cf, "    INDEX 01 %02u:%02u:%02u\n", rel / 4500, (rel / 75) % 60, rel % 75);
        }
        fclose(cf);
    } else {
        say("cd: can't write %s: errno %d", cue, errno);
    }

    char crc[9], md5[33], sha1[41];
    disc_hash_final(&hc, crc, md5, sha1);
    FILE *hf = fopen(sums, "w");
    if (hf) {
        fprintf(hf, "%s.bin\nsize %llu\ncrc32 %s\nmd5 %s\nsha1 %s\nsectors %u, tracks %u, unreadable %u\n", stem, (unsigned long long)total, crc, md5,
                sha1, sectors, toc.count, bad);
        fclose(hf);
    }
    char inc[540];
    snprintf(inc, sizeof(inc), "%s.incomplete", bin);
    if (bad) { FILE *f = fopen(inc, "w"); if (f) { fprintf(f, "%u unreadable sectors\n", bad); fclose(f); } }
    else unlink(inc);
    const double secs = mono_s() - t0;
    say("cd: %s copied in %.0f s (%.0f kB/s = %.1fx CD), %u unreadable; crc32 %s md5 %s sha1 %s", bin, secs, total / secs / 1000.0,
        total / secs / 1000.0 / 176.4, bad, crc, md5, sha1);
    return 1;
}

enum state { S_NONE, S_SETTLE, S_DUMPING, S_DONE };

int main(void) {
    say("disc-auto daemon (live-14) pid %d", (int)getpid());

    /* /dev/cd0 and notifications whatever uid the loader gave us (160b). */
    {
        uint8_t all[16];
        memset(all, 0xff, sizeof(all));
        kernel_set_ucred_authid(getpid(), 0x4800000000010003ull);
        kernel_set_ucred_caps(getpid(), all);
    }
    signal(SIGCHLD, SIG_IGN);
    take_over();
    load_lnc();

    notify("PS5SX2 disc-auto running: put a PS2 DVD in");
    say("PS5SX2 %s", emu_alive() ? "is running" : "is not running");

    char serial[17] = {0};
    off_t media = 0;
    time_t seen = 0;
    enum state st = S_NONE;
    int startup = 1;

    int focus_done = 0;

    /* live-12: a PS2 CD (dumped here, not by PS5SX2), seen through /dev/cd0 or only through the pass device */
    int is_cd = 0, cd_only = 0, cd_serial_tries = 0, last_prof = PROFILE_UNKNOWN;
    time_t prof_poll = 0;

    /* live-2: USB drive state */
    char usb_path[64] = {0}, usb_what[64] = {0}, usb_serial[17] = {0};
    int usb_fd = -1, usb_done = 0, usb_tries = 0;
    time_t usb_scan = 0, usb_seen = 0, usb_poll = 0;

    for (;;) {
        if (superseded()) {
            say("another daemon took over (" PID_FILE "): exiting");
            return 0;
        }
        stamp_self();

        /* vk-285-160g: PS5SX2 consumes disc-launch-exec.txt right before it re-execs (keeping its app id).
         * From then, hand the running PS5SX2 focus at +3/+8 s. */
        if (g_focus_armed) {
            const time_t now = time(NULL);
            struct stat est;
            if (!g_focus_seen) {
                if (stat(EXEC_TXT, &est) != 0) {
                    g_focus_seen = now;
                    focus_done = 0;
                    say("exec signal consumed: PS5SX2 is re-executing");
                } else if (now - g_focus_armed > 90) {
                    say("focus: exec signal still not consumed after 90 s");
                    g_focus_armed = 0;
                }
            } else {
                static const int at[1] = { 3 };
                if (focus_done < 1 && now - g_focus_seen >= at[focus_done]) {
                    const uint32_t id = emu_appid();
                    if (id) give_focus(id, focus_done + 1);
                    else say("focus #%d: PS5SX2 not the running big app yet", focus_done + 1);
                    focus_done++;
                }
                if (focus_done >= 1) { g_focus_armed = 0; g_focus_seen = 0; }
            }
        }

        /* Is a disc in, and is it the same one? DIOCGMEDIASIZE doesn't touch the disc, so a
         * dump in progress isn't slowed by us re-reading its directory every loop. */
        off_t size = 0;
        const int fd = open("/dev/cd0", O_RDONLY);
        if (fd >= 0 && ioctl(fd, DIOCGMEDIASIZE, &size) != 0)
            size = 0;

        if (fd < 0 || size <= 0) {
            if (fd >= 0) close(fd);
            if (cd_only) {
                /* live-12: a PS2 CD /dev/cd0 doesn't show: out when the drive's profile says so */
                if (time(NULL) - prof_poll >= 3) {
                    prof_poll = time(NULL);
                    const int p = drive_profile();
                    if (!is_cd_profile(p) && p != PROFILE_UNKNOWN) {
                        cleanup(serial);
                        serial[0] = '\0';
                        cd_only = is_cd = 0;
                        st = S_NONE;
                        last_prof = p;
                    }
                }
            } else {
                if (serial[0]) cleanup(serial);
                serial[0] = '\0';
                media = 0;
                st = S_NONE;
                is_cd = 0;
                /* live-12: no media on /dev/cd0; the drive's own profile every 3 s (GET CONFIGURATION doesn't spin the
                 * disc), for a disc cd0 doesn't give: a PS2 CD is read through the pass device */
                if (time(NULL) - prof_poll >= 3) {
                    prof_poll = time(NULL);
                    const int p = drive_profile();
                    if (p != last_prof) {
                        last_prof = p;
                        cd_serial_tries = 0;
                        if (p != PROFILE_NONE && p != PROFILE_UNKNOWN)
                            say("disc profile %#06x (%s), /dev/cd0 without media", (unsigned)p, profile_name(p));
                        if (p == PROFILE_PS1_CD || p == PROFILE_PS3_DVD || p == PROFILE_PS3_BD)
                            say("%s: not handled yet (PS2 discs only for now)", profile_name(p));
                    }
                    if (is_cd_profile(p) && cd_serial_tries < 5) {
                        char s2[17] = {0};
                        int pass = drive_pass();
                        cd_serial_tries++;
                        if (pass >= 0 && read_serial_with(cd_user_sector, &pass, s2)) {
                            memcpy(serial, s2, sizeof(serial));
                            cd_only = is_cd = 1;
                            seen = time(NULL);
                            if (startup) seen -= kSettleSec;
                            st = S_SETTLE;
                            say("PS2 CD %s (through the pass device)%s", serial, startup ? ", already in at start" : "");
                        } else {
                            say("PS2 CD: no PS2 SYSTEM.CNF read yet (try %d of 5)", cd_serial_tries);
                        }
                    }
                }
            }
        } else {
            if (size != media) {
                char s[17] = {0};
                const int ok = read_serial(fd, s);
                if (serial[0] && (!ok || strcmp(s, serial) != 0)) cleanup(serial);
                media = size;
                const int prof = drive_profile(); /* live-12: Sony's disc kind, logged */
                last_prof = prof;
                is_cd = cd_only = 0;
                if (ok) {
                    memcpy(serial, s, sizeof(serial));
                    seen = time(NULL);
                    st = S_SETTLE;
                    say("PS2 disc %s (%lld bytes, profile %#06x %s)%s", serial, (long long)size, (unsigned)prof, profile_name(prof),
                        startup ? ", already in at start" : "");
                    if (startup) seen -= kSettleSec; /* no shell takeover to wait out */
                } else if (is_cd_profile(prof)) {
                    /* live-12: /dev/cd0 gives no PS2 volume for a CD (mode 2 sectors); the pass device's READ CD does */
                    char s2[17] = {0};
                    int pass = drive_pass();
                    if (pass >= 0 && read_serial_with(cd_user_sector, &pass, s2)) {
                        memcpy(serial, s2, sizeof(serial));
                        is_cd = 1;
                        seen = time(NULL);
                        if (startup) seen -= kSettleSec;
                        st = S_SETTLE;
                        say("PS2 CD %s (%lld bytes on cd0, read through the pass device)%s", serial, (long long)size,
                            startup ? ", already in at start" : "");
                    } else {
                        serial[0] = '\0';
                        st = S_NONE;
                        say("PS2 CD in (%lld bytes) but no PS2 SYSTEM.CNF through the pass device either", (long long)size);
                    }
                } else {
                    serial[0] = '\0';
                    st = S_NONE;
                    say("disc in (%lld bytes, profile %#06x %s) but no PS2 SYSTEM.CNF: ignoring%s", (long long)size, (unsigned)prof,
                        profile_name(prof), (prof == PROFILE_PS1_CD || prof == PROFILE_PS3_DVD || prof == PROFILE_PS3_BD) ?
                        " (not handled yet: PS2 discs only for now)" : "");
                }
            }
            close(fd);
        }
        startup = 0;


        /* live-2: a USB DVD drive (pass device only, see usb_dump). Checked every 3 s with TEST UNIT READY. */
        if (time(NULL) - usb_poll >= 3) {
            usb_poll = time(NULL);
            if (usb_fd < 0 && time(NULL) - usb_scan >= 10) {
                usb_scan = time(NULL);
                if (usb_find(usb_path, sizeof(usb_path), usb_what, sizeof(usb_what))) {
                    usb_fd = open(usb_path, O_RDWR);
                    if (usb_fd >= 0) say("usb: optical drive %s: %s", usb_path, usb_what);
                }
            }
            if (usb_fd >= 0) {
                if (!usb_ready(usb_fd)) {
                    if (usb_serial[0]) { cleanup(usb_serial); usb_serial[0] = '\0'; }
                    usb_done = 0;
                    usb_tries = 0;
                    /* a drive that went away: look again later */
                    uint8_t inq[8];
                    const uint8_t c[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
                    if (scsi(usb_fd, c, 6, CAM_DIR_IN, inq, sizeof(inq), 3000, NULL, 0) != 0) {
                        say("usb: %s gone", usb_path);
                        close(usb_fd); usb_fd = -1;
                    }
                } else if (!usb_serial[0] && !usb_done) {
                    char s2[17] = {0};
                    if (read_serial_with(usb_sector, &usb_fd, s2)) {
                        memcpy(usb_serial, s2, sizeof(usb_serial));
                        usb_seen = time(NULL);
                        usb_tries = 0;
                        say("usb: PS2 disc %s in %s", usb_serial, usb_path);
                    } else {
                        /* say why: sector 16 straight, and what it holds */
                        uint8_t pvd[2048] = {0};
                        char why[80] = "";
                        if (usb_tries == 0) { usb_release(usb_fd); usb_probe_reads(usb_fd); }
                        const int ok16 = usb_read(usb_fd, 16, 1, pvd, why, sizeof(why));
                        say("usb: sector 16 raw: ccb status %#x, scsi status %#x, resid %u of 2048; bytes %02x %02x %02x %02x %02x %02x",
                            g_raw_status, g_raw_scsi, g_raw_resid, pvd[0], pvd[1], pvd[2], pvd[3], pvd[4], pvd[5]);
                        {
                            uint8_t rc8[8] = {0};
                            const uint8_t c[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                            char w2[64] = "";
                            const int r = scsi(usb_fd, c, 10, CAM_DIR_IN, rc8, sizeof(rc8), 10000, w2, sizeof(w2));
                            say("usb: READ CAPACITY rc %d %s raw: ccb status %#x, scsi %#x, resid %u of 8; %02x%02x%02x%02x %02x%02x%02x%02x",
                                r, w2, g_raw_status, g_raw_scsi, g_raw_resid, rc8[0], rc8[1], rc8[2], rc8[3], rc8[4], rc8[5], rc8[6], rc8[7]);
                            uint8_t gc[8] = {0};
                            const uint8_t cg[10] = {0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0};
                            const int r2 = scsi(usb_fd, cg, 10, CAM_DIR_IN, gc, sizeof(gc), 10000, w2, sizeof(w2));
                            say("usb: GET CONFIGURATION rc %d %s: profile %#06x, resid %u", r2, w2, gc[6] << 8 | gc[7], g_raw_resid);
                            uint8_t inq[36] = {0};
                            const uint8_t ci[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
                            const int r3 = scsi(usb_fd, ci, 6, CAM_DIR_IN, inq, sizeof(inq), 5000, w2, sizeof(w2));
                            say("usb: INQUIRY again rc %d: resid %u, '%.8s'", r3, g_raw_resid, inq + 8);
                        }
                        char label[33] = {0};
                        if (ok16) memcpy(label, pvd + 40, 32);
                        say("usb: no PS2 serial yet (try %d): sector 16 %s%s%.5s%s%s; capacity %u sectors", usb_tries + 1,
                            ok16 ? "read, id '" : "", ok16 ? "" : why, ok16 ? (const char *)pvd + 1 : "", ok16 ? "', label " : "",
                            ok16 ? label : "", usb_capacity(usb_fd));
                        if (++usb_tries >= 5) {
                            usb_done = 1;
                            say("usb: disc in %s: no PS2 SYSTEM.CNF after %d tries: ignoring until it's taken out", usb_path, usb_tries);
                        }
                    }
                }
                if (usb_serial[0] && !usb_done && time(NULL) - usb_seen >= kSettleSec) {
                    usb_done = 1;
                    char uiso[1024];
                    if (!find_iso(usb_serial, uiso, sizeof(uiso), 1)) {
                        const uint32_t sectors = usb_capacity(usb_fd);
                        if (sectors && usb_dump(usb_fd, usb_serial, sectors)) find_iso(usb_serial, uiso, sizeof(uiso), 0);
                        else uiso[0] = '\0';
                    }
                    if (uiso[0]) {
                        char inc[1100];
                        snprintf(inc, sizeof(inc), "%s.incomplete", uiso);
                        struct stat ist;
                        if (stat(inc, &ist) == 0) notify("PS5SX2: %s was dumped with read errors; pick it from the shelf to try it", usb_serial);
                        else launch(usb_serial, uiso);
                    }
                }
            }
        }

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
            } else if (is_cd) {
                /* live-12: a PS2 CD is dumped here (PS5SX2's dumper reads /dev/cd0, which doesn't give it) */
                const int pass = drive_pass();
                say("%s: no image in " GAMES_DIR " yet: dumping the CD here", serial);
                if (pass >= 0 && cd_dump(pass, serial) && find_iso(serial, iso, sizeof(iso), 0)) {
                    char inc[1100];
                    snprintf(inc, sizeof(inc), "%s.incomplete", iso);
                    struct stat ist;
                    if (stat(inc, &ist) == 0) notify("PS5SX2: %s was dumped with read errors; pick it from the shelf to try it", serial);
                    else launch(serial, iso);
                }
                st = S_DONE;
            } else {
                say("%s: no ISO in " GAMES_DIR " yet: PS5SX2 dumps it, then it starts", serial);
                notify("PS5SX2: dumping %s, it starts when the copy is done", serial);
                if (!emu_alive()) {
                    say("PS5SX2 not running: starting it to dump");
                    launch_title("start PS5SX2");
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
                launch_title("start PS5SX2");
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
