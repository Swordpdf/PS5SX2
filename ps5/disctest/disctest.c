/* PS5SX2 disc speed test payload (vk-285-152 investigation, AI-assisted).
 *
 * PS5SX2 (a home-screen title, jailbroken by swordpdf's PS5SX2 Helper) reads a PS2 DVD at a flat 2.0x whatever way it
 * reads: pread on /dev/cd0, READ(10)/READ(12) through pass0, 64 KiB commands, two in flight (vk-285-151 on 13.60). The
 * drive (SONY PS-SYSTEM 503R) reports 3.2x..8.0x, and someone's payload copied a DVD at 10.4 MiB/s ("[Payload] [Dump]").
 * So: is it who reads? This payload, sent to the ELF loader like the Helper, times reading /dev/cd0 as itself, then
 * with its authid switched to the one the Helper gives PS5SX2 (0x4801000000000013), then to SceShellCore's kind
 * (0x4800000000010003), and puts its own back. If the payload is fast and slows down with PS5SX2's authid, the Helper
 * could give PS5SX2 a fast authid; if it stays fast, PS5SX2 needs a payload to copy discs.
 *
 * It only reads the disc. Results: klog, /data/PCSX2/logs/disctest.log, and notifications.
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
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#define CHUNK (4u << 20)
#define SPAN (128ull << 20)

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

/* MB/s over SPAN from `from` (4 MiB preads), or -1. */
static double timed_read(int fd, uint64_t from, uint64_t size, const char *what) {
  static uint8_t *buf;
  if (!buf)
    buf = malloc(CHUNK);
  if (!buf)
    return -1;
  from &= ~(uint64_t)2047;
  const double t0 = now_s();
  uint64_t got = 0;
  for (uint64_t off = from; off < from + SPAN && off + CHUNK <= size; off += CHUNK) {
    const ssize_t n = pread(fd, buf, CHUNK, (off_t)off);
    if (n != (ssize_t)CHUNK) {
      say("%s: pread at %llu: %zd (errno %d)", what, (unsigned long long)off, n, errno);
      break;
    }
    got += CHUNK;
  }
  const double s = now_s() - t0;
  const double mbs = s > 0 ? got / s / 1e6 : 0;
  say("%s: %llu MiB in %.1f s = %.2f MB/s = %.2fx DVD", what, (unsigned long long)(got >> 20), s, mbs, mbs / 1.385);
  return mbs;
}

static int open_cd(pid_t pid) {
  int fd = open("/dev/cd0", O_RDONLY);
  if (fd >= 0 || (errno != EPERM && errno != EACCES))
    return fd;
  /* as RPCS3To5's ps3dumpd: once more with system credentials, then ours back */
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
  const uint64_t own = kernel_get_ucred_authid(pid);
  say("pid %d, uid %d, authid %#llx", (int)pid, (int)getuid(), (unsigned long long)own);
  notify("PS5SX2 disc test: running (about 1 to 4 minutes)");

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
  say("/dev/cd0: %lld bytes", (long long)size);

  const uint64_t sz = (uint64_t)size;
  const double a = timed_read(fd, 64ull << 20, sz, "payload, own authid, start of the disc");
  const double b = timed_read(fd, sz - SPAN - (8ull << 20), sz, "payload, own authid, end of the disc");

  kernel_set_ucred_authid(pid, 0x4801000000000013ull);
  say("authid now %#llx (what the Helper gives PS5SX2)", (unsigned long long)kernel_get_ucred_authid(pid));
  const double c = timed_read(fd, 256ull << 20, sz, "payload, PS5SX2's authid");

  kernel_set_ucred_authid(pid, 0x4800000000010003ull);
  say("authid now %#llx (SceShellCore's kind)", (unsigned long long)kernel_get_ucred_authid(pid));
  const double d = timed_read(fd, 448ull << 20, sz, "payload, system authid");

  kernel_set_ucred_authid(pid, own);
  say("authid back to %#llx", (unsigned long long)kernel_get_ucred_authid(pid));
  close(fd);

  notify("PS5SX2 disc test: payload %.1f MB/s (%.1fx) start, %.1f (%.1fx) end; as PS5SX2 %.1f (%.1fx); as system %.1f (%.1fx)",
         a, a / 1.385, b, b / 1.385, c, c / 1.385, d, d / 1.385);
  say("done");
  if (g_log)
    fclose(g_log);
  return 0;
}
