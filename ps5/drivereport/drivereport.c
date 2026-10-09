/* PS5 disc drive report (AI-assisted, from PS5SX2's disc speed tests).
 *
 * For comparing PS5 drives: what the drive is, what it says about its speeds, and which of Sony's speed settings it takes
 * for the disc in it, each timed on 16 MiB. On swordpdf's console (SONY PS-SYSTEM 503R, firmware 2305, a PS2 DVD) only
 * DB 00 00 20 (2.0x, the default) and DB 00 00 32 (3.2x) are taken; SceShellCore (11.40) itself gives a "PS2 DVD-ROM"
 * 3.2 (its speed table entry 2). Someone else dumps PS2 DVDs at ~8x on his PS5's own drive: this tells whether his drive
 * takes 8.0 where this one doesn't.
 *
 * Send it to the ELF loader (port 9021) with a PS2 DVD in the drive. It only reads the disc and puts the drive back to
 * 2.0. Report: a notification, klog ([drive report]) and /data/drive-report.txt.
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
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

_Static_assert(sizeof(union ccb) == 0x4E0, "union ccb: SceShellCore's CAMIOCOMMAND (0xC4E01902) carries 1248 bytes");

static FILE *g_out;
static char g_sum[2800];
static int g_at;

static void say(const char *fmt, ...) {
  char line[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  klog_printf("[drive report] %s\n", line);
  if (g_out) {
    fprintf(g_out, "%s\n", line);
    fflush(g_out);
  }
}

static void sum(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (g_at < (int)sizeof(g_sum) - 1)
    g_at += vsnprintf(g_sum + g_at, sizeof(g_sum) - g_at, fmt, ap);
  va_end(ap);
}

typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int32_t device, void *request, size_t size, int32_t blocking);
static void notify(const char *text) {
  static notify_request_t req;
  memset(&req, 0, sizeof(req));
  snprintf(req.message, sizeof(req.message), "%s", text);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* One command; data in (dir CAM_DIR_IN) or none. 0 when done; *sense_out "key/asc/ascq" text otherwise. */
static int cmd(int pass, const uint8_t *cdb, int cdb_len, uint32_t dir, uint8_t *data, uint32_t len, char *why, size_t whylen) {
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  cam_fill_csio(&ccb.csio, 1, NULL, dir | CAM_DEV_QFRZDIS | (dir == CAM_DIR_NONE ? CAM_PASS_ERR_RECOVER : 0), MSG_SIMPLE_Q_TAG, data,
                len, SSD_FULL_SIZE, (uint8_t)cdb_len, 10000);
  memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, (size_t)cdb_len);
  if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0) {
    snprintf(why, whylen, "errno %d", errno);
    return -1;
  }
  if ((ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP)
    return 0;
  const uint8_t *s = (const uint8_t *)&ccb.csio.sense_data;
  snprintf(why, whylen, "sense %X/%02X/%02X", s[2] & 15, s[12], s[13]);
  return -1;
}

static double timed(int fd, uint64_t from, uint64_t size) {
  static uint8_t *buf;
  const uint32_t chunk = 4u << 20;
  if (!buf && !(buf = malloc(chunk)))
    return 0;
  from &= ~(uint64_t)2047;
  if (from + 5 * (uint64_t)chunk > size)
    return 0;
  pread(fd, buf, chunk, (off_t)from); /* warm-up */
  const double t0 = now_s();
  uint64_t got = 0;
  for (uint64_t off = from + chunk; off < from + 5 * (uint64_t)chunk; off += chunk) {
    if (pread(fd, buf, chunk, (off_t)off) != (ssize_t)chunk)
      break;
    got += chunk;
  }
  const double s = now_s() - t0;
  return got == 4 * (uint64_t)chunk && s > 0 ? got / s / 1e6 : 0;
}

int main(void) {
  g_out = fopen("/data/drive-report.txt", "w");
  notify("Drive report: running (about 1 minute)");
  int fd = open("/dev/cd0", O_RDONLY);
  if (fd < 0 && (errno == EPERM || errno == EACCES)) {
    const pid_t pid = getpid();
    const uint64_t authid = kernel_get_ucred_authid(pid);
    uint8_t caps[16], all[16];
    memset(all, 0xff, sizeof(all));
    kernel_get_ucred_caps(pid, caps);
    kernel_set_ucred_authid(pid, 0x4800000000010003ull);
    kernel_set_ucred_caps(pid, all);
    fd = open("/dev/cd0", O_RDONLY);
    kernel_set_ucred_authid(pid, authid);
    kernel_set_ucred_caps(pid, caps);
  }
  if (fd < 0) {
    say("/dev/cd0: open errno %d", errno);
    notify("Drive report: can't open /dev/cd0");
    return 1;
  }
  off_t size = 0;
  if (ioctl(fd, DIOCGMEDIASIZE, &size) != 0 || size < (off_t)(256ull << 20)) {
    say("no disc (size %lld)", (long long)size);
    notify("Drive report: put a PS2 DVD in the drive");
    close(fd);
    return 1;
  }
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  ccb.ccb_h.func_code = XPT_GDEVLIST;
  char path[64] = "";
  if (ioctl(fd, CAMGETPASSTHRU, &ccb) == 0)
    snprintf(path, sizeof(path), "/dev/%.*s%u", (int)sizeof(ccb.cgdl.periph_name), ccb.cgdl.periph_name, ccb.cgdl.unit_number);
  const int pass = path[0] ? open(path, O_RDWR) : -1;
  if (pass < 0) {
    say("no pass device (%s, errno %d)", path, errno);
    notify("Drive report: no pass device");
    close(fd);
    return 1;
  }
  char why[64];
  uint8_t inq[36] = {0};
  const uint8_t c_inq[6] = {0x12, 0, 0, 0, 36, 0};
  if (cmd(pass, c_inq, 6, CAM_DIR_IN, inq, sizeof(inq), why, sizeof(why)) == 0) {
    say("drive: %.8s | %.16s | %.4s", inq + 8, inq + 16, inq + 32);
    sum("Drive %.16s fw %.4s", inq + 16, inq + 32);
  }
  uint8_t gc[8] = {0};
  const uint8_t c_gc[10] = {0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0};
  if (cmd(pass, c_gc, 10, CAM_DIR_IN, gc, sizeof(gc), why, sizeof(why)) == 0) {
    say("disc profile %#x, %lld bytes", gc[6] << 8 | gc[7], (long long)size);
    sum(" | disc profile %#x", gc[6] << 8 | gc[7]);
  }
  uint8_t perf[40] = {0};
  const uint8_t c_perf[12] = {0xAC, 0x10, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0};
  if (cmd(pass, c_perf, 12, CAM_DIR_IN, perf, sizeof(perf), why, sizeof(why)) == 0) {
    const uint32_t a = (uint32_t)perf[12] << 24 | perf[13] << 16 | perf[14] << 8 | perf[15];
    const uint32_t b = (uint32_t)perf[20] << 24 | perf[21] << 16 | perf[22] << 8 | perf[23];
    say("GET PERFORMANCE: %u..%u kB/s", a, b);
    sum(" | reports %u..%u kB/s", a, b);
  }
  uint8_t page[64] = {0};
  const uint8_t c_ms[10] = {0x5A, 0x08, 0x2F, 0, 0, 0, 0, 0, 64, 0};
  if (cmd(pass, c_ms, 10, CAM_DIR_IN, page, sizeof(page), why, sizeof(why)) == 0)
    say("mode page 2F: %02x%02x %02x%02x %02x%02x%02x%02x", page[8], page[9], page[10], page[11], page[12], page[13], page[14], page[15]);

  const double base = timed(fd, 64ull << 20, (uint64_t)size);
  say("as the drive is: %.2f MB/s (%.2fx DVD)", base, base / 1.385);
  sum(" | as is %.1f MB/s", base);
  static const struct {
    uint8_t rot;
    uint16_t speed;
    const char *name;
  } kTry[] = {{1, 0x0080, "8.0 r1"}, {0, 0x0080, "8.0 r0"}, {1, 0x0060, "6.0 r1"}, {1, 0x0100, "0x100 r1"}, {0, 0x0032, "3.2 r0"}};
  for (int i = 0; i < (int)(sizeof(kTry) / sizeof(kTry[0])); i++) {
    const uint8_t c_db[12] = {0xDB, kTry[i].rot, (uint8_t)(kTry[i].speed >> 8), (uint8_t)kTry[i].speed, 0, 0, 0, 0, 0, 0, 0, 0};
    if (cmd(pass, c_db, 12, CAM_DIR_NONE, NULL, 0, why, sizeof(why)) != 0) {
      say("DB %02x %02x %02x (%s): refused, %s", c_db[1], c_db[2], c_db[3], kTry[i].name, why);
      sum(" | %s no", kTry[i].name);
      continue;
    }
    const double mbs = timed(fd, (64ull + 32ull * (i + 1)) << 20, (uint64_t)size);
    say("DB %02x %02x %02x (%s): taken, %.2f MB/s (%.2fx DVD)", c_db[1], c_db[2], c_db[3], kTry[i].name, mbs, mbs / 1.385);
    sum(" | %s %.1f MB/s", kTry[i].name, mbs);
  }
  const uint8_t c_back[12] = {0xDB, 0, 0, 0x20, 0, 0, 0, 0, 0, 0, 0, 0};
  cmd(pass, c_back, 12, CAM_DIR_NONE, NULL, 0, why, sizeof(why));
  say("drive back to 2.0");
  close(pass);
  close(fd);
  say("%s", g_sum);
  notify(g_sum);
  if (g_out)
    fclose(g_out);
  return 0;
}
