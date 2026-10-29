#include "jb.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "progress.h"

#define SANDBOX_BASE "/mnt/sandbox"
#define WHITELIST "/data/whitelist.txt"
#define JB_AUTHID UINT64_C(0x4801000000000013)
#define POLL_US 200000

static const char *kBuiltIn[] = {
    "PPSA99203", "PPSA99303",
    /* the disc payload's tile slots (README section 4): built in, not
       appended to whitelist.txt */
    "PPSA99231", "PPSA99232", "PPSA99233", "PPSA99234",
    "PPSA99235", "PPSA99236", "PPSA99237", "PPSA99238",
};
static const char *kMarkerNames[] = {"etahen_jailbreak", "onionhen_jailbreak"};

static void step(const char *what) {
  klog_printf("jb: %s\n", what);
  payload_progress(what);
}

static void stepf(const char *fmt, ...) {
  char line[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  step(line);
}

static int title_allowed(const char *tid) {
  for (size_t i = 0; i < sizeof(kBuiltIn) / sizeof(kBuiltIn[0]); ++i)
    if (strcmp(tid, kBuiltIn[i]) == 0)
      return 1;
  FILE *f = fopen(WHITELIST, "r");
  if (!f)
    return 0;
  char row[128];
  while (fgets(row, sizeof(row), f)) {
    char *p = row;
    while (*p == ' ' || *p == '\t')
      ++p;
    if (*p == '#' || *p == '\n' || *p == '\0')
      continue;
    char *end = p;
    while (*end && *end != '\n' && *end != ' ' && *end != '\t' &&
           *end != '#')
      ++end;
    const size_t len = (size_t)(end - p);
    if (len == strlen(tid) && strncmp(p, tid, len) == 0) {
      fclose(f);
      return 1;
    }
  }
  fclose(f);
  return 0;
}

/* "PPSA99203_000", "ITEM00001_007": any title id + '_' + digits. The old
 * onionHEN daemon watched whitelisted titles of any prefix; match that. */
static int slot_tid(const char *name, char tid[12]) {
  size_t n = strlen(name);
  size_t id = 0;
  while (id < n && name[id] != '_') {
    const char c = name[id];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z')))
      return 0;
    ++id;
  }
  if (id < 4 || id > 11 || id >= n || name[id] != '_')
    return 0;
  if (n - id - 1 < 1 || n - id - 1 > 4)
    return 0;
  for (size_t i = id + 1; i < n; ++i)
    if (name[i] < '0' || name[i] > '9')
      return 0;
  memcpy(tid, name, id);
  tid[id] = '\0';
  return 1;
}

/* A PS5 debug-style text notification (the "Jailbroken!" popup). */
typedef struct {
  int32_t type;
  int32_t req_id;
  int32_t priority;
  int32_t msg_id;
  int32_t target_id;
  int32_t user_id;
  int32_t unk1;
  int32_t unk2;
  int32_t app_id;
  int32_t error_num;
  int32_t unk3;
  char use_icon_image_uri;
  char message[1024];
  char uri[1024];
  char unkstr[1024];
  char pad[3];
} jb_notify_request_t;

int sceKernelSendNotificationRequest(int32_t device, void *request,
                                     size_t size, int32_t blocking);

static void notify_jailbroken(void) {
  static jb_notify_request_t req;
  memset(&req, 0, sizeof(req));
  req.type = 0;
  req.target_id = -1;
  req.use_icon_image_uri = 0;
  memcpy(req.message, "Jailbroken!", sizeof("Jailbroken!") - 1);
  (void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static int jb_apply(pid_t pid) {
  static const uint8_t kFullCaps[16] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff};
  uint8_t attrs[32];

  if (kernel_get_proc(pid) == 0)
    return 0;
  if (kernel_set_ucred_uid(pid, 0) != 0)
    return 0;
  (void)kernel_set_ucred_ruid(pid, 0);
  (void)kernel_set_ucred_svuid(pid, 0);
  (void)kernel_set_ucred_rgid(pid, 0);
  (void)kernel_set_ucred_svgid(pid, 0);
  {
    const intptr_t ucred = kernel_get_proc_ucred(pid);
    if (ucred) {
      const uint32_t ngroups = 0;
      (void)kernel_copyin(&ngroups, ucred + 0x10, sizeof(ngroups));
    }
  }
  if (kernel_set_ucred_authid(pid, JB_AUTHID) != 0)
    return 0;
  if (kernel_set_ucred_caps(pid, kFullCaps) != 0)
    return 0;
  if (kernel_get_ucred_attrs(pid, attrs) == 0) {
    attrs[3] |= 0x80; /* ptrace, as onionHEN and the Lapy daemon set it */
    (void)kernel_set_ucred_attrs(pid, attrs);
  }
  {
    const intptr_t root = kernel_get_root_vnode();
    if (root) {
      (void)kernel_set_proc_rootdir(pid, root);
      (void)kernel_set_proc_jaildir(pid, root);
    }
  }
  return kernel_get_ucred_uid(pid) == 0;
}

static int parse_pid(const char *body) {
  const char *p = strstr(body, "\"PID\"");
  if (!p || !(p = strchr(p + 5, ':')))
    return -1;
  ++p;
  while (*p == ' ' || *p == '\t' || *p == '"')
    ++p;
  errno = 0;
  char *end = NULL;
  const long v = strtol(p, &end, 10);
  if (errno || end == p || v <= 1 || v > 0x7fffffff)
    return -1;
  return (int)v;
}

static void check_slot(const char *name) {
  char tid[12];
  if (!slot_tid(name, tid) || !title_allowed(tid))
    return;
  for (size_t m = 0; m < sizeof(kMarkerNames) / sizeof(kMarkerNames[0]);
       ++m) {
    char path[192];
    snprintf(path, sizeof(path), "%s/%s/download0/%s", SANDBOX_BASE, name,
             kMarkerNames[m]);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;
    char body[512] = {0};
    (void)!fgets(body, sizeof(body) - 1, f);
    fclose(f);
    const pid_t pid = parse_pid(body);
    if (pid > 1 && jb_apply(pid)) {
      stepf("jailbroken pid=%d tid=%s (marker unlinked)", (int)pid, tid);
      notify_jailbroken();
      unlink(path);
    } else {
      stepf("jb FAILED pid=%d tid=%s (marker unlinked)",
            pid > 0 ? (int)pid : -1, tid);
      unlink(path);
    }
  }
}

void *payload_jb_thread(void *arg) {
  (void)arg;
  step("onionHEN-style jb watcher started (built-ins PPSA99203, PPSA99303 "
       "+ /data/whitelist.txt)");
  for (;;) {
    DIR *d = opendir(SANDBOX_BASE);
    if (d) {
      struct dirent *e;
      while ((e = readdir(d)))
        check_slot(e->d_name);
      closedir(d);
    }
    usleep(POLL_US);
  }
  return NULL;
}
