/* PS5 optical drive scan v1 (AI-assisted): does the console see a USB DVD drive, and how fast does it read a PS2 DVD?
 *
 * The PS5's own drive tops out at 3.2x for PS2 DVDs on every firmware tested (4.03 to 13.60), and SceShellCore's own
 * table gives "PS2 DVD-ROM" 3.2x. Someone dumps them at ~8x with "his own drive": if that's a USB DVD drive plugged into
 * the PS5, it would show up as another CD device (FreeBSD's cd driver, through umass). This lists what /dev has, asks
 * every CD and pass device what it is, and for each optical drive with a disc in it times reading as it is, then after
 * SET CD SPEED to its maximum (a PC drive takes that; the PS5's own drive refuses it), inner and outer part of the disc.
 *
 * Read-only apart from SET CD SPEED (standard; drives go back to their default speed when the disc changes).
 * Send it to the ELF loader (port 9021). For the USB test: a USB DVD drive plugged into the PS5 with a PS2 DVD in it
 * (the PS5's own drive can be empty). Report: notification, klog ([optical scan]), /data/optical-scan.txt.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <dirent.h>
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

#define SCAN_VERSION "v1"

static FILE *g_out;
static char g_sum[2800];
static int g_at;

static void say(const char *fmt, ...) {
  char line[700];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  klog_printf("[optical scan] %s\n", line);
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

/* Opens with the shell's credentials when this process may not (as PS5SX2 does for /dev/cd0). */
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

/* INQUIRY on a pass device: peripheral type into *type, "vendor | model | rev" into text. */
static int inquiry(int pass, int *type, char *text, size_t n) {
  uint8_t inq[36] = {0};
  char why[64];
  const uint8_t c[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
  if (cmd(pass, c, 6, CAM_DIR_IN, inq, sizeof(inq), why, sizeof(why)) != 0) {
    snprintf(text, n, "INQUIRY %s", why);
    return -1;
  }
  *type = inq[0] & 0x1f;
  snprintf(text, n, "%.8s | %.16s | %.4s", inq + 8, inq + 16, inq + 32);
  return 0;
}

static int g_optical, g_usb_optical;

/* One CD device: what it is, its disc, and its speeds. */
static void scan_cd(const char *name) {
  char path[80];
  snprintf(path, sizeof(path), "/dev/%s", name);
  const int fd = open_dev(path, O_RDONLY);
  if (fd < 0) {
    say("%s: open errno %d", path, errno);
    return;
  }
  union ccb ccb;
  memset(&ccb, 0, sizeof(ccb));
  ccb.ccb_h.func_code = XPT_GDEVLIST;
  char ppath[64] = "";
  if (ioctl(fd, CAMGETPASSTHRU, &ccb) == 0)
    snprintf(ppath, sizeof(ppath), "/dev/%.*s%u", (int)sizeof(ccb.cgdl.periph_name), ccb.cgdl.periph_name, ccb.cgdl.unit_number);
  const int pass = ppath[0] ? open_dev(ppath, O_RDWR) : -1;
  char what[96] = "?";
  int type = -1;
  if (pass >= 0)
    inquiry(pass, &type, what, sizeof(what));
  const int sony = strstr(what, "PS-SYSTEM") != NULL;
  g_optical++;
  g_usb_optical += !sony;
  off_t size = 0;
  const int have_disc = ioctl(fd, DIOCGMEDIASIZE, &size) == 0 && size >= (off_t)(512ll << 20);
  say("%s (pass %s): %s%s, disc: %lld bytes", path, ppath[0] ? ppath : "none", what, sony ? " (the PS5's own drive)" : " (NOT the PS5's drive)",
      (long long)size);
  sum(" | %s: %.40s", name, what);
  if (pass < 0 || !have_disc) {
    sum(have_disc ? "" : " no disc");
    if (pass >= 0)
      close(pass);
    close(fd);
    return;
  }
  char why[64];
  uint8_t gc[8] = {0};
  const uint8_t c_gc[10] = {0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0};
  if (cmd(pass, c_gc, 10, CAM_DIR_IN, gc, sizeof(gc), why, sizeof(why)) == 0)
    say("  disc profile %#06x", gc[6] << 8 | gc[7]);
  uint8_t perf[40] = {0};
  const uint8_t c_perf[12] = {0xAC, 0x10, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0};
  if (cmd(pass, c_perf, 12, CAM_DIR_IN, perf, sizeof(perf), why, sizeof(why)) == 0)
    say("  GET PERFORMANCE: %u..%u kB/s", (uint32_t)perf[12] << 24 | perf[13] << 16 | perf[14] << 8 | perf[15],
        (uint32_t)perf[20] << 24 | perf[21] << 16 | perf[22] << 8 | perf[23]);
  const uint64_t usize = (uint64_t)size;
  const uint64_t outer = (usize > 4700000000ull ? usize / 2 : usize) - (160ull << 20);
  const double in0 = timed(fd, 64ull << 20, usize), out0 = timed(fd, outer, usize);
  say("  as it is: inner %.2f MB/s (%.2fx DVD), outer %.2f MB/s (%.2fx)", in0, in0 / 1.385, out0, out0 / 1.385);
  sum(" as is %.1f/%.1f MB/s", in0, out0);
  if (sony) { /* the PS5's own drive: DriveReport covers it */
    close(pass);
    close(fd);
    return;
  }
  const uint8_t c_bb[12] = {0xBB, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
  if (cmd(pass, c_bb, 12, CAM_DIR_NONE, NULL, 0, why, sizeof(why)) == 0) {
    const double in1 = timed(fd, 128ull << 20, usize), out1 = timed(fd, outer - (64ull << 20), usize);
    say("  SET CD SPEED max: inner %.2f MB/s (%.2fx DVD), outer %.2f MB/s (%.2fx)", in1, in1 / 1.385, out1, out1 / 1.385);
    sum(", max %.1f/%.1f MB/s", in1, out1);
  } else {
    say("  SET CD SPEED max: %s", why);
  }
  close(pass);
  close(fd);
}

int main(void) {
  g_out = fopen("/data/optical-scan.txt", "w");
  notify("Optical scan " SCAN_VERSION ": running (up to 2 minutes)");
  const uint32_t v = kernel_get_fw_version();
  say("PS5 optical drive scan %s, console firmware %x.%02x", SCAN_VERSION, v >> 24, (v >> 16) & 0xff);
  sum("Optical scan FW %x.%02x", v >> 24, (v >> 16) & 0xff);

  /* What /dev has that could be a drive or USB: names only. */
  char names[64][32];
  int ncd = 0;
  char list[1500] = "";
  int at = 0;
  DIR *d = opendir("/dev");
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      const char *n = e->d_name;
      const int interesting = !strncmp(n, "cd", 2) || !strncmp(n, "pass", 4) || !strncmp(n, "da", 2) || !strncmp(n, "ugen", 4) ||
                              !strncmp(n, "usb", 3) || !strncmp(n, "umass", 5) || !strncmp(n, "acd", 3) || !strncmp(n, "xpt", 3) ||
                              !strncmp(n, "bd", 2) || !strncmp(n, "sbram", 5);
      if (!interesting)
        continue;
      if (at < (int)sizeof(list) - 1)
        at += snprintf(list + at, sizeof(list) - at, " %s", n);
      if (!strncmp(n, "cd", 2) && n[2] >= '0' && n[2] <= '9' && ncd < 64)
        snprintf(names[ncd++], sizeof(names[0]), "%s", n);
    }
    closedir(d);
  } else
    say("/dev: opendir errno %d", errno);
  say("/dev:%s", list);
  /* /dev/usb: the USB devices (bus.address.endpoint) */
  d = opendir("/dev/usb");
  if (d) {
    char ulist[1500] = "";
    int ua = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
      if (e->d_name[0] != '.' && ua < (int)sizeof(ulist) - 1)
        ua += snprintf(ulist + ua, sizeof(ulist) - ua, " %s", e->d_name);
    closedir(d);
    say("/dev/usb:%s", ulist);
  }

  /* Every pass device: its kind (5 = CD/DVD/BD), so a drive the cd driver didn't take still shows. */
  for (int i = 0; i < 16; i++) {
    char p[32];
    snprintf(p, sizeof(p), "/dev/pass%d", i);
    if (access(p, F_OK) != 0)
      continue;
    const int pass = open_dev(p, O_RDWR);
    if (pass < 0) {
      say("%s: open errno %d", p, errno);
      continue;
    }
    char what[96];
    int type = -1;
    inquiry(pass, &type, what, sizeof(what));
    say("%s: type %d%s: %s", p, type, type == 5 ? " (CD/DVD/BD)" : type == 0 ? " (disk)" : "", what);
    close(pass);
  }

  for (int i = 0; i < ncd; i++)
    scan_cd(names[i]);
  if (!ncd)
    say("no CD devices in /dev");
  say("optical drives: %d, not the PS5's own: %d", g_optical, g_usb_optical);
  sum(" | %d optical, %d not the PS5's", g_optical, g_usb_optical);
  say("summary: %s", g_sum);
  notify(g_sum);
  if (g_out)
    fclose(g_out);
  return 0;
}
