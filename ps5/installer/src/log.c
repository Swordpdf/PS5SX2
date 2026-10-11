/* PS5SX2 Installer: its own log and the notifications. */
#include "log.h"

#include "paths.h"
#include "plat.h"
#include "util.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_MAX_BYTES (1u << 20)

static void log_write(const char *text) {
  if (!g_p.log[0])
    return;
  struct stat st;
  if (stat(g_p.log, &st) == 0 && st.st_size > (off_t)LOG_MAX_BYTES) {
    char old[PATH_LEN + 4];
    snprintf(old, sizeof(old), "%s.1", g_p.log);
    rename(g_p.log, old); /* the one before is replaced: our own file */
  }
  const int fd = open(g_p.log, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0666);
  if (fd < 0)
    return;
  char stamp[32];
  fmt_utc(stamp, sizeof(stamp), now_utc());
  char line[2048];
  const int n = snprintf(line, sizeof(line), "%s [%d] %s\n", stamp, (int)getpid(), text);
  if (n > 0)
    (void)!write(fd, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
  close(fd);
}

void log_line(const char *fmt, ...) {
  char text[1536];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  log_write(text);
#ifdef PS5SX2_HOST
  fprintf(stderr, "[log] %s\n", text);
#endif
}

void notify(const char *fmt, ...) {
  char text[1000];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  char line[1100];
  snprintf(line, sizeof(line), "notification: %s", text);
  log_write(line);
  plat_notify_text(text);
}
