/* LD_PRELOAD shim for the host tests: pretends some paths are on another drive (rename -> EXDEV) and that
 * a drive fills up (write -> ENOSPC after a byte budget, for files whose path contains SHIM_ENOSPC_PATH).
 *   SHIM_EXDEV=<substring>          rename(a, b) fails with EXDEV when one of a, b contains it and the other doesn't
 *   SHIM_ENOSPC_PATH=<substring>    ...
 *   SHIM_ENOSPC_AFTER=<bytes>       writes to such files fail with ENOSPC once this many bytes went in */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long long g_written;

int rename(const char *a, const char *b) {
  static int (*real)(const char *, const char *);
  if (!real)
    real = (int (*)(const char *, const char *))dlsym(RTLD_NEXT, "rename");
  const char *pat = getenv("SHIM_EXDEV");
  if (pat && *pat && (!strstr(a, pat) != !strstr(b, pat))) { /* only across the boundary, like real drives */
    errno = EXDEV;
    return -1;
  }
  return real(a, b);
}

ssize_t write(int fd, const void *buf, size_t n) {
  static ssize_t (*real)(int, const void *, size_t);
  if (!real)
    real = (ssize_t(*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
  const char *pat = getenv("SHIM_ENOSPC_PATH");
  const char *after = getenv("SHIM_ENOSPC_AFTER");
  if (pat && after && fd > 2) {
    char link[64], path[4096];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const ssize_t l = readlink(link, path, sizeof(path) - 1);
    if (l > 0) {
      path[l] = '\0';
      if (strstr(path, pat)) {
        const long long budget = atoll(after);
        if (g_written >= budget) {
          errno = ENOSPC;
          return -1;
        }
        if (g_written + (long long)n > budget)
          n = (size_t)(budget - g_written);
        g_written += (long long)n;
      }
    }
  }
  return real(fd, buf, n);
}
