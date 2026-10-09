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
  say("v3 (Sony speed command, DVD variants), pid %d, uid %d, authid %#llx", (int)pid, (int)getuid(),
      (unsigned long long)kernel_get_ucred_authid(pid));
  notify("PS5SX2 disc test v3: running (about 1 to 3 minutes)");

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

  static const struct {
    uint8_t rot;
    uint16_t speed;
  } kTry[] = {{0, 0x0080}, {0, 0x0050}, {0, 0x0032}, {2, 0x0080}, {2, 0x0050}, {2, 0x0040}, {3, 0x0080}};
  const int n = (int)(sizeof(kTry) / sizeof(kTry[0]));
  double res[16] = {0};
  const uint64_t step = 32ull << 20, first = 64ull << 20;
  say("current profile: %#x", current_profile(pass));

  const double base = timed_read(fd, first, sz, "as the drive is");
  for (int i = 0; i < n; i++) {
    char what[64], st[16];
    snprintf(what, sizeof(what), "after DB rotation %u, %s", kTry[i].rot, speed_text(kTry[i].speed, st, sizeof(st)));
    if (set_speed(pass, kTry[i].rot, kTry[i].speed) == 0)
      res[i] = timed_read(fd, first + (uint64_t)(i + 1) * step, sz, what);
    else
      res[i] = -1;
  }
  /* the best one again, at the end of the disc (a drive spinning at a set rate reads the outer edge fastest) */
  int best = -1;
  for (int i = 0; i < n; i++)
    if (res[i] > 0 && (best < 0 || res[i] > res[best]))
      best = i;
  double end = 0;
  if (best >= 0 && set_speed(pass, kTry[best].rot, kTry[best].speed) == 0)
    end = timed_read(fd, sz - TIMED - (16ull << 20), sz, "the fastest setting, at the end of the disc");

  set_speed(pass, 0, 0x20); /* back to the drive's default, 2.0, as SceShellCore does */
  close(pass);
  close(fd);

  char line[3000];
  int at = snprintf(line, sizeof(line), "PS5SX2 disc test v3: as is %.1f MB/s (%.1fx)", base, base / 1.385);
  for (int i = 0; i < n && at < (int)sizeof(line) - 80; i++) {
    char st[16];
    if (res[i] < 0)
      at += snprintf(line + at, sizeof(line) - at, " | r%u %s refused", kTry[i].rot, speed_text(kTry[i].speed, st, sizeof(st)));
    else
      at += snprintf(line + at, sizeof(line) - at, " | r%u %s: %.1f MB/s (%.1fx)", kTry[i].rot, speed_text(kTry[i].speed, st, sizeof(st)),
                     res[i], res[i] / 1.385);
  }
  if (best >= 0 && at < (int)sizeof(line) - 60)
    snprintf(line + at, sizeof(line) - at, " | end of disc %.1f MB/s (%.1fx)", end, end / 1.385);
  notify("%s", line);
  say("%s", line);
  say("done (drive back to 2.0)");
  if (g_log)
    fclose(g_log);
  return 0;
}
