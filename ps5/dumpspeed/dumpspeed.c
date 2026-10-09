/* PS5 disc read-speed test (live-13, AI-assisted).
 *
 * swordpdf: "he said he dumps at 8x" -- the author of the disc_detect payload, whose source he sent on 2026-10-10.
 * Its DVD path does nothing our 150..154 tests didn't: the drive's pass device found through /dev/xpt0
 * (XPT_GDEVLIST "cd" 0), GET CONFIGURATION, READ DISC STRUCTURE, then SET STREAMING to the disc's last sector with
 * 0x10000000 kB/s (SET CD SPEED to 0xFFFF if that is refused), then plain read() of /dev/cd0 128 KiB at a time.
 * Both PS5 drive models refused SET STREAMING and SET CD SPEED in 150/154 ("invalid command operation code") and
 * read a PS2 DVD at 2.0x as they were, 3.2x with Sony's own DB 00 00 32. This payload runs exactly his sequence on
 * this console and times it, so the two can be compared on the same disc:
 *   1. his sequence, then 64 MiB at the start, the middle and the end of the disc;
 *   2. Sony's DB 00 00 32 (3.2), the same three places;
 *   3. the drive back to Sony's default (DB 00 00 20, 2.0).
 * Only reads the disc. Results: notification, klog "[dump speed]" and /data/PCSX2/logs/dumpspeed.txt.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

typedef struct { char unused[45]; char message[3075]; } notify_req_t;
int sceKernelSendNotificationRequest(int32_t device, void *req, size_t size, int32_t blocking);

static FILE *g_out;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog_printf("[dump speed] %s\n", buf);
    if (g_out) { fprintf(g_out, "%s\n", buf); fflush(g_out); }
}
static void notify(const char *msg) {
    static notify_req_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
static double mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int scsi(int fd, uint32_t path, uint32_t target, uint32_t lun, const uint8_t *cdb, int cdb_len, uint32_t dir, void *data,
                uint32_t len, char *why, size_t wmax) {
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.csio.ccb_h.retry_count = 1;
    ccb.csio.ccb_h.func_code = XPT_SCSI_IO;
    ccb.csio.ccb_h.path_id = path;
    ccb.csio.ccb_h.target_id = target;
    ccb.csio.ccb_h.target_lun = lun;
    ccb.csio.ccb_h.flags = dir;
    ccb.csio.ccb_h.timeout = 10000;
    ccb.csio.data_ptr = data;
    ccb.csio.dxfer_len = len;
    ccb.csio.sense_len = SSD_FULL_SIZE;
    ccb.csio.cdb_len = (uint8_t)cdb_len;
    ccb.csio.tag_action = MSG_SIMPLE_Q_TAG;
    memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, (size_t)cdb_len);
    if (ioctl(fd, CAMIOCOMMAND, &ccb) < 0) { snprintf(why, wmax, "ioctl errno %d", errno); return -1; }
    if ((ccb.csio.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP) { snprintf(why, wmax, "ok"); return 0; }
    const uint8_t *sd = (const uint8_t *)&ccb.csio.sense_data;
    snprintf(why, wmax, "cam %#x scsi %#x sense %X/%02X/%02X", ccb.csio.ccb_h.status, ccb.csio.scsi_status, sd[2] & 15, sd[12], sd[13]);
    return -1;
}

/* his open_cd0: /dev/xpt0, CAMGETPASSTHRU for "cd" 0, then the pass device's own path/target/lun */
static int open_cd0_pass(uint32_t *path, uint32_t *target, uint32_t *lun, char *name, size_t nmax) {
    union ccb ccb;
    const int xpt = open("/dev/xpt0", O_RDWR);
    if (xpt < 0) { say("open /dev/xpt0: errno %d", errno); return -1; }
    memset(&ccb, 0, sizeof(ccb));
    ccb.cgdl.ccb_h.func_code = XPT_GDEVLIST;
    snprintf(ccb.cgdl.periph_name, sizeof(ccb.cgdl.periph_name), "cd");
    ccb.cgdl.unit_number = 0;
    if (ioctl(xpt, CAMGETPASSTHRU, &ccb) < 0 || ccb.cgdl.status == CAM_GDEVLIST_ERROR) {
        say("CAMGETPASSTHRU cd0 on xpt0: errno %d status %d", errno, (int)ccb.cgdl.status);
        close(xpt);
        return -1;
    }
    close(xpt);
    snprintf(name, nmax, "/dev/%s%u", ccb.cgdl.periph_name, ccb.cgdl.unit_number);
    const int fd = open(name, O_RDWR);
    if (fd < 0) { say("open %s: errno %d", name, errno); return -1; }
    memset(&ccb, 0, sizeof(ccb));
    ccb.cgdl.ccb_h.func_code = XPT_GDEVLIST;
    if (ioctl(fd, CAMGETPASSTHRU, &ccb) < 0) { say("CAMGETPASSTHRU on %s: errno %d", name, errno); close(fd); return -1; }
    *path = ccb.cgdl.ccb_h.path_id;
    *target = ccb.cgdl.ccb_h.target_id;
    *lun = ccb.cgdl.ccb_h.target_lun;
    return fd;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* 64 MiB of read() on /dev/cd0 at `at`, 128 KiB a call (his DUMP_CHUNK). MB/s, or -1. */
static double time_reads(int cd, off_t at, const char *what) {
    static uint8_t buf[128 * 1024];
    const off_t want = 64ll << 20;
    if (lseek(cd, at, SEEK_SET) < 0) { say("%s: lseek errno %d", what, errno); return -1; }
    /* 4 MiB first, untimed (spin-up, seek) */
    for (int i = 0; i < 32; i++) if (read(cd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) { say("%s: warm-up read errno %d", what, errno); return -1; }
    const double t0 = mono_s();
    off_t got = 0;
    while (got < want) {
        const ssize_t n = read(cd, buf, sizeof(buf));
        if (n <= 0) { say("%s: read at +%lld: %zd errno %d", what, (long long)got, n, errno); break; }
        got += n;
    }
    const double s = mono_s() - t0;
    const double mbs = got / (s > 0.001 ? s : 0.001) / 1e6;
    say("%s: %lld MB in %.1f s = %.2f MB/s = %.2fx DVD", what, (long long)(got / 1000000), s, mbs, mbs / 1.385);
    return mbs;
}

int main(void) {
    uint8_t all[16];
    memset(all, 0xff, sizeof(all));
    kernel_set_ucred_authid(getpid(), 0x4800000000010003ull);
    kernel_set_ucred_caps(getpid(), all);
    g_out = fopen("/data/PCSX2/logs/dumpspeed.txt", "w");
    say("disc read-speed test (live-13): the disc_detect payload's DVD sequence, then Sony's 3.2");
    notify("Dump speed test: running (about 2 minutes)");

    const int cd = open("/dev/cd0", O_RDONLY);
    off_t media = 0;
    unsigned sector = 0;
    if (cd < 0 || ioctl(cd, DIOCGMEDIASIZE, &media) != 0 || media <= 0) { say("/dev/cd0: no disc (errno %d)", errno); notify("Dump speed test: no disc in /dev/cd0"); return 1; }
    ioctl(cd, DIOCGSECTORSIZE, &sector);
    say("/dev/cd0: %lld bytes, sector %u", (long long)media, sector);

    uint32_t path = 0, target = 0, lun = 0;
    char name[32] = "";
    const int pass = open_cd0_pass(&path, &target, &lun, name, sizeof(name));
    if (pass < 0) { notify("Dump speed test: no pass device"); return 1; }
    char why[96];
    uint8_t inq[36] = {0};
    const uint8_t c_inq[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
    scsi(pass, path, target, lun, c_inq, 6, CAM_DIR_IN, inq, sizeof(inq), why, sizeof(why));
    say("%s (path %u target %u lun %u): %.8s %.16s %.4s", name, path, target, lun, inq + 8, inq + 16, inq + 32);

    /* his detect_disc: GET CONFIGURATION (0x804 bytes), READ DISC STRUCTURE format 0 for DVD profiles */
    static uint8_t data[0x804];
    const uint8_t c_gc[12] = {0x46, 0, 0, 0, 0, 0, 0, 0x08, 0x04, 0, 0, 0};
    memset(data, 0, sizeof(data));
    scsi(pass, path, target, lun, c_gc, 12, CAM_DIR_IN, data, sizeof(data), why, sizeof(why));
    say("GET CONFIGURATION: %s, profile %#06x", why, data[6] << 8 | data[7]);
    const uint8_t c_rds[12] = {0xAD, 0, 0, 0, 0, 0, 0, 0, 0x08, 0x04, 0, 0};
    memset(data, 0, sizeof(data));
    scsi(pass, path, target, lun, c_rds, 12, CAM_DIR_IN, data, sizeof(data), why, sizeof(why));
    say("READ DISC STRUCTURE: %s, book type %u", why, data[4] >> 4);

    /* his disc_request_read_speed(end_lba) */
    const uint32_t end_lba = sector ? (uint32_t)(media / sector - 1) : 0;
    uint8_t c_ss[12] = {0xB6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 28, 0};
    uint8_t pd[28];
    memset(pd, 0, sizeof(pd));
    be32(pd + 8, end_lba);
    be32(pd + 12, 0x10000000u);
    be32(pd + 16, 1000);
    be32(pd + 20, 0x10000000u);
    be32(pd + 24, 1000);
    const int ss = scsi(pass, path, target, lun, c_ss, 12, CAM_DIR_OUT, pd, sizeof(pd), why, sizeof(why));
    say("SET STREAMING to LBA %u, 0x10000000 kB/s: %s", end_lba, why);
    if (ss != 0) {
        const uint8_t c_bb[12] = {0xBB, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
        scsi(pass, path, target, lun, c_bb, 12, CAM_DIR_NONE, NULL, 0, why, sizeof(why));
        say("SET CD SPEED max: %s", why);
    }

    const off_t mid = (media / 2) & ~(off_t)0xFFFFF, end = (media - (72ll << 20)) & ~(off_t)0xFFFFF;
    const double a0 = time_reads(cd, 0, "his sequence, start");
    const double a1 = time_reads(cd, mid, "his sequence, middle");
    const double a2 = time_reads(cd, end, "his sequence, end");

    const uint8_t c_32[12] = {0xDB, 0x00, 0x00, 0x32, 0, 0, 0, 0, 0, 0, 0, 0};
    const int s32 = scsi(pass, path, target, lun, c_32, 12, CAM_DIR_NONE, NULL, 0, why, sizeof(why));
    say("Sony DB 00 00 32 (3.2): %s", why);
    double b0 = -1, b1 = -1, b2 = -1;
    if (s32 == 0) {
        b0 = time_reads(cd, 0, "Sony 3.2, start");
        b1 = time_reads(cd, mid, "Sony 3.2, middle");
        b2 = time_reads(cd, end, "Sony 3.2, end");
    }
    const uint8_t c_20[12] = {0xDB, 0x00, 0x00, 0x20, 0, 0, 0, 0, 0, 0, 0, 0};
    scsi(pass, path, target, lun, c_20, 12, CAM_DIR_NONE, NULL, 0, why, sizeof(why));
    say("drive back to Sony's 2.0: %s", why);
    close(pass);
    close(cd);

    char msg[300];
    snprintf(msg, sizeof(msg), "Dump speed: his way %.1fx/%.1fx/%.1fx, Sony 3.2: %.1fx/%.1fx/%.1fx (start/middle/end, DVD x)", a0 / 1.385,
             a1 / 1.385, a2 / 1.385, b0 / 1.385, b1 / 1.385, b2 / 1.385);
    say("%s", msg);
    notify(msg);
    if (g_out) fclose(g_out);
    return 0;
}
