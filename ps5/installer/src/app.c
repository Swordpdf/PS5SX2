/* PS5SX2 Installer: facts about the installed PS5SX2 app. */
#include "app.h"

#include "fsx.h"
#include "paths.h"
#include "plat.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int app_running(pid_t *pid_out, time_t *pid_mtime_out) {
  char path[PATH_LEN];
  if (path_join(path, sizeof(path), g_p.pcsx2, "pid.txt") != 0)
    return -1;
  if (pid_out)
    *pid_out = 0;
  if (pid_mtime_out)
    *pid_mtime_out = 0;
  sbuf b;
  sb_init(&b);
  if (fs_read_file(path, 64, &b) != 0) {
    sb_free(&b);
    return fs_exists(path) ? -1 : 0; /* no pid.txt: PS5SX2 never ran */
  }
  char *end = NULL;
  const long pid = strtol(b.data ? b.data : "", &end, 10);
  sb_free(&b);
  if (end == NULL || pid <= 0 || pid > 0x7fffffff)
    return 0;
  const time_t mt = fs_mtime(path);
  if (pid_out)
    *pid_out = (pid_t)pid;
  if (pid_mtime_out)
    *pid_mtime_out = mt;
  /* pid.txt is never removed: one written before the PS5 last started names a process of an earlier boot, and
   * its number may now belong to something else. */
  const time_t boot = plat_boot_time();
  if (boot > 0 && mt > 0 && mt + 5 < boot)
    return 0;
  return plat_pid_alive((pid_t)pid);
}

int app_installed_tag(char *tag, size_t size) {
  char path[PATH_LEN];
  if (path_join(path, sizeof(path), g_p.app, "eboot.bin") != 0)
    return -1;
  const int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  static const char marker[] = "[boot] build=";
  const size_t ml = sizeof(marker) - 1;
  static char buf[(1 << 20) + 128];
  size_t keep = 0;
  int found = -1;
  for (;;) {
    const ssize_t r = read(fd, buf + keep, (1 << 20));
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      break;
    const size_t have = keep + (size_t)r;
    size_t from = 0;
    const char *hit;
    while (found != 0 && (hit = mem_find(buf + from, have - from, marker, ml)) != NULL) {
      const char *s = hit + ml;
      size_t n = 0;
      while ((size_t)(s - buf) + n < have && n + 1 < size &&
             (isalnum((unsigned char)s[n]) || s[n] == '-' || s[n] == '.' || s[n] == '_' || s[n] == '+'))
        n++;
      if (n > 0 && (size_t)(s - buf) + n < have) {
        memcpy(tag, s, n);
        tag[n] = '\0';
        found = 0;
      }
      from = (size_t)(hit - buf) + 1;
    }
    if (found == 0)
      break;
    /* keep the end in case the marker straddles two reads */
    keep = have < 128 ? have : 128;
    memmove(buf, buf + have - keep, keep);
  }
  close(fd);
  return found;
}

/* "vk-285-112" -> prefix "vk" and the numbers 285, 112. 0 on success. */
static int split_tag(const char *t, char *prefix, size_t psize, long long nums[8], int *count) {
  *count = 0;
  prefix[0] = '\0';
  const char *p = t;
  size_t pl = 0;
  while (*p) {
    const char *dash = strchr(p, '-');
    const size_t n = dash ? (size_t)(dash - p) : strlen(p);
    int digits = n > 0 && n <= 12;
    for (size_t i = 0; i < n && digits; i++)
      digits = isdigit((unsigned char)p[i]);
    if (digits) {
      if (*count == 8)
        return -1;
      nums[(*count)++] = strtoll(p, NULL, 10);
    } else {
      if (*count > 0 || n == 0 || pl + n + 1 >= psize)
        return -1; /* words after numbers, or empty parts: not our kind of tag */
      if (pl)
        prefix[pl++] = '-';
      memcpy(prefix + pl, p, n);
      pl += n;
      prefix[pl] = '\0';
    }
    if (!dash)
      break;
    p = dash + 1;
  }
  return *count > 0 ? 0 : -1;
}

int tag_compare(const char *a, const char *b) {
  if (!strcmp(a, b))
    return 0;
  char pa[64], pb[64];
  long long na[8], nb[8];
  int ca, cb;
  if (split_tag(a, pa, sizeof(pa), na, &ca) != 0 || split_tag(b, pb, sizeof(pb), nb, &cb) != 0 || strcmp(pa, pb) != 0 ||
      ca != cb)
    return 2;
  for (int i = 0; i < ca; i++)
    if (na[i] != nb[i])
      return na[i] < nb[i] ? -1 : 1;
  return 0;
}
