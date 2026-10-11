/* PS5SX2 Installer: small helpers (growing buffers, strings, errors, time). */
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ---- the last error, in words (single-threaded program) ---- */
void err_set(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *err_get(void);
void err_clear(void);

/* ---- growing byte buffer; data is always NUL-terminated ---- */
typedef struct {
  char *data;
  size_t len;
  size_t cap;
} sbuf;

void sb_init(sbuf *b);
void sb_free(sbuf *b);
void sb_clear(sbuf *b);
int sb_reserve(sbuf *b, size_t extra);
int sb_append(sbuf *b, const void *p, size_t n);
int sb_puts(sbuf *b, const char *s);
int sb_printf(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sb_vprintf(sbuf *b, const char *fmt, va_list ap);

/* ---- strings ---- */
size_t str_copy(char *dst, size_t size, const char *src); /* strlcpy */
int str_starts(const char *s, const char *prefix);
int str_ends(const char *s, const char *suffix);
const void *mem_find(const void *hay, size_t n, const void *needle, size_t m);
char *str_dup(const char *s);
char *str_ndup(const char *s, size_t n);
char *str_trim(char *s); /* in place: leading/trailing blanks and newlines */
/* Printable ASCII only (other bytes -> '?'), at most max-1 chars: for HTTP headers and notifications. */
void str_ascii(char *dst, size_t size, const char *src);
int hex_is(const char *s, size_t n); /* n lowercase-or-uppercase hex digits */
void hex_encode(char *dst, const uint8_t *src, size_t n); /* dst: 2n+1 */
void str_lower(char *s);

/* ---- paths ---- */
/* dst = a + "/" + b (b without a leading slash); returns -1 if it doesn't fit. */
int path_join(char *dst, size_t size, const char *a, const char *b);
const char *path_base(const char *p);

/* ---- time ---- */
time_t now_utc(void);
/* "2026-09-28T20:04:24Z" */
void fmt_utc(char *dst, size_t size, time_t t);
/* "2026-09-28_2004" (for file names) */
void fmt_stamp(char *dst, size_t size, time_t t);
void human_bytes(char *dst, size_t size, uint64_t n);
void sleep_ms(unsigned ms);
uint64_t mono_ms(void);
