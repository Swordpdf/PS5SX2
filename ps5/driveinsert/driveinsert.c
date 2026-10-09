/* PS5 disc drive insert test v1 (AI-assisted): does the drive take a fast (CAV) speed while a disc is loading?
 *
 * Once a PS2 DVD is loaded, the drive takes Sony's DB speed command only at 2.0 and 3.2 (CLV) and refuses the faster
 * settings (rotation 1/2) as an invalid field, on every console tested (4.03, 13.60). Someone reads a PS2 DVD at ~8x on
 * his PS5's own drive (10.4 MiB/s at the end of the disc, 10 minutes for GTA Vice City: a CAV signature). Untested so
 * far: the drive while it's still working out what the disc is. This waits for the disc to come out and go back in, then
 * from the moment the drive sees it, keeps asking for the faster settings (every few ms) and notes the drive's state
 * (TEST UNIT READY sense, current profile) each time it changes. A setting taken at any point is timed once the disc is
 * ready (inner and outer part of the disc).
 *
 * Only Sony's own speed command and reads; the drive goes back to 2.0 at the end. Steps: send to the ELF loader (port
 * 9021); when told, eject the disc with the PS5's eject button, then put the PS2 DVD back in. Report: notification,
 * klog ([drive insert]), /data/drive-insert.txt.
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

#define TEST_VERSION "v1"

static FILE *g_out;
static char g_sum[2000];
static int g_at;
static int g_pass = -1;
static double g_t0;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *fmt, ...) {
  char line[600];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  const double t = g_t0 > 0 ? now_s() - g_t0 : 0;
  klog_printf("[drive insert] %8.3f %s\n", t, line);
  if (g_out) {
    fprintf(g_out, "%8.3f %s\n", t, line);
    fflush(g_out);
  }
}

static void sum(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (g_at < (int)sizeof(g_sum) - 1)
    g_at += vsnprintf(g_sum + g_at, sizeof(g_sum) - g_at, fmt, ap);
  if (g_at > (int)sizeof(g_sum) - 1)
    g_at = (int)sizeof(g_sum) - 1;
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

static int open_dev(const char *path, int flags) {
  int fd = open(path, flags);
  if (fd < 0 && (errno == EPERM || errno == EACCES)) {
    const pid_t pid = getpid();
    const uint64_t authid = kernel_get_ucred_authid(pid);
    uint8_t caps[16], all[16];
    memset(all, 0xff, sizeof(all));
    kernel_get_ucred_caps(pid, caps);
    kernel_set_ucred_authid(pid, 0x4800000000010003ull);
    kernel_set_ucred_caps(pid, all);
    fd = open(path, flags);
    const int e = errno;
    kernel_set_ucred_authid(pid, authid);
    kernel_set_ucred_caps(pid, caps);
    errno = e;
  }
  return fd;
}

/* One command. Returns 0 when done, else the sense as key<<16 | asc<<8 | ascq (or 0xFF0000 | errno-ish for a failed
 * ioctl / no sense). */
static int cmd(const uint8_t *cdb, int cdb_len, uint32_t dir, uint8_t *data, uint32_t len) {
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  cam_fill_csio(&ccb.csio, 0, NULL, dir | CAM_DEV_QFRZDIS | (dir == CAM_DIR_NONE ? CAM_PASS_ERR_RECOVER : 0), MSG_SIMPLE_Q_TAG, data,
                len, SSD_FULL_SIZE, (uint8_t)cdb_len, 10000);
  memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, (size_t)cdb_len);
  if (ioctl(g_pass, CAMIOCOMMAND, &ccb) != 0)
    return 0xFE0000 | (errno & 0xffff);
  const uint32_t st = ccb.ccb_h.status & CAM_STATUS_MASK;
  if (st == CAM_REQ_CMP)
    return 0;
  if (!(ccb.ccb_h.status & CAM_AUTOSNS_VALID))
    return 0xFF0000 | st;
  const uint8_t *s = (const uint8_t *)&ccb.csio.sense_data;
  return (s[2] & 15) << 16 | s[12] << 8 | s[13];
}

static int tur(void) {
  const uint8_t c[6] = {0, 0, 0, 0, 0, 0};
  return cmd(c, 6, CAM_DIR_NONE, NULL, 0);
}

static int profile(void) {
  uint8_t gc[8] = {0};
  const uint8_t c[10] = {0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0};
  return cmd(c, 10, CAM_DIR_IN, gc, sizeof(gc)) == 0 ? (gc[6] << 8 | gc[7]) : -1;
}

static int db(uint8_t rot, uint16_t speed) {
  const uint8_t c[12] = {0xDB, rot, (uint8_t)(speed >> 8), (uint8_t)speed, 0, 0, 0, 0, 0, 0, 0, 0};
  return cmd(c, 12, CAM_DIR_NONE, NULL, 0);
}

static const char *state_name(int s) {
  static char b[48];
  if (s == 0)
    return "ready";
  if (s == 0x023A00 || s == 0x023A01 || s == 0x023A02)
    return "no disc";
  if (s == 0x020401)
    return "becoming ready";
  if (s == 0x062800)
    return "disc changed";
  if (s == 0x062900)
    return "reset";
  snprintf(b, sizeof(b), "sense %06x", s);
  return b;
}

static double timed(int fd, uint64_t from, uint64_t size) {
  static uint8_t *buf;
  const uint32_t chunk = 4u << 20;
  if (!buf && !(buf = malloc(chunk)))
    return 0;
  from &= ~(uint64_t)2047;
  if (from + 5 * (uint64_t)chunk > size)
    return 0;
  pread(fd, buf, chunk, (off_t)from);
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

static const struct {
  uint8_t rot;
  uint16_t speed;
} kFast[] = {{1, 0x0080}, {0, 0x0080}, {2, 0x0040}, {1, 0x0060}, {2, 0x0050}, {1, 0x0050}};
#define NFAST ((int)(sizeof(kFast) / sizeof(kFast[0])))

int main(void) {
  g_out = fopen("/data/drive-insert.txt", "w");
  const uint32_t v = kernel_get_fw_version();
  say("PS5 drive insert test %s, console firmware %x.%02x", TEST_VERSION, v >> 24, (v >> 16) & 0xff);
  sum("Insert test FW %x.%02x", v >> 24, (v >> 16) & 0xff);
  for (int i = 0; i < 16 && g_pass < 0; i++) {
    char p[32];
    snprintf(p, sizeof(p), "/dev/pass%d", i);
    if (access(p, F_OK) != 0)
      continue;
    const int fd = open_dev(p, O_RDWR);
    if (fd < 0)
      continue;
    uint8_t inq[36] = {0};
    g_pass = fd;
    const uint8_t c[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
    if (cmd(c, 6, CAM_DIR_IN, inq, sizeof(inq)) == 0 && (inq[0] & 0x1f) == 5) {
      say("drive: %s, %.16s fw %.4s", p, inq + 16, inq + 32);
      sum(" | %.16s fw %.4s", inq + 16, inq + 32);
    } else {
      close(fd);
      g_pass = -1;
    }
  }
  if (g_pass < 0) {
    say("no optical pass device");
    notify("Drive insert test: no drive found");
    return 1;
  }

  /* 1. The disc out. */
  g_t0 = now_s();
  int s = tur();
  if (s != 0x023A00 && s != 0x023A01 && s != 0x023A02) {
    notify("Drive insert test: EJECT the disc now (PS5 eject button). Then put the PS2 DVD back in when asked.");
    say("waiting for the disc to come out (state %s)", state_name(s));
    const double until = now_s() + 90;
    while (now_s() < until) {
      s = tur();
      if (s == 0x023A00 || s == 0x023A01 || s == 0x023A02)
        break;
      usleep(100000);
    }
    if (s != 0x023A00 && s != 0x023A01 && s != 0x023A02) {
      say("the disc didn't come out in 90 s (state %s)", state_name(s));
      notify("Drive insert test: the disc wasn't ejected; run it again");
      return 1;
    }
  }
  say("no disc in the drive");
  sleep(1);
  notify("Drive insert test: now PUT THE PS2 DVD IN");

  /* 2. From the moment the drive notices the disc: state changes, and the fast settings over and over. */
  int last_state = s, last_prof = -2, i = 0, taken = -1;
  int last_db[NFAST];
  for (int k = 0; k < NFAST; k++)
    last_db[k] = -1;
  double seen = 0, ready_at = 0;
  const double until = now_s() + 120;
  while (now_s() < until) {
    s = tur();
    if (s != last_state) {
      const int p = profile();
      say("state: %s, profile %#06x", state_name(s), p);
      last_state = s;
      last_prof = p;
      if (!seen && s != 0x023A00 && s != 0x023A01 && s != 0x023A02)
        seen = now_s();
    } else if (seen && (i % 20) == 0) {
      const int p = profile();
      if (p != last_prof) {
        say("profile now %#06x (state %s)", p, state_name(s));
        last_prof = p;
      }
    }
    if (seen && taken < 0) {
      const int k = i % NFAST;
      const int r = db(kFast[k].rot, kFast[k].speed);
      if (r != last_db[k]) {
        say("DB %02x %04x: %s (state %s)", kFast[k].rot, kFast[k].speed, r == 0 ? "TAKEN" : "refused", state_name(s));
        if (r)
          say("  refusal %06x", r);
        last_db[k] = r;
      }
      if (r == 0) {
        taken = k;
        sum(" | TAKEN r%d/%x while %s", kFast[k].rot, kFast[k].speed, state_name(s));
      }
    }
    if (s == 0 && !ready_at)
      ready_at = now_s();
    /* keep going 10 s past ready (the shell's own disc checks happen then) */
    if (ready_at && now_s() - ready_at > 10)
      break;
    i++;
    usleep(seen ? 3000 : 20000);
  }
  if (!seen) {
    say("no disc was put in within 2 minutes");
    notify("Drive insert test: no disc was put in");
    return 1;
  }
  say("disc noticed %.2f s, ready %.2f s after", seen - g_t0, ready_at ? ready_at - g_t0 : -1.0);

  /* 3. Timing, and whether a taken setting stuck. */
  sleep(2);
  const int fd = open_dev("/dev/cd0", O_RDONLY);
  off_t size = 0;
  if (fd >= 0 && ioctl(fd, DIOCGMEDIASIZE, &size) == 0 && size > (off_t)(512ll << 20)) {
    const uint64_t usize = (uint64_t)size;
    const uint64_t outer = (usize > 4700000000ull ? usize / 2 : usize) - (160ull << 20);
    const double in = timed(fd, 64ull << 20, usize), out = timed(fd, outer, usize);
    say("reads now: inner %.2f MB/s (%.2fx DVD), outer %.2f MB/s (%.2fx)", in, in / 1.385, out, out / 1.385);
    sum(" | reads %.1f/%.1f MB/s", in, out);
    if (taken < 0) {
      /* once more, now that it's loaded: the expected refusals, for comparison */
      for (int k = 0; k < NFAST; k++) {
        const int r = db(kFast[k].rot, kFast[k].speed);
        say("loaded: DB %02x %04x %s (%06x)", kFast[k].rot, kFast[k].speed, r == 0 ? "TAKEN" : "refused", r);
        if (r == 0) {
          const double in2 = timed(fd, 160ull << 20, usize), out2 = timed(fd, outer - (64ull << 20), usize);
          say("  reads: inner %.2f MB/s, outer %.2f MB/s", in2, out2);
          sum(" | loaded r%d/%x %.1f/%.1f", kFast[k].rot, kFast[k].speed, in2, out2);
        }
      }
    }
  } else
    say("/dev/cd0: open %d errno %d, size %lld", fd, errno, (long long)size);
  if (fd >= 0)
    close(fd);
  db(0, 0x20);
  say("drive back to 2.0");
  sum(taken < 0 ? " | no fast setting taken while loading" : "");
  say("summary: %s", g_sum);
  notify(g_sum);
  close(g_pass);
  if (g_out)
    fclose(g_out);
  return 0;
}
