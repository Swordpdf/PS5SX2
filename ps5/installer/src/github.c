/* PS5SX2 Installer: the latest release on GitHub, and its download (size and SHA-256 checked). */
#include "github.h"

#include "config.h"
#include "fsx.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include "plat.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "mbedtls/sha256.h"

static int safe_name(const char *s, size_t max) {
  const size_t n = strlen(s);
  if (n == 0 || n > max)
    return 0;
  for (const char *p = s; *p; p++)
    if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '_' || *p == '-' || *p == '+'))
      return 0;
  return s[0] != '.';
}

/* Where the zip may come from: github.com and GitHub's download servers. */
static int allowed_host(const char *host) {
  if (!strcmp(host, "github.com") || !strcmp(host, "api.github.com"))
    return 1;
  if (str_ends(host, ".githubusercontent.com"))
    return 1;
  const char *test = plat_test_env("PS5SX2_TEST_ALLOW_HOST"); /* host tests only */
  return test && !strcmp(host, test);
}

static int collect(void *ctx, const void *data, size_t len) {
  sbuf *b = ctx;
  if (b->len + len > (4u << 20)) {
    err_set("GitHub's answer is too big");
    return -1;
  }
  return sb_append(b, data, len);
}

int gh_latest(gh_release *out) {
  memset(out, 0, sizeof(*out));
  const char *api = plat_test_env("PS5SX2_TEST_API_URL");
  if (!api)
    api = GH_API_URL;
  static const char *const headers[] = {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28",
                                        NULL};
  sbuf body;
  sb_init(&body);
  http_resp resp;
  http_resp_init(&resp);
  http_req rq;
  memset(&rq, 0, sizeof(rq));
  rq.method = "GET";
  rq.url = api;
  rq.headers = headers;
  rq.sink = collect;
  rq.ctx = &body;
  rq.max_redirects = 3;
  rq.allow_host = allowed_host;
  int rc = -1;
  if (http_do(&rq, &resp) != 0) {
    char why[600];
    str_copy(why, sizeof(why), err_get());
    err_set("couldn't reach GitHub: %s", why);
    goto done;
  }
  if (resp.status == 404) {
    err_set("no build is published yet (GitHub: no release)");
    goto done;
  }
  if ((resp.status == 403 || resp.status == 429) && resp.ratelimit_remaining == 0) {
    char when[32] = "later";
    if (resp.ratelimit_reset > 0)
      fmt_utc(when, sizeof(when), (time_t)resp.ratelimit_reset);
    err_set("GitHub's hourly limit for this network is used up; try again after %s (UTC)", when);
    goto done;
  }
  if (resp.status != 200) {
    err_set("GitHub answered HTTP %d", resp.status);
    goto done;
  }
  js_node *root = js_parse(body.data ? body.data : "", body.len);
  if (!root) {
    char why[300];
    str_copy(why, sizeof(why), err_get());
    err_set("GitHub's answer couldn't be read (%s)", why);
    goto done;
  }
  const char *tag = js_str(js_get(root, "tag_name"));
  if (!tag || !safe_name(tag, 64)) {
    err_set("the release has no usable tag");
    js_free(root);
    goto done;
  }
  if (js_is_true(js_get(root, "draft"))) {
    err_set("the latest release is a draft");
    js_free(root);
    goto done;
  }
  str_copy(out->tag, sizeof(out->tag), tag);
  const char *title = js_str(js_get(root, "name"));
  str_ascii(out->title, sizeof(out->title), title ? title : tag);
  const char *pub = js_str(js_get(root, "published_at"));
  str_copy(out->published_at, sizeof(out->published_at), pub ? pub : "");

  const js_node *assets = js_get(root, "assets");
  const js_node *pick = NULL;
  int zips = 0;
  if (assets && assets->type == JS_ARR) {
    for (const js_node *a = assets->child; a; a = a->next) {
      const char *name = js_str(js_get(a, "name"));
      const char *state = js_str(js_get(a, "state"));
      if (!name || strlen(name) < 5 || strcasecmp(name + strlen(name) - 4, ".zip") != 0)
        continue;
      if (state && strcmp(state, "uploaded") != 0)
        continue;
      zips++;
      if (!pick && str_starts(name, "PS5SX2-"))
        pick = a;
    }
  }
  if (!pick) {
    err_set(zips ? "the release has no PS5SX2 zip" : "the release has no zip file");
    js_free(root);
    goto done;
  }
  const char *name = js_str(js_get(pick, "name"));
  const char *url = js_str(js_get(pick, "browser_download_url"));
  const char *digest = js_str(js_get(pick, "digest"));
  long long size = 0;
  if (!safe_name(name, 200)) {
    err_set("the zip's name has characters we don't accept");
    js_free(root);
    goto done;
  }
  if (js_int64(js_get(pick, "size"), &size) != 0 || size < 1024 || (unsigned long long)size > MAX_ZIP_BYTES) {
    err_set("the zip's size isn't plausible");
    js_free(root);
    goto done;
  }
  if (!digest || strlen(digest) != 7 + 64 || strncmp(digest, "sha256:", 7) != 0 || !hex_is(digest + 7, 64)) {
    err_set("GitHub gave no SHA-256 for the zip, so it can't be checked (not installed)");
    js_free(root);
    goto done;
  }
  /* The address must be exactly the release download of this repository. */
  const char *prefix = plat_test_env("PS5SX2_TEST_DOWNLOAD_PREFIX");
  if (!prefix)
    prefix = GH_DOWNLOAD_PREFIX;
  char expect[2048];
  snprintf(expect, sizeof(expect), "%s%s/%s", prefix, tag, name);
  if (!url || strcmp(url, expect) != 0) {
    err_set("the zip's address isn't this repository's release download");
    js_free(root);
    goto done;
  }
  str_copy(out->asset_name, sizeof(out->asset_name), name);
  str_copy(out->asset_url, sizeof(out->asset_url), url);
  out->asset_size = size;
  str_copy(out->asset_sha256, sizeof(out->asset_sha256), digest + 7);
  str_lower(out->asset_sha256);
  js_free(root);
  rc = 0;
done:
  sb_free(&body);
  http_resp_free(&resp);
  return rc;
}

/* ---- download ---- */

typedef struct {
  int fd;
  uint64_t got, total;
  mbedtls_sha256_context sha;
  gh_progress progress;
  void *ctx;
} dl_ctx;

static int dl_headers(void *ctx, const http_resp *resp) {
  dl_ctx *d = ctx;
  if (resp->content_length >= 0 && (uint64_t)resp->content_length != d->total) {
    err_set("the server's size (%lld bytes) isn't the release's (%llu bytes)", (long long)resp->content_length,
            (unsigned long long)d->total);
    return -1;
  }
  return 0;
}

static int dl_sink(void *ctx, const void *data, size_t len) {
  dl_ctx *d = ctx;
  if (d->got + len > d->total) {
    err_set("the download is bigger than the release says");
    return -1;
  }
  if (fs_write_all(d->fd, data, len) != 0) {
    err_set("can't write the download: %s", strerror(errno));
    return -1;
  }
  mbedtls_sha256_update(&d->sha, data, len);
  d->got += len;
  if (d->progress)
    d->progress(d->ctx, d->got, d->total);
  return 0;
}

int gh_download(const gh_release *rel, const char *dest, gh_progress progress, void *ctx) {
  char part[1100];
  snprintf(part, sizeof(part), "%s.part", dest);
  dl_ctx d;
  memset(&d, 0, sizeof(d));
  d.fd = fs_create_work_file(part, 0666);
  if (d.fd < 0)
    return -1;
  d.total = (uint64_t)rel->asset_size;
  d.progress = progress;
  d.ctx = ctx;
  mbedtls_sha256_init(&d.sha);
  mbedtls_sha256_starts(&d.sha, 0);

  static const char *const headers[] = {"Accept: application/octet-stream", NULL};
  http_resp resp;
  http_resp_init(&resp);
  http_req rq;
  memset(&rq, 0, sizeof(rq));
  rq.method = "GET";
  rq.url = rel->asset_url;
  rq.headers = headers;
  rq.sink = dl_sink;
  rq.on_headers = dl_headers;
  rq.ctx = &d;
  rq.max_redirects = 5;
  rq.allow_host = allowed_host;
  int rc = -1;
  if (http_do(&rq, &resp) != 0) {
    char why[600];
    str_copy(why, sizeof(why), err_get());
    err_set("the download failed: %s", why);
  } else if (resp.status != 200) {
    err_set("the download failed: HTTP %d from %s", resp.status, resp.final_host);
  } else if (d.got != d.total) {
    err_set("the download is incomplete (%llu of %llu bytes)", (unsigned long long)d.got,
            (unsigned long long)d.total);
  } else {
    unsigned char digest[32];
    char hex[65];
    mbedtls_sha256_finish(&d.sha, digest);
    hex_encode(hex, digest, 32);
    if (strcmp(hex, rel->asset_sha256) != 0) {
      err_set("the download is damaged: its SHA-256 isn't the one GitHub published");
      log_line("download: sha256 %s, expected %s", hex, rel->asset_sha256);
    } else if (fsync(d.fd) != 0) {
      err_set("can't save the download: %s", strerror(errno));
    } else {
      rc = 0;
    }
  }
  mbedtls_sha256_free(&d.sha);
  close(d.fd);
  http_resp_free(&resp);
  if (rc == 0 && rename(part, dest) != 0) {
    err_set("can't save the download: %s", strerror(errno));
    rc = -1;
  }
  if (rc != 0)
    fs_remove_work_file(part);
  return rc;
}
