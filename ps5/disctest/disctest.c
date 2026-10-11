/* PS5SX2 disc speed test payload (AI-assisted).
 *
 * v1 (2026-10-09): /dev/cd0 timed from a payload: 2.00x (128 MiB in 48.6 s), the same as the PS5SX2 app. So who reads
 * isn't it.
 *
 * v2: Sony's own drive speed command, from SceShellCore 11.40's _AutoMounter::OpticalDisc::setSpeed(int) (0x376790):
 * libcam on "cd0", CDB DB <rotation & 3> <speed hi> <speed lo> + 8 zeros, no data, 10 s, CAM_DIR_NONE |
 * CAM_DEV_QFRZDIS | CAM_PASS_ERR_RECOVER, simple tag. Its table (0x1d823a0) has 0/0x20 "2.0" as the default (the 2x we
 * get) and 1/0x80 "8.0", 1/0x100, 0/0xFFFF max, 1/0x60 "6.0" among the faster ones. This payload, sent to the ELF loader
 * like the PS5SX2 Helper, finds the drive's pass device through /dev/cd0 (CAMGETPASSTHRU), times 32 MiB as the drive is,
 * then sends each of those settings and times a fresh 32 MiB after each, and puts the drive back to 2.0 at the end.
 * Only values from Sony's table are sent; the disc is only read.
 *
 * v2 on swordpdf's console (13.60, NFS Underground 2): DB 01 00 80 / 01 01 00 / 01 00 60 refused with ILLEGAL REQUEST
 * asc 0x24 (invalid field: the drive knows the command); DB 00 FF FF taken, still 2.00x. SceShellCore's only setSpeed
 * call at media detection (0x36882c) is for BD profiles 0x40..0x43 and Sony types 0xFF71/0xFF80/0xFF90: a DVD never gets
 * it, so the rotation 1 entries are BD speeds.
 *
 * v3: the disc's current profile (GET CONFIGURATION), then DB with rotation 0 and 2 (and 3) and explicit speeds, each
 * timed on 16 MiB when the drive takes it; refusals log the sense data's field pointer (which byte the drive objects to).
 *
 * v3 on swordpdf's console: profile 0x10 (DVD-ROM). Rotation 0: 0x80 and 0x50 refused (asc 0x24, no field pointer), 0x32
 * taken and read at 3.20x (4.43 MB/s) at both ends of the disc. Rotation 2 (0x80, 0x50, 0x40) and 3 (0x80) refused.
 *
 * v4: a sweep, to learn what the drive takes for a DVD: rotation 0 between 0x32 and 0x50 and beyond (is 3.2 a ceiling or
 * one of a few fixed values?), values read as kB/s (the log string says "kbyte/sec": 5540, 8310, 11080 = 4x, 6x, 8x),
 * and rotations 1-3 with the low values 0x20/0x32 (is the rotation or the speed what's refused?). Each value the drive
 * takes is timed on 16 MiB.
 *
 * v4: rotation 0 takes only 0x20, 0x32 and 0xFFFF (0x26, 0x30, 0x33..0x4F, 0x60, 0x100, 0x15A4, 0x2076, 0x2B48 all
 * refused, asc 0x24, no field pointer); rotations 1-3 refused even at 0x20/0x32. So DB's DVD settings are a fixed set,
 * 3.2x at most.
 *
 * v5: looking for what else sets the speed, reading only:
 *   - GET CONFIGURATION, all features (which the drive reports: Real-Time Streaming 0x0107 and others)
 *   - MODE SENSE(10) of every page (0x3F), current values and the changeable mask (a vendor page with a speed field?)
 *   - INQUIRY VPD page list
 *   - DB with CDB byte 1's upper bits set (SceShellCore masks the rotation to 2 bits; the rest may be flags) and with
 *     bytes 4/5 set, at speed 0x80 (8.0) and 0x32; each the drive takes is timed on 16 MiB, and the drive goes back to
 *     2.0 at the end. No MODE SELECT, nothing written.
 *
 * v5: features 0107* (real-time streaming, flags 0x0c), 0108* (serial), ff00* and ff10..ff60; mode pages 01, 08, 1a (power
 * timers), 1d, 2a (nothing changeable), 2d, 2f "04 32" (not changeable: 0x32 = the 3.2 ceiling?), 30, 31 "01 00 0898
 * 0064" (byte 2 two bits, bytes 4-7 changeable), 32 "03 00 00" (byte 2 two bits, byte 3 bit 0, byte 4 bit 0 changeable).
 * DB with any other bits set: refused at 0x80, taken at 0x32 (3.2x).
 *
 * v6: Sony's pages 31 and 32 changed one field at a time with MODE SELECT(10), PF 1, SP 0 (not saved: the drive forgets
 * on reset or power off), and the originals put back after each one and at the end. After each change: DB 00 00 80, then
 * DB 01 00 80, then DB 00 00 32, the first the drive takes timed on 16 MiB, and the drive back to 2.0.
 *
 * v6: page 32's changes are taken but ignored (it reads back unchanged); page 31 byte 2 sticks; neither changes anything:
 * DB 00 00 80 / 01 00 80 still refused, DB 00 00 32 still 3.2x. Both pages back, drive back to 2.0 (SP 0, nothing saved).
 *
 * v7: page 31 bytes 4-5 (0x0898 = 2200) and 6-7 (0x0064 = 100), the last changeable fields, one value at a time (SP 0, not
 * saved; the original back after each and at the end). After each: 16 MiB timed with no speed command (does the field set
 * the speed itself?), then DB 00 00 80 and DB 01 00 80, timed if taken; the drive back to 2.0.
 *
 * Results: klog ([PS5SX2 disctest]), /data/PCSX2/logs/disctest.log, and notifications.
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
#include <sys/stat.h>
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

#define CHUNK (4u << 20)
#define TIMED (16ull << 20)

static FILE *g_log;

static void say(const char *fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  klog_printf("[PS5SX2 disctest] %s\n", line);
  if (g_log) {
    fprintf(g_log, "%s\n", line);
    fflush(g_log);
  }
}

typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int32_t device, void *request, size_t size, int32_t blocking);

static void notify(const char *fmt, ...) {
  static notify_request_t req;
  memset(&req, 0, sizeof(req));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof(req.message), fmt, ap);
  va_end(ap);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *speed_text(uint16_t speed, char *b, size_t n) {
  if (speed == 0xFFFF)
    snprintf(b, n, "max");
  else if (speed > 0xFF)
    snprintf(b, n, "%#x", speed);
  else
    snprintf(b, n, "%u.%ux", (speed >> 4) & 15, speed & 15);
  return b;
}

/* Sony's speed command through the pass device. 0 when the drive took it. */
static int set_speed(int pass, uint8_t rot, uint16_t speed) {
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  cam_fill_csio(&ccb.csio, 1, NULL, CAM_DIR_NONE | CAM_DEV_QFRZDIS | CAM_PASS_ERR_RECOVER, MSG_SIMPLE_Q_TAG, NULL, 0,
                SSD_FULL_SIZE, 12, 10000);
  uint8_t *cdb = ccb.csio.cdb_io.cdb_bytes;
  cdb[0] = 0xDB;
  cdb[1] = rot & 3;
  cdb[2] = (uint8_t)(speed >> 8);
  cdb[3] = (uint8_t)speed;
  char st[16];
  if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0) {
    say("DB %02x %02x %02x (rotation %u, %s): CAMIOCOMMAND errno %d", rot, speed >> 8, speed & 0xff, rot,
        speed_text(speed, st, sizeof(st)), errno);
    return -1;
  }
  const uint32_t status = ccb.ccb_h.status & CAM_STATUS_MASK;
  if (status == CAM_REQ_CMP) {
    say("DB %02x %02x %02x (rotation %u, %s): ok", rot, speed >> 8, speed & 0xff, rot, speed_text(speed, st, sizeof(st)));
    return 0;
  }
  const uint8_t *s = (const uint8_t *)&ccb.csio.sense_data;
  int key = -1, asc = -1, ascq = -1;
  if ((ccb.ccb_h.status & CAM_AUTOSNS_VALID) && ((s[0] & 0x7f) == 0x70 || (s[0] & 0x7f) == 0x71)) {
    key = s[2] & 15;
    asc = s[12];
    ascq = s[13];
  } else if ((ccb.ccb_h.status & CAM_AUTOSNS_VALID) && ((s[0] & 0x7f) == 0x72 || (s[0] & 0x7f) == 0x73)) {
    key = s[1] & 15;
    asc = s[2];
    ascq = s[3];
  }
  /* fixed sense bytes 15-17: SKSV, C/D, BPV, bit pointer, field pointer (which CDB byte is wrong) */
  const int sksv = (s[0] & 0x7f) <= 0x71 && (s[15] & 0x80);
  say("DB %02x %02x %02x (rotation %u, %s): CAM status %#x, SCSI status %#x, sense key %d asc %#x ascq %#x%s%s byte %d bit %d",
      rot, speed >> 8, speed & 0xff, rot, speed_text(speed, st, sizeof(st)), ccb.ccb_h.status, ccb.csio.scsi_status, key, asc,
      ascq, sksv ? ", field in the " : ", no field pointer", sksv ? ((s[15] & 0x40) ? "CDB" : "data") : "",
      sksv ? (s[16] << 8 | s[17]) : -1, sksv && (s[15] & 0x08) ? (s[15] & 7) : -1);
  return -1;
}

/* Any read-type command: 0 when done, *got = bytes the drive returned. */
static int cmd_in(int pass, const uint8_t *cdb, int cdb_len, uint8_t *data, uint32_t len, uint32_t *got, const char *what) {
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  cam_fill_csio(&ccb.csio, 1, NULL, CAM_DIR_IN | CAM_DEV_QFRZDIS, MSG_SIMPLE_Q_TAG, data, len, SSD_FULL_SIZE, (uint8_t)cdb_len, 10000);
  memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, (size_t)cdb_len);
  if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0) {
    say("%s: CAMIOCOMMAND errno %d", what, errno);
    return -1;
  }
  if ((ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
    const uint8_t *s = (const uint8_t *)&ccb.csio.sense_data;
    say("%s: CAM status %#x, sense key %d asc %#x ascq %#x", what, ccb.ccb_h.status, s[2] & 15, s[12], s[13]);
    return -1;
  }
  if (got)
    *got = len - ccb.csio.resid;
  return 0;
}

static void hexdump(const char *what, const uint8_t *p, uint32_t n) {
  char line[200];
  for (uint32_t at = 0; at < n; at += 32) {
    int k = 0;
    for (uint32_t i = at; i < n && i < at + 32; i++)
      k += snprintf(line + k, sizeof(line) - k, "%02x%s", p[i], ((i - at) & 3) == 3 ? " " : "");
    say("%s +%03x: %s", what, at, line);
  }
}

static void describe_drive(int pass) {
  static uint8_t buf[8192];
  uint32_t got = 0;
  /* GET CONFIGURATION, all features: header 8 bytes, then descriptors {code u16, flags, len, data} */
  uint8_t gc[10] = {0x46, 0x00, 0, 0, 0, 0, 0, (uint8_t)(sizeof(buf) >> 8), (uint8_t)sizeof(buf), 0};
  if (cmd_in(pass, gc, 10, buf, sizeof(buf), &got, "GET CONFIGURATION (all)") == 0 && got >= 8) {
    const uint32_t len = ((uint32_t)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3]) + 4;
    const uint32_t end = len < got ? len : got;
    char list[2000];
    int k = 0;
    for (uint32_t at = 8; at + 4 <= end && k < (int)sizeof(list) - 32; at += 4 + buf[at + 3]) {
      const unsigned code = buf[at] << 8 | buf[at + 1];
      k += snprintf(list + k, sizeof(list) - k, " %04x%s", code, (buf[at + 2] & 1) ? "*" : "");
      if (code == 0x0107 || code == 0x0108 || code >= 0xff00)
        hexdump(code == 0x0107 ? "  feature 0107 (real-time streaming)" : "  feature", buf + at, 4 + buf[at + 3]);
    }
    say("features (* = current):%s", list);
  }
  /* MODE SENSE(10) all pages: current values (PC 00), then the changeable mask (PC 01) */
  for (int pc = 0; pc < 2; pc++) {
    uint8_t ms[10] = {0x5A, 0x08, (uint8_t)((pc << 6) | 0x3F), 0, 0, 0, 0, (uint8_t)(sizeof(buf) >> 8), (uint8_t)sizeof(buf), 0};
    memset(buf, 0, sizeof(buf));
    if (cmd_in(pass, ms, 10, buf, sizeof(buf), &got, pc ? "MODE SENSE all pages, changeable" : "MODE SENSE all pages, current") == 0 && got >= 8) {
      const uint32_t len = (uint32_t)(buf[0] << 8 | buf[1]) + 2;
      const uint32_t end = len < got ? len : got;
      for (uint32_t at = 8 + (uint32_t)(buf[6] << 8 | buf[7]); at + 2 <= end; at += 2 + buf[at + 1]) {
        char what[64];
        snprintf(what, sizeof(what), "  page %02x %s", buf[at] & 0x3f, pc ? "changeable" : "current");
        hexdump(what, buf + at, 2 + buf[at + 1] < end - at ? 2 + buf[at + 1] : end - at);
      }
    }
  }
  /* INQUIRY VPD page 00: the supported VPD pages */
  uint8_t vpd[6] = {0x12, 0x01, 0x00, 0, 255, 0};
  if (cmd_in(pass, vpd, 6, buf, 255, &got, "INQUIRY VPD 00") == 0 && got >= 4)
    hexdump("  VPD pages", buf, got < 4u + buf[3] ? got : 4u + buf[3]);
}

/* DB with any CDB bytes 1, 4, 5 (SceShellCore sends only rotation & 3 in byte 1). 0 when taken. */
static int set_speed_raw(int pass, uint8_t b1, uint16_t speed, uint8_t b4, uint8_t b5) {
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  cam_fill_csio(&ccb.csio, 1, NULL, CAM_DIR_NONE | CAM_DEV_QFRZDIS | CAM_PASS_ERR_RECOVER, MSG_SIMPLE_Q_TAG, NULL, 0,
                SSD_FULL_SIZE, 12, 10000);
  uint8_t *cdb = ccb.csio.cdb_io.cdb_bytes;
  cdb[0] = 0xDB;
  cdb[1] = b1;
  cdb[2] = (uint8_t)(speed >> 8);
  cdb[3] = (uint8_t)speed;
  cdb[4] = b4;
  cdb[5] = b5;
  if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0) {
    say("DB %02x %02x %02x %02x %02x: CAMIOCOMMAND errno %d", b1, speed >> 8, speed & 0xff, b4, b5, errno);
    return -1;
  }
  const int ok = (ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP;
  const uint8_t *s = (const uint8_t *)&ccb.csio.sense_data;
  say("DB %02x %02x %02x %02x %02x: %s", b1, speed >> 8, speed & 0xff, b4, b5, ok ? "ok" : "refused");
  if (!ok)
    say("  sense key %d asc %#x ascq %#x", s[2] & 15, s[12], s[13]);
  return ok ? 0 : -1;
}

/* MODE SENSE(10) of one page (current values) into page[], its length (2 + page length) or -1. */
static int read_page(int pass, uint8_t code, uint8_t *page, int cap) {
  uint8_t buf[252] = {0};
  uint32_t got = 0;
  uint8_t ms[10] = {0x5A, 0x08, code, 0, 0, 0, 0, 0, (uint8_t)sizeof(buf), 0};
  if (cmd_in(pass, ms, 10, buf, sizeof(buf), &got, "MODE SENSE") != 0 || got < 10)
    return -1;
  const int at = 8 + (buf[6] << 8 | buf[7]);
  const int n = 2 + buf[at + 1];
  if ((buf[at] & 0x3f) != code || n > cap || at + n > (int)got)
    return -1;
  memcpy(page, buf + at, (size_t)n);
  return n;
}

/* MODE SELECT(10), PF 1, SP 0 (not saved), with one page. 0 when taken. */
static int write_page(int pass, const uint8_t *page, int n, const char *what) {
  uint8_t data[264] = {0};
  memcpy(data + 8, page, (size_t)n);
  data[8] &= 0x3f; /* PS is reserved in MODE SELECT */
  const uint32_t len = 8 + (uint32_t)n;
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  cam_fill_csio(&ccb.csio, 1, NULL, CAM_DIR_OUT | CAM_DEV_QFRZDIS, MSG_SIMPLE_Q_TAG, data, len, SSD_FULL_SIZE, 10, 10000);
  uint8_t *cdb = ccb.csio.cdb_io.cdb_bytes;
  cdb[0] = 0x55;
  cdb[1] = 0x10; /* PF 1, SP 0 */
  cdb[7] = (uint8_t)(len >> 8);
  cdb[8] = (uint8_t)len;
  char hex[80];
  int k = 0;
  for (int i = 0; i < n && k < (int)sizeof(hex) - 3; i++)
    k += snprintf(hex + k, sizeof(hex) - k, "%02x", page[i]);
  if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0) {
    say("MODE SELECT %s (%s): errno %d", what, hex, errno);
    return -1;
  }
  const int ok = (ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP;
  const uint8_t *s = (const uint8_t *)&ccb.csio.sense_data;
  if (ok)
    say("MODE SELECT %s (%s): ok", what, hex);
  else
    say("MODE SELECT %s (%s): refused, sense key %d asc %#x ascq %#x", what, hex, s[2] & 15, s[12], s[13]);
  return ok ? 0 : -1;
}

/* GET CONFIGURATION (0x46), the current profile (0x10 DVD-ROM, 0x40 BD-ROM, Sony's own above 0xFF00), or -1. */
static int current_profile(int pass) {
  union ccb ccb;
  uint8_t data[8] = {0};
  memset(&ccb, 0, sizeof(ccb));
  cam_fill_csio(&ccb.csio, 1, NULL, CAM_DIR_IN | CAM_DEV_QFRZDIS, MSG_SIMPLE_Q_TAG, data, sizeof(data), SSD_FULL_SIZE, 10, 10000);
  uint8_t *cdb = ccb.csio.cdb_io.cdb_bytes;
  cdb[0] = 0x46;
  cdb[1] = 0x02; /* RT 10b: the feature header only */
  cdb[8] = sizeof(data);
  if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0 || (ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
    say("GET CONFIGURATION: errno %d, CAM status %#x", errno, ccb.ccb_h.status);
    return -1;
  }
  return data[6] << 8 | data[7];
}

/* MB/s over TIMED from `from` after a 4 MiB warm-up (the drive settling at a new speed), or 0. */
static double timed_read(int fd, uint64_t from, uint64_t size, const char *what) {
  static uint8_t *buf;
  if (!buf)
    buf = malloc(CHUNK);
  if (!buf)
    return 0;
  from &= ~(uint64_t)2047;
  if (from + CHUNK + TIMED > size) {
    say("%s: the disc is too small", what);
    return 0;
  }
  pread(fd, buf, CHUNK, (off_t)from);
  const double t0 = now_s();
  uint64_t got = 0;
  for (uint64_t off = from + CHUNK; off < from + CHUNK + TIMED; off += CHUNK) {
    if (pread(fd, buf, CHUNK, (off_t)off) != (ssize_t)CHUNK) {
      say("%s: pread at %llu: errno %d", what, (unsigned long long)off, errno);
      break;
    }
    got += CHUNK;
  }
  const double s = now_s() - t0;
  const double mbs = s > 0 ? got / s / 1e6 : 0;
  say("%s: %llu MiB in %.1f s = %.2f MB/s = %.2fx DVD", what, (unsigned long long)(got >> 20), s, mbs, mbs / 1.385);
  return got == TIMED ? mbs : 0;
}

static int open_cd(pid_t pid) {
  int fd = open("/dev/cd0", O_RDONLY);
  if (fd >= 0 || (errno != EPERM && errno != EACCES))
    return fd;
  const uint64_t authid = kernel_get_ucred_authid(pid);
  uint8_t caps[16], all[16];
  memset(all, 0xff, sizeof(all));
  kernel_get_ucred_caps(pid, caps);
  kernel_set_ucred_authid(pid, 0x4800000000010003ull);
  kernel_set_ucred_caps(pid, all);
  fd = open("/dev/cd0", O_RDONLY);
  const int e = errno;
  kernel_set_ucred_authid(pid, authid);
  kernel_set_ucred_caps(pid, caps);
  errno = e;
  return fd;
}

int main(void) {
  mkdir("/data/PCSX2/logs", 0777);
  g_log = fopen("/data/PCSX2/logs/disctest.log", "w");
  const pid_t pid = getpid();
  say("v7 (Sony mode page 31 bytes 4-7, not saved), pid %d, uid %d, authid %#llx", (int)pid, (int)getuid(),
      (unsigned long long)kernel_get_ucred_authid(pid));
  notify("PS5SX2 disc test v7: running (about 2 minutes)");

  const int fd = open_cd(pid);
  if (fd < 0) {
    say("/dev/cd0: open errno %d", errno);
    notify("PS5SX2 disc test: can't open the drive (errno %d)", errno);
    return 1;
  }
  off_t size = 0;
  if (ioctl(fd, DIOCGMEDIASIZE, &size) != 0 || size < (off_t)(512ull << 20)) {
    say("/dev/cd0: no disc of 512 MiB or more (size %lld, errno %d)", (long long)size, errno);
    notify("PS5SX2 disc test: put a PS2 DVD in the drive");
    close(fd);
    return 1;
  }
  const uint64_t sz = (uint64_t)size;
  say("/dev/cd0: %llu bytes", (unsigned long long)sz);

  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  ccb.ccb_h.func_code = XPT_GDEVLIST;
  if (ioctl(fd, CAMGETPASSTHRU, &ccb) != 0) {
    say("CAMGETPASSTHRU: errno %d", errno);
    notify("PS5SX2 disc test: no pass device (errno %d)", errno);
    close(fd);
    return 1;
  }
  char path[64];
  snprintf(path, sizeof(path), "/dev/%.*s%u", (int)sizeof(ccb.cgdl.periph_name), ccb.cgdl.periph_name, ccb.cgdl.unit_number);
  const int pass = open(path, O_RDWR);
  say("%s: %s%d", path, pass < 0 ? "open errno " : "fd ", pass < 0 ? errno : pass);
  if (pass < 0) {
    notify("PS5SX2 disc test: can't open %s", path);
    close(fd);
    return 1;
  }

  say("current profile: %#x", current_profile(pass));
  (void)describe_drive; /* v5's full listing, not needed again */
  (void)set_speed_raw;  /* v5's DB flag sweep */

  uint8_t p31[64], p32[64];
  const int n31 = read_page(pass, 0x31, p31, sizeof(p31)), n32 = read_page(pass, 0x32, p32, sizeof(p32));
  say("page 31: %d bytes, page 32: %d bytes", n31, n32);
  if (n31 < 8 || n32 < 5) {
    notify("PS5SX2 disc test v6: can't read pages 31/32 (see disctest.log)");
    close(pass);
    close(fd);
    return 1;
  }
  hexdump("page 31 before", p31, (uint32_t)n31);
  hexdump("page 32 before", p32, (uint32_t)n32);

  static const struct {
    uint8_t byte;  /* 4: bytes 4-5, 6: bytes 6-7 */
    uint16_t value;
  } kTry[] = {{4, 0x0000}, {4, 0x0450}, {4, 0x1130}, {4, 0x2B48}, {4, 0xFFFF}, {6, 0x0000}, {6, 0x0032}, {6, 0x00C8}, {6, 0xFFFF}};
  const int n = (int)(sizeof(kTry) / sizeof(kTry[0]));
  char line[3000];
  const uint64_t step = 40ull << 20, first = 64ull << 20;
  const double base = timed_read(fd, first, sz, "as the drive is");
  int at = snprintf(line, sizeof(line), "PS5SX2 disc test v7: as is %.1f MB/s (%.1fx)", base, base / 1.385);
  for (int i = 0; i < n; i++) {
    uint8_t page[64];
    memcpy(page, p31, (size_t)n31);
    page[kTry[i].byte] = (uint8_t)(kTry[i].value >> 8);
    page[kTry[i].byte + 1] = (uint8_t)kTry[i].value;
    char what[64];
    snprintf(what, sizeof(what), "page 31 bytes %u-%u = %#06x", kTry[i].byte, kTry[i].byte + 1, kTry[i].value);
    if (write_page(pass, page, n31, what) != 0) {
      at += snprintf(line + at, sizeof(line) - at, " | p31[%u]=%04x refused", kTry[i].byte, kTry[i].value);
      continue;
    }
    uint8_t check[64];
    if (read_page(pass, 0x31, check, sizeof(check)) == n31)
      hexdump("  reads back", check, (uint32_t)n31);
    const uint64_t from = first + (uint64_t)(i + 1) * step;
    char w1[96];
    snprintf(w1, sizeof(w1), "%s, no speed command", what);
    const double plain = timed_read(fd, from, sz, w1);
    const char *taken = NULL;
    double fast = 0;
    if (set_speed(pass, 0, 0x80) == 0)
      taken = "DB 00 00 80";
    else if (set_speed(pass, 1, 0x80) == 0)
      taken = "DB 01 00 80";
    if (taken) {
      char w2[96];
      snprintf(w2, sizeof(w2), "%s, after %s", what, taken);
      fast = timed_read(fd, from + (20ull << 20), sz, w2);
    }
    set_speed(pass, 0, 0x20);
    write_page(pass, p31, n31, "back to the original");
    if (at < (int)sizeof(line) - 80)
      at += snprintf(line + at, sizeof(line) - at, " | p31[%u]=%04x: %.1f MB/s%s%s%s", kTry[i].byte, kTry[i].value, plain,
                     taken ? ", " : "", taken ? taken : "", taken ? " taken" : "");
    if (taken && at < (int)sizeof(line) - 40)
      at += snprintf(line + at, sizeof(line) - at, " %.1f MB/s", fast);
  }
  write_page(pass, p31, n31, "page 31 back to the original (end)");
  write_page(pass, p32, n32, "page 32 back to the original (end)");
  set_speed(pass, 0, 0x20); /* back to the drive's default, 2.0, as SceShellCore does */
  close(pass);
  close(fd);

  notify("%s", line);
  say("%s", line);
  say("done (drive back to 2.0)");
  if (g_log)
    fclose(g_log);
  return 0;
}
