/* PS5 disc drive probe v1 (AI-assisted): looking for a way past the drive's 3.2x on PS2 DVDs.
 *
 * DriveReport v2 showed a 4.03 console (drive fw 1137) and a 13.60 one (fw 1305) behaving the same: Sony's DB speed
 * command takes only 2.0 and 3.2 (rotation 0) for a DVD, SET CD SPEED / SET STREAMING are refused as unknown opcodes.
 * This tries what no earlier test did:
 *   1. mode page 2D (vendor, byte 0 fully changeable, never tried): each bit set, then the faster DB settings;
 *   2. page 31 byte 0 (changeable bits 0x03) and page 32's changeable bits, every combination, then the DB settings;
 *   3. the DB settings sent while the disc is stopped (START STOP UNIT, never ejects), then spun up again;
 *   4. a survey of the vendor opcodes C0..FF: which ones the drive knows. Only run when the drive is seen to refuse a
 *      command whose control byte is invalid (a TEST UNIT READY, INQUIRY, MODE SENSE, GET PERFORMANCE and DB with the
 *      control byte's Link bit set), and every survey command carries that invalid control byte (in the 6-, 10- and
 *      12-byte positions), so a known command is refused before it does anything. Otherwise the survey is skipped.
 * Any DB setting taken that wasn't before is timed (inner and outer part of the disc).
 *
 * Nothing is saved in the drive (MODE SELECT with SP 0); every page is put back and the drive goes back to 2.0.
 * Send it to the ELF loader (port 9021) with a PS2 DVD in the drive. Report: notification, klog ([drive probe]),
 * /data/drive-probe.txt (send that file).
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

#define PROBE_VERSION "v1"

static FILE *g_out;
static char g_sum[2800];
static int g_at;
static int g_pass = -1, g_fd = -1;
static uint64_t g_size;

static void say(const char *fmt, ...) {
  char line[700];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  klog_printf("[drive probe] %s\n", line);
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

/* What the drive said about one command. */
typedef struct {
  int done;       /* 1: completed */
  int err;        /* errno of the ioctl, 0 */
  int key, asc, ascq;
  int field;      /* sense-key-specific field pointer, -1 when none */
  int in_cdb;     /* the field pointer points into the CDB (not the data) */
  int bit;        /* bit pointer, -1 */
  uint32_t cam;
} result_t;

static const char *describe(const result_t *r) {
  static char b[96];
  if (r->done)
    snprintf(b, sizeof(b), "ok");
  else if (r->err)
    snprintf(b, sizeof(b), "errno %d", r->err);
  else {
    int at = snprintf(b, sizeof(b), "sense %X/%02X/%02X", r->key, r->asc, r->ascq);
    if (r->field >= 0)
      at += snprintf(b + at, sizeof(b) - (size_t)at, ", %s byte %d", r->in_cdb ? "CDB" : "data", r->field);
    if (r->bit >= 0)
      snprintf(b + at, sizeof(b) - (size_t)at, " bit %d", r->bit);
  }
  return b;
}

static result_t cmd(const uint8_t *cdb, int cdb_len, uint32_t dir, uint8_t *data, uint32_t len) {
  result_t r = {0, 0, -1, -1, -1, -1, 0, -1, 0};
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  cam_fill_csio(&ccb.csio, 1, NULL, dir | CAM_DEV_QFRZDIS | (dir == CAM_DIR_NONE ? CAM_PASS_ERR_RECOVER : 0), MSG_SIMPLE_Q_TAG, data,
                len, SSD_FULL_SIZE, (uint8_t)cdb_len, 30000);
  memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, (size_t)cdb_len);
  if (ioctl(g_pass, CAMIOCOMMAND, &ccb) != 0) {
    r.err = errno;
    return r;
  }
  r.cam = ccb.ccb_h.status & CAM_STATUS_MASK;
  if (r.cam == CAM_REQ_CMP) {
    r.done = 1;
    return r;
  }
  const uint8_t *s = (const uint8_t *)&ccb.csio.sense_data;
  if ((s[0] & 0x7f) == 0x70 || (s[0] & 0x7f) == 0x71) {
    r.key = s[2] & 15, r.asc = s[12], r.ascq = s[13];
    if (s[15] & 0x80) {
      r.field = s[16] << 8 | s[17];
      r.in_cdb = (s[15] & 0x40) != 0;
      r.bit = (s[15] & 0x08) ? (s[15] & 7) : -1;
    }
  } else if ((s[0] & 0x7f) == 0x72 || (s[0] & 0x7f) == 0x73) {
    r.key = s[1] & 15, r.asc = s[2], r.ascq = s[3];
  }
  return r;
}

static int db(uint8_t rot, uint16_t speed, result_t *out) {
  const uint8_t c[12] = {0xDB, rot, (uint8_t)(speed >> 8), (uint8_t)speed, 0, 0, 0, 0, 0, 0, 0, 0};
  const result_t r = cmd(c, 12, CAM_DIR_NONE, NULL, 0);
  if (out)
    *out = r;
  return r.done ? 0 : -1;
}

static double timed(uint64_t from) {
  static uint8_t *buf;
  const uint32_t chunk = 4u << 20;
  if (!buf && !(buf = malloc(chunk)))
    return 0;
  from &= ~(uint64_t)2047;
  if (from + 5 * (uint64_t)chunk > g_size)
    return 0;
  pread(g_fd, buf, chunk, (off_t)from);
  const double t0 = now_s();
  uint64_t got = 0;
  for (uint64_t off = from + chunk; off < from + 5 * (uint64_t)chunk; off += chunk) {
    if (pread(g_fd, buf, chunk, (off_t)off) != (ssize_t)chunk)
      break;
    got += chunk;
  }
  const double s = now_s() - t0;
  return got == 4 * (uint64_t)chunk && s > 0 ? got / s / 1e6 : 0;
}

static uint64_t outer_offset(void) { return (g_size > 4700000000ull ? g_size / 2 : g_size) - (160ull << 20); }

/* The faster settings refused so far on every drive: one taken is the news this probe is after. */
static const struct {
  uint8_t rot;
  uint16_t speed;
} kFast[] = {{1, 0x0080}, {0, 0x0080}, {0, 0x0040}, {2, 0x0040}, {1, 0x0050}, {2, 0x0050}, {1, 0x0060}};
static int g_found;

/* Sends each faster DB setting; times and logs any taken. `when` says under what condition. */
static void try_fast(const char *when, int timed_ok) {
  char refused[160] = "";
  int at = 0;
  for (int i = 0; i < (int)(sizeof(kFast) / sizeof(kFast[0])); i++) {
    result_t r;
    if (db(kFast[i].rot, kFast[i].speed, &r) != 0) {
      if (at < (int)sizeof(refused) - 1)
        at += snprintf(refused + at, sizeof(refused) - at, "%s%d/%x(%02X)", at ? " " : "", kFast[i].rot, kFast[i].speed, r.asc);
      continue;
    }
    g_found++;
    if (timed_ok) {
      const double in = timed(96ull << 20), out = timed(outer_offset());
      say("  %s: DB %02x %04x TAKEN: inner %.2f MB/s (%.2fx), outer %.2f MB/s (%.2fx)", when, kFast[i].rot, kFast[i].speed, in, in / 1.385,
          out, out / 1.385);
      sum(" | FOUND %s: r%d/%x %.1f/%.1f MB/s", when, kFast[i].rot, kFast[i].speed, in, out);
    } else {
      say("  %s: DB %02x %04x TAKEN (not timed here)", when, kFast[i].rot, kFast[i].speed);
      sum(" | FOUND %s: r%d/%x", when, kFast[i].rot, kFast[i].speed);
    }
    db(0, 0x20, NULL);
  }
  if (at)
    say("  %s: refused %s", when, refused);
}

/* MODE SENSE(10), one page, current (pc 0) or changeable (pc 1): the page (from its code byte) into page[], length or -1. */
static int read_page(uint8_t code, int pc, uint8_t *page, int cap) {
  uint8_t buf[256] = {0};
  const uint8_t c[10] = {0x5A, 0x08, (uint8_t)(pc << 6 | code), 0, 0, 0, 0, 1, 0, 0};
  const result_t r = cmd(c, 10, CAM_DIR_IN, buf, sizeof(buf));
  if (!r.done)
    return -1;
  const int at = 8 + (buf[6] << 8 | buf[7]);
  const int n = 2 + buf[at + 1];
  if ((buf[at] & 0x3f) != code || n > cap || at + n > (int)sizeof(buf))
    return -1;
  memcpy(page, buf + at, (size_t)n);
  return n;
}

/* MODE SELECT(10), PF 1, SP 0 (nothing saved). */
static result_t write_page(const uint8_t *page, int n) {
  uint8_t data[264] = {0};
  memcpy(data + 8, page, (size_t)n);
  data[8] &= 0x3f;
  const uint32_t len = 8 + (uint32_t)n;
  const uint8_t c[10] = {0x55, 0x10, 0, 0, 0, 0, 0, (uint8_t)(len >> 8), (uint8_t)len, 0};
  return cmd(c, 10, CAM_DIR_OUT, data, len);
}

static const char *hex(const uint8_t *p, int n) {
  static char out[200];
  int at = 0;
  out[0] = 0;
  for (int i = 0; i < n && at < (int)sizeof(out) - 3; i++)
    at += snprintf(out + at, sizeof(out) - at, "%02x", p[i]);
  return out;
}

/* Sets page `code`'s byte `b` (counted from the page code byte) to v, reads it back, tries the fast settings, puts the
 * page back. */
static void page_try(uint8_t code, const uint8_t *orig, int n, int b, uint8_t v) {
  uint8_t p[64];
  memcpy(p, orig, (size_t)n);
  p[b] = v;
  char when[48];
  snprintf(when, sizeof(when), "page %02x byte %d = %02x", code, b - 2, v);
  const result_t r = write_page(p, n);
  if (!r.done) {
    say("%s: MODE SELECT refused, %s", when, describe(&r));
    return;
  }
  uint8_t back[64];
  const int m = read_page(code, 0, back, sizeof(back));
  say("%s: MODE SELECT ok, reads back %s", when, m > 0 ? hex(back, m) : "?");
  try_fast(when, 1);
  write_page(orig, n);
}

static void test_pages(void) {
  say("-- 1/2: vendor mode pages, then the faster DB settings");
  uint8_t orig[64], chg[64];
  /* page 2D: every bit of byte 0 (changeable ff) */
  int n = read_page(0x2D, 0, orig, sizeof(orig));
  if (n >= 3 && read_page(0x2D, 1, chg, sizeof(chg)) == n) {
    say("page 2D: %s, changeable %s", hex(orig, n), hex(chg, n));
    for (int b = 2; b < n; b++)
      for (int bit = 0; bit < 8; bit++)
        if (chg[b] & (1 << bit))
          page_try(0x2D, orig, n, b, (uint8_t)(orig[b] ^ (1 << bit)));
    if (chg[2] == 0xff) /* a few whole values too */
      for (int v = 0; v < 4; v++) {
        static const uint8_t kV[4] = {0xff, 0x03, 0x0f, 0x80 | 0x01};
        page_try(0x2D, orig, n, 2, kV[v]);
      }
  } else
    say("page 2D: not read");
  /* page 31 byte 0 (changeable 03) and page 32 (changeable 03 01 01): every combination */
  uint8_t o31[64], o32[64], c31[64], c32[64];
  const int n31 = read_page(0x31, 0, o31, sizeof(o31)), n32 = read_page(0x32, 0, o32, sizeof(o32));
  if (n31 < 3 || n32 < 5 || read_page(0x31, 1, c31, sizeof(c31)) != n31 || read_page(0x32, 1, c32, sizeof(c32)) != n32) {
    say("pages 31/32: not read");
    return;
  }
  say("page 31: %s, changeable %s; page 32: %s, changeable %s", hex(o31, n31), hex(c31, n31), hex(o32, n32), hex(c32, n32));
  /* the changeable bits: page 31 byte 0's, and page 32's (bytes 0..3), six at most */
  struct {
    int page, byte, bit;
  } bits[8];
  int nb = 0;
  for (int bit = 0; bit < 8 && nb < 2; bit++)
    if (c31[2] & (1 << bit))
      bits[nb].page = 0x31, bits[nb].byte = 2, bits[nb].bit = bit, nb++;
  for (int b = 2; b < n32 && b < 6; b++)
    for (int bit = 0; bit < 8 && nb < 6; bit++)
      if (c32[b] & (1 << bit))
        bits[nb].page = 0x32, bits[nb].byte = b, bits[nb].bit = bit, nb++;
  say("%d changeable bits in pages 31/32: %d combinations", nb, 1 << nb);
  for (int m = 0; m < (1 << nb); m++) {
    uint8_t p31[64], p32[64];
    memcpy(p31, o31, (size_t)n31);
    memcpy(p32, o32, (size_t)n32);
    for (int i = 0; i < nb; i++) {
      uint8_t *p = bits[i].page == 0x31 ? p31 : p32;
      p[bits[i].byte] = (uint8_t)((p[bits[i].byte] & ~(1 << bits[i].bit)) | (((m >> i) & 1) << bits[i].bit));
    }
    char when[64];
    snprintf(when, sizeof(when), "page 31 %02x, page 32 %02x%02x%02x%02x", p31[2], p32[2], p32[3], p32[4], p32[5]);
    const result_t r1 = write_page(p31, n31), r2 = write_page(p32, n32);
    if (!r1.done || !r2.done) {
      char a[96];
      snprintf(a, sizeof(a), "%s", describe(&r1));
      say("%s: MODE SELECT 31 %s / 32 %s", when, a, describe(&r2));
      continue;
    }
    uint8_t b31[64], b32[64];
    const int m31 = read_page(0x31, 0, b31, sizeof(b31)), m32 = read_page(0x32, 0, b32, sizeof(b32));
    char back31[40];
    snprintf(back31, sizeof(back31), "%s", m31 > 0 ? hex(b31, m31) : "?");
    say("%s: set, reads back %s / %s", when, back31, m32 > 0 ? hex(b32, m32) : "?");
    try_fast(when, 1);
  }
  write_page(o31, n31);
  write_page(o32, n32);
}

static void test_stopped(void) {
  say("-- 3: the faster DB settings while the disc is stopped");
  const uint8_t stop[6] = {0x1B, 0, 0, 0, 0x00, 0}; /* START 0, LOEJ 0: stop, never eject */
  const uint8_t start[6] = {0x1B, 0, 0, 0, 0x01, 0};
  result_t r = cmd(stop, 6, CAM_DIR_NONE, NULL, 0);
  say("STOP: %s", describe(&r));
  if (r.done) {
    sleep(2);
    try_fast("stopped", 0);
    /* the settings taken while stopped (if any) stay set when it spins up again: time what the drive does then */
    for (int i = 0; i < (int)(sizeof(kFast) / sizeof(kFast[0])); i++) {
      if (db(kFast[i].rot, kFast[i].speed, NULL) != 0)
        continue;
      const result_t s = cmd(start, 6, CAM_DIR_NONE, NULL, 0);
      const double in = timed(160ull << 20), out = timed(outer_offset());
      say("  stopped, DB %02x %04x, START (%s): inner %.2f MB/s, outer %.2f MB/s", kFast[i].rot, kFast[i].speed, describe(&s), in, out);
      sum(" | stopped r%d/%x then start: %.1f/%.1f MB/s", kFast[i].rot, kFast[i].speed, in, out);
      cmd(stop, 6, CAM_DIR_NONE, NULL, 0);
      sleep(1);
    }
  }
  r = cmd(start, 6, CAM_DIR_NONE, NULL, 0);
  say("START: %s", describe(&r));
  db(0, 0x20, NULL);
  const double check = timed(224ull << 20);
  say("after: %.2f MB/s", check);
}

/* 1: the drive refused it for its invalid control byte (an ILLEGAL REQUEST that isn't "unknown opcode"). */
static int refused_for_control(const char *what, const uint8_t *c, int len, uint32_t dir, uint8_t *data, uint32_t dlen) {
  const result_t r = cmd(c, len, dir, data, dlen);
  say("  %s with Link set: %s", what, describe(&r));
  return !r.done && !r.err && r.key == 5 && r.asc != 0x20;
}

static void test_opcodes(void) {
  say("-- 4: vendor opcodes C0..FF");
  uint8_t buf[256];
  /* the gate: known commands with the control byte's Link bit (obsolete, invalid on MMC drives) */
  const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0x01};
  const uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0x01};
  const uint8_t ms[10] = {0x5A, 0x08, 0x2A, 0, 0, 0, 0, 0, 64, 0x01};
  const uint8_t gp[12] = {0xAC, 0x10, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0x01};
  const uint8_t dbc[12] = {0xDB, 0, 0, 0x20, 0, 0, 0, 0, 0, 0, 0, 0x01};
  int gate = refused_for_control("TEST UNIT READY", tur, 6, CAM_DIR_NONE, NULL, 0);
  gate &= refused_for_control("INQUIRY", inq, 6, CAM_DIR_IN, buf, 36);
  gate &= refused_for_control("MODE SENSE(10)", ms, 10, CAM_DIR_IN, buf, 64);
  gate &= refused_for_control("GET PERFORMANCE", gp, 12, CAM_DIR_IN, buf, sizeof(buf));
  gate &= refused_for_control("DB", dbc, 12, CAM_DIR_NONE, NULL, 0);
  db(0, 0x20, NULL);
  if (!gate) {
    say("the drive doesn't refuse every command with an invalid control byte: the survey would run unknown commands for real, so it's skipped");
    sum(" | opcode survey skipped (no control-byte check)");
    return;
  }
  say("the drive checks the control byte: surveying with it invalid (bytes 5, 9 and 11 = 01)");
  int known = 0;
  char list[600] = "";
  int at = 0;
  for (int op = 0xC0; op <= 0xFF; op++) {
    uint8_t c[12] = {(uint8_t)op, 0, 0, 0, 0, 0x01, 0, 0, 0, 0x01, 0, 0x01};
    const result_t r = cmd(c, 12, CAM_DIR_NONE, NULL, 0);
    if (!r.done && !r.err && r.key == 5 && r.asc == 0x20)
      continue; /* unknown opcode */
    known++;
    say("  opcode %02X: %s", op, describe(&r));
    if (at < (int)sizeof(list) - 1)
      at += snprintf(list + at, sizeof(list) - at, " %02X%s", op, r.done ? "!" : "");
  }
  db(0, 0x20, NULL);
  say("vendor opcodes the drive knows: %d:%s", known, list);
  sum(" | vendor opcodes:%s", known ? list : " none");
}

int main(void) {
  g_out = fopen("/data/drive-probe.txt", "w");
  notify("Drive probe " PROBE_VERSION ": running (2-5 minutes, the disc stops and starts once)");
  const uint32_t v = kernel_get_fw_version();
  say("PS5 drive probe %s, console firmware %x.%02x", PROBE_VERSION, v >> 24, (v >> 16) & 0xff);
  sum("Probe %s FW %x.%02x", PROBE_VERSION, v >> 24, (v >> 16) & 0xff);
  g_fd = open("/dev/cd0", O_RDONLY);
  if (g_fd < 0 && (errno == EPERM || errno == EACCES)) {
    const pid_t pid = getpid();
    const uint64_t authid = kernel_get_ucred_authid(pid);
    uint8_t caps[16], all[16];
    memset(all, 0xff, sizeof(all));
    kernel_get_ucred_caps(pid, caps);
    kernel_set_ucred_authid(pid, 0x4800000000010003ull);
    kernel_set_ucred_caps(pid, all);
    g_fd = open("/dev/cd0", O_RDONLY);
    kernel_set_ucred_authid(pid, authid);
    kernel_set_ucred_caps(pid, caps);
  }
  if (g_fd < 0) {
    say("/dev/cd0: open errno %d", errno);
    notify("Drive probe: can't open /dev/cd0");
    return 1;
  }
  off_t size = 0;
  if (ioctl(g_fd, DIOCGMEDIASIZE, &size) != 0 || size < (off_t)(512ull << 20)) {
    say("no disc (size %lld)", (long long)size);
    notify("Drive probe: put a PS2 DVD in the drive");
    close(g_fd);
    return 1;
  }
  g_size = (uint64_t)size;
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  ccb.ccb_h.func_code = XPT_GDEVLIST;
  char path[64] = "";
  if (ioctl(g_fd, CAMGETPASSTHRU, &ccb) == 0)
    snprintf(path, sizeof(path), "/dev/%.*s%u", (int)sizeof(ccb.cgdl.periph_name), ccb.cgdl.periph_name, ccb.cgdl.unit_number);
  g_pass = path[0] ? open(path, O_RDWR) : -1;
  if (g_pass < 0) {
    say("no pass device (%s, errno %d)", path, errno);
    notify("Drive probe: no pass device");
    close(g_fd);
    return 1;
  }
  uint8_t inq[36] = {0};
  const uint8_t c_inq[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
  if (cmd(c_inq, 6, CAM_DIR_IN, inq, sizeof(inq)).done) {
    say("drive: %.16s fw %.4s, disc %llu bytes", inq + 16, inq + 32, (unsigned long long)g_size);
    sum(" | %.16s fw %.4s", inq + 16, inq + 32);
  }
  const double base = timed(64ull << 20);
  say("as the drive is: %.2f MB/s (%.2fx DVD)", base, base / 1.385);
  say("-- 0: the faster DB settings as things are (expected: all refused)");
  try_fast("as is", 1);

  test_pages();
  db(0, 0x20, NULL);
  test_stopped();
  test_opcodes();

  db(0, 0x20, NULL);
  say("drive back to 2.0; pages put back (nothing was saved)");
  sum(" | %s", g_found ? "a faster setting was TAKEN (see file)" : "no faster setting taken");
  close(g_pass);
  close(g_fd);
  say("summary: %s", g_sum);
  notify(g_sum);
  if (g_out)
    fclose(g_out);
  return 0;
}
