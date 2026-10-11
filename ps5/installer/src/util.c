/* PS5SX2 Installer: small helpers. */
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static char g_err[1024];

void err_set(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_err, sizeof(g_err), fmt, ap);
  va_end(ap);
}

const char *err_get(void) { return g_err[0] ? g_err : "unknown error"; }

void err_clear(void) { g_err[0] = '\0'; }

/* ---- sbuf ---- */

void sb_init(sbuf *b) {
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void sb_free(sbuf *b) {
  free(b->data);
  sb_init(b);
}

void sb_clear(sbuf *b) {
  b->len = 0;
  if (b->data)
    b->data[0] = '\0';
}

int sb_reserve(sbuf *b, size_t extra) {
  if (extra > (size_t)-1 / 2 - b->len)
    return -1;
  const size_t need = b->len + extra + 1;
  if (need <= b->cap)
    return 0;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < need)
    cap *= 2;
  char *p = realloc(b->data, cap);
  if (!p) {
    err_set("out of memory (%zu bytes)", cap);
    return -1;
  }
  b->data = p;
  b->cap = cap;
  return 0;
}

int sb_append(sbuf *b, const void *p, size_t n) {
  if (sb_reserve(b, n) != 0)
    return -1;
  if (n)
    memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = '\0';
  return 0;
}

int sb_puts(sbuf *b, const char *s) { return sb_append(b, s, strlen(s)); }

int sb_vprintf(sbuf *b, const char *fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  const int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (n < 0)
    return -1;
  if (sb_reserve(b, (size_t)n) != 0)
    return -1;
  vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
  b->len += (size_t)n;
  return 0;
}

int sb_printf(sbuf *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const int rc = sb_vprintf(b, fmt, ap);
  va_end(ap);
  return rc;
}

/* ---- strings ---- */

size_t str_copy(char *dst, size_t size, const char *src) {
  const size_t n = strlen(src);
  if (size) {
    const size_t c = n < size - 1 ? n : size - 1;
    memcpy(dst, src, c);
    dst[c] = '\0';
  }
  return n;
}

int str_starts(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

int str_ends(const char *s, const char *suffix) {
  const size_t a = strlen(s), b = strlen(suffix);
  return a >= b && memcmp(s + a - b, suffix, b) == 0;
}

const void *mem_find(const void *hay, size_t n, const void *needle, size_t m) {
  if (m == 0)
    return hay;
  if (m > n)
    return NULL;
  const unsigned char *h = hay;
  const unsigned char first = *(const unsigned char *)needle;
  const size_t last = n - m;
  for (size_t i = 0; i <= last; i++) {
    const unsigned char *p = memchr(h + i, first, last - i + 1);
    if (!p)
      return NULL;
    i = (size_t)(p - h);
    if (memcmp(p, needle, m) == 0)
      return p;
  }
  return NULL;
}

char *str_dup(const char *s) { return str_ndup(s, strlen(s)); }

char *str_ndup(const char *s, size_t n) {
  char *p = malloc(n + 1);
  if (!p) {
    err_set("out of memory");
    return NULL;
  }
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

char *str_trim(char *s) {
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
    s[--n] = '\0';
  return s;
}

void str_ascii(char *dst, size_t size, const char *src) {
  size_t o = 0;
  if (!size)
    return;
  for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < size; p++) {
    if (*p >= 0x20 && *p < 0x7f)
      dst[o++] = (char)*p;
    else if (*p >= 0x80) {
      /* one '?' per UTF-8 character, not per byte */
      if ((*p & 0xc0) != 0x80)
        dst[o++] = '?';
    } else
      dst[o++] = ' ';
  }
  dst[o] = '\0';
}

int hex_is(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (!isxdigit((unsigned char)s[i]))
      return 0;
  return 1;
}

void hex_encode(char *dst, const uint8_t *src, size_t n) {
  static const char d[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    dst[2 * i] = d[src[i] >> 4];
    dst[2 * i + 1] = d[src[i] & 15];
  }
  dst[2 * n] = '\0';
}

void str_lower(char *s) {
  for (; *s; s++)
    *s = (char)tolower((unsigned char)*s);
}

/* ---- paths ---- */

int path_join(char *dst, size_t size, const char *a, const char *b) {
  while (*b == '/')
    b++;
  const size_t la = strlen(a);
  const int slash = la && a[la - 1] == '/';
  const int n = snprintf(dst, size, slash ? "%s%s" : "%s/%s", a, b);
  if (n < 0 || (size_t)n >= size) {
    err_set("path too long: %s/%s", a, b);
    return -1;
  }
  return 0;
}

const char *path_base(const char *p) {
  const char *s = strrchr(p, '/');
  return s ? s + 1 : p;
}

/* ---- time ---- */

time_t now_utc(void) { return time(NULL); }

void fmt_utc(char *dst, size_t size, time_t t) {
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  gmtime_r(&t, &tm);
  snprintf(dst, size, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
           tm.tm_min, tm.tm_sec);
}

void fmt_stamp(char *dst, size_t size, time_t t) {
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  gmtime_r(&t, &tm);
  snprintf(dst, size, "%04d-%02d-%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
           tm.tm_min, tm.tm_sec);
}

void human_bytes(char *dst, size_t size, uint64_t n) {
  if (n >= (10ull << 20))
    snprintf(dst, size, "%llu MB", (unsigned long long)((n + (1u << 19)) >> 20));
  else if (n >= (1ull << 20))
    snprintf(dst, size, "%.1f MB", (double)n / (1 << 20));
  else if (n >= 1024)
    snprintf(dst, size, "%llu KB", (unsigned long long)((n + 512) >> 10));
  else
    snprintf(dst, size, "%llu B", (unsigned long long)n);
}

void sleep_ms(unsigned ms) {
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
