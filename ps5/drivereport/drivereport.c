/* PS5 disc drive report v2 (AI-assisted, from PS5SX2's disc speed tests).
 *
 * For comparing PS5 drives across system firmwares: the console firmware, what the drive is, everything it says about
 * itself (every GET CONFIGURATION feature, Sony's ff10..ff60 included, and every mode page), and which of Sony's speed
 * settings (SceShellCore's DB command) and the standard MMC speed commands it takes for the disc in it, each timed.
 *
 * Known so far: on 13.xx (SONY PS-SYSTEM 502R fw 1305, 503R fw 2305) a PS2 DVD only takes DB 00 00 20 (2.0x, CLV, the
 * default) and DB 00 00 32 (3.2x, CLV). Someone dumps PS2 DVDs at ~8x on 10.xx: this report from a 10.xx console tells
 * which command and setting does it there, so PS5SX2 can use it where the drive allows it.
 *
 * Note on timing: CAV speed depends on where on the disc it reads (an "8x CAV" reads ~3.3x at the inner edge), so each
 * accepted setting is timed at the inner part of the disc, and the fastest one again at the outer part.
 *
 * Send it to the ELF loader (port 9021) with a PS2 DVD in the drive. It only reads the disc, never writes it, and puts the
 * drive back to 2.0 at the end. Takes 1-4 minutes. Report: a notification, klog ([drive report]) and
 * /data/drive-report.txt (send that file).
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
#include <sys/sysctl.h>
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

#define REPORT_VERSION "v2"

static FILE *g_out;
static char g_sum[2800];
static int g_at;

static void say(const char *fmt, ...) {
  char line[700];
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

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* Hex of up to max bytes into out ("..." when cut). */
static const char *hex(const uint8_t *p, size_t n, size_t max) {
  static char out[400];
  size_t at = 0;
  const size_t m = n < max ? n : max;
  for (size_t i = 0; i < m && at + 4 < sizeof(out); i++)
    at += (size_t)snprintf(out + at, sizeof(out) - at, "%02x%s", p[i], (i % 4 == 3 && i + 1 < m) ? " " : "");
  out[at] = 0;
  if (m < n && at + 4 < sizeof(out))
    snprintf(out + at, sizeof(out) - at, "...");
  return out;
}

/* One command, data in (CAM_DIR_IN), out (CAM_DIR_OUT) or none. 0 when done; else why = "errno N" or "sense K/ASC/ASCQ". */
static int cmd(int pass, const uint8_t *cdb, int cdb_len, uint32_t dir, uint8_t *data, uint32_t len, char *why, size_t whylen) {
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  cam_fill_csio(&ccb.csio, 1, NULL, dir | CAM_DEV_QFRZDIS | (dir == CAM_DIR_NONE ? CAM_PASS_ERR_RECOVER : 0), MSG_SIMPLE_Q_TAG, data,
                len, SSD_FULL_SIZE, (uint8_t)cdb_len, 15000);
  memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, (size_t)cdb_len);
  if (ioctl(pass, CAMIOCOMMAND, &ccb) != 0) {
    snprintf(why, whylen, "errno %d", errno);
    return -1;
  }
  if ((ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP)
    return 0;
  const uint8_t *s = (const uint8_t *)&ccb.csio.sense_data;
  snprintf(why, whylen, "sense %X/%02X/%02X (cam %#x)", s[2] & 15, s[12], s[13], ccb.ccb_h.status & CAM_STATUS_MASK);
  return -1;
}

/* MB/s of pread on fd over 16 MiB at from (after a 4 MiB warm-up read that also lets the drive spin up). */
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

static int sony_speed(int pass, uint8_t rot, uint16_t speed, char *why, size_t whylen) {
  const uint8_t c_db[12] = {0xDB, rot, (uint8_t)(speed >> 8), (uint8_t)speed, 0, 0, 0, 0, 0, 0, 0, 0};
  return cmd(pass, c_db, 12, CAM_DIR_NONE, NULL, 0, why, whylen);
}

static void report_firmware(void) {
  const uint32_t v = kernel_get_fw_version();
  char sdk[64] = "";
  uint32_t sdkv = 0;
  size_t sl = sizeof(sdkv);
  if (sysctlbyname("kern.sdk_version", &sdkv, &sl, NULL, 0) == 0)
    snprintf(sdk, sizeof(sdk), "%x.%03x", sdkv >> 24, (sdkv >> 12) & 0xfff);
  say("PS5 drive report %s", REPORT_VERSION);
  say("console firmware: %x.%02x (kernel %#x, kern.sdk_version %s)", v >> 24, (v >> 16) & 0xff, v, sdk[0] ? sdk : "?");
  sum("FW %x.%02x", v >> 24, (v >> 16) & 0xff);
}

/* Every feature the drive lists (GET CONFIGURATION, RT 0), current or not, with its data. */
static void report_features(int pass) {
  static uint8_t buf[8192];
  char why[64];
  memset(buf, 0, sizeof(buf));
  const uint8_t c[10] = {0x46, 0x00, 0, 0, 0, 0, 0, (uint8_t)(sizeof(buf) >> 8), (uint8_t)sizeof(buf), 0};
  if (cmd(pass, c, 10, CAM_DIR_IN, buf, sizeof(buf), why, sizeof(why)) != 0) {
    say("GET CONFIGURATION: %s", why);
    return;
  }
  uint32_t total = be32(buf) + 4;
  if (total > sizeof(buf))
    total = sizeof(buf);
  say("GET CONFIGURATION: %u bytes, current profile %#06x", total, be16(buf + 6));
  for (uint32_t at = 8; at + 4 <= total;) {
    const uint16_t code = be16(buf + at);
    const uint8_t flags = buf[at + 2], n = buf[at + 3];
    if (at + 4 + n > total)
      break;
    say("  feature %04x v%u %s%s: %s", code, (flags >> 2) & 15, (flags & 1) ? "current" : "not current", (flags & 2) ? " persistent" : "",
        n ? hex(buf + at + 4, n, 96) : "-");
    at += 4 + n;
  }
}

/* Every mode page (MODE SENSE(10), current values, then the changeable mask). */
static void report_pages(int pass) {
  static uint8_t buf[4096];
  char why[64];
  for (int pc = 0; pc < 2; pc++) {
    memset(buf, 0, sizeof(buf));
    const uint8_t c[10] = {0x5A, 0x08, (uint8_t)(pc << 6 | 0x3F), 0, 0, 0, 0, (uint8_t)(sizeof(buf) >> 8), (uint8_t)sizeof(buf), 0};
    const char *what = pc ? "changeable" : "current";
    if (cmd(pass, c, 10, CAM_DIR_IN, buf, sizeof(buf), why, sizeof(why)) == 0) {
      uint32_t total = (uint32_t)be16(buf) + 2;
      if (total > sizeof(buf))
        total = sizeof(buf);
      const uint32_t bd = be16(buf + 6);
      say("MODE SENSE all pages (%s): %u bytes", what, total);
      for (uint32_t at = 8 + bd; at + 2 <= total;) {
        const uint8_t page = buf[at] & 0x3F, n = buf[at + 1];
        if (at + 2 + n > total || (page == 0 && n == 0))
          break;
        say("  page %02x (%u): %s", page, n, hex(buf + at + 2, n, 96));
        at += 2u + n;
      }
      continue;
    }
    say("MODE SENSE all pages (%s): %s; one at a time", what, why);
    for (int page = 1; page < 0x3F; page++) {
      memset(buf, 0, 256);
      const uint8_t c1[10] = {0x5A, 0x08, (uint8_t)(pc << 6 | page), 0, 0, 0, 0, 1, 0, 0};
      if (cmd(pass, c1, 10, CAM_DIR_IN, buf, 256, why, sizeof(why)) != 0)
        continue;
      const uint32_t bd = be16(buf + 6), at = 8 + bd;
      const uint8_t n = buf[at + 1];
      say("  page %02x (%u): %s", buf[at] & 0x3F, n, hex(buf + at + 2, n, 96));
    }
  }
}

/* GET PERFORMANCE: nominal read performance (all descriptors) and the drive's speed list (type 3). */
static void report_performance(int pass) {
  static uint8_t buf[1024];
  char why[64];
  memset(buf, 0, sizeof(buf));
  const uint8_t c[12] = {0xAC, 0x00, 0, 0, 0, 0, 0, 0, 0, 32, 0x00, 0};
  if (cmd(pass, c, 12, CAM_DIR_IN, buf, sizeof(buf), why, sizeof(why)) == 0) {
    const uint32_t n = (be32(buf) > 4 ? be32(buf) - 4 : 0) / 16;
    for (uint32_t i = 0; i < n && i < 32; i++) {
      const uint8_t *d = buf + 8 + 16 * i;
      say("GET PERFORMANCE read %u: LBA %u at %u kB/s .. LBA %u at %u kB/s", i, be32(d), be32(d + 4), be32(d + 8), be32(d + 12));
      if (i == 0)
        sum(" | perf %u..%u kB/s", be32(d + 4), be32(d + 12));
    }
  } else
    say("GET PERFORMANCE read: %s", why);
  memset(buf, 0, sizeof(buf));
  const uint8_t c3[12] = {0xAC, 0x00, 0, 0, 0, 0, 0, 0, 0, 32, 0x03, 0};
  if (cmd(pass, c3, 12, CAM_DIR_IN, buf, sizeof(buf), why, sizeof(why)) == 0) {
    const uint32_t n = (be32(buf) > 4 ? be32(buf) - 4 : 0) / 16;
    for (uint32_t i = 0; i < n && i < 32; i++) {
      const uint8_t *d = buf + 8 + 16 * i;
      say("GET PERFORMANCE speed %u: flags %02x, end LBA %u, read %u kB/s, write %u kB/s", i, d[0], be32(d + 4), be32(d + 8), be32(d + 12));
    }
  } else
    say("GET PERFORMANCE speed list: %s", why);
}

int main(void) {
  g_out = fopen("/data/drive-report.txt", "w");
  notify("Drive report " REPORT_VERSION ": running (1-4 minutes)");
  report_firmware();
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
  if (ioctl(fd, DIOCGMEDIASIZE, &size) != 0 || size < (off_t)(512ull << 20)) {
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
  uint8_t inq[96] = {0};
  const uint8_t c_inq[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
  if (cmd(pass, c_inq, 6, CAM_DIR_IN, inq, sizeof(inq), why, sizeof(why)) == 0) {
    say("drive: %.8s | %.16s | %.4s", inq + 8, inq + 16, inq + 32);
    say("INQUIRY: %s", hex(inq, sizeof(inq), sizeof(inq)));
    sum(" | %.16s fw %.4s", inq + 16, inq + 32);
  }
  uint8_t gc[8] = {0};
  const uint8_t c_gc[10] = {0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0};
  uint16_t profile = 0;
  if (cmd(pass, c_gc, 10, CAM_DIR_IN, gc, sizeof(gc), why, sizeof(why)) == 0) {
    profile = be16(gc + 6);
    say("disc profile %#x, %lld bytes (%s)", profile, (long long)size, size > 4700000000ll ? "dual layer" : "single layer");
    sum(" | disc %#x %.2f GB", profile, size / 1e9);
  }
  report_features(pass);
  report_pages(pass);
  report_performance(pass);

  /* Where to time: the inner part (from 64 MiB on), and the outer edge of layer 0 (PS2 DVD-9s are opposite track
   * path, so the end of the image is back at the inner edge; layer 0's outer edge is about the middle of the image). */
  const uint64_t usize = (uint64_t)size;
  const uint64_t outer = (usize > 4700000000ull ? usize / 2 : usize) - (160ull << 20);
  uint64_t inner_at = 64ull << 20;
  const double base = timed(fd, inner_at, usize);
  const double base_o = timed(fd, outer, usize);
  say("as the drive is: inner %.2f MB/s (%.2fx DVD), outer (offset %llu MiB) %.2f MB/s (%.2fx)", base, base / 1.385,
      (unsigned long long)(outer >> 20), base_o, base_o / 1.385);
  sum(" | as is %.1f/%.1f MB/s", base, base_o);

  /* Sony's DB command: every rotation 0..3 with a range of speeds; refused ones cost nothing, taken ones are timed. */
  static const uint16_t kSpeeds[] = {0x0020, 0x0032, 0x0040, 0x0050, 0x0060, 0x0080, 0x00A0, 0x00C0, 0x0100, 0x0180, 0xFFFF};
  double best = 0;
  int best_rot = 0;
  uint16_t best_speed = 0x20;
  int refused = 0;
  sum(" | DB taken:");
  for (int rot = 0; rot < 4; rot++) {
    for (int i = 0; i < (int)(sizeof(kSpeeds) / sizeof(kSpeeds[0])); i++) {
      const uint16_t sp = kSpeeds[i];
      if (sony_speed(pass, (uint8_t)rot, sp, why, sizeof(why)) != 0) {
        say("DB %02x %02x %02x: refused, %s", rot, sp >> 8, sp & 0xff, why);
        refused++;
        continue;
      }
      inner_at += 32ull << 20;
      if (inner_at + (64ull << 20) > outer)
        inner_at = 96ull << 20;
      const double mbs = timed(fd, inner_at, usize);
      say("DB %02x %02x %02x: taken, inner %.2f MB/s (%.2fx DVD)", rot, sp >> 8, sp & 0xff, mbs, mbs / 1.385);
      sum(" r%d/%x %.1f", rot, sp, mbs);
      if (mbs > best) {
        best = mbs;
        best_rot = rot;
        best_speed = sp;
      }
    }
  }
  sum(" (%d refused)", refused);
  if (best > 0 && sony_speed(pass, (uint8_t)best_rot, best_speed, why, sizeof(why)) == 0) {
    const double o = timed(fd, outer, usize);
    say("fastest DB %02x %02x %02x again at the outer part: %.2f MB/s (%.2fx DVD)", best_rot, best_speed >> 8, best_speed & 0xff, o, o / 1.385);
    sum(" | best r%d/%x outer %.1f MB/s", best_rot, best_speed, o);
  }
  sony_speed(pass, 0, 0x20, why, sizeof(why));

  /* The standard MMC speed commands. SET CD SPEED: read speed max (0xFFFF) and 8x DVD (11080 kB/s). */
  static const uint16_t kCd[] = {0xFFFF, 11080};
  for (int i = 0; i < 2; i++) {
    const uint8_t c[12] = {0xBB, 0x00, (uint8_t)(kCd[i] >> 8), (uint8_t)kCd[i], 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    if (cmd(pass, c, 12, CAM_DIR_NONE, NULL, 0, why, sizeof(why)) != 0) {
      say("SET CD SPEED %u: refused, %s", kCd[i], why);
      sum(" | BB %u no", kCd[i]);
      continue;
    }
    const double o = timed(fd, outer, usize);
    say("SET CD SPEED %u: taken, outer %.2f MB/s (%.2fx DVD)", kCd[i], o, o / 1.385);
    sum(" | BB %u %.1f", kCd[i], o);
  }
  /* SET STREAMING: the whole disc at 11080 kB/s (8x DVD), then the drive's defaults back (RDD). */
  {
    uint8_t p[28] = {0};
    const uint32_t end = (uint32_t)(usize / 2048 - 1);
    p[8] = (uint8_t)(end >> 24), p[9] = (uint8_t)(end >> 16), p[10] = (uint8_t)(end >> 8), p[11] = (uint8_t)end;
    p[12] = 0, p[13] = 0, p[14] = 11080 >> 8, p[15] = 11080 & 0xff; /* read size kB */
    p[18] = 1000 >> 8, p[19] = 1000 & 0xff;                          /* per 1000 ms */
    p[22] = 11080 >> 8, p[23] = 11080 & 0xff, p[26] = 1000 >> 8, p[27] = 1000 & 0xff;
    const uint8_t c[12] = {0xB6, 0, 0, 0, 0, 0, 0, 0, 0, 0, sizeof(p), 0};
    if (cmd(pass, c, 12, CAM_DIR_OUT, p, sizeof(p), why, sizeof(why)) != 0) {
      say("SET STREAMING 11080 kB/s: refused, %s", why);
      sum(" | B6 no");
    } else {
      const double o = timed(fd, outer, usize);
      say("SET STREAMING 11080 kB/s: taken, outer %.2f MB/s (%.2fx DVD)", o, o / 1.385);
      sum(" | B6 %.1f", o);
    }
    memset(p, 0, sizeof(p));
    p[0] = 0x04; /* RDD: restore the drive's defaults */
    cmd(pass, c, 12, CAM_DIR_OUT, p, sizeof(p), why, sizeof(why));
  }
  sony_speed(pass, 0, 0x20, why, sizeof(why));
  say("drive back to 2.0");
  close(pass);
  close(fd);
  say("summary: %s", g_sum);
  notify(g_sum);
  if (g_out)
    fclose(g_out);
  return 0;
}
