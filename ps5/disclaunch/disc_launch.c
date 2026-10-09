/* PS5SX2 disc-launch payload (vk-285-160b, AI-assisted):
 * Reads the PS2 serial from the disc in the drive, scans /data/PCSX2/games/
 * for a matching ISO, writes its path to /data/PCSX2/logs/disc-launch.txt,
 * and relaunches PS5SX2, which skips the shelf and starts that game directly.
 *
 * When the ISO is not found yet: notifies the user so they know to dump it
 * with PS5SX2 first. Send to the console's ELF loader (port 9021).
 *
 * vk-285-160b: sceSystemServiceLoadExec is found at runtime via dlopen so the
 * ELF has no hard libSceSystemService.sprx dependency (which the ELF loader
 * process may not have loaded). Privilege escalation via kernel_set_ucred_authid
 * ensures notifications and /dev/cd0 access work regardless of the loader's uid.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <dlfcn.h>
#include <dirent.h>
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
    klog_printf("[disc-launch] %s\n", buf);
    printf("[disc-launch] %s\n", buf);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* PS2 serial from a disc or ISO file                                    */
/* ------------------------------------------------------------------ */

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read exactly 2048 bytes at sector `lba` from `fd`.
 * For the drive (/dev/cd0) pread must be 2048-byte aligned and sized.
 * For a plain ISO file this is the same — always 2048-byte sectors. */
static int read_lba(int fd, uint32_t lba, uint8_t *out) {
    return pread(fd, out, 2048, (off_t)lba * 2048) == 2048;
}

/* Extract the PS2 serial from `fd` (which may be /dev/cd0 or a .iso file).
 * Fills `out` (17 bytes) with e.g. "SLUS-21351" and returns 1, or 0 on failure.
 * Normalization matches fe_games SerialFromDisc: '_' → '-', dots removed, uppercase. */
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
            /* match SYSTEM.CNF (with or without ";1" version) */
            if (nlen >= 10 && strncasecmp((char *)(dir + off + 33), "SYSTEM.CNF", 10) == 0) {
                const uint32_t clba = le32(dir + off + 2);
                uint32_t csz = le32(dir + off + 10);
                if (csz > 4096) csz = 4096;
                /* read in full 2048-byte sectors (drive requirement) */
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
                /* BOOT2 = cdrom0:\SLUS_213.51;1 */
                const char *b2 = strstr(cnf, "BOOT2");
                if (!b2) break;
                const char *bs = strchr(b2, '\\');
                if (!bs) break;
                bs++;
                /* copy raw name up to ';', newline, or end */
                char raw[17];
                int ri = 0;
                while (ri < 16 && bs[ri] && bs[ri] != ';' && bs[ri] != '\r' && bs[ri] != '\n' && bs[ri] != ' ') {
                    raw[ri] = bs[ri];
                    ri++;
                }
                raw[ri] = '\0';
                /* normalise: SLUS_213.51 → SLUS-21351 (match fe_games SerialFromDisc) */
                int ni = 0;
                for (int i = 0; raw[i] && ni < 16; i++) {
                    if (raw[i] == '_')      out[ni++] = '-';
                    else if (raw[i] == '.') /* skip dot */;
                    else                    out[ni++] = (char)toupper((unsigned char)raw[i]);
                }
                out[ni] = '\0';
                /* must be 9-12 characters (e.g. SLUS-21351 = 10, SCES-51235 = 9) */
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

/* Does the filename start with `serial` (case-insensitive) followed by
 * '.', '-', '_', or end? (Matches "SLUS-21351.Game Title.iso") */
static int name_starts_with_serial(const char *name, const char *serial) {
    const size_t slen = strlen(serial);
    if (strncasecmp(name, serial, slen) != 0) return 0;
    return name[slen] == '\0' || name[slen] == '.' || name[slen] == '-' || name[slen] == '_';
}

/* Scan `games_dir` for an .iso whose serial matches `serial`.
 * Pass 1: filename prefix (fast, covers all PS5SX2-dumped ISOs).
 * Pass 2: read SYSTEM.CNF from each remaining .iso (covers user-renamed files).
 * Fills `out_path` and returns 1 when found. */
static int find_iso(const char *games_dir, const char *serial, char *out_path, size_t max) {
    DIR *d = opendir(games_dir);
    if (!d) { say("opendir(%s) failed: errno %d", games_dir, errno); return 0; }

    /* pass 1: filename prefix */
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

    /* pass 2: SYSTEM.CNF for unmatched .iso files */
    rewinddir(d);
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        const size_t nlen = strlen(n);
        if (nlen < 5 || strcasecmp(n + nlen - 4, ".iso") != 0) continue;
        if (name_starts_with_serial(n, serial)) continue; /* already tried */
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
/* main                                                                  */
/* ------------------------------------------------------------------ */
int main(void) {
    say("disc-launch v1 (vk-285-160b)");

    /* 0. Privilege escalation: ensure we can open /dev/cd0, send notifications,
     *    and call sceSystemServiceLoadExec from any loader uid. */
    {
        const pid_t pid = getpid();
        uint8_t caps[16], all[16];
        memset(all, 0xff, sizeof(all));
        kernel_get_ucred_caps(pid, caps);
        kernel_set_ucred_authid(pid, 0x4800000000010003ull);
        kernel_set_ucred_caps(pid, all);
    }

    /* 1. Read the serial from the disc in the drive. */
    const int fd = open("/dev/cd0", O_RDONLY);
    if (fd < 0) {
        say("/dev/cd0: open errno %d (is a PS2 disc inserted?)", errno);
        notify("Disc launch: no disc detected (/dev/cd0 unavailable)");
        return 1;
    }
    char serial[17] = {};
    const int ok = read_serial(fd, serial);
    close(fd);
    if (!ok) {
        say("no PS2 serial on disc (not a PS2 DVD, or a CD game not supported by the drive)");
        notify("Disc launch: no PS2 serial found (must be a PS2 DVD)");
        return 1;
    }
    say("disc serial: %s", serial);

    /* 2. Find the matching ISO in /data/PCSX2/games/. */
    char iso[1024] = {};
    if (!find_iso("/data/PCSX2/games", serial, iso, sizeof(iso))) {
        say("no ISO found for %s in /data/PCSX2/games", serial);
        notify("Disc launch: no ISO for this game yet — let PS5SX2 dump it first");
        return 1;
    }
    say("ISO found: %s", iso);

    /* 3. Write disc-launch.txt for the eboot to pick up on its next start. */
    const char *const launch_txt = "/data/PCSX2/logs/disc-launch.txt";
    {
        FILE *f = fopen(launch_txt, "w");
        if (!f) {
            say("fopen(%s) failed: errno %d", launch_txt, errno);
            notify("Disc launch: could not write disc-launch.txt");
            return 1;
        }
        fprintf(f, "%s\n", iso);
        fclose(f);
        say("wrote %s", launch_txt);
    }

    /* 4. Relaunch PS5SX2. */
    {
        char msg[220];
        snprintf(msg, sizeof(msg), "PS5SX2: launching disc game (%s)...", serial);
        notify(msg);
    }

    const char *eboot = "/data/homebrew/PPSA99203/eboot.bin";
    {
        struct stat st;
        if (stat(eboot, &st) != 0) eboot = "/app0/eboot.bin";
    }
    say("sceSystemServiceLoadExec(%s)", eboot);
    fflush(stdout);

    /* Find sceSystemServiceLoadExec at runtime — the ELF loader process may not
     * have libSceSystemService.sprx loaded, so a hard NEEDED entry prevents this
     * ELF from loading at all.  dlopen avoids that dependency entirely. */
    void *lib = dlopen("libSceSystemService.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        say("dlopen(libSceSystemService.sprx) failed: %s", dlerror());
        notify("Disc launch: could not load libSceSystemService — relaunch PS5SX2 manually");
        return 1;
    }
    typedef int (*LoadExec_t)(const char *, const char **);
    LoadExec_t LoadExec = (LoadExec_t)(uintptr_t)dlsym(lib, "sceSystemServiceLoadExec");
    if (!LoadExec) {
        say("dlsym(sceSystemServiceLoadExec) failed: %s", dlerror());
        notify("Disc launch: could not find sceSystemServiceLoadExec — relaunch PS5SX2 manually");
        return 1;
    }

    const int rc = LoadExec(eboot, NULL);
    say("LoadExec returned 0x%08x%s", (unsigned)rc, rc == 0 ? " (restarting)" : " (failed)");
    return rc == 0 ? 0 : 1;
}
