/* PS5SX2 Installer: a small HTTPS/1.1 client (GET and POST, redirects, chunked bodies). */
#pragma once

#include "util.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
  int status;
  int64_t content_length; /* -1 when the server didn't say */
  char location[4096];
  char content_type[128];
  long ratelimit_remaining; /* -1 when absent */
  long long ratelimit_reset;
  long retry_after; /* -1 when absent */
  sbuf errbody;     /* the start of a body that isn't 2xx */
  char final_host[256];
} http_resp;

/* Gets the 2xx body in pieces; return -1 (after err_set) to stop. */
typedef int (*http_sink)(void *ctx, const void *data, size_t len);
/* Called once the final response's headers are in, before the body. */
typedef int (*http_on_headers)(void *ctx, const http_resp *resp);

typedef struct {
  const char *method; /* "GET" or "POST" */
  const char *url;    /* https:// only */
  const char *const *headers; /* "Name: value", NULL-terminated; may be NULL */
  const void *body;
  size_t body_len;
  http_sink sink;
  http_on_headers on_headers;
  void *ctx;
  int max_redirects; /* GET only */
  int (*allow_host)(const char *host); /* redirect targets must pass this; NULL = any */
} http_req;

void http_resp_init(http_resp *r);
void http_resp_free(http_resp *r);

/* 0 = a final response came (any status; see resp->status), -1 = no answer (err_get()). */
int http_do(const http_req *rq, http_resp *resp);

/* Splits https://host[:port]/path; 0 on success. */
int url_split(const char *url, char *host, size_t host_size, int *port, char *path, size_t path_size);
