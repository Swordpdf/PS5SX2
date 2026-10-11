/* PS5SX2 Installer: stays running, and sends each PS5SX2 session's logs when the app closes.
 *
 * How a session is seen: PS5SX2 writes its pid to /data/PCSX2/pid.txt when it starts (main-boot.cpp). The
 * logger checks every few seconds whether that process still exists (kill(pid, 0)). When it's gone, the
 * session's report goes into outbox/ and is sent to the relay (a Cloudflare Worker that posts it to a private
 * Discord channel). Reports that can't be sent wait in the outbox (at most 20 / 60 MB, oldest dropped).
 *
 * 1.3: a session that only showed the shelf and ended normally isn't sent (report_quiet_shelf; the next report's
 * header counts them); the send-shelf-logs switch sends them too. The logger shows no notification when it starts
 * or sends (only when it can't run, or is switched off while running). */
#include "logger.h"

#include "app.h"
#include "config.h"
#include "fsx.h"
#include "http.h"
#include "log.h"
#include "paths.h"
#include "plat.h"
#include "report.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  pid_t pid;
  time_t started;
} sess_id;

static char g_relay[1024];
static unsigned g_shelf_skipped; /* sessions not sent since the last report (logger-state.txt, skipped_shelf=) */

const char *logger_relay_url(void) {
  g_relay[0] = '\0';
  sbuf t;
  sb_init(&t);
  if (fs_read_file(g_p.upload_url, 2048, &t) == 0 && t.data) {
    char *nl = strchr(t.data, '\n');
    if (nl)
      *nl = '\0';
    char *u = str_trim(t.data);
    char host[256], path[1024];
    int port;
    if (url_split(u, host, sizeof(host), &port, path, sizeof(path)) == 0)
      str_copy(g_relay, sizeof(g_relay), u);
    else
      log_line("upload-url.txt ignored: %s", err_get());
  }
  sb_free(&t);
  if (!g_relay[0]) {
    const char *test = plat_test_env("PS5SX2_TEST_RELAY_URL");
    str_copy(g_relay, sizeof(g_relay), test ? test : RELAY_URL_DEFAULT);
  }
  return g_relay;
}

void logger_ensure_console_id(void) {
  if (fs_exists(g_p.console_id))
    return;
  uint8_t r[8];
  char hex[17 + 1];
  if (plat_random(r, sizeof(r)) != 0)
    return;
  hex_encode(hex, r, sizeof(r));
  str_copy(hex + 16, 2, "\n");
  fs_write_atomic(g_p.console_id, hex, 17, 0666);
}

static void state_load(sess_id *last) {
  memset(last, 0, sizeof(*last));
  sbuf t;
  sb_init(&t);
  if (fs_read_file(g_p.logger_state, 4096, &t) == 0 && t.data) {
    const char *p = strstr(t.data, "last_pid=");
    const char *q = strstr(t.data, "last_started=");
    const char *k = strstr(t.data, "skipped_shelf=");
    if (p)
      last->pid = (pid_t)strtol(p + 9, NULL, 10);
    if (q)
      last->started = (time_t)strtoll(q + 13, NULL, 10);
    if (k)
      g_shelf_skipped = (unsigned)strtoul(k + 14, NULL, 10);
  }
  sb_free(&t);
}

static void state_save(const sess_id *last) {
  char buf[160];
  const int n = snprintf(buf, sizeof(buf), "last_pid=%d\nlast_started=%lld\nskipped_shelf=%u\n", (int)last->pid,
                         (long long)last->started, g_shelf_skipped);
  fs_write_atomic(g_p.logger_state, buf, (size_t)n, 0666);
}

/* ---- outbox ---- */

typedef struct {
  char (*names)[96];
  size_t n, cap;
} txt_list;

static int collect_txt(void *ctx, const char *name, int is_dir) {
  txt_list *l = ctx;
  if (is_dir || !str_ends(name, ".txt") || strlen(name) >= 96)
    return 0;
  if (l->n == l->cap) {
    const size_t cap = l->cap ? l->cap * 2 : 16;
    char(*p)[96] = realloc(l->names, cap * sizeof(*p));
    if (!p)
      return 1;
    l->names = p;
    l->cap = cap;
  }
  str_copy(l->names[l->n++], 96, name);
  return 0;
}

static int cmp96(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void outbox_list(txt_list *l) {
  memset(l, 0, sizeof(*l));
  if (fs_is_dir(g_p.outbox))
    fs_list(g_p.outbox, collect_txt, l);
  if (l->n)
    qsort(l->names, l->n, sizeof(*l->names), cmp96);
}

static void outbox_remove(const char *txt_name) {
  char p[PATH_LEN], meta[PATH_LEN];
  if (path_join(p, sizeof(p), g_p.outbox, txt_name) != 0)
    return;
  str_copy(meta, sizeof(meta), p);
  const size_t n = strlen(meta);
  if (n > 4) {
    memcpy(meta + n - 4, ".meta", 6);
    fs_remove_work_file(meta);
  }
  fs_remove_work_file(p);
}

static void outbox_trim(void) {
  txt_list l;
  outbox_list(&l);
  uint64_t total = 0;
  for (size_t i = 0; i < l.n; i++) {
    char p[PATH_LEN];
    path_join(p, sizeof(p), g_p.outbox, l.names[i]);
    const int64_t s = fs_size(p);
    total += s > 0 ? (uint64_t)s : 0;
  }
  size_t i = 0;
  while (l.n - i > OUTBOX_MAX_FILES || (total > OUTBOX_MAX_BYTES && l.n - i > 1)) {
    char p[PATH_LEN];
    path_join(p, sizeof(p), g_p.outbox, l.names[i]);
    const int64_t s = fs_size(p);
    total -= s > 0 ? (uint64_t)s : 0;
    log_line("outbox: dropping the oldest report %s (outbox full)", l.names[i]);
    outbox_remove(l.names[i]);
    i++;
  }
  free(l.names);
}

static int outbox_add(const session_info *s, const sbuf *report) {
  if (fs_mkdirs(g_p.outbox, 0777) != 0)
    return -1;
  char stamp[32], base[64], p[PATH_LEN];
  fmt_stamp(stamp, sizeof(stamp), now_utc());
  snprintf(base, sizeof(base), "%s_%d", stamp, (int)s->pid);
  char name[96];
  snprintf(name, sizeof(name), "%s.meta", base);
  path_join(p, sizeof(p), g_p.outbox, name);
  sbuf m;
  sb_init(&m);
  char game[256], build[80], label[160];
  str_ascii(game, sizeof(game), s->game);
  str_ascii(build, sizeof(build), s->build);
  str_ascii(label, sizeof(label), s->label);
  sb_printf(&m, "build=%s\nend=%s\ngame=%s\nlabel=%s\npid=%d\nstarted=%lld\n", build, s->end, game, label,
            (int)s->pid, (long long)s->started);
  int rc = fs_write_atomic(p, m.data, m.len, 0666);
  sb_free(&m);
  snprintf(name, sizeof(name), "%s.txt", base);
  path_join(p, sizeof(p), g_p.outbox, name);
  if (rc == 0)
    rc = fs_write_atomic(p, report->data, report->len, 0666);
  outbox_trim();
  return rc;
}

static void meta_get(const char *meta, const char *key, char *out, size_t size) {
  out[0] = '\0';
  char k[32];
  snprintf(k, sizeof(k), "%s=", key);
  for (const char *l = meta; l && *l;) {
    const char *nl = strchr(l, '\n');
    const size_t len = nl ? (size_t)(nl - l) : strlen(l);
    if (len >= strlen(k) && !strncmp(l, k, strlen(k))) {
      size_t n = len - strlen(k);
      if (n >= size)
        n = size - 1;
      memcpy(out, l + strlen(k), n);
      out[n] = '\0';
      return;
    }
    l = nl ? nl + 1 : NULL;
  }
}

static void meta_bump_attempts(const char *meta_path, const char *meta_text) {
  char n[16];
  meta_get(meta_text, "attempts", n, sizeof(n));
  const long a = strtol(n[0] ? n : "0", NULL, 10) + 1;
  sbuf m;
  sb_init(&m);
  for (const char *l = meta_text; l && *l;) {
    const char *nl = strchr(l, '\n');
    const size_t len = nl ? (size_t)(nl - l) + 1 : strlen(l);
    if (strncmp(l, "attempts=", 9) != 0)
      sb_append(&m, l, len);
    l += len;
  }
  sb_printf(&m, "attempts=%ld\n", a);
  if (m.data)
    fs_write_atomic(meta_path, m.data, m.len, 0666);
  sb_free(&m);
}

static void outbox_reject(const char *txt_path, const char *meta_path, const char *name, const char *why) {
  char rej[PATH_LEN], dst[PATH_LEN];
  path_join(rej, sizeof(rej), g_p.outbox, "rejected");
  fs_mkdirs(rej, 0777);
  path_join(dst, sizeof(dst), rej, name);
  log_line("outbox: %s moved to outbox/rejected (%s)", name, why);
  if (fs_move(txt_path, dst) != 0)
    fs_remove_work_file(txt_path);
  memcpy(dst + strlen(dst) - 4, ".meta", 6);
  if (fs_move(meta_path, dst) != 0)
    fs_remove_work_file(meta_path);
  /* keep the last 10 */
  txt_list l;
  memset(&l, 0, sizeof(l));
  fs_list(rej, collect_txt, &l);
  if (l.n > 10) {
    qsort(l.names, l.n, sizeof(*l.names), cmp96);
    for (size_t i = 0; i + 10 < l.n; i++) {
      char p[PATH_LEN], m[PATH_LEN];
      path_join(p, sizeof(p), rej, l.names[i]);
      str_copy(m, sizeof(m), p);
      memcpy(m + strlen(m) - 4, ".meta", 6);
      fs_remove_work_file(p);
      fs_remove_work_file(m);
    }
  }
  free(l.names);
}

int logger_send_outbox(void) {
  const char *url = logger_relay_url();
  if (!url[0])
    return -1;
  logger_ensure_console_id();
  char cid[40] = "", tester[48] = "";
  {
    sbuf t;
    sb_init(&t);
    if (fs_read_file(g_p.console_id, 64, &t) == 0 && t.data)
      str_ascii(cid, sizeof(cid), str_trim(t.data));
    if (fs_read_file(g_p.tester_name, 256, &t) == 0 && t.data)
      str_ascii(tester, sizeof(tester), str_trim(t.data));
    sb_free(&t);
  }
  if (strlen(cid) != 16 || !hex_is(cid, 16)) {
    log_line("outbox: no console ID (console-id.txt), not sending");
    return -1;
  }
  txt_list l;
  outbox_list(&l);
  int sent = 0;
  for (size_t i = 0; i < l.n; i++) {
    char p[PATH_LEN], mp[PATH_LEN];
    path_join(p, sizeof(p), g_p.outbox, l.names[i]);
    str_copy(mp, sizeof(mp), p);
    memcpy(mp + strlen(mp) - 4, ".meta", 6);
    sbuf body, meta;
    sb_init(&body);
    sb_init(&meta);
    if (fs_read_file(p, REPORT_MAX_BYTES + 4096, &body) != 0 || !body.data) {
      log_line("outbox: %s: %s", l.names[i], err_get());
      sb_free(&body);
      outbox_remove(l.names[i]);
      continue;
    }
    fs_read_file(mp, 8192, &meta);
    const char *mt = meta.data ? meta.data : "";
    char build[80], end[48], game[256], tries[16];
    meta_get(mt, "build", build, sizeof(build));
    meta_get(mt, "end", end, sizeof(end));
    meta_get(mt, "game", game, sizeof(game));
    meta_get(mt, "attempts", tries, sizeof(tries));
    char h_cid[80], h_tester[96], h_build[128], h_end[96], h_game[300], h_ver[64];
    snprintf(h_cid, sizeof(h_cid), "X-PS5SX2-Console: %s", cid);
    snprintf(h_tester, sizeof(h_tester), "X-PS5SX2-Tester: %s", tester);
    snprintf(h_build, sizeof(h_build), "X-PS5SX2-Build: %s", build[0] ? build : "unknown");
    snprintf(h_end, sizeof(h_end), "X-PS5SX2-End: %s", end[0] ? end : "ok");
    snprintf(h_game, sizeof(h_game), "X-PS5SX2-Game: %s", game);
    snprintf(h_ver, sizeof(h_ver), "X-PS5SX2-Installer: %s", INSTALLER_VERSION);
    const char *headers[] = {"Content-Type: text/plain; charset=utf-8", h_cid, h_tester, h_build, h_end, h_game,
                             h_ver, NULL};
    http_resp resp;
    http_resp_init(&resp);
    http_req rq;
    memset(&rq, 0, sizeof(rq));
    rq.method = "POST";
    rq.url = url;
    rq.headers = headers;
    rq.body = body.data;
    rq.body_len = body.len;
    const int rc = http_do(&rq, &resp);
    sb_free(&body);
    int stop = 0;
    if (rc != 0) {
      log_line("outbox: sending %s failed: %s", l.names[i], err_get());
      stop = 1; /* no network or no server: try again later */
    } else if (resp.status >= 200 && resp.status < 300) {
      /* 1.3: no notification (it came after every session, over whatever was on the screen) */
      log_line("outbox: %s sent (HTTP %d; %s%s%s)", l.names[i], resp.status, game[0] ? game : "the shelf",
               end[0] && strcmp(end, "ok") ? ", " : "", end[0] && strcmp(end, "ok") ? end : "");
      outbox_remove(l.names[i]);
      sent++;
    } else if (resp.status == 408 || resp.status == 429) {
      log_line("outbox: the log server is busy (HTTP %d); trying later", resp.status);
      stop = 1;
    } else if (resp.status >= 400 && resp.status < 500) {
      char why[300];
      snprintf(why, sizeof(why), "the log server refused it: HTTP %d %.200s", resp.status,
               resp.errbody.data ? resp.errbody.data : "");
      outbox_reject(p, mp, l.names[i], why);
    } else if (strtol(tries[0] ? tries : "0", NULL, 10) + 1 >= 6) {
      outbox_reject(p, mp, l.names[i], "the log server failed on it 6 times");
    } else {
      log_line("outbox: the log server answered HTTP %d for %s; trying later", resp.status, l.names[i]);
      meta_bump_attempts(mp, mt);
      stop = 1;
    }
    sb_free(&meta);
    http_resp_free(&resp);
    if (stop)
      break;
  }
  free(l.names);
  return sent;
}

static size_t outbox_count(void) {
  txt_list l;
  outbox_list(&l);
  free(l.names);
  return l.n;
}

static void report_session(pid_t pid, time_t started, int watched, const char *suffix, sess_id *last) {
  if (pid == last->pid && started == last->started)
    return;
  session_info s;
  memset(&s, 0, sizeof(s));
  s.pid = pid;
  s.started = started;
  s.watched = watched;
  s.suffix = suffix;
  s.shelf_skipped = g_shelf_skipped;
  sbuf out;
  sb_init(&out);
  if (report_build(&s, &out) != 0) {
    log_line("report: session pid %d not reported: %s", (int)pid, err_get());
  } else if (report_quiet_shelf(&s) && !fs_exists(g_p.send_shelf_logs)) {
    g_shelf_skipped++;
    log_line("report: session pid %d (%s, the shelf only, ended %s) not sent: only sessions with a game or a "
             "problem are (%u since the last report; the switch send-shelf-logs sends these too)",
             (int)pid, s.build, s.end, g_shelf_skipped);
  } else if (outbox_add(&s, &out) == 0) {
    log_line("report: session pid %d (%s, %s, ended %s) is in the outbox (%zu bytes)", (int)pid, s.build,
             s.game[0] ? s.game : "shelf", s.end, out.len);
    g_shelf_skipped = 0;
  } else {
    log_line("report: session pid %d not reported: %s", (int)pid, err_get());
  }
  sb_free(&out);
  last->pid = pid;
  last->started = started;
  state_save(last);
}

int logger_run(void) {
  if (fs_exists(g_p.no_log_upload)) {
    log_line("logger: switched off (%s)", g_p.no_log_upload);
    return 0;
  }
  logger_ensure_console_id();
  if (plat_pid_alive(getpid()) != 1) {
    log_line("logger: can't check processes on this system (kill(self, 0) failed: %s)", strerror(errno));
    notify("PS5SX2 log upload can't run on this system (see installer.log)");
    return -1;
  }
  sess_id last, cur = {0, 0}, seen = {0, 0};
  state_load(&last);
  int tracking = 0;
  pid_t pid = 0;
  time_t started = 0;
  int r = app_running(&pid, &started);
  seen.pid = pid;
  seen.started = started;
  if (r == 1) {
    tracking = 1;
    cur = seen;
  } else if (r == 0 && pid > 0 && started > 0 && now_utc() - started < 24 * 3600) {
    report_session(pid, started, 0, "", &last); /* ended while we weren't running (or with the last boot) */
  }
  /* 1.3: no notice on the screen (swordpdf: less clutter); the testers are told when they get the ELF */
  const int relay = logger_relay_url()[0] != '\0';
  log_line("logger: running (relay %s): the logs of game sessions and problems are %s when PS5SX2 closes",
           relay ? logger_relay_url() : "not set", relay ? "sent" : "kept in outbox/");

  uint64_t next_send = 0, backoff_ms = 60000;
  for (;;) {
    if (outbox_count() > 0 && mono_ms() >= next_send) {
      const int n = logger_send_outbox();
      if (n < 0) {
        next_send = mono_ms() + 3600000; /* no relay: look again in an hour (upload-url.txt may appear) */
      } else if (outbox_count() > 0) {
        next_send = mono_ms() + backoff_ms;
        backoff_ms = backoff_ms * 2 > 3600000 ? 3600000 : backoff_ms * 2;
      } else {
        backoff_ms = 60000;
      }
    }
    sleep_ms(LOGGER_POLL_SECONDS * 1000);
    if (fs_exists(g_p.no_log_upload)) {
      log_line("logger: switched off");
      notify("PS5SX2 log upload is off");
      return 0;
    }
    r = app_running(&pid, &started);
    if (r < 0)
      continue; /* can't tell this time */
    int reported = 0;
    if (pid > 0 && (pid != seen.pid || started != seen.started)) {
      /* pid.txt was written again: a new session started since the last check */
      if (tracking) {
        report_session(cur.pid, cur.started, 1, ".1", &last); /* the one we watched ended first */
        tracking = 0;
        reported = 1;
      }
      seen.pid = pid;
      seen.started = started;
      if (r == 1) {
        tracking = 1;
        cur = seen;
      } else {
        sleep_ms(3000); /* started and ended between two checks */
        report_session(pid, started, 1, "", &last);
        reported = 1;
      }
    } else if (tracking && r == 0) {
      sleep_ms(3000); /* let the last log lines land */
      report_session(cur.pid, cur.started, 1, "", &last);
      tracking = 0;
      reported = 1;
    }
    if (reported) {
      next_send = 0;
      backoff_ms = 60000;
    }
  }
}
