/* PS5SX2 Installer: the PS5 side of plat.h.
 *
 * Every system call here has a known signature:
 *   - sceKernelSendNotificationRequest: as in the payload SDK's notify samples, with the request laid out
 *     like OnionHEN's debug toast (the helper's "PS5SX2 Helper Loaded!");
 *   - sysctl(CTL_KERN, KERN_ARND), kill(pid, 0), signal(): FreeBSD's own, from the SDK headers;
 *   - 1.4: sceLncUtilGetAppIdOfRunningBigApp, sceLncUtilGetAppTitleId, sceLncUtilLaunchApp (libSceSystemService) and
 *     sceUserServiceGetForegroundUser (libSceUserService), loaded with dlopen at run time as the PS5SX3 Installer does
 *     (its plat_ps5.c, where they run on consoles since TB10): starting the app that way is what its tile does. Never
 *     fork or LoadExec (they panicked the kernel in the disc daemons' live tests).
 */
#include "plat.h"

#include "config.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
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
/* fix181: the system state event flags (as relaunch/relaunch.c reads them) */
typedef intptr_t plat_evf_t;
int sceKernelOpenEventFlag(plat_evf_t *ef, const char *name);
int sceKernelPollEventFlag(plat_evf_t ef, uint64_t bit_pattern, unsigned int wait_mode, uint64_t *result_pattern);
int sceKernelCloseEventFlag(plat_evf_t ef);

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

/* ---- 1.4: the shell: which app runs, and starting PS5SX2 ---- */

typedef struct {
  uint32_t size;
  int32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint64_t check_flag;
} lnc_app_param_t;

typedef uint32_t (*get_big_app_t)(void);
typedef int (*get_title_t)(uint32_t, char *);
typedef int (*launch_app_t)(const char *, const char **, lnc_app_param_t *);
typedef int (*user_init_t)(void *);
typedef int (*user_fg_t)(int *);

static get_big_app_t p_big_app;
static get_title_t p_title;
static launch_app_t p_launch;
static user_fg_t p_fg_user;
static int g_lnc_loaded;

static void load_lnc(void) {
  if (g_lnc_loaded)
    return;
  g_lnc_loaded = 1;
  void *h = dlopen("libSceSystemService.sprx", RTLD_NOW | RTLD_GLOBAL);
  if (h) {
    p_big_app = (get_big_app_t)dlsym(h, "sceLncUtilGetAppIdOfRunningBigApp");
    p_title = (get_title_t)dlsym(h, "sceLncUtilGetAppTitleId");
    p_launch = (launch_app_t)dlsym(h, "sceLncUtilLaunchApp");
  }
  void *u = dlopen("libSceUserService.sprx", RTLD_NOW | RTLD_GLOBAL);
  if (u) {
    user_init_t init = (user_init_t)dlsym(u, "sceUserServiceInitialize");
    p_fg_user = (user_fg_t)dlsym(u, "sceUserServiceGetForegroundUser");
    if (init)
      (void)init(NULL); /* (already initialized in a process that used it before: harmless) */
  }
}

int plat_shutting_down(void) {
  int quiet = 0;
  plat_evf_t info = -1, status = -1;
  if (sceKernelOpenEventFlag(&info, "SceSystemStateMgrInfo") >= 0) {
    uint64_t pat = 0;
    if (sceKernelPollEventFlag(info, UINT64_MAX, 2u /* OR */, &pat) == 0) {
      const unsigned st = (unsigned)(pat & 0xffffu);
      quiet = st == 100u || st == 300u || st == 500u;
    }
    sceKernelCloseEventFlag(info);
  }
  if (sceKernelOpenEventFlag(&status, "SceSystemStateMgrStatus") >= 0) {
    uint64_t pat = 0;
    if (sceKernelPollEventFlag(status, UINT64_MAX, 2u /* OR */, &pat) == 0 && (pat & 0x0000000000200000ULL))
      quiet = 1;
    sceKernelCloseEventFlag(status);
  }
  return quiet;
}

int plat_app_running(void) {
  load_lnc();
  if (!p_big_app || !p_title)
    return -1;
  const uint32_t id = p_big_app();
  if (id == 0 || id == 0xffffffffu)
    return 0;
  char title[32];
  memset(title, 0, sizeof(title));
  if (p_title(id, title) != 0)
    return -1;
  return strncmp(title, TITLE_ID, 9) == 0 ? 1 : 0;
}

int plat_launch_app(char *why, size_t why_size) {
  load_lnc();
  if (!p_launch) {
    snprintf(why, why_size, "sceLncUtilLaunchApp isn't available");
    return -1;
  }
  int user = -1;
  if (p_fg_user)
    (void)p_fg_user(&user);
  lnc_app_param_t prm;
  memset(&prm, 0, sizeof(prm));
  prm.size = sizeof(prm);
  prm.user_id = user;
  const int rc = p_launch(TITLE_ID, NULL, &prm);
  snprintf(why, why_size, "sceLncUtilLaunchApp(" TITLE_ID ", user %d) = 0x%08x", user, (unsigned)rc);
  return rc < 0 ? -1 : 0;
}

const char *plat_test_env(const char *name) {
  (void)name;
  return NULL;
}

void plat_init(void) { signal(SIGPIPE, SIG_IGN); }
