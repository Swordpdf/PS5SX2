/* PS5SX2 Installer: a small HTTPS/1.1 client. One request per connection ("Connection: close"). */
#include "http.h"

#include "config.h"
#include "log.h"
#include "tls.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ERRBODY_MAX 4096
#define LINE_MAX_LEN 8192
#define MAX_HEADER_LINES 200

void http_resp_init(http_resp *r) {
  memset(r, 0, sizeof(*r));
  sb_init(&r->errbody);
  r->content_length = -1;
  r->ratelimit_remaining = -1;
  r->retry_after = -1;
}

void http_resp_free(http_resp *r) { sb_free(&r->errbody); }

static void resp_reset(http_resp *r) {
  sbuf keep = r->errbody;
  memset(r, 0, sizeof(*r));
  r->errbody = keep;
  sb_clear(&r->errbody);
  r->content_length = -1;
  r->ratelimit_remaining = -1;
  r->retry_after = -1;
}

static int host_ok(const char *h) {
  const size_t n = strlen(h);
  if (n == 0 || n > 253)
    return 0;
  for (const char *p = h; *p; p++)
    if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-'))
      return 0;
  return 1;
}

int url_split(const char *url, char *host, size_t host_size, int *port, char *path, size_t path_size) {
  static const char scheme[] = "https://";
  if (strncasecmp(url, scheme, sizeof(scheme) - 1) != 0) {
    err_set("not an https:// address: %.80s", url);
    return -1;
  }
  const char *h = url + sizeof(scheme) - 1;
  const char *e = h;
  while (*e && *e != '/' && *e != ':' && *e != '?' && *e != '#')
    e++;
  const size_t hn = (size_t)(e - h);
  if (hn == 0 || hn >= host_size) {
    err_set("bad address: %.80s", url);
    return -1;
  }
  memcpy(host, h, hn);
  host[hn] = '\0';
  str_lower(host);
  if (!host_ok(host)) {
    err_set("bad server name in %.80s", url);
    return -1;
  }
  *port = 443;
  if (*e == ':') {
    char *end = NULL;
    const long p = strtol(e + 1, &end, 10);
    if (end == e + 1 || p < 1 || p > 65535) {
      err_set("bad port in %.80s", url);
      return -1;
    }
    *port = (int)p;
    e = end;
  }
  const char *rest = e;
  const char *hash = strchr(rest, '#');
  size_t rn = hash ? (size_t)(hash - rest) : strlen(rest);
  const int need_slash = rn == 0 || rest[0] != '/';
  if (rn + (size_t)need_slash >= path_size) {
    err_set("address too long");
    return -1;
  }
  size_t o = 0;
  if (need_slash)
    path[o++] = '/';
  for (size_t i = 0; i < rn; i++) {
    const unsigned char ch = (unsigned char)rest[i];
    if (ch <= 0x20 || ch >= 0x7f) {
      err_set("bad character in the address");
      return -1;
    }
    path[o++] = (char)ch;
  }
  path[o] = '\0';
  return 0;
}

/* ---- buffered reading over TLS ---- */

typedef struct {
  tls_conn *c;
  size_t pos, len;
  char buf[1 << 16];
} reader;

/* >0 bytes added, 0 = closed, -1 = error, -2 = buffer full */
static int rd_fill(reader *r) {
  if (r->pos == r->len)
    r->pos = r->len = 0;
  if (r->len == sizeof(r->buf) && r->pos > 0) {
    memmove(r->buf, r->buf + r->pos, r->len - r->pos);
    r->len -= r->pos;
    r->pos = 0;
  }
  if (r->len == sizeof(r->buf))
    return -2;
  const ssize_t n = tls_read(r->c, r->buf + r->len, sizeof(r->buf) - r->len);
  if (n < 0)
    return -1;
  if (n == 0)
    return 0;
  r->len += (size_t)n;
  return (int)n;
}

static int rd_line(reader *r, char *out, size_t outsz) {
  for (;;) {
    char *nl = memchr(r->buf + r->pos, '\n', r->len - r->pos);
    if (nl) {
      const size_t n = (size_t)(nl - (r->buf + r->pos));
      size_t m = n;
      if (m && r->buf[r->pos + m - 1] == '\r')
        m--;
      if (m >= outsz) {
        err_set("the server sent a line that is too long");
        return -1;
      }
      memcpy(out, r->buf + r->pos, m);
      out[m] = '\0';
      r->pos += n + 1;
      return 0;
    }
    if (r->len - r->pos >= outsz) {
      err_set("the server sent a line that is too long");
      return -1;
    }
    const int f = rd_fill(r);
    if (f == 0) {
      err_set("the server closed the connection early");
      return -1;
    }
    if (f == -2) {
      err_set("the server sent a line that is too long");
      return -1;
    }
    if (f < 0)
      return -1;
  }
}

/* Up to max bytes: >0, 0 = closed, -1 = error */
static ssize_t rd_some(reader *r, size_t max, const char **p) {
  if (r->pos == r->len) {
    const int f = rd_fill(r);
    if (f == 0)
      return 0;
    if (f < 0)
      return -1;
  }
  size_t avail = r->len - r->pos;
  if (avail > max)
    avail = max;
  *p = r->buf + r->pos;
  r->pos += avail;
  return (ssize_t)avail;
}

/* ---- body delivery ---- */

typedef struct {
  const http_req *rq;
  http_resp *resp;
  int ok2xx;
  uint64_t got;
} body_ctx;

static int deliver(body_ctx *b, const char *p, size_t n) {
  b->got += n;
  if (b->ok2xx) {
    if (b->rq->sink)
      return b->rq->sink(b->rq->ctx, p, n);
    return 0;
  }
  if (b->resp->errbody.len < ERRBODY_MAX) {
    const size_t room = ERRBODY_MAX - b->resp->errbody.len;
    sb_append(&b->resp->errbody, p, n < room ? n : room);
  }
  return 0;
}

static int read_body(reader *r, body_ctx *b, int chunked, int64_t length) {
  const char *p;
  ssize_t n;
  if (chunked) {
    char line[LINE_MAX_LEN];
    for (;;) {
      if (rd_line(r, line, sizeof(line)) != 0)
        return -1;
      char *end = NULL;
      errno = 0;
      const unsigned long long size = strtoull(line, &end, 16);
      if (end == line || errno != 0 || (*end && *end != ';' && *end != ' ' && *end != '\t')) {
        err_set("the server sent a bad chunk header");
        return -1;
      }
      if (size == 0) {
        /* trailers until an empty line */
        for (int i = 0; i < MAX_HEADER_LINES; i++) {
          if (rd_line(r, line, sizeof(line)) != 0)
            return -1;
          if (!line[0])
            return 0;
        }
        err_set("the server sent too many trailer lines");
        return -1;
      }
      unsigned long long left = size;
      while (left) {
        n = rd_some(r, left > (1u << 16) ? (1u << 16) : (size_t)left, &p);
        if (n <= 0) {
          if (n == 0)
            err_set("the server closed the connection in the middle of the data");
          return -1;
        }
        if (deliver(b, p, (size_t)n) != 0)
          return -1;
        left -= (unsigned long long)n;
      }
      if (rd_line(r, line, sizeof(line)) != 0)
        return -1;
      if (line[0]) {
        err_set("the server sent a bad chunk end");
        return -1;
      }
    }
  }
  if (length >= 0) {
    uint64_t left = (uint64_t)length;
    while (left) {
      n = rd_some(r, left > (1u << 16) ? (1u << 16) : (size_t)left, &p);
      if (n == 0) {
        err_set("the connection ended early (%llu of %lld bytes)", (unsigned long long)b->got, (long long)length);
        return -1;
      }
      if (n < 0)
        return -1;
      if (deliver(b, p, (size_t)n) != 0)
        return -1;
      left -= (uint64_t)n;
    }
    return 0;
  }
  for (;;) {
    n = rd_some(r, 1u << 16, &p);
    if (n == 0)
      return 0;
    if (n < 0)
      return -1;
    if (deliver(b, p, (size_t)n) != 0)
      return -1;
  }
}

static int is_redirect(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

/* Location -> absolute URL (absolute, //host/path or /path; never plain http). */
static int resolve_location(const char *loc, const char *host, int port, char *out, size_t outsz) {
  int n;
  if (strncasecmp(loc, "https://", 8) == 0)
    n = snprintf(out, outsz, "%s", loc);
  else if (loc[0] == '/' && loc[1] == '/')
    n = snprintf(out, outsz, "https:%s", loc);
  else if (loc[0] == '/')
    n = port == 443 ? snprintf(out, outsz, "https://%s%s", host, loc)
                    : snprintf(out, outsz, "https://%s:%d%s", host, port, loc);
  else {
    err_set("the server redirected to an address we don't follow: %.80s", loc);
    return -1;
  }
  if (n < 0 || (size_t)n >= outsz) {
    err_set("redirect address too long");
    return -1;
  }
  return 0;
}

int http_do(const http_req *rq, http_resp *resp) {
  char url[4096];
  if (str_copy(url, sizeof(url), rq->url) >= sizeof(url)) {
    err_set("address too long");
    return -1;
  }
  static char host[256], path[4096];
  reader *r = malloc(sizeof(reader));
  if (!r) {
    err_set("out of memory");
    return -1;
  }
  int result = -1;
  for (int hop = 0;; hop++) {
    int port = 443;
    if (url_split(url, host, sizeof(host), &port, path, sizeof(path)) != 0)
      break;
    if (hop > 0 && rq->allow_host && !rq->allow_host(host)) {
      err_set("redirected to an unexpected server: %s", host);
      break;
    }
    resp_reset(resp);
    str_copy(resp->final_host, sizeof(resp->final_host), host);
    tls_conn *c = tls_open(host, port);
    if (!c)
      break;
    sbuf req;
    sb_init(&req);
    char hosthdr[300];
    if (port == 443)
      snprintf(hosthdr, sizeof(hosthdr), "%s", host);
    else
      snprintf(hosthdr, sizeof(hosthdr), "%s:%d", host, port);
    sb_printf(&req, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n",
              rq->method, path, hosthdr, INSTALLER_UA);
    if (rq->headers)
      for (const char *const *h = rq->headers; *h; h++)
        sb_printf(&req, "%s\r\n", *h);
    if (rq->body)
      sb_printf(&req, "Content-Length: %zu\r\n", rq->body_len);
    sb_puts(&req, "\r\n");
    int ok = req.data && tls_write_all(c, req.data, req.len) == 0;
    sb_free(&req);
    if (ok && rq->body && rq->body_len)
      ok = tls_write_all(c, rq->body, rq->body_len) == 0;
    if (!ok) {
      tls_close(c);
      break;
    }

    r->c = c;
    r->pos = r->len = 0;
    char line[LINE_MAX_LEN];
    int chunked = 0;
    int bad = 0;
    /* Status line and headers; 1xx answers are skipped. */
    for (;;) {
      if (rd_line(r, line, sizeof(line)) != 0) {
        bad = 1;
        break;
      }
      /* "HTTP/1.1 200 OK": the version, one space, three digits */
      int status = 0;
      if (strncmp(line, "HTTP/", 5) != 0 || !strchr(line, ' ')) {
        err_set("the server's answer isn't HTTP: %.60s", line);
        bad = 1;
        break;
      }
      {
        const char *sp = strchr(line, ' ');
        while (*sp == ' ')
          sp++;
        if (sp[0] >= '1' && sp[0] <= '9' && sp[1] >= '0' && sp[1] <= '9' && sp[2] >= '0' && sp[2] <= '9' &&
            (sp[3] == '\0' || sp[3] == ' '))
          status = (sp[0] - '0') * 100 + (sp[1] - '0') * 10 + (sp[2] - '0');
      }
      if (status < 100) {
        err_set("the server's answer isn't HTTP: %.60s", line);
        bad = 1;
        break;
      }
      resp->status = status;
      chunked = 0;
      resp->content_length = -1;
      int lines = 0;
      for (;;) {
        if (rd_line(r, line, sizeof(line)) != 0) {
          bad = 1;
          break;
        }
        if (!line[0])
          break;
        if (++lines > MAX_HEADER_LINES) {
          err_set("the server sent too many headers");
          bad = 1;
          break;
        }
        char *colon = strchr(line, ':');
        if (!colon)
          continue;
        *colon = '\0';
        char *name = str_trim(line);
        char *value = str_trim(colon + 1);
        if (!strcasecmp(name, "Content-Length")) {
          char *end = NULL;
          errno = 0;
          const long long v = strtoll(value, &end, 10);
          if (end == value || *end || v < 0 || errno) {
            err_set("the server sent a bad Content-Length");
            bad = 1;
            break;
          }
          resp->content_length = v;
        } else if (!strcasecmp(name, "Transfer-Encoding")) {
          char low[128];
          str_copy(low, sizeof(low), value);
          str_lower(low);
          if (strstr(low, "chunked"))
            chunked = 1;
        } else if (!strcasecmp(name, "Location")) {
          str_copy(resp->location, sizeof(resp->location), value);
        } else if (!strcasecmp(name, "Content-Type")) {
          str_copy(resp->content_type, sizeof(resp->content_type), value);
        } else if (!strcasecmp(name, "X-RateLimit-Remaining")) {
          resp->ratelimit_remaining = strtol(value, NULL, 10);
        } else if (!strcasecmp(name, "X-RateLimit-Reset")) {
          resp->ratelimit_reset = strtoll(value, NULL, 10);
        } else if (!strcasecmp(name, "Retry-After")) {
          resp->retry_after = strtol(value, NULL, 10);
        }
      }
      if (bad)
        break;
      if (resp->status >= 200 || resp->status == 101)
        break;
    }
    if (bad) {
      tls_close(c);
      break;
    }

    if (is_redirect(resp->status) && !strcmp(rq->method, "GET") && resp->location[0]) {
      tls_close(c);
      if (hop >= rq->max_redirects) {
        err_set("too many redirects");
        break;
      }
      char next[4096];
      if (resolve_location(resp->location, host, port, next, sizeof(next)) != 0)
        break;
      str_copy(url, sizeof(url), next);
      continue;
    }

    if (rq->on_headers && resp->status >= 200 && resp->status < 300 && rq->on_headers(rq->ctx, resp) != 0) {
      tls_close(c);
      break;
    }
    body_ctx b;
    b.rq = rq;
    b.resp = resp;
    b.ok2xx = resp->status >= 200 && resp->status < 300;
    b.got = 0;
    int body_rc = 0;
    if (resp->status != 204 && resp->status != 304)
      body_rc = read_body(r, &b, chunked, chunked ? -1 : resp->content_length);
    tls_close(c);
    if (body_rc == 0)
      result = 0;
    break;
  }
  free(r);
  return result;
}
