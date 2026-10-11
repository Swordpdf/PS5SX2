/* PS5SX2 Installer: the PS5 side of plat.h.
 *
 * Every system call here has a known signature:
 *   - sceKernelSendNotificationRequest: as in the payload SDK's notify samples, with the request laid out
 *     like OnionHEN's debug toast (the helper's "PS5SX2 Helper Loaded!");
 *   - sysctl(CTL_KERN, KERN_ARND), kill(pid, 0), signal(): FreeBSD's own, from the SDK headers.
 */
#include "plat.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
  int32_t type;            /* 0x00 */
  int32_t req_id;          /* 0x04 */
  int32_t priority;        /* 0x08 */
  int32_t msg_id;          /* 0x0C */
  int32_t target_id;       /* 0x10 */
  int32_t user_id;         /* 0x14 */
  int32_t unk1;            /* 0x18 */
  int32_t unk2;            /* 0x1C */
  int32_t app_id;          /* 0x20 */
  int32_t error_num;       /* 0x24 */
  int32_t unk3;            /* 0x28 */
  char use_icon_image_uri; /* 0x2C */
  char message[1024];      /* 0x2D */
  char uri[1024];          /* 0x42D */
  char unkstr[1024];       /* 0x82D */
  char pad[3];             /* to 0xC30 */
} notify_request_t;

_Static_assert(sizeof(notify_request_t) == 0xC30, "notification request must be 0xC30 bytes");
_Static_assert(offsetof(notify_request_t, message) == 0x2D, "message at 0x2D");

int sceKernelSendNotificationRequest(int32_t device, void *request, size_t size, int32_t blocking);

const char *plat_root(void) { return ""; }

void plat_notify_text(const char *text) {
  static notify_request_t req; /* 3 KB: not on the stack */
  memset(&req, 0, sizeof(req));
  req.type = 0;
  req.target_id = -1;
  req.use_icon_image_uri = 0; /* debug style: text only */
  size_t n = strlen(text);
  if (n > sizeof(req.message) - 1)
    n = sizeof(req.message) - 1;
  memcpy(req.message, text, n);
  (void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

int plat_random(void *buf, size_t len) {
  uint8_t *p = buf;
  while (len) {
    size_t chunk = len > 256 ? 256 : len; /* kern.arandom hands out at most 256 bytes per call */
    size_t got = chunk;
    int mib[2] = {CTL_KERN, KERN_ARND};
    if (sysctl(mib, 2, p, &got, NULL, 0) != 0 || got == 0) {
      /* The same kernel generator through the device node. */
      const int fd = open("/dev/urandom", O_RDONLY);
      if (fd < 0)
        return -1;
      const ssize_t r = read(fd, p, chunk);
      close(fd);
      if (r <= 0)
        return -1;
      got = (size_t)r;
    }
    if (got > chunk)
      return -1;
    p += got;
    len -= got;
  }
  return 0;
}

time_t plat_boot_time(void) {
  struct timeval tv;
  size_t len = sizeof(tv);
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  if (sysctl(mib, 2, &tv, &len, NULL, 0) != 0 || len != sizeof(tv))
    return 0;
  return tv.tv_sec;
}

int plat_pid_alive(pid_t pid) {
  if (pid <= 0)
    return 0;
  if (kill(pid, 0) == 0)
    return 1;
  if (errno == EPERM)
    return 1;
  if (errno == ESRCH)
    return 0;
  return -1;
}

const char *plat_test_env(const char *name) {
  (void)name;
  return NULL;
}

void plat_init(void) { signal(SIGPIPE, SIG_IGN); }
