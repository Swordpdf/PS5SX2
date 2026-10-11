/* PS5SX2 Installer: install the latest release without touching what belongs to the user.
 *
 * What goes where (paths inside the zip's top folder):
 *   PPSA99203/...        -> /data/homebrew/PPSA99203/...  replaced (the app itself)
 *   PCSX2/resources/...  -> /data/PCSX2/resources/...     replaced (shaders, GameIndex.yaml)
 *   PCSX2/bios, games, memcards, savestates, textures, covers, cache, logs, ... (the user's folders)
 *                        -> the folder is created when missing; nothing is ever written inside
 *   PCSX2/flags/...      -> all of them on a first install (no flags folder yet); later only switches that are
 *                           new in a build, never one the user removed or moved to flags-off
 *   PCSX2/... (gs.ini, live.ini, settings/<game>.ini, patches, cheats, anything else)
 *                        -> added when missing; replaced only when the file on the console is still exactly the
 *                           one an earlier install put there (manifest.txt) or, before the first install, exactly
 *                           an earlier test build's copy (history/<build>.json); a file the user changed or removed
 *                           stays as the user left it
 *   top-level texts      -> /data/PS5SX2-Installer/release-notes/<tag>/
 *   anything else        -> skipped
 *
 * Nothing is deleted outside /data/PS5SX2-Installer. A replaced file is moved into
 * backup/<time>_before_<tag>/ first (the last two sets are kept).
 * Every move is written to journal.txt (and fsync'ed) before it happens, so an install cut off by a power
 * loss or a crash is undone at the next start. */
#include "install.h"

#include "app.h"
#include "config.h"
#include "fsx.h"
#include "github.h"
#include "history.h"
#include "log.h"
#include "paths.h"
#include "plat.h"
#include "util.h"
#include "zipx.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mbedtls/sha256.h"

enum { CLS_SKIP, CLS_DIR, CLS_BUILD, CLS_CONFIG, CLS_FLAG, CLS_NOTE };
enum { ACT_NONE, ACT_PUT, ACT_REPLACE, ACT_SAME, ACT_KEEP_USER, ACT_KEEP_REMOVED, ACT_FLAG_SKIP };

typedef struct {
  const zentry *ze;
  int cls;
  int act;
  char key[PATH_LEN];    /* "PCSX2/gs.ini": the name in the manifest */
  char target[PATH_LEN]; /* where it goes */
  char staged[PATH_LEN]; /* the extracted copy */
  char sha[65];     /* the new file */
  char cur_sha[65]; /* the file on the console now, if any */
  const char *why;
  mode_t mode; /* for the file placed */
} item;

/* Host tests only: PS5SX2_TEST_CRASH_AFTER=N stops the process at the N-th step of an install, as a power cut
 * would, so the undo at the next start can be checked. Not in the PS5 build. */
#ifdef PS5SX2_HOST
static void test_crash_point(void) {
  static long left = -1;
  if (left == -1) {
    const char *v = plat_test_env("PS5SX2_TEST_CRASH_AFTER");
    left = v ? strtol(v, NULL, 10) : 0;
  }
  if (left > 0 && --left == 0) {
    log_line("test: simulated crash");
    _exit(99);
  }
}
#else
#define test_crash_point() ((void)0)
#endif

/* ---------------- manifest ---------------- */

typedef struct {
  char *key;
  char sha[65];
} mf_file;

typedef struct {
  int loaded;
  mf_file *files;
  size_t nfiles, capf;
  char **flags;
  size_t nflags, capfl;
} manifest;

static void mf_free(manifest *m) {
  for (size_t i = 0; i < m->nfiles; i++)
    free(m->files[i].key);
  for (size_t i = 0; i < m->nflags; i++)
    free(m->flags[i]);
  free(m->files);
  free(m->flags);
  memset(m, 0, sizeof(*m));
}

static const mf_file *mf_find(const manifest *m, const char *key) {
  for (size_t i = 0; i < m->nfiles; i++)
    if (!strcmp(m->files[i].key, key))
      return &m->files[i];
  return NULL;
}

static int mf_has_flag(const manifest *m, const char *name) {
  for (size_t i = 0; i < m->nflags; i++)
    if (!strcmp(m->flags[i], name))
      return 1;
  return 0;
}

static int mf_add_file(manifest *m, const char *key, const char *sha) {
  for (size_t i = 0; i < m->nfiles; i++)
    if (!strcmp(m->files[i].key, key)) {
      str_copy(m->files[i].sha, sizeof(m->files[i].sha), sha);
      return 0;
    }
  if (m->nfiles == m->capf) {
    const size_t cap = m->capf ? m->capf * 2 : 64;
    mf_file *p = realloc(m->files, cap * sizeof(*p));
    if (!p)
      return -1;
    m->files = p;
    m->capf = cap;
  }
  m->files[m->nfiles].key = str_dup(key);
  if (!m->files[m->nfiles].key)
    return -1;
  str_copy(m->files[m->nfiles].sha, sizeof(m->files[m->nfiles].sha), sha);
  m->nfiles++;
  return 0;
}

static int mf_add_flag(manifest *m, const char *name) {
  if (mf_has_flag(m, name))
    return 0;
  if (m->nflags == m->capfl) {
    const size_t cap = m->capfl ? m->capfl * 2 : 32;
    char **p = realloc(m->flags, cap * sizeof(*p));
    if (!p)
      return -1;
    m->flags = p;
    m->capfl = cap;
  }
  m->flags[m->nflags] = str_dup(name);
  if (!m->flags[m->nflags])
    return -1;
  m->nflags++;
  return 0;
}

static int mf_load(manifest *m) {
  memset(m, 0, sizeof(*m));
  if (!fs_exists(g_p.manifest))
    return 0;
  sbuf b;
  sb_init(&b);
  if (fs_read_file(g_p.manifest, 8u << 20, &b) != 0) {
    sb_free(&b);
    return -1;
  }
  char *save = NULL;
  for (char *line = strtok_r(b.data ? b.data : "", "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    if (!strncmp(line, "file ", 5) && strlen(line) > 5 + 64 + 1 && line[5 + 64] == ' ' && hex_is(line + 5, 64)) {
      char sha[65];
      memcpy(sha, line + 5, 64);
      sha[64] = '\0';
      mf_add_file(m, line + 5 + 64 + 1, sha);
    } else if (!strncmp(line, "flag ", 5) && line[5]) {
      mf_add_flag(m, line + 5);
    }
  }
  sb_free(&b);
  m->loaded = 1;
  return 0;
}

static int mf_save(const manifest *m, const char *tag, const char *path) {
  sbuf b;
  sb_init(&b);
  char when[32];
  fmt_utc(when, sizeof(when), now_utc());
  sb_printf(&b, "# PS5SX2 Installer manifest: the files it installed, as it installed them (SHA-256).\n"
                "# A file on the console that still matches is replaced by the next build; one you changed is kept.\n"
                "tag %s\nwritten %s\n",
            tag, when);
  for (size_t i = 0; i < m->nfiles; i++)
    sb_printf(&b, "file %s %s\n", m->files[i].sha, m->files[i].key);
  for (size_t i = 0; i < m->nflags; i++)
    sb_printf(&b, "flag %s\n", m->flags[i]);
  const int rc = b.data ? fs_write_atomic(path, b.data, b.len, 0666) : -1;
  sb_free(&b);
  return rc;
}

/* ---------------- journal and undo ---------------- */

/* One record per step, written and fsync'ed before the step is taken:
 *   BEGIN  <tag>
 *   MV     <file on the console> <its backup> <SHA-256 of that file>   (it goes into the backup set)
 *   PUT    <staged copy> <target> <SHA-256 of the new file>          (the new file goes in place)
 *   COMMIT <tag>        everything is in place; manifest.new and installed.new are written
 *   UNDONE <n>          record n has been undone (or was found never done)
 * Each record is "\n" + fields + "\t#" + 16 hex digits of SHA-256 over the fields + "\n". A record cut off by a
 * power loss fails that check and is ignored: the step it announced never started, because a record is on disk
 * before its step. The leading "\n" ends whatever a cut-off record left behind.
 * The undo checks content before it moves anything: a file is moved away only if it is exactly the file the
 * install put there, and a backup comes back only if it is exactly the file that was there before and nothing
 * else has taken its place. So an undo can run any number of times, and a file changed after the crash stays. */

typedef struct {
  char type[8];
  char a[PATH_LEN], b[PATH_LEN];
  char sha[65];
  int undone;
} jop;

static void record_check(const char *body, size_t n, char out[17]) {
  unsigned char d[32];
  mbedtls_sha256((const unsigned char *)body, n, d, 0);
  char hex[65];
  hex_encode(hex, d, 32);
  memcpy(out, hex, 16);
  out[16] = '\0';
}

static int journal_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int journal_write(const char *fmt, ...) {
  char body[3 * PATH_LEN];
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof(body)) {
    err_set("journal record too long");
    return -1;
  }
  char chk[17];
  record_check(body, (size_t)n, chk);
  char line[3 * PATH_LEN + 32];
  snprintf(line, sizeof(line), "\n%s\t#%s\n", body, chk);
  return fs_append_sync(g_p.journal, line);
}

/* p is inside root ("<root>/..."), with no "." or ".." parts. */
static int path_under(const char *p, const char *root) {
  const size_t n = strlen(root);
  if (!n || strncmp(p, root, n) != 0 || p[n] != '/' || !p[n + 1])
    return 0;
  for (const char *c = p + n; *c;) {
    while (*c == '/')
      c++;
    const char *e = strchr(c, '/');
    const size_t l = e ? (size_t)(e - c) : strlen(c);
    if ((l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.'))
      return 0;
    c += l;
  }
  return 1;
}

static int is_target_path(const char *p) { return path_under(p, g_p.app) || path_under(p, g_p.pcsx2); }

static int sha_is(const char *path, const char *sha) {
  char cur[65];
  return fs_is_file(path) && fs_sha256(path, cur) == 0 && !strcmp(cur, sha);
}

static int rollback_move_away(const char *path, const char *dir, size_t idx) {
  char dst[PATH_LEN];
  char name[PATH_LEN];
  snprintf(name, sizeof(name), "%zu-%s", idx, path_base(path));
  if (fs_mkdirs(dir, 0777) != 0 || path_join(dst, sizeof(dst), dir, name) != 0)
    return -1;
  return fs_move(path, dst);
}

/* The journal of a finished (or undone) install goes to rollback/journal-<kind>-<time>.txt. */
static int archive_journal(const char *kind, const char *stamp) {
  char name[96], dst[PATH_LEN];
  snprintf(name, sizeof(name), "journal-%s-%s.txt", kind, stamp);
  fs_mkdirs(g_p.rollback, 0777);
  if (path_join(dst, sizeof(dst), g_p.rollback, name) == 0 && fs_move(g_p.journal, dst) == 0)
    return 0;
  return fs_remove_work_file(g_p.journal);
}

static void work_path(char *dst, const char *name) { path_join(dst, PATH_LEN, g_p.work, name); }

/* Host tests only: PS5SX2_TEST_CRASH_UNDO_AFTER=N stops the process at the N-th step of an undo. */
#ifdef PS5SX2_HOST
static void test_crash_point_undo(void) {
  static long left = -1;
  if (left == -1) {
    const char *v = plat_test_env("PS5SX2_TEST_CRASH_UNDO_AFTER");
    left = v ? strtol(v, NULL, 10) : 0;
  }
  if (left > 0 && --left == 0) {
    log_line("test: simulated crash during the undo");
    _exit(98);
  }
}
#else
#define test_crash_point_undo() ((void)0)
#endif

static int g_undo_kept; /* files changed after the crash that the undo left alone */

typedef struct {
  char **v;
  size_t n, cap;
} name_set;

/* Adds path once; returns 1 if it was new. */
static int note_kept(name_set *s, const char *path) {
  for (size_t i = 0; i < s->n; i++)
    if (!strcmp(s->v[i], path))
      return 0;
  if (s->n == s->cap) {
    const size_t cap = s->cap ? s->cap * 2 : 8;
    char **p = realloc(s->v, cap * sizeof(*p));
    if (!p)
      return 0;
    s->v = p;
    s->cap = cap;
  }
  s->v[s->n] = str_dup(path);
  if (!s->v[s->n])
    return 0;
  s->n++;
  return 1;
}

int install_recover(void) {
  g_undo_kept = 0;
  if (!fs_exists(g_p.journal))
    return 0;
  sbuf b;
  sb_init(&b);
  if (fs_read_file(g_p.journal, 16u << 20, &b) != 0) {
    log_line("recover: can't read the journal: %s", err_get());
    sb_free(&b);
    return -1;
  }
  size_t nops = 0, cap = 0;
  jop *ops = NULL;
  int committed = 0, damaged = 0;
  const char *text = b.data ? b.data : "";
  const char *end = text + b.len;
  for (const char *l = text; l < end;) {
    const char *nl = memchr(l, '\n', (size_t)(end - l));
    const char *le = nl ? nl : end;
    const size_t len = (size_t)(le - l);
    const char *next = nl ? nl + 1 : end;
    if (len == 0) {
      l = next;
      continue;
    }
    /* "<fields>\t#<16 hex>", no NUL (a cut-off write can leave zeros) */
    if (memchr(l, '\0', len) || len < 19 || l[len - 18] != '\t' || l[len - 17] != '#') {
      damaged++;
      l = next;
      continue;
    }
    char chk[17];
    record_check(l, len - 18, chk);
    if (memcmp(chk, l + len - 16, 16) != 0) {
      damaged++;
      l = next;
      continue;
    }
    char rec[3 * PATH_LEN];
    if (len - 18 >= sizeof(rec)) {
      damaged++;
      l = next;
      continue;
    }
    memcpy(rec, l, len - 18);
    rec[len - 18] = '\0';
    l = next;
    char *f[5] = {rec, NULL, NULL, NULL, NULL};
    int nf = 1;
    for (char *c = rec; *c && nf < 5; c++)
      if (*c == '\t') {
        *c = '\0';
        f[nf++] = c + 1;
      }
    if (!strcmp(f[0], "COMMIT") && nf == 2) {
      committed = 1;
    } else if (!strcmp(f[0], "UNDONE") && nf == 2) {
      const size_t k = (size_t)strtoul(f[1], NULL, 10);
      if (k < nops)
        ops[k].undone = 1;
    } else if ((!strcmp(f[0], "MV") || !strcmp(f[0], "PUT")) && nf == 4 && strlen(f[3]) == 64 && hex_is(f[3], 64)) {
      const int mv = !strcmp(f[0], "MV");
      /* MV: a file on the console -> the backup set; PUT: staging -> a file on the console */
      const int ok = mv ? (is_target_path(f[1]) && path_under(f[2], g_p.backup))
                        : (path_under(f[1], g_p.staging) && is_target_path(f[2]));
      if (!ok) {
        log_line("recover: ignored a record with unexpected paths: %s %.200s", f[0], f[1]);
        continue;
      }
      if (nops == cap) {
        cap = cap ? cap * 2 : 64;
        jop *np = realloc(ops, cap * sizeof(jop));
        if (!np) {
          free(ops);
          sb_free(&b);
          err_set("out of memory");
          return -1;
        }
        ops = np;
      }
      jop *o = &ops[nops++];
      memset(o, 0, sizeof(*o));
      str_copy(o->type, sizeof(o->type), f[0]);
      str_copy(o->a, sizeof(o->a), f[1]);
      str_copy(o->b, sizeof(o->b), f[2]);
      str_copy(o->sha, sizeof(o->sha), f[3]);
    }
  }
  sb_free(&b);
  if (damaged)
    log_line("recover: %d damaged journal record(s) ignored", damaged);

  char stamp[32];
  fmt_stamp(stamp, sizeof(stamp), now_utc());
  char mnew[PATH_LEN], inew[PATH_LEN];
  work_path(mnew, "manifest.new");
  work_path(inew, "installed.new");

  if (committed) {
    /* Everything was in place; only the last renames may be missing. */
    if (fs_exists(mnew) && fs_move(mnew, g_p.manifest) != 0)
      log_line("recover: %s", err_get());
    if (fs_exists(inew) && fs_move(inew, g_p.installed) != 0)
      log_line("recover: %s", err_get());
    free(ops);
    archive_journal("done", stamp);
    log_line("recover: finished an install whose last step was cut off");
    return 2;
  }

  char dir[PATH_LEN], sub[64];
  snprintf(sub, sizeof(sub), "undo_%s", stamp);
  path_join(dir, sizeof(dir), g_p.rollback, sub);
  int failures = 0;
  name_set kept = {NULL, 0, 0};
  for (size_t i = nops; i-- > 0;) {
    jop *o = &ops[i];
    if (o->undone)
      continue;
    int ok = 1;
    if (!strcmp(o->type, "PUT")) {
      /* a = staged copy, b = target, sha = the new file */
      if (!fs_exists(o->a) && fs_exists(o->b)) {
        if (sha_is(o->b, o->sha)) {
          ok = rollback_move_away(o->b, dir, i) == 0;
        } else {
          /* not the new file: either the old one is already back (an earlier undo), or someone changed it */
          const jop *mv = NULL;
          for (size_t k = 0; k < i; k++)
            if (!strcmp(ops[k].type, "MV") && !strcmp(ops[k].a, o->b))
              mv = &ops[k];
          if (!(mv && sha_is(o->b, mv->sha)) && note_kept(&kept, o->b))
            log_line("undo: %s isn't the file the update put there any more: left as it is", o->b);
        }
      }
    } else {
      /* a = the file that was on the console, b = its backup, sha = that file's content */
      if (fs_is_file(o->b)) {
        if (!sha_is(o->b, o->sha)) {
          log_line("undo: the backup %s isn't complete: not used", o->b);
        } else if (!fs_exists(o->a)) {
          ok = fs_move(o->b, o->a) == 0;
        } else if (!sha_is(o->a, o->sha)) {
          note_kept(&kept, o->a);
          log_line("undo: %s was changed after the update stopped: kept; the version from before the update is %s",
                   o->a, o->b);
        }
      }
    }
    test_crash_point_undo();
    if (!ok) {
      log_line("undo: step %zu (%s %s) failed: %s", i, o->type, o->a, err_get());
      failures++;
      continue;
    }
    if (journal_write("UNDONE\t%zu", i) != 0) {
      log_line("undo: can't write to the journal: %s", err_get());
      free(ops);
      for (size_t k = 0; k < kept.n; k++)
        free(kept.v[k]);
      free(kept.v);
      return -1;
    }
    test_crash_point_undo();
  }
  free(ops);
  g_undo_kept = (int)kept.n;
  for (size_t k = 0; k < kept.n; k++)
    free(kept.v[k]);
  free(kept.v);
  if (failures) {
    err_set("%d file(s) couldn't be put back; see installer.log", failures);
    return -1;
  }
  fs_remove_work_file(mnew);
  fs_remove_work_file(inew);
  archive_journal("undone", stamp);
  log_line("recover: undid an unfinished install (%zu steps, %d file(s) changed since were left alone)", nops,
           g_undo_kept);
  return 1;
}

/* ---------------- what earlier test builds shipped ---------------- */

static int shipped_before(const char *key, const char *sha) {
  for (size_t i = 0; i < g_shipped_files_n; i++)
    if (!strcmp(g_shipped_files[i].key, key) && !strcmp(g_shipped_files[i].sha, sha))
      return 1;
  return 0;
}

static int flag_shipped_before(const char *name) {
  for (size_t i = 0; i < g_shipped_flags_n; i++)
    if (!strcmp(g_shipped_flags[i], name))
      return 1;
  return 0;
}

/* ---------------- classification ---------------- */

static const char *const k_user_dirs[] = {"bios",   "games", "memcards", "savestates", "sstates",   "textures",
                                          "covers", "cache", "logs",     "snaps",      "inputprofiles", "videos",
                                          "flags-off", "_parked", "_to_delete", NULL};
static const char *const k_user_files[] = {"webui_token.txt", "playtime.dat", "lastgame.txt", "pid.txt",
                                           "killapp.log",     "settings.log", NULL};

static int in_list(const char *const *list, const char *s, size_t n) {
  for (; *list; list++)
    if (strlen(*list) == n && !strncmp(*list, s, n))
      return 1;
  return 0;
}

static int set_target(item *it, const char *base, const char *sub) {
  if (!*sub)
    return str_copy(it->target, sizeof(it->target), base) < sizeof(it->target) ? 0 : -1;
  return path_join(it->target, sizeof(it->target), base, sub);
}

static int classify(item *it, const char *tag) {
  const zentry *e = it->ze;
  const char *rel = e->rel;
  str_copy(it->key, sizeof(it->key), rel);
  it->cls = CLS_SKIP;
  it->why = "not something PS5SX2 needs";
  if (!*rel) {
    it->why = "the zip's folder";
    return 0;
  }
  if (!strcmp(rel, TITLE_ID) || str_starts(rel, TITLE_ID "/")) {
    const char *sub = rel + strlen(TITLE_ID);
    if (*sub == '/')
      sub++;
    it->cls = e->is_dir ? CLS_DIR : CLS_BUILD;
    return set_target(it, g_p.app, sub);
  }
  if (!strcmp(rel, "PCSX2") || str_starts(rel, "PCSX2/")) {
    const char *sub = rel + 5;
    if (*sub == '/')
      sub++;
    if (!*sub) {
      it->cls = CLS_DIR;
      return set_target(it, g_p.pcsx2, "");
    }
    const char *slash = strchr(sub, '/');
    const size_t fl = slash ? (size_t)(slash - sub) : strlen(sub);
    if (fl == 9 && !strncmp(sub, "resources", 9)) {
      it->cls = e->is_dir ? CLS_DIR : CLS_BUILD;
      return set_target(it, g_p.pcsx2, sub);
    }
    if (fl == 5 && !strncmp(sub, "flags", 5)) {
      if (e->is_dir) {
        it->cls = slash ? CLS_SKIP : CLS_FLAG; /* the flags folder itself */
        it->why = "a folder inside flags";
        return set_target(it, g_p.pcsx2, sub);
      }
      if (!slash || strchr(slash + 1, '/')) {
        it->why = "not a switch file";
        return 0;
      }
      it->cls = CLS_FLAG;
      return set_target(it, g_p.pcsx2, sub);
    }
    if (in_list(k_user_dirs, sub, fl)) {
      if (e->is_dir && !slash) {
        it->cls = CLS_DIR; /* only make sure the folder exists */
        return set_target(it, g_p.pcsx2, sub);
      }
      it->why = "inside one of your folders (never written)";
      return 0;
    }
    if (!slash && !e->is_dir && (in_list(k_user_files, sub, fl) || str_ends(sub, ".log"))) {
      it->why = "one of your files (never written)";
      return 0;
    }
    it->cls = e->is_dir ? CLS_DIR : CLS_CONFIG;
    return set_target(it, g_p.pcsx2, sub);
  }
  if (!strchr(rel, '/') && !e->is_dir) {
    char dir[PATH_LEN];
    if (path_join(dir, sizeof(dir), g_p.notes, tag) != 0)
      return -1;
    it->cls = CLS_NOTE;
    return path_join(it->target, sizeof(it->target), dir, rel);
  }
  it->why = "a folder the installer doesn't know";
  return 0;
}

/* ---------------- progress ---------------- */

typedef struct {
  const char *tag;
  int last_quarter;
} dl_progress;

static void on_progress(void *ctx, uint64_t got, uint64_t total) {
  dl_progress *p = ctx;
  if (!total)
    return;
  const int q = (int)((got * 4) / total);
  if (q > p->last_quarter && q >= 1 && q <= 3) {
    p->last_quarter = q;
    notify("Downloading %s: %d%%", p->tag, q * 25);
  }
}

/* ---------------- helpers ---------------- */

static int target_parent_ok(const char *target) {
  /* every existing part of the parent path must be a folder (a link to a folder counts: /data may be one) */
  char tmp[PATH_LEN];
  str_copy(tmp, sizeof(tmp), target);
  for (char *s = tmp + 1; *s; s++) {
    if (*s != '/')
      continue;
    *s = '\0';
    struct stat st;
    if (stat(tmp, &st) == 0 && !S_ISDIR(st.st_mode)) {
      err_set("%s is a file where a folder is needed", tmp);
      return -1;
    }
    *s = '/';
  }
  return 0;
}

typedef struct {
  char (*names)[128];
  size_t n, cap;
  int want_dirs;
  const char *prefix;
} name_list;

static int collect_names(void *ctx, const char *name, int is_dir) {
  name_list *l = ctx;
  if (is_dir != l->want_dirs || strlen(name) >= 128 || (l->prefix && !str_starts(name, l->prefix)))
    return 0;
  if (l->n == l->cap) {
    const size_t cap = l->cap ? l->cap * 2 : 16;
    char(*p)[128] = realloc(l->names, cap * sizeof(*p));
    if (!p)
      return 1;
    l->names = p;
    l->cap = cap;
  }
  str_copy(l->names[l->n++], 128, name);
  return 0;
}

static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static int count_file(void *ctx, const char *name, int is_dir) {
  (void)name;
  if (!is_dir)
    ++*(int *)ctx;
  return 0;
}

/* 1 if dir has at least one entry that isn't a folder. */
static int dir_has_files(const char *dir) {
  int n = 0;
  if (fs_is_dir(dir))
    fs_list(dir, count_file, &n);
  return n > 0;
}

static int tree_has_files(const char *dir, int depth);
typedef struct {
  const char *dir;
  int depth;
  int found;
} tree_ctx;

static int tree_visit(void *ctx, const char *name, int is_dir) {
  tree_ctx *t = ctx;
  if (!is_dir) {
    t->found = 1;
    return 1;
  }
  char sub[PATH_LEN];
  if (t->depth < 16 && path_join(sub, sizeof(sub), t->dir, name) == 0 && tree_has_files(sub, t->depth + 1)) {
    t->found = 1;
    return 1;
  }
  return 0;
}

static int tree_has_files(const char *dir, int depth) {
  tree_ctx t = {dir, depth, 0};
  fs_list(dir, tree_visit, &t);
  return t.found;
}

typedef struct {
  const char *dir;
} empty_ctx;

static int drop_if_empty(void *ctx, const char *name, int is_dir) {
  empty_ctx *e = ctx;
  char sub[PATH_LEN];
  if (is_dir && path_join(sub, sizeof(sub), e->dir, name) == 0 && !tree_has_files(sub, 0) &&
      fs_remove_work_tree(sub) != 0)
    log_line("cleanup: %s", err_get());
  return 0;
}

/* Backup sets left empty (an undo put their files back) are removed so they don't count as sets. */
static void prune_empty(const char *dir) {
  empty_ctx e = {dir};
  if (fs_is_dir(dir))
    fs_list(dir, drop_if_empty, &e);
}

/* In one of our folders: keeps `except` and the last `keep` entries by name (our names start with the
 * time, so that is the newest), deletes the other folders (dirs=1) or files (dirs=0) starting with prefix. */
static void prune(const char *dir, int dirs, const char *prefix, size_t keep, const char *except) {
  name_list l;
  memset(&l, 0, sizeof(l));
  l.want_dirs = dirs;
  l.prefix = prefix;
  if (!fs_is_dir(dir) || fs_list(dir, collect_names, &l) < 0) {
    free(l.names);
    return;
  }
  if (l.n)
    qsort(l.names, l.n, sizeof(*l.names), cmp_name);
  size_t kept = 0;
  for (size_t i = l.n; i-- > 0;) {
    if (except && !strcmp(l.names[i], except))
      continue;
    if (kept < keep) {
      kept++;
      continue;
    }
    char p[PATH_LEN];
    if (path_join(p, sizeof(p), dir, l.names[i]) != 0)
      continue;
    if ((dirs ? fs_remove_work_tree(p) : fs_remove_work_file(p)) != 0)
      log_line("cleanup: %s", err_get());
  }
  free(l.names);
}

static int read_switch_exists(const char *p) { return fs_exists(p); }

/* ---------------- the install ---------------- */

static int do_install(const gh_release *rel, const char *have_tag, int *changed_out, int *kept_out);

int install_run(void) {
  if (read_switch_exists(g_p.no_install)) {
    log_line("install: switched off (%s)", g_p.no_install);
    return INSTALL_NOTHING;
  }
  int lockfd = -1;
  const int lk = fs_lock(g_p.install_lock, &lockfd);
  if (lk == 1) {
    notify("PS5SX2 Installer: an update is already running");
    return INSTALL_NOTHING;
  }
  if (lk < 0) {
    notify("PS5SX2 update failed: %s", err_get());
    return INSTALL_FAILED;
  }
  const int rec = install_recover();
  if (rec == 2)
    notify("PS5SX2 Installer: the last update was cut off at its very end; it is finished now");
  else if (rec == 1 && g_undo_kept)
    notify("PS5SX2 Installer: the last update was cut off; your previous files were put back, except %d you "
           "changed since, which were left as they are (see installer.log)",
           g_undo_kept);
  else if (rec == 1)
    notify("PS5SX2 Installer: the last update was cut off; your previous files were put back");
  else if (rec < 0) {
    notify("PS5SX2 Installer: an unfinished update couldn't be undone (%s). Nothing else was changed.",
           err_get());
    fs_unlock(lockfd);
    return INSTALL_FAILED;
  }

  notify("PS5SX2 Installer %s%s: checking for a new build...", INSTALLER_VERSION, INSTALLER_VARIANT);
  gh_release rel;
  if (gh_latest(&rel) != 0) {
    notify("PS5SX2 update: %s. Nothing was changed.", err_get());
    fs_unlock(lockfd);
    return INSTALL_FAILED;
  }
  log_line("latest release: %s (%s, %lld bytes, sha256 %s)", rel.tag, rel.asset_name, rel.asset_size,
           rel.asset_sha256);

  char have[80] = "";
  const int have_ok = app_installed_tag(have, sizeof(have)) == 0;
  const int force = read_switch_exists(g_p.reinstall);
  if (have_ok && !force) {
    const int cmp = tag_compare(have, rel.tag);
    if (cmp == 0) {
      notify("PS5SX2 is up to date (%s)", rel.tag);
      fs_unlock(lockfd);
      return INSTALL_NOTHING;
    }
    if (cmp == 1) {
      notify("Your PS5SX2 (%s) is newer than the latest release (%s): nothing to install", have, rel.tag);
      fs_unlock(lockfd);
      return INSTALL_NOTHING;
    }
  }
  if (force)
    log_line("install: 'reinstall' switch present");
  notify("New build %s (installed: %s)", rel.tag, have_ok ? have : "none found");

  int changed = 0, kept = 0;
  const int rc = do_install(&rel, have_ok ? have : "", &changed, &kept);
  if (rc == INSTALL_DONE) {
    if (force) {
      char used[PATH_LEN + 8];
      snprintf(used, sizeof(used), "%s.used", g_p.reinstall);
      rename(g_p.reinstall, used);
    }
    if (kept)
      notify("PS5SX2 %s installed (%d files updated). %d settings/patch file(s) of yours were kept as they are. "
             "BIOS, games and saves untouched.",
             rel.tag, changed, kept);
    else
      notify("PS5SX2 %s installed (%d files updated). BIOS, games, saves and settings untouched.", rel.tag,
             changed);
  }
  fs_unlock(lockfd);
  return rc;
}

static int do_install(const gh_release *rel, const char *have_tag, int *changed_out, int *kept_out) {
  (void)have_tag;
  int result = INSTALL_FAILED;
  item *items = NULL;
  size_t nitems = 0;
  zarch *z = NULL;
  manifest old, neu;
  memset(&old, 0, sizeof(old));
  memset(&neu, 0, sizeof(neu));
  char zip_path[PATH_LEN], stage_dir[PATH_LEN], backup_dir[PATH_LEN], notes_dir[PATH_LEN];
  char stamp[32];
  fmt_stamp(stamp, sizeof(stamp), now_utc());
  if (path_join(zip_path, sizeof(zip_path), g_p.download, rel->asset_name) != 0 ||
      path_join(stage_dir, sizeof(stage_dir), g_p.staging, rel->tag) != 0 ||
      path_join(notes_dir, sizeof(notes_dir), g_p.notes, rel->tag) != 0) {
    notify("PS5SX2 update failed: %s", err_get());
    return INSTALL_FAILED;
  }
  {
    char sub[128];
    snprintf(sub, sizeof(sub), "%s_before_%s", stamp, rel->tag); /* time first: sorts oldest to newest */
    path_join(backup_dir, sizeof(backup_dir), g_p.backup, sub);
  }

  pid_t pid = 0;
  const int running = app_running(&pid, NULL);
  if (running == 1) {
    notify("Close PS5SX2 first, then send the installer again (nothing was changed)");
    return INSTALL_FAILED;
  }
  if (running < 0) {
    notify("Can't tell whether PS5SX2 is running (/data/PCSX2/pid.txt can't be read). Close it, then send the "
           "installer again (nothing was changed)");
    return INSTALL_FAILED;
  }

  /* ---- download (or reuse a checked one) ---- */
  if (fs_mkdirs(g_p.download, 0777) != 0) {
    notify("PS5SX2 update failed: %s", err_get());
    return INSTALL_FAILED;
  }
  int have_zip = 0;
  if (fs_size(zip_path) == rel->asset_size) {
    char sha[65];
    if (fs_sha256(zip_path, sha) == 0 && !strcmp(sha, rel->asset_sha256)) {
      have_zip = 1;
      log_line("download: reusing %s (SHA-256 matches)", zip_path);
    }
  }
  if (!have_zip) {
    prune(g_p.download, 0, NULL, 0, NULL); /* older zips and cut-off downloads */
    const uint64_t free_now = fs_free_bytes(g_p.data);
    if (free_now && free_now < (uint64_t)rel->asset_size + MIN_FREE_MARGIN) {
      char need[32], got[32];
      human_bytes(need, sizeof(need), (uint64_t)rel->asset_size + MIN_FREE_MARGIN);
      human_bytes(got, sizeof(got), free_now);
      notify("PS5SX2 update: not enough space (%s needed, %s free). Nothing was changed.", need, got);
      return INSTALL_FAILED;
    }
    char size[32];
    human_bytes(size, sizeof(size), (uint64_t)rel->asset_size);
    notify("Downloading %s (%s)...", rel->tag, size);
    dl_progress prog = {rel->tag, 0};
    if (gh_download(rel, zip_path, on_progress, &prog) != 0) {
      notify("PS5SX2 update: %s. Nothing was changed.", err_get());
      return INSTALL_FAILED;
    }
    notify("Download complete, SHA-256 checked");
  }

  /* ---- open and check the zip ---- */
  z = zip_open_checked(zip_path);
  if (!z) {
    notify("PS5SX2 update: %s. Nothing was changed.", err_get());
    goto out;
  }
  const size_t n = zip_count(z);
  items = calloc(n, sizeof(item));
  if (!items) {
    notify("PS5SX2 update failed: out of memory");
    goto out;
  }
  int have_eboot = 0, have_param = 0;
  uint64_t to_extract = 0;
  size_t nfiles = 0;
  for (size_t i = 0; i < n; i++) {
    item *it = &items[nitems++];
    it->ze = zip_entry(z, i);
    if (classify(it, rel->tag) != 0) {
      notify("PS5SX2 update failed: %s", err_get());
      goto out;
    }
    if (it->cls == CLS_BUILD && !strcmp(it->key, TITLE_ID "/eboot.bin"))
      have_eboot = 1;
    if (it->cls == CLS_BUILD && !strcmp(it->key, TITLE_ID "/sce_sys/param.json"))
      have_param = 1;
    if (it->cls == CLS_BUILD || it->cls == CLS_CONFIG || it->cls == CLS_FLAG || it->cls == CLS_NOTE) {
      if (!it->ze->is_dir) {
        to_extract += it->ze->size;
        nfiles++;
      }
    }
  }
  if (!have_eboot || !have_param) {
    notify("PS5SX2 update: the zip has no %s app in it. Nothing was changed.", TITLE_ID);
    goto out;
  }

  /* ---- unpack everything we may place into staging/<tag>/ ---- */
  {
    const uint64_t free_now = fs_free_bytes(g_p.data);
    if (free_now && free_now < to_extract + MIN_FREE_MARGIN) {
      char need[32], got[32];
      human_bytes(need, sizeof(need), to_extract + MIN_FREE_MARGIN);
      human_bytes(got, sizeof(got), free_now);
      notify("PS5SX2 update: not enough space to unpack (%s needed, %s free). Nothing was changed.", need, got);
      goto out;
    }
  }
  if ((fs_exists(g_p.staging) && fs_remove_work_tree(g_p.staging) != 0) || fs_mkdirs(stage_dir, 0777) != 0) {
    notify("PS5SX2 update failed: %s", err_get());
    goto out;
  }
  notify("Unpacking %zu files...", nfiles);
  for (size_t i = 0; i < nitems; i++) {
    item *it = &items[i];
    if (it->ze->is_dir || (it->cls != CLS_BUILD && it->cls != CLS_CONFIG && it->cls != CLS_FLAG &&
                           it->cls != CLS_NOTE))
      continue;
    char name[32];
    snprintf(name, sizeof(name), "f%u", it->ze->index);
    if (path_join(it->staged, sizeof(it->staged), stage_dir, name) != 0 || zip_extract(z, it->ze, it->staged) != 0 ||
        fs_sha256(it->staged, it->sha) != 0) {
      notify("PS5SX2 update: %s. Nothing was changed.", err_get());
      goto out;
    }
    if (!strcmp(it->key, TITLE_ID "/sce_sys/param.json")) {
      sbuf pj;
      sb_init(&pj);
      const int ok = fs_read_file(it->staged, 1u << 20, &pj) == 0 && pj.data && strstr(pj.data, TITLE_ID);
      sb_free(&pj);
      if (!ok) {
        notify("PS5SX2 update: the zip's app isn't %s. Nothing was changed.", TITLE_ID);
        goto out;
      }
    }
  }

  /* ---- decide, file by file ---- */
  if (mf_load(&old) != 0)
    log_line("manifest: can't read it (%s); treating every existing file as yours", err_get());
  char flags_dir[PATH_LEN], flags_off[PATH_LEN];
  path_join(flags_dir, sizeof(flags_dir), g_p.pcsx2, "flags");
  path_join(flags_off, sizeof(flags_off), g_p.pcsx2, "flags-off");
  /* A first install: no manifest yet and no switch files (an interrupted first install can leave an empty
   * flags folder behind). */
  const int first_flags = !old.loaded && !dir_has_files(flags_dir);
  int changed = 0, kept = 0;
  for (size_t i = 0; i < nitems; i++) {
    item *it = &items[i];
    if (it->ze->is_dir || it->cls == CLS_SKIP || it->cls == CLS_DIR) {
      if (it->cls == CLS_SKIP && !it->ze->is_dir)
        log_line("skip %s: %s", it->key, it->why);
      continue;
    }
    struct stat st;
    const int exists = lstat(it->target, &st) == 0;
    const int is_reg = exists && S_ISREG(st.st_mode);
    it->mode = is_reg ? (st.st_mode & 07777) : 0777;
    if (it->cls == CLS_NOTE) {
      it->act = ACT_PUT; /* our own folder */
      continue;
    }
    if (exists && !is_reg) {
      if (it->cls == CLS_BUILD) {
        notify("PS5SX2 update: %s is a folder or link where the app needs a file. Nothing was changed.",
               it->target);
        goto out;
      }
      it->act = ACT_KEEP_USER;
      it->why = "not a regular file on the console";
      continue;
    }
    char cur[65] = "";
    if (is_reg && fs_sha256(it->target, cur) != 0) {
      notify("PS5SX2 update failed: %s. Nothing was changed.", err_get());
      goto out;
    }
    str_copy(it->cur_sha, sizeof(it->cur_sha), cur);
    if (it->cls == CLS_BUILD) {
      it->act = !exists ? ACT_PUT : !strcmp(cur, it->sha) ? ACT_SAME : ACT_REPLACE;
      if (mf_add_file(&neu, it->key, it->sha) != 0)
        goto oom;
    } else if (it->cls == CLS_FLAG) {
      const char *name = path_base(it->key);
      char off[PATH_LEN];
      path_join(off, sizeof(off), flags_off, name);
      if (first_flags && !exists && !fs_exists(off)) {
        it->act = ACT_PUT;
        it->why = "first install";
      } else if (first_flags) {
        it->act = ACT_FLAG_SKIP;
        it->why = exists ? "already on" : "you moved it to flags-off";
      } else if (mf_has_flag(&old, name)) {
        it->act = ACT_FLAG_SKIP;
        it->why = exists ? "already on" : "you removed it";
      } else if (exists) {
        it->act = ACT_FLAG_SKIP;
        it->why = "already on";
      } else if (fs_exists(off)) {
        it->act = ACT_FLAG_SKIP;
        it->why = "you moved it to flags-off";
      } else if (old.loaded || !flag_shipped_before(name)) {
        it->act = ACT_PUT;
        it->why = "new in this build";
      } else {
        it->act = ACT_FLAG_SKIP;
        it->why = "an earlier build had it and it's off now: left off";
      }
      if (mf_add_flag(&neu, name) != 0)
        goto oom;
    } else { /* CLS_CONFIG */
      const mf_file *m = mf_find(&old, it->key);
      if (!exists) {
        if (m) {
          it->act = ACT_KEEP_REMOVED;
          it->why = "you removed it";
          if (mf_add_file(&neu, it->key, m->sha) != 0)
            goto oom;
        } else {
          it->act = ACT_PUT;
          if (mf_add_file(&neu, it->key, it->sha) != 0)
            goto oom;
        }
      } else if (!strcmp(cur, it->sha)) {
        it->act = ACT_SAME;
        if (mf_add_file(&neu, it->key, it->sha) != 0)
          goto oom;
      } else if ((m && !strcmp(m->sha, cur)) || (!m && shipped_before(it->key, cur))) {
        it->act = ACT_REPLACE;
        it->why = m ? "unchanged since the last install" : "an earlier test build's copy, unchanged";
        if (mf_add_file(&neu, it->key, it->sha) != 0)
          goto oom;
      } else {
        it->act = ACT_KEEP_USER;
        it->why = m ? "you changed it" : "yours (it was there before the installer)";
        kept++;
      }
    }
    if (it->act == ACT_PUT || it->act == ACT_REPLACE) {
      if (target_parent_ok(it->target) != 0) {
        notify("PS5SX2 update: %s. Nothing was changed.", err_get());
        goto out;
      }
      changed++;
    }
  }
  /* Remember what the old manifest knew about files this build doesn't ship any more. */
  for (size_t i = 0; i < old.nfiles; i++)
    if (!mf_find(&neu, old.files[i].key) && mf_add_file(&neu, old.files[i].key, old.files[i].sha) != 0)
      goto oom;
  for (size_t i = 0; i < old.nflags; i++)
    if (mf_add_flag(&neu, old.flags[i]) != 0)
      goto oom;

  for (size_t i = 0; i < nitems; i++) {
    const item *it = &items[i];
    if (it->ze->is_dir || it->cls == CLS_SKIP || it->cls == CLS_DIR || it->cls == CLS_NOTE)
      continue;
    static const char *const names[] = {"none", "add", "replace", "same", "keep (yours)", "keep (removed)",
                                        "switch left as is"};
    log_line("plan %-14s %s%s%s", names[it->act], it->key, it->why && it->act >= ACT_KEEP_USER ? ": " : "",
             it->why && it->act >= ACT_KEEP_USER ? it->why : "");
  }

  /* ---- last checks, then apply ---- */
  if (app_running(&pid, NULL) != 0) {
    notify("PS5SX2 was started: close it and send the installer again (the download is kept, nothing was changed)");
    goto out;
  }
  {
    /* /data/PCSX2 or the app folder may be a link to another drive: each needs room for its new files */
    const char *roots[2] = {g_p.app, g_p.pcsx2};
    for (int r = 0; r < 2; r++) {
      uint64_t need = 0;
      for (size_t i = 0; i < nitems; i++)
        if ((items[i].act == ACT_PUT || items[i].act == ACT_REPLACE) && items[i].cls != CLS_NOTE &&
            str_starts(items[i].target, roots[r]) && items[i].target[strlen(roots[r])] == '/')
          need += items[i].ze->size;
      if (!need)
        continue;
      const uint64_t free_now = fs_free_bytes(fs_is_dir(roots[r]) ? roots[r] : g_p.data);
      if (free_now && free_now < need + (8u << 20)) {
        char a[32], b[32];
        human_bytes(a, sizeof(a), need + (8u << 20));
        human_bytes(b, sizeof(b), free_now);
        notify("PS5SX2 update: not enough space for %s (%s needed, %s free). Nothing was changed.", roots[r], a, b);
        goto out;
      }
    }
  }
  notify("Installing %s...", rel->tag);

  /* folders first (never removed later: an empty folder is harmless) */
  for (size_t i = 0; i < nitems; i++) {
    const item *it = &items[i];
    if (it->cls == CLS_DIR || (it->cls == CLS_FLAG && it->ze->is_dir && first_flags)) {
      if (target_parent_ok(it->target) != 0 || fs_mkdirs(it->target, 0777) != 0) {
        notify("PS5SX2 update: %s. Nothing was changed.", err_get());
        goto out;
      }
    }
  }
  if (journal_write("BEGIN\t%s", rel->tag) != 0) {
    notify("PS5SX2 update: %s. Nothing was changed.", err_get());
    goto out;
  }
  int failed = 0;
  /* two passes: everything but eboot.bin, then eboot.bin */
  for (int pass = 0; pass < 2 && !failed; pass++) {
    for (size_t i = 0; i < nitems && !failed; i++) {
      item *it = &items[i];
      if (it->cls == CLS_NOTE || (it->act != ACT_PUT && it->act != ACT_REPLACE))
        continue;
      const int is_eboot = !strcmp(it->key, TITLE_ID "/eboot.bin");
      if (is_eboot != pass)
        continue;
      if (fs_mkdirs_parent(it->target, 0777) != 0) {
        failed = 1;
        break;
      }
      if (it->act == ACT_REPLACE) {
        char bk[PATH_LEN];
        if (path_join(bk, sizeof(bk), backup_dir, it->key) != 0 || fs_mkdirs_parent(bk, 0777) != 0 ||
            journal_write("MV\t%s\t%s\t%s", it->target, bk, it->cur_sha) != 0) {
          failed = 1;
          break;
        }
        test_crash_point();
        if (fs_move(it->target, bk) != 0) {
          failed = 1;
          break;
        }
        test_crash_point();
      }
      chmod(it->staged, it->mode ? it->mode : 0777);
      if (journal_write("PUT\t%s\t%s\t%s", it->staged, it->target, it->sha) != 0) {
        failed = 1;
        break;
      }
      test_crash_point();
      /* A file that appeared meanwhile is never overwritten by an "add". */
      if (it->act == ACT_PUT && fs_exists(it->target)) {
        err_set("%s appeared during the install", it->target);
        failed = 1;
        break;
      }
      if (fs_move(it->staged, it->target) != 0) {
        failed = 1;
        break;
      }
      test_crash_point();
    }
  }
  if (failed) {
    char why[600];
    str_copy(why, sizeof(why), err_get());
    log_line("install failed: %s; undoing", why);
    if (install_recover() >= 0)
      notify("PS5SX2 update failed while installing (%s). Your previous files were put back.", why);
    else
      notify("PS5SX2 update failed while installing (%s), and putting files back failed too: see "
             "/data/PS5SX2-Installer/installer.log",
             why);
    goto out;
  }

  /* release notes (our folder) */
  if (fs_mkdirs(notes_dir, 0777) == 0) {
    for (size_t i = 0; i < nitems; i++) {
      const item *it = &items[i];
      if (it->cls == CLS_NOTE && it->act == ACT_PUT && fs_move(it->staged, it->target) != 0)
        log_line("notes: %s", err_get());
    }
  }

  /* commit: manifest.new and installed.new, then COMMIT in the journal, then they take their names. A cut
   * before COMMIT is undone at the next start; a cut after it only finishes the renames. */
  {
    char mnew[PATH_LEN], inew[PATH_LEN];
    work_path(mnew, "manifest.new");
    work_path(inew, "installed.new");
    int ok = mf_save(&neu, rel->tag, mnew) == 0;
    sbuf b;
    sb_init(&b);
    char when[32];
    fmt_utc(when, sizeof(when), now_utc());
    sb_printf(&b, "tag=%s\ntitle=%s\nzip=%s\nsha256=%s\ninstalled=%s\ninstaller=%s\nbackup=%s\n", rel->tag,
              rel->title, rel->asset_name, rel->asset_sha256, when, INSTALLER_VERSION, backup_dir);
    ok = ok && b.data && fs_write_atomic(inew, b.data, b.len, 0666) == 0;
    sb_free(&b);
    test_crash_point();
    if (!ok || journal_write("COMMIT\t%s", rel->tag) != 0) {
      char why[600];
      str_copy(why, sizeof(why), err_get());
      log_line("install: can't finish (%s); undoing", why);
      if (install_recover() >= 0)
        notify("PS5SX2 update failed at the end (%s). Your previous files were put back.", why);
      else
        notify("PS5SX2 update failed at the end (%s), and putting files back failed too: see "
               "/data/PS5SX2-Installer/installer.log",
               why);
      goto out;
    }
    test_crash_point();
    if (fs_move(mnew, g_p.manifest) != 0 || fs_move(inew, g_p.installed) != 0)
      log_line("commit: %s", err_get());
    test_crash_point();
    if (archive_journal("done", stamp) != 0)
      log_line("journal: %s", err_get());
  }
  log_line("installed %s: %d files placed, %d kept as the user left them", rel->tag, changed, kept);
  *changed_out = changed;
  *kept_out = kept;
  result = INSTALL_DONE;

  /* tidy our own folder: staging, the zip, old backups/notes/undo folders */
  zip_close(z);
  z = NULL;
  if (fs_remove_work_tree(g_p.staging) != 0)
    log_line("cleanup: %s", err_get());
  if (fs_remove_work_file(zip_path) != 0)
    log_line("cleanup: %s", err_get());
  prune_empty(g_p.backup);
  prune(g_p.backup, 1, NULL, 2, path_base(backup_dir));
  prune(g_p.notes, 1, NULL, 1, rel->tag);
  prune(g_p.rollback, 1, "undo_", 3, NULL);
  prune(g_p.rollback, 0, "journal-", 10, NULL);
  goto out;

oom:
  notify("PS5SX2 update failed: out of memory. Nothing was changed.");
out:
  zip_close(z);
  free(items);
  mf_free(&old);
  mf_free(&neu);
  return result;
}
