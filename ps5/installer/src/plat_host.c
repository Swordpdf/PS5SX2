/* PS5SX2 Installer: the host (Linux) side of plat.h, for tests. Everything happens under $PS5SX2_ROOT. */
#include "plat.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

const char *plat_root(void) {
  const char *r = getenv("PS5SX2_ROOT");
  if (!r || !*r) {
    fprintf(stderr, "PS5SX2_ROOT is not set (the host build works inside a test folder)\n");
    exit(2);
  }
  return r;
}

void plat_notify_text(const char *text) {
  fprintf(stderr, "[notify] %s\n", text);
  char path[4096];
  snprintf(path, sizeof(path), "%s/notifications.txt", plat_root());
  FILE *f = fopen(path, "a");
  if (f) {
    fprintf(f, "%s\n", text);
    fclose(f);
  }
}

int plat_random(void *buf, size_t len) {
  unsigned char *p = buf;
  while (len) {
    const ssize_t n = getrandom(p, len, 0);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

time_t plat_boot_time(void) {
  const char *t = getenv("PS5SX2_TEST_BOOT_TIME");
  if (t)
    return (time_t)strtoll(t, NULL, 10);
  FILE *f = fopen("/proc/stat", "r");
  if (!f)
    return 0;
  char line[256];
  long long bt = 0;
  while (fgets(line, sizeof(line), f))
    if (sscanf(line, "btime %lld", &bt) == 1)
      break;
  fclose(f);
  return (time_t)bt;
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
  const char *v = getenv(name);
  return (v && *v) ? v : NULL;
}

void plat_init(void) { signal(SIGPIPE, SIG_IGN); }
