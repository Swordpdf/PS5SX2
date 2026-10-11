/* PS5SX2 Installer: the report of one PS5SX2 session.
 *
 * The same pieces as the settings page's "Download logs" (fe_web.cpp ApiReport), for one session: its lines of
 * settings.log, its boot.log, emulog.txt and stderr.log, a GPU hang dump written during it, the settings files,
 * the switch names, and the end of the helper's log. Not included: the game list, the settings page's token.
 * IPv4 addresses are replaced with x.x.x.x. The logs are in /data/PCSX2/logs or, on a console set up from a
 * release (no logs/ folder), in /data/PCSX2 itself (log_dirs). report_build also tells the logger whether the
 * session only showed the shelf and ended normally (report_quiet_shelf): 1.3 doesn't send those. */
#include "report.h"

#include "config.h"
#include "fsx.h"
#include "paths.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *line_end(const char *p, const char *end) {
  const char *nl = memchr(p, '\n', (size_t)(end - p));
  return nl ? nl + 1 : end;
}

static int line_has(const char *p, const char *e, const char *needle) {
  return mem_find(p, (size_t)(e - p), needle, strlen(needle)) != NULL;
}

static void copy_after(char *dst, size_t size, const char *p, const char *e, const char *key, const char *stop) {
  const char *k = mem_find(p, (size_t)(e - p), key, strlen(key));
  dst[0] = '\0';
  if (!k)
    return;
  k += strlen(key);
  const char *s = stop ? mem_find(k, (size_t)(e - k), stop, strlen(stop)) : NULL;
  const char *end = s ? s : e;
  while (end > k && (end[-1] == '\n' || end[-1] == '\r'))
    end--;
  size_t n = (size_t)(end - k);
  if (n >= size)
    n = size - 1;
  memcpy(dst, k, n);
  dst[n] = '\0';
}

static void add_file(sbuf *out, const char *path, const char *name, const char *what, size_t cap, size_t head,
                     time_t newer_than) {
  sbuf text;
  sb_init(&text);
  int64_t size = 0;
  time_t mtime = 0;
  if (fs_read_capped(path, cap, head, &text, &size, &mtime) != 0) {
    sb_printf(out, "\n===== %s (%s): not there =====\n", name, what);
    sb_free(&text);
    return;
  }
  if (newer_than && mtime + 5 < newer_than) {
    sb_printf(out, "\n===== %s (%s): from an earlier session, left out =====\n", name, what);
    sb_free(&text);
    return;
  }
  char when[32], hs[32];
  fmt_utc(when, sizeof(when), mtime);
  human_bytes(hs, sizeof(hs), (uint64_t)size);
  sb_printf(out, "\n===== %s (%s): %s, last written %s =====\n", name, what, hs, when);
  if (text.len)
    sb_append(out, text.data, text.len);
  if (!text.len || text.data[text.len - 1] != '\n')
    sb_puts(out, "\n");
  sb_printf(out, "===== end of %s =====\n", name);
  sb_free(&text);
}

typedef struct {
  sbuf *out;
  size_t count;
} flag_list;

static int list_flag(void *ctx, const char *name, int is_dir) {
  flag_list *f = ctx;
  if (!is_dir) {
    sb_printf(f->out, "%s\n", name);
    f->count++;
  }
  return 0;
}

static int is_ip_char(char c) { return isdigit((unsigned char)c) || c == '.'; }
static int is_hex_colon(char c) { return isxdigit((unsigned char)c) || c == ':'; }

/* An IPv6 address or a MAC address starting at s: a run of hex digits and colons with "::", or at least 5
 * colons, or at least 3 colons and a letter a-f. Times ("20:04:24") don't qualify. Returns its length or 0. */
static size_t v6_len(const char *s, const char *end) {
  const char *p = s;
  int colons = 0, letters = 0, dbl = 0, group = 0;
  while (p < end && is_hex_colon(*p)) {
    if (*p == ':') {
      colons++;
      if (p + 1 < end && p[1] == ':')
        dbl = 1;
      group = 0;
    } else {
      if (++group > 4)
        return 0;
      if (isalpha((unsigned char)*p))
        letters = 1;
    }
    p++;
  }
  if (p < end && (isalnum((unsigned char)*p) || *p == '_'))
    return 0;
  if (colons < 2 || p - s < 3)
    return 0;
  if (dbl || colons >= 5 || (colons >= 3 && letters))
    return (size_t)(p - s);
  return 0;
}

int report_redact(sbuf *text, const char *secret) {
  if (!text->data)
    return 0;
  sbuf o;
  sb_init(&o);
  if (sb_reserve(&o, text->len + 64) != 0)
    return -1;
  const char *s = text->data, *end = text->data + text->len;
  const size_t sl = secret ? strlen(secret) : 0;
  int rc = 0;
  while (s < end && rc == 0) {
    if (sl >= 6 && (size_t)(end - s) >= sl && !memcmp(s, secret, sl)) {
      rc = sb_puts(&o, "<token>");
      s += sl;
      continue;
    }
    const int boundary = s == text->data || !(isalnum((unsigned char)s[-1]) || s[-1] == ':' || s[-1] == '.');
    if (boundary && is_hex_colon(*s)) {
      const size_t n = v6_len(s, end);
      if (n) {
        rc = sb_puts(&o, "x:x:x:x");
        s += n;
        continue;
      }
    }
    if (isdigit((unsigned char)*s) && (s == text->data || !is_ip_char(s[-1]))) {
      /* d{1,3}.d{1,3}.d{1,3}.d{1,3}, not part of a longer dotted number */
      const char *p = s;
      int parts = 0, ok = 1;
      while (parts < 4) {
        int digits = 0, v = 0;
        while (p < end && isdigit((unsigned char)*p) && digits < 4) {
          v = v * 10 + (*p - '0');
          p++;
          digits++;
        }
        if (digits == 0 || digits > 3 || v > 255) {
          ok = 0;
          break;
        }
        parts++;
        if (parts < 4) {
          if (p < end && *p == '.' && p + 1 < end && isdigit((unsigned char)p[1]))
            p++;
          else {
            ok = 0;
            break;
          }
        }
      }
      if (ok && parts == 4 &&
          (p == end || !is_ip_char(*p) || (*p == '.' && (p + 1 == end || !isdigit((unsigned char)p[1]))))) {
        rc = sb_puts(&o, "x.x.x.x");
        s = p;
        continue;
      }
      /* not an address: copy the whole run of digits and dots */
      const char *q = s;
      while (q < end && is_ip_char(*q))
        q++;
      rc = sb_append(&o, s, (size_t)(q - s));
      s = q;
      continue;
    }
    rc = sb_append(&o, s, 1);
    s++;
  }
  if (rc != 0) {
    sb_free(&o);
    err_set("out of memory while removing addresses from the report");
    return -1;
  }
  sb_free(text);
  *text = o;
  return 0;
}

/* ---- where the session's logs are ---- */

/* PS5SX2 writes its logs where OrbisLogPath (coreorbis/orbis-shims/orbis_paths.cpp) puts them: in logs/ when that
 * folder exists, otherwise in /data/PCSX2 itself. The release zips have no logs/ folder and PS5SX2 doesn't make
 * one, so a console set up from a release keeps every log in /data/PCSX2 (1.0 and 1.1 only looked in logs/, so
 * their reports from those consoles had no logs and no game). Both places are searched, logs/ first. Returns how
 * many places there are. */
static int log_dirs(char dirs[2][PATH_LEN]) {
  int n = 0;
  if (path_join(dirs[0], PATH_LEN, g_p.pcsx2, "logs") == 0 && fs_is_dir(dirs[0]))
    n = 1;
  str_copy(dirs[n], PATH_LEN, g_p.pcsx2);
  return n + 1;
}

/* "/data/PCSX2/logs" or "/data/PCSX2", for the report's header (on the host, dirs have the test root in front). */
static const char *log_dir_name(const char *dir) {
  return str_ends(dir, "/logs") ? P_PCSX2 "/logs" : P_PCSX2;
}

/* One place's settings.log, with the settings.1.log before it (the settings page starts a new settings.log at
 * 512 KB and the old one becomes settings.1.log). */
static void read_settings_log(const char *dir, sbuf *slog) {
  char p[PATH_LEN];
  sbuf part;
  sb_init(&part);
  sb_clear(slog);
  path_join(p, sizeof(p), dir, "settings.1.log");
  if (fs_read_capped(p, 1u << 20, 0, &part, NULL, NULL) == 0 && part.len) {
    sb_append(slog, part.data, part.len);
    if (part.data[part.len - 1] != '\n')
      sb_puts(slog, "\n");
  }
  path_join(p, sizeof(p), dir, "settings.log");
  if (fs_read_capped(p, 2u << 20, 0, &part, NULL, NULL) == 0 && part.len)
    sb_append(slog, part.data, part.len);
  sb_free(&part);
}

/* The session's "app start: ... (pid N)" line: the last one, since pids come round again after a restart. */
static const char *find_session(const sbuf *slog, pid_t pid) {
  if (!slog->data || !slog->len)
    return NULL;
  const char *b = slog->data, *e = slog->data + slog->len, *sess = NULL;
  char pidmark[32];
  snprintf(pidmark, sizeof(pidmark), "(pid %d)", (int)pid);
  for (const char *l = b; l < e; l = line_end(l, e)) {
    const char *le = line_end(l, e);
    if (line_has(l, le, "  app start: ") && line_has(l, le, pidmark))
      sess = l;
  }
  return sess;
}

/* The session's own logs. PS5SX2 starts new ones at every start and the earlier ones move up (.1, .2, ...), so
 * they are the set whose boot log has this session's "[boot] pid=N" line. Sets *dir (an index into dirs) and *sfx
 * and keeps that boot log's start and end in text; returns 1. Without such a line: the newest boot log there is
 * (dirs[*dir]/boot<*sfx>.log, *sfx left as given), and 0. */
static int find_boot_log(char dirs[2][PATH_LEN], int ndirs, pid_t pid, int *dir, const char **sfx, sbuf *text) {
  static const char *const sfxs[] = {"", ".1", ".2"};
  char mark[48], name[32], p[PATH_LEN];
  snprintf(mark, sizeof(mark), "[boot] pid=%d\n", (int)pid);
  sb_clear(text);
  for (int d = 0; d < ndirs; d++)
    for (int k = 0; k < 3; k++) {
      snprintf(name, sizeof(name), "boot%s.log", sfxs[k]);
      path_join(p, sizeof(p), dirs[d], name);
      if (fs_read_capped(p, 256u << 10, 128u << 10, text, NULL, NULL) == 0 && text->data &&
          strstr(text->data, mark) != NULL) {
        *dir = d;
        *sfx = sfxs[k];
        return 1;
      }
    }
  sb_clear(text);
  *dir = 0;
  for (int d = 0; d < ndirs; d++) {
    snprintf(name, sizeof(name), "boot%s.log", *sfx);
    path_join(p, sizeof(p), dirs[d], name);
    if (fs_exists(p)) {
      *dir = d;
      break;
    }
  }
  return 0;
}

/* The last line of text starting with key, what follows it (to the end of the line) into dst. */
static void last_value(char *dst, size_t size, const sbuf *text, const char *key) {
  dst[0] = '\0';
  if (!text->data)
    return;
  const char *b = text->data, *e = text->data + text->len;
  for (const char *l = b; l < e; l = line_end(l, e))
    if ((size_t)(e - l) > strlen(key) && !memcmp(l, key, strlen(key)))
      copy_after(dst, size, l, line_end(l, e), key, NULL);
}

int report_quiet_shelf(const session_info *s) {
  return s->logs_found && !s->game[0] && !s->problem && (!strcmp(s->end, "ok") || !strcmp(s->end, "no-game"));
}

int report_build(session_info *s, sbuf *out) {
  sb_clear(out);
  char p[PATH_LEN];
  char dirs[2][PATH_LEN];
  const int ndirs = log_dirs(dirs);

  /* settings.log: the session's lines, from whichever place has its start line */
  sbuf slog;
  sb_init(&slog);
  const char *sess = NULL, *sess_end = NULL, *ctx_start = NULL;
  int slog_dir = -1;
  for (int d = 0; d < ndirs && !sess; d++) {
    read_settings_log(dirs[d], &slog);
    sess = find_session(&slog, s->pid);
    if (sess || (slog_dir < 0 && slog.len))
      slog_dir = d;
  }
  if (!sess && slog_dir >= 0 && slog_dir != ndirs - 1)
    read_settings_log(dirs[slog_dir], &slog); /* no start line anywhere: the end of the first settings.log found */
  const int have_slog = slog.data && slog.len;
  s->label[0] = s->build[0] = s->game[0] = s->end_line[0] = '\0';
  str_copy(s->end, sizeof(s->end), "ok");
  if (have_slog) {
    const char *b = slog.data, *e = slog.data + slog.len;
    if (sess) {
      sess_end = e;
      for (const char *l = line_end(sess, e); l < e; l = line_end(l, e))
        if (line_has(l, line_end(l, e), "  app start: ")) {
          sess_end = l;
          break;
        }
      /* 30 lines of what came before */
      ctx_start = sess;
      for (int k = 0; k < 30 && ctx_start > b; k++) {
        const char *q = ctx_start - 1;
        while (q > b && q[-1] != '\n')
          q--;
        ctx_start = q;
      }
      copy_after(s->label, sizeof(s->label), sess, line_end(sess, e), "app start: ", " (pid ");
      const char *dot = NULL;
      for (const char *q = s->label; (q = strstr(q, " \xc2\xb7 ")) != NULL; q++)
        dot = q;
      str_copy(s->build, sizeof(s->build), dot ? dot + 4 : s->label);
      /* how it ended: the most serious event of the session names it (its first line explains it) */
      int best = 0, note = 0;
      for (const char *l = sess; l < sess_end; l = line_end(l, sess_end)) {
        const char *le = line_end(l, sess_end);
        if (line_has(l, le, "  game start: "))
          copy_after(s->game, sizeof(s->game), l, le, "game start: ", " | ");
        const int rank = line_has(l, le, "  crash: ")                 ? 4
                         : line_has(l, le, "GPU hang (")              ? 3
                         : line_has(l, le, "the game didn't start")   ? 2
                         : line_has(l, le, "no game to start")        ? 1
                                                                      : 0;
        if (line_has(l, le, "tester note: "))
          note = 1;
        if (rank > best) {
          best = rank;
          size_t n = (size_t)(le - l);
          while (n && (l[n - 1] == '\n' || l[n - 1] == '\r'))
            n--;
          if (n >= sizeof(s->end_line))
            n = sizeof(s->end_line) - 1;
          memcpy(s->end_line, l, n);
          s->end_line[n] = '\0';
        }
      }
      static const char *const ends[] = {"ok", "no-game", "no-start", "gpu-hang", "crash"};
      snprintf(s->end, sizeof(s->end), "%s%s", ends[best], note ? "+note" : "");
    }
  }

  /* this session's boot.log, emulog.txt and stderr.log */
  const char *sfx = s->suffix ? s->suffix : "";
  int ldir = 0;
  sbuf boot;
  sb_init(&boot);
  const int boot_found = find_boot_log(dirs, ndirs, s->pid, &ldir, &sfx, &boot);
  /* no start line in settings.log (it's gone, or a build that doesn't write one): the boot log names the build
   * and the game too */
  if (boot_found && !s->build[0])
    last_value(s->build, sizeof(s->build), &boot, "[boot] build=");
  if (boot_found && !s->game[0]) {
    char path[512];
    last_value(path, sizeof(path), &boot, "[boot] game: ");
    if (path[0])
      str_copy(s->game, sizeof(s->game), path_base(path));
  }
  s->logs_found = sess != NULL || boot_found;
  s->problem = 0;
  if (boot_found) {
    /* what went wrong, as PS5SX2 prints it to boot.log: the crash handler (ProsperoCrash.cpp), a GPU hang
     * (GSDeviceVK.cpp), a game that didn't start (main-boot.cpp), and a game list with nothing in it (the shelf,
     * fe_ps5.cpp, or the plain list, game_select.cpp). settings.log has most of these too, when it has the session. */
    static const char *const marks[] = {"[crash] ", "GPU hang (", "the game didn't start",
                                        "[frontend] 0 disc image(s)", "[menu] 0 disc image(s)"};
    for (size_t k = 0; k < sizeof(marks) / sizeof(marks[0]) && !s->problem; k++)
      s->problem = strstr(boot.data, marks[k]) != NULL;
    if (!sess) {
      /* no settings.log lines for the session: how it ended, from the boot log (the most serious first) */
      static const struct {
        const char *mark, *end;
      } boot_ends[] = {{"[crash] ", "crash"},
                       {"GPU hang (", "gpu-hang"},
                       {"the game didn't start", "no-start"},
                       {"[boot] no game to start", "no-game"}};
      for (size_t k = 0; k < sizeof(boot_ends) / sizeof(boot_ends[0]); k++) {
        const char *m = strstr(boot.data, boot_ends[k].mark);
        if (!m)
          continue;
        str_copy(s->end, sizeof(s->end), boot_ends[k].end);
        while (m > boot.data && m[-1] != '\n')
          m--;
        const char *le = line_end(m, boot.data + boot.len);
        size_t n = (size_t)(le - m);
        while (n && (m[n - 1] == '\n' || m[n - 1] == '\r'))
          n--;
        if (n >= sizeof(s->end_line))
          n = sizeof(s->end_line) - 1;
        memcpy(s->end_line, m, n);
        s->end_line[n] = '\0';
        break;
      }
    }
  }
  sb_free(&boot);
  if (!s->logs_found)
    str_copy(s->end, sizeof(s->end), "no-logs");
  if (!s->build[0])
    str_copy(s->build, sizeof(s->build), "unknown");

  /* a GPU hang dump written during the session */
  for (int k = 0; k < 2 && !s->problem; k++) {
    path_join(p, sizeof(p), dirs[ldir], k ? "ps5vk-hang.txt" : "vkhang.txt");
    const time_t t = fs_mtime(p);
    s->problem = t != 0 && t + 5 >= s->started;
  }

  /* header */
  char tester[64] = "", cid[40] = "", started[32];
  {
    sbuf t;
    sb_init(&t);
    if (fs_read_file(g_p.tester_name, 256, &t) == 0 && t.data)
      str_ascii(tester, sizeof(tester), str_trim(t.data));
    if (fs_read_file(g_p.console_id, 64, &t) == 0 && t.data)
      str_ascii(cid, sizeof(cid), str_trim(t.data));
    sb_free(&t);
  }
  fmt_utc(started, sizeof(started), s->started);
  sb_printf(out, "PS5SX2 session report\n");
  sb_printf(out, "Sent by: %s %s, automatically after PS5SX2 closed%s\n", INSTALLER_NAME, INSTALLER_VERSION,
            s->watched ? "" : " (this session ended before the installer was running: the PS5 may have restarted)");
  sb_printf(out, "Build: %s\n", s->label[0] ? s->label : s->build);
  sb_printf(out, "Ended: %s%s%s\n", s->end, s->end_line[0] ? " - " : "", s->end_line);
  sb_printf(out, "Game: %s\n", s->game[0]       ? s->game
                               : s->logs_found ? "none (the shelf only)"
                                               : "unknown (this session's logs weren't found)");
  sb_printf(out, "Session: pid %d, started %s\n", (int)s->pid, started);
  sb_printf(out, "Tester: %s  Console ID: %s\n", tester[0] ? tester : "(no tester-name.txt)", cid[0] ? cid : "-");
  if (boot_found)
    sb_printf(out, "Logs: %s%s (boot%s.log is this session's)\n", log_dir_name(dirs[ldir]),
              ndirs == 1 ? ", no logs/ folder" : "", sfx);
  else
    sb_printf(out, "Logs: no boot log with this session's pid in %s\n",
              ndirs == 2 ? P_PCSX2 "/logs or " P_PCSX2 : P_PCSX2 " (no logs/ folder)");
  if (s->shelf_skipped)
    sb_printf(out, "Not sent since the last report: %u session(s) that only showed the shelf and ended normally\n",
              s->shelf_skipped);
  sb_printf(out, "IP addresses in this report are replaced with x.x.x.x. No game list, no token.\n");

  /* settings.log */
  if (sess) {
    sb_printf(out, "\n===== settings.log (this session, with the 30 lines before it) =====\n");
    size_t n = (size_t)(sess_end - ctx_start);
    if (n > (384u << 10)) {
      const char *cut = sess_end - (384u << 10);
      sb_printf(out, "[... %zu bytes left out ...]\n", (size_t)(cut - ctx_start));
      ctx_start = cut;
      n = (size_t)(sess_end - ctx_start);
    }
    sb_append(out, ctx_start, n);
    if (n && ctx_start[n - 1] != '\n')
      sb_puts(out, "\n");
    sb_puts(out, "===== end of settings.log =====\n");
  } else {
    sb_printf(out, "\n===== settings.log: %s =====\n",
              have_slog ? "this session's start line wasn't found; the end of the file follows" : "not there");
    if (have_slog) {
      const size_t n = slog.len > (64u << 10) ? (64u << 10) : slog.len;
      sb_append(out, slog.data + slog.len - n, n);
      sb_puts(out, "\n===== end of settings.log =====\n");
    }
  }
  sb_free(&slog);

  /* this session's logs (find_boot_log chose the place and the set) */
  const char *logs = dirs[ldir];
  char name[64];
  const char *what = boot_found ? "this session"
                     : *sfx     ? "the one before the newest; this session's pid isn't in it, so it may be another's"
                                : "the newest; this session's pid isn't in it, so it may be another's";
  snprintf(name, sizeof(name), "boot%s.log", sfx);
  path_join(p, sizeof(p), logs, name);
  add_file(out, p, name, what, 512u << 10, 64u << 10, 0);
  snprintf(name, sizeof(name), "emulog%s.txt", sfx);
  path_join(p, sizeof(p), logs, name);
  add_file(out, p, name, what, 2u << 20, 160u << 10, 0);
  snprintf(name, sizeof(name), "stderr%s.log", sfx);
  path_join(p, sizeof(p), logs, name);
  add_file(out, p, name, what, 512u << 10, 32u << 10, 0);
  path_join(p, sizeof(p), logs, "vkhang.txt");
  add_file(out, p, "vkhang.txt", "PCSX2's GPU hang dump, if written in this session", 64u << 10, 32u << 10,
           s->started);
  path_join(p, sizeof(p), logs, "ps5vk-hang.txt");
  add_file(out, p, "ps5vk-hang.txt", "the driver's GPU hang dump, if written in this session", 64u << 10,
           32u << 10, s->started);
  path_join(p, sizeof(p), logs, "pf.log");
  add_file(out, p, "pf.log", "page faults, if written in this session", 64u << 10, 16u << 10, s->started);

  /* settings */
  path_join(p, sizeof(p), g_p.pcsx2, "gs.ini");
  add_file(out, p, "gs.ini", "the settings every game starts from", 64u << 10, 32u << 10, 0);
  path_join(p, sizeof(p), g_p.pcsx2, "live.ini");
  add_file(out, p, "live.ini", "display and overlay switches", 16u << 10, 8u << 10, 0);
  if (s->game[0]) {
    char stem[256];
    str_copy(stem, sizeof(stem), s->game);
    char *dot = strrchr(stem, '.');
    if (dot && (!strcasecmp(dot, ".iso") || !strcasecmp(dot, ".chd") || !strcasecmp(dot, ".cso") ||
                !strcasecmp(dot, ".bin")))
      *dot = '\0';
    if (stem[0] && !strchr(stem, '/') && strcmp(stem, "..") && strcmp(stem, ".")) {
      char file[300], dir[PATH_LEN];
      snprintf(file, sizeof(file), "%s.ini", stem);
      path_join(dir, sizeof(dir), g_p.pcsx2, "settings");
      path_join(p, sizeof(p), dir, file);
      char label[340];
      snprintf(label, sizeof(label), "settings/%s", file);
      add_file(out, p, label, "this game's own settings", 32u << 10, 16u << 10, 0);
    }
  }
  {
    path_join(p, sizeof(p), g_p.pcsx2, "flags");
    sb_puts(out, "\n===== switch files in flags/ =====\n");
    flag_list fl = {out, 0};
    if (fs_is_dir(p))
      fs_list(p, list_flag, &fl);
    sb_printf(out, "===== %zu switch files =====\n", fl.count);
  }
  /* the helper (jailbreak, /data mount) */
  path_join(p, sizeof(p), g_p.data, "PS5SXHelper.log");
  add_file(out, p, "PS5SXHelper.log", "the PS5SX2 Helper's log, its end", 64u << 10, 0, 0);
  add_file(out, g_p.installed, "installed.txt", "the last install by the installer", 4096, 0, 0);
  sb_puts(out, "\n===== end of the report =====\n");

  /* the settings page's token must never leave the console */
  char secret[128] = "";
  {
    sbuf t;
    sb_init(&t);
    path_join(p, sizeof(p), g_p.pcsx2, "webui_token.txt");
    if (fs_exists(p)) {
      if (fs_read_file(p, 120, &t) != 0 || !t.data) {
        sb_free(&t);
        err_set("the settings page's token file can't be read, so it couldn't be removed from the report");
        return -1;
      }
      str_copy(secret, sizeof(secret), str_trim(t.data));
    }
    sb_free(&t);
  }
  if (report_redact(out, secret[0] ? secret : NULL) != 0)
    return -1;
  if (out->len > REPORT_MAX_BYTES) {
    out->len = REPORT_MAX_BYTES - 64;
    out->data[out->len] = '\0';
    sb_puts(out, "\n[... the report was cut at 8 MB ...]\n");
  }
  return out->data ? 0 : -1;
}
