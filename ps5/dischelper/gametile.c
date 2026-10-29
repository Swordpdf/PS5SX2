/* gametile.c (helper 171g, 2026-10-16, AI-assisted; run on the host against a made-up ShadowMount+ and shell, NOT yet on a console).
 *
 * A tile on the PS5 home screen for ONE GAME of the shelf (an image file in the games folder, a game on a USB drive, ...), made the way the disc tiles are made:
 * a copy of PS5SX2's own app under a title ID of its own, registered by ShadowMount+, started with no shelf. This file is INCLUDED by disctile.c (hosted builds only,
 * `#ifdef DISC_HOST_THREAD`), after reload_finish_tick(), so that it can use the small helpers of the disc payload. It adds NO code to any disc path: the disc
 * tiles' functions are not changed, and this file never writes a disc tile's state (g_drv, g_slot_*, g_stage_*, g_scan_*, g_gone). The only edits in disctile.c are
 * the include, gt_tick(now) in the main loop (gt_init() runs from the first gt_tick(), when the console is not shutting down) and the revision string.
 *
 * 179: ON by default (no flag file needed); the flag gametile_off switches it off and wins (re-checked each tick, so it is honoured without a reload). Off means:
 * no folder, no alive.txt, no state.txt, no request is read, nothing is polled. The one thing that still runs is the sandbox clean-up of game tiles made earlier
 * (their played apps leave a sandbox that keeps ShadowMount+ "busy" for the DISC tiles), and only when a ledger <id>.sig of an earlier game tile exists.
 *
 * Contract with the shelf (PS5SX2's app), directory /data/PCSX2/tiles/ (the helper makes it, mode 0777; the full text is README-GameTile.txt):
 *   request-<unix ms>-<nnn>.txt  app -> helper, ONE FILE PER PRESS (written as the same name + .tmp, then rename; the app never opens or changes a file that is
 *                there). First line `v1`, then one request per line, TAB separated:
 *                   add<TAB><absolute game path><TAB><serial or empty><TAB><title>[<TAB><absolute path of a cover picture>]
 *                   remove<TAB><absolute game path>      or      remove<TAB><tile id, e.g. PPSA99251>   (for a game that is not listed, a drive that is out)
 *                The helper takes ONE file a second, in the order of the names, renames it to request.work, acts, deletes request.work. A request.txt of the same
 *                form is still taken (after the request-* files). Every request is idempotent. Files older than 10 minutes when the helper starts are dropped unread.
 *   state.txt    helper -> app, written whole by the helper at start and after every change (temporary file, then rename). One line per game tile (and per
 *                failed request, for 10 minutes): <tile id or ->TAB<path>TAB<serial>TAB<title>TAB<status>, status = installing | ok | removing | failed:<reason>
 *                (reasons: nogame, badpath, limit, nospace, build, disabled, busy, damaged). Lines with fewer than 5 fields are to be ignored by the reader.
 *   alive.txt    helper -> app, every 20 s: `<unix time> <pid> gametile 1`. A file older than a minute (or none): the helper that reads the requests is not running.
 * The tile's folder /data/homebrew/PPSA99251..PPSA99260 holds game-tile.txt: line 1 = the absolute game path (read by the app before its jailbreak, as
 * /app0/disc-tile.txt is), then `serial=`, `title=`, `tile=`, `rev=`; and disc-tile.txt holding the single line `game` (so that the app's "am I a tile" tests are true).
 *
 * Everything here obeys kp_quiet() (no file, database, ShadowMount+ or notification access while the console shuts down or rests), runs its heavy steps on
 * threads of its own (one build at a time, never while a disc tile is being made), and uses SQLite from the main loop only (the library is built THREADSAFE=0).
 * Copyright (C) 2026 swordpdf; SPDX-License-Identifier: GPL-3.0-or-later */

#define GT_N        10
#define GT_LOAD(x)     __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define GT_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define GT_PATH     512
#define GT_QMAX     16
#define GT_REQ_MAX  8192
#define GT_DIR      "/data/PCSX2/tiles"
#define GT_REQ      GT_DIR "/request.txt"
#define GT_REQ_WORK GT_DIR "/request.work"
#define GT_STATE    GT_DIR "/state.txt"
#define GT_PRIV     WORK "/gametiles"
#define GT_MARKER   "game-tile.txt"
#define GT_DISCMARK "disc-tile.txt" /* holds the line `game`: every "am I a tile" test of the app is true, and an app that does not know game tiles simply opens the shelf */
#define GT_REV      "1"
#define GT_MIN_FREE (192ll << 20)
#ifndef GT_ROOTS
#define GT_ROOTS {"/data/", "/mnt/"} /* where a game can be: the console's data folder (the games folder) and the drives (USB, extended storage) */
#endif
#ifndef GT_HEAL_MIN_AGE
#define GT_HEAL_MIN_AGE 10 /* a sandbox that changed less than this many seconds ago may be a start in progress */
#endif

/* PPSA99251..PPSA99260: apart from the disc tiles' PPSA99231..38 and the Reload tile's PPSA99230, from PS5SX2's PPSA99203, and from PPSA99299 (the id smp_busy()
 * asks ShadowMount+ about). The ids are reserved for this payload: a folder in this range that has no game-tile.txt is somebody else's and is never touched. */
static const char *const kGameSlots[GT_N] = {"PPSA99251", "PPSA99252", "PPSA99253", "PPSA99254", "PPSA99255",
                                             "PPSA99256", "PPSA99257", "PPSA99258", "PPSA99259", "PPSA99260"};

enum { GT_FREE = 0, GT_BUILDING, GT_INSTALLING, GT_OK, GT_REMOVING, GT_FOREIGN };
enum { GT_JOB_BUILD = 1, GT_JOB_REFRESH = 2, GT_JOB_REMOVE = 3 };

typedef struct {
    int st;
    char path[GT_PATH], serial[20], title[128], cover[300];
    int worker;          /* a thread works on this slot (GT_LOAD / GT_STORE: the thread and the main loop both look at it) */
    int result;          /* set by the thread: 0 not yet, 1 done, -1 failed (rwhy), 2 removal accepted by ShadowMount+ (GT_LOAD / GT_STORE) */
    char rwhy[40];
    double t_acc;        /* when the uninstall was accepted (monotonic seconds) */
    time_t scan_at, rows_at, refresh_at;
    int scan_tries, purges, rows_deleted, rows_zero;
    time_t look_at;      /* REMOVING: when the process table was looked at last */
    int idle_looks, unk_looks; /* REMOVING: looks in a row that found no app / could not read the process table */
} GtSlot;

typedef struct { int kind; char path[GT_PATH], serial[20], title[128], cover[300]; } GtReq; /* kind 1 add, 2 remove by path, 3 remove by tile ID (path holds the ID) */
typedef struct { char path[GT_PATH], serial[20], title[128], why[40]; time_t t; } GtFail;
typedef struct { int slot, kind; } GtJob;

static pthread_mutex_t g_gt_mu = PTHREAD_MUTEX_INITIALIZER;
static GtSlot g_gt[GT_N];
static GtReq g_gt_q[GT_QMAX];
static int g_gt_qn;
static GtFail g_gt_fail[6];
static int g_gt_fail_n;
static int g_gt_build_busy; /* the one build / refresh thread (GT_LOAD / GT_STORE) */
static int g_gt_next, g_gt_ready, g_gt_init_done, g_gt_state_dirty, g_gt_heal_only;

/* ---------------------------------------------------------------- small helpers */
static void gt_cpy(char *dst, size_t n, const char *src) {
    if (n == 0) return;
    snprintf(dst, n, "%s", src ? src : "");
}

/* text for a file name, a title or a state line: no control characters, TAB and line breaks as spaces, UTF-8 only (an invalid byte becomes '?'), trimmed, at most
 * n-1 bytes and never in the middle of a character */
static void gt_clean_text(char *dst, size_t n, const char *src) {
    size_t k = 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s && k + 5 < n) {
        const unsigned char c = *s;
        if (c < 0x20 || c == 0x7f) { if (k && dst[k - 1] != ' ') dst[k++] = ' '; s++; continue; }
        if (c < 0x80) { if (!(c == ' ' && (k == 0 || dst[k - 1] == ' '))) dst[k++] = (char)c; s++; continue; }
        const int len = c >= 0xC2 && c <= 0xDF ? 2 : (c >= 0xE0 && c <= 0xEF ? 3 : (c >= 0xF0 && c <= 0xF4 ? 4 : 0));
        int ok = len > 0;
        for (int i = 1; ok && i < len; i++) if ((s[i] & 0xC0) != 0x80) ok = 0;
        if (!ok) { dst[k++] = '?'; s++; continue; }
        for (int i = 0; i < len; i++) dst[k++] = (char)s[i];
        s += len;
    }
    while (k && dst[k - 1] == ' ') k--;
    dst[k] = '\0';
}

/* "//" collapsed, no trailing '/' */
static void gt_norm_path(char *p) {
    char *w = p;
    for (const char *r = p; *r; r++) {
        if (*r == '/' && w > p && w[-1] == '/') continue;
        *w++ = *r;
    }
    *w = '\0';
    while (w - p > 1 && w[-1] == '/') *--w = '\0';
}

/* an absolute path under /data or /mnt, no control characters, no ".." component (so never a device either) */
static int gt_path_ok(const char *p) {
    static const char *const roots[] = GT_ROOTS;
    const size_t n = strlen(p);
    if (n >= GT_PATH) return 0;
    int under = 0;
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        const size_t rl = strlen(roots[i]);
        if (strncmp(p, roots[i], rl) == 0 && p[rl]) under = 1; /* a name must follow the folder */
    }
    if (!under) return 0;
    for (const char *q = p; *q; q++)
        if ((unsigned char)*q < 0x20 || *q == 0x7f) return 0;
    for (const char *q = p; (q = strstr(q, "..")) != NULL; q += 2)
        if ((q == p || q[-1] == '/') && (q[2] == '/' || q[2] == '\0')) return 0;
    return 1;
}

/* the cover picture named in a request: absolute, at most 299 bytes, no ".." anywhere, no control byte (the picture itself is judged by stb_image: JPEG, PNG, BMP,
 * GIF, TGA; not WebP). A cover that is refused is simply not used: the cover cache of the shelf, a title card, PS5SX2's own art follow. */
static int gt_cover_ok(const char *c) {
    if (c[0] != '/' || strlen(c) >= 300 || strstr(c, "..") != NULL) return 0;
    for (; *c; c++)
        if ((unsigned char)*c < 0x20 || *c == 0x7f) return 0;
    return 1;
}

static void gt_serial(char *dst, size_t n, const char *src) {
    size_t k = 0;
    for (const char *p = src; *p && k + 1 < n && k < 15; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.')) { k = 0; break; }
        dst[k++] = *p;
    }
    dst[k] = '\0';
}

/* a title from the file name when the shelf sent none: the name without folders and extension */
static void gt_title_from_path(char *dst, size_t n, const char *path) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    char tmp[160];
    gt_cpy(tmp, sizeof(tmp), b);
    char *dot = strrchr(tmp, '.');
    if (dot && dot != tmp) *dot = '\0';
    gt_clean_text(dst, n, tmp);
}

/* the next non-empty line of *cur, cut in place (what strtok_r(.., "\n", ..) gave); NULL at the end. strtok_r is not among the helper's imports, and a new import is a new symbol
 * that the payload loader has to find in the console's libc: nothing in this file may add one. */
static char *gt_next_line(char **cur) {
    char *p = *cur;
    while (p && *p == '\n') p++;
    if (!p || !*p) { *cur = p; return NULL; }
    char *e = strchr(p, '\n');
    if (e) { *e = '\0'; *cur = e + 1; } else { *cur = p + strlen(p); }
    return p;
}

static void gt_tile_path(char *out, size_t n, int s, const char *rel) {
    if (rel && rel[0]) snprintf(out, n, HB "/%s/%s", kGameSlots[s], rel);
    else snprintf(out, n, HB "/%s", kGameSlots[s]);
}

static int gt_slot_by_id(const char *id) {
    for (int s = 0; s < GT_N; s++)
        if (strcmp(id, kGameSlots[s]) == 0) return s;
    return -1;
}

static int gt_find_path(const char *path) {
    for (int s = 0; s < GT_N; s++)
        if ((g_gt[s].st == GT_BUILDING || g_gt[s].st == GT_INSTALLING || g_gt[s].st == GT_OK || g_gt[s].st == GT_REMOVING) && strcmp(g_gt[s].path, path) == 0)
            return s;
    return -1;
}

/* is the feature on? 179: ON by default - the shelf and the helper no longer need the flags/gametiles file; the flag gametile_off is the kill switch and wins.
 * flag() looks only in the flags folder, which is why the note for testers says "flags folder only". */
static int gt_enabled(void) { return !flag("gametile_off"); } /* 179: ON by default (no flag file needed); the flag gametile_off is the kill switch and wins */

/* disc tiles are being made, or the Reload tile works: a game tile waits (the disc tiles' timing is what the person sees) */
static int gt_disc_busy(void) {
    if (g_reload_running || g_stage_running) return 1;
    for (int i = 0; i < MAX_DRIVES; i++)
        if (g_drv[i].building) return 1;
    return 0;
}

/* ---------------------------------------------------------------- files: marker, signature, ring pointer, pending removals */
static int gt_marker_read(int s, char *path, size_t pn, char *serial, size_t sn, char *title, size_t tn) {
    char p[160];
    gt_tile_path(p, sizeof(p), s, GT_MARKER);
    char *t = read_file(p, NULL);
    if (!t) return 0;
    path[0] = serial[0] = title[0] = '\0';
    char *save = t;
    int line = 0;
    for (char *l = gt_next_line(&save); l; l = gt_next_line(&save), line++) {
        const size_t len = strlen(l);
        if (len && l[len - 1] == '\r') l[len - 1] = '\0';
        if (line == 0) gt_cpy(path, pn, l);
        else if (strncmp(l, "serial=", 7) == 0) gt_serial(serial, sn, l + 7);
        else if (strncmp(l, "title=", 6) == 0) gt_clean_text(title, tn, l + 6);
    }
    free(t);
    return path[0] != '\0';
}

static int gt_marker_write(const char *file, const char *tid, const char *path, const char *serial, const char *title) {
    char buf[1400];
    const int n = snprintf(buf, sizeof(buf), "%s\nserial=%s\ntitle=%s\ntile=%s\nrev=" GT_REV "\n", path, serial, title, tid);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return -1;
    if (write_file(file, buf, (size_t)n) != 0) return -1;
    chmod(file, 0777);
    return 0;
}

/* disc-tile.txt = the single line `game` (5 bytes): written after game-tile.txt. The app reads /app0/disc-tile.txt before its jailbreak, as in a disc tile. */
static int gt_discmark_write(const char *file) {
    if (write_file(file, "game\n", 5) != 0) return -1;
    chmod(file, 0777);
    return 0;
}

static void gt_sig_path(char *out, size_t n, int s) { snprintf(out, n, GT_PRIV "/%s.sig", kGameSlots[s]); }

static void gt_sig_write(int s) {
    char want[64], line[96], p[160];
    if (!eboot_sig(want, sizeof(want))) return;
    snprintf(line, sizeof(line), "%s rev " GT_REV "\n", want);
    gt_sig_path(p, sizeof(p), s);
    write_file(p, line, strlen(line));
}

/* Was this ID ever used by this payload? (<id>.sig is written by every build and never deleted.) A title that is installed under one of the IDs of the range
 * and was not made here belongs to somebody else: the slot is not used, and nothing of it is uninstalled or purged. */
static int gt_ours(int s) {
    char p[160];
    gt_sig_path(p, sizeof(p), s);
    return exists(p);
}

/* is the tile's copy of the eboot the one PS5SX2 has now? */
static int gt_sig_current(int s) {
    char want[64], exp[96], p[160];
    if (!eboot_sig(want, sizeof(want))) return 1; /* PS5SX2's eboot cannot be looked at: nothing to compare with, the tile is left alone */
    gt_sig_path(p, sizeof(p), s);
    char *t = read_file(p, NULL);
    if (!t) return 0;
    t[strcspn(t, "\r\n")] = '\0';
    snprintf(exp, sizeof(exp), "%s rev " GT_REV, want);
    const int same = strcmp(exp, t) == 0;
    free(t);
    return same;
}

static void gt_removing_save(void) {
    char buf[GT_N * 12 + 4];
    size_t k = 0;
    buf[0] = '\0';
    for (int s = 0; s < GT_N; s++)
        if (g_gt[s].st == GT_REMOVING) k += (size_t)snprintf(buf + k, sizeof(buf) - k, "%s\n", kGameSlots[s]);
    if (k) write_file(GT_PRIV "/removing.txt", buf, k);
    else unlink(GT_PRIV "/removing.txt");
}

static void gt_next_save(void) {
    char ns[16];
    snprintf(ns, sizeof(ns), "%d\n", g_gt_next);
    write_file(GT_PRIV "/next.txt", ns, strlen(ns));
}

/* is the tile's folder whole? (its eboot may be an OLDER copy of PS5SX2's: that is gt_recheck's business, not damage; but a copy that is not the size it was made with is) */
static int gt_folder_ok(int s) {
    char p[160];
    struct stat a;
    gt_tile_path(p, sizeof(p), s, "eboot.bin");
    if (stat(p, &a) != 0 || a.st_size < (1 << 20)) return 0;
    gt_sig_path(p, sizeof(p), s);
    char *sig = read_file(p, NULL);
    if (sig) { /* "<size> <mtime> rev 1" of the eboot it was copied from */
        const long long want = atoll(sig);
        free(sig);
        if (want > 0 && (long long)a.st_size != want) {
            struct stat sx; /* a refresh cut off between the rename of the eboot and the new signature leaves the new eboot with the old size in <id>.sig */
            if (stat(APP_SRC "/eboot.bin", &sx) != 0 || sx.st_size != a.st_size) return 0;
        }
    }
    gt_tile_path(p, sizeof(p), s, "sce_sys/param.json");
    if (!exists(p)) return 0;
    gt_tile_path(p, sizeof(p), s, "sce_sys/icon0.png");
    if (!exists(p)) return 0;
    gt_tile_path(p, sizeof(p), s, GT_MARKER);
    return exists(p);
}

/* what the shell keeps of the tile's title ID (see slot_clean for the disc tiles) */
static int gt_slot_clean(int s) {
    char p[96];
    snprintf(p, sizeof(p), "/user/app/%s", kGameSlots[s]);
    if (exists(p)) return 0;
    snprintf(p, sizeof(p), "/user/appmeta/%s", kGameSlots[s]);
    return !exists(p);
}

/* ---------------------------------------------------------------- state.txt */
static void gt_fail_forget(const char *path) {
    for (int i = 0; i < g_gt_fail_n;) {
        if (strcmp(g_gt_fail[i].path, path) == 0) {
            memmove(&g_gt_fail[i], &g_gt_fail[i + 1], sizeof(g_gt_fail[0]) * (size_t)(g_gt_fail_n - i - 1));
            g_gt_fail_n--;
            g_gt_state_dirty = 1;
        } else {
            i++;
        }
    }
}

static void gt_fail_add(const char *path, const char *serial, const char *title, const char *why) {
    gt_fail_forget(path);
    if (g_gt_fail_n == 6) {
        memmove(&g_gt_fail[0], &g_gt_fail[1], sizeof(g_gt_fail[0]) * 5);
        g_gt_fail_n = 5;
    }
    GtFail *f = &g_gt_fail[g_gt_fail_n++];
    gt_cpy(f->path, sizeof(f->path), path);
    gt_cpy(f->serial, sizeof(f->serial), serial);
    gt_cpy(f->title, sizeof(f->title), title);
    gt_cpy(f->why, sizeof(f->why), why);
    f->t = time(NULL);
    g_gt_state_dirty = 1;
    say("game tile request failed: %s (%s)", path, why);
    notify("Could not put %s on the home screen (%s).", f->title[0] ? f->title : "the game", why); /* a failure always says so (the success texts are off unless switch.txt is 1) */
}

static const char *gt_status(int st) {
    switch (st) {
    case GT_BUILDING:
    case GT_INSTALLING: return "installing";
    case GT_OK: return "ok";
    case GT_REMOVING: return "removing";
    default: return NULL;
    }
}

static void gt_state_flush(void) {
    if (kp_quiet()) return;
    const time_t now = time(NULL);
    for (int i = 0; i < g_gt_fail_n;) { /* ten minutes: then the line goes (and the file is written without it) */
        if (now - g_gt_fail[i].t > 600) {
            memmove(&g_gt_fail[i], &g_gt_fail[i + 1], sizeof(g_gt_fail[0]) * (size_t)(g_gt_fail_n - i - 1));
            g_gt_fail_n--;
            g_gt_state_dirty = 1;
        } else {
            i++;
        }
    }
    if (!g_gt_state_dirty) return;
    const size_t cap = (size_t)(GT_N + 6) * 1300 + 64;
    size_t k = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return;
    for (int s = 0; s < GT_N; s++) {
        const char *st = gt_status(g_gt[s].st);
        if (!st) continue;
        k += (size_t)snprintf(buf + k, cap - k, "%s\t%s\t%s\t%s\t%s\n", kGameSlots[s], g_gt[s].path, g_gt[s].serial, g_gt[s].title, st);
    }
    for (int i = 0; i < g_gt_fail_n; i++) {
        k += (size_t)snprintf(buf + k, cap - k, "-\t%s\t%s\t%s\tfailed:%s\n", g_gt_fail[i].path, g_gt_fail[i].serial, g_gt_fail[i].title, g_gt_fail[i].why);
    }
    mkdir(GT_DIR, 0777);
    const char *tmp = GT_STATE ".tmp";
    if (write_file(tmp, buf, k) == 0) {
        chmod(tmp, 0666);
        if (rename(tmp, GT_STATE) == 0) g_gt_state_dirty = 0;
        else unlink(tmp);
    }
    free(buf);
}

/* ---------------------------------------------------------------- the pictures */
/* icon0.png and pic0.dds from the shelf's cover (the path in the request, else the cover cache of the shelf); no cover: a title card; else PS5SX2's own. No
 * download and no SteamGridDB here: the shelf has the cover already, and this thread must not wait for the network. */
static int gt_make_art(const char *sys_dir, const char *serial, const char *title, const char *cover_req, char *used, size_t used_n) {
    char icon[260], pic[260], cover[320] = "", cache[320] = "";
    snprintf(icon, sizeof(icon), "%s/icon0.png", sys_dir);
    snprintf(pic, sizeof(pic), "%s/pic0.dds", sys_dir);
    snprintf(used, used_n, "PS5SX2's own");
    if (serial[0]) { /* the shelf's cover cache of the serial */
        snprintf(cache, sizeof(cache), COVERS "/%s.jpg", serial);
        if (!exists(cache)) cache[0] = '\0';
    }
    if (cover_req[0] && exists(cover_req)) gt_cpy(cover, sizeof(cover), cover_req);
    else gt_cpy(cover, sizeof(cover), cache);
    tile_set_icon_case(flag("disctile_iconcase"));
    const TileArt ta = {cover[0] ? cover : NULL, NULL, NULL, NULL, NULL, NULL, "/preinst/common/font/SST-Bold.otf", "PlayStation 2"};
    int art = -1, ok = 1;
    if (ta.cover) {
        art = tile_make_art(&ta, icon, pic, used, used_n);
        if (art != 0) {
            say("game tile: the art could not be made from %s (%d)", cover, art);
            snprintf(used, used_n, "PS5SX2's own");
            if (cache[0] && strcmp(cache, cover) != 0) { /* the picture named in the request is no use (a format that stb_image cannot read, a damaged file): the cover cache of the serial is next, then the card */
                const TileArt tb = {cache, NULL, NULL, NULL, NULL, NULL, ta.font, ta.label};
                art = tile_make_art(&tb, icon, pic, used, used_n);
                if (art != 0) { say("game tile: the art could not be made from %s (%d): a title card instead", cache, art); snprintf(used, used_n, "PS5SX2's own"); }
            }
        }
    }
    if (art != 0 && tile_make_title_art(icon, pic, ta.font, title) == 0) {
        art = 0;
        snprintf(used, used_n, "title card");
    }
    if (art != 0) {
        ok = copy_file(APP_SRC "/sce_sys/icon0.png", icon) == 0;
        if (exists(APP_SRC "/sce_sys/pic0.dds")) copy_file(APP_SRC "/sce_sys/pic0.dds", pic);
    }
    chmod(icon, 0777);
    chmod(pic, 0777);
    return ok;
}

/* ---------------------------------------------------------------- building a tile (a thread) */
static void gt_copy_modules(const char *dir, int *ok) {
    DIR *dd = opendir(APP_SRC "/sce_module");
    if (!dd) return;
    struct dirent *e;
    char p[300], q[300];
    while ((e = readdir(dd)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), APP_SRC "/sce_module/%s", e->d_name);
        snprintf(q, sizeof(q), "%s/sce_module/%s", dir, e->d_name);
        if (*ok && put_exec_file(p, q, NULL, 0) != 0) *ok = 0;
    }
    closedir(dd);
}

/* The folder is made where ShadowMount+ looks, sce_sys LAST (made beside, then renamed in), as build_in_place() does for a disc. 1 when it is whole. */
static int gt_build(int s, char *why, size_t wmax) {
    const char *tid = kGameSlots[s];
    GtSlot *g = &g_gt[s];
    char dir[128], p[300], sys_tmp[160], sys[160], how[96] = "", used[96] = "";
    snprintf(dir, sizeof(dir), HB "/%s", tid);
    snprintf(why, wmax, "build");
    struct statfs sf;
    if (statfs("/data", &sf) == 0 && (long long)sf.f_bavail * (long long)sf.f_bsize < GT_MIN_FREE) { snprintf(why, wmax, "nospace"); return 0; }
    const double t0 = mono_s();
    rm_tree(dir);
    mkdir(dir, 0777);
    chmod(dir, 0777);
    snprintf(sys_tmp, sizeof(sys_tmp), "%s/sce_sys.new", dir);
    snprintf(sys, sizeof(sys), "%s/sce_sys", dir);
    mkdir(sys_tmp, 0777); /* first of all: a folder of ours that a crash or a power cut leaves half made is then known for what it is (gt_adopt takes away a folder with sce_sys.new and no marker); without it
                           * the half folder would look like somebody else's and its slot would be lost for good */
    snprintf(p, sizeof(p), "%s/sce_module", dir);
    mkdir(p, 0777);
    chmod(p, 0777);
    snprintf(p, sizeof(p), "%s/eboot.bin", dir);
    int ok = put_exec_file(APP_SRC "/eboot.bin", p, how, sizeof(how)) == 0;
    if (ok && !kp_quiet()) gt_copy_modules(dir, &ok);
    snprintf(p, sizeof(p), "%s/" GT_MARKER, dir);
    ok = ok && !kp_quiet() && gt_marker_write(p, tid, g->path, g->serial, g->title) == 0;
    snprintf(p, sizeof(p), "%s/" GT_DISCMARK, dir); /* after game-tile.txt */
    ok = ok && !kp_quiet() && gt_discmark_write(p) == 0;
    ok = ok && !kp_quiet() && write_param_json(sys_tmp, 30 + s, tid, g->title);
    ok = ok && !kp_quiet() && gt_make_art(sys_tmp, g->serial, g->title, g->cover, used, sizeof(used));
    if (!ok || kp_quiet()) {
        if (!kp_quiet()) rm_tree(dir); /* a folder left by a stop at shutdown is taken away at the next start (gt_adopt) */
        return 0;
    }
    rename(sys_tmp, sys);
    chmod(sys, 0777);
    gt_sig_write(s);
    whitelist_add(tid);
    say("game tile %s: \"%s\" (%s) from %s, art: %s, eboot.bin %s; built in %.1f s", tid, g->title, g->serial[0] ? g->serial : "no serial", g->path, used, how, mono_s() - t0);
    return 1;
}

/* A newer PS5SX2 eboot (the installer replaced PPSA99203's): the tile's copy of eboot.bin and sce_module is replaced file by file (copied beside, then renamed over),
 * sce_sys is not touched (ShadowMount+ judges a tile by it). */
static int gt_refresh(int s) {
    char dir[128], p[300], q[300], how[96] = "";
    snprintf(dir, sizeof(dir), HB "/%s", kGameSlots[s]);
    snprintf(p, sizeof(p), "%s/eboot.bin.new", dir);
    snprintf(q, sizeof(q), "%s/eboot.bin", dir);
    int ok = !kp_quiet() && put_exec_file(APP_SRC "/eboot.bin", p, how, sizeof(how)) == 0 && rename(p, q) == 0;
    DIR *dd = opendir(APP_SRC "/sce_module");
    if (dd) {
        struct dirent *e;
        while ((e = readdir(dd)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char f[300], t[300], n[300];
            snprintf(f, sizeof(f), APP_SRC "/sce_module/%s", e->d_name);
            snprintf(t, sizeof(t), "%s/sce_module/%s.new", dir, e->d_name);
            snprintf(n, sizeof(n), "%s/sce_module/%s", dir, e->d_name);
            if (ok && (kp_quiet() || put_exec_file(f, t, NULL, 0) != 0 || rename(t, n) != 0)) ok = 0;
        }
        closedir(dd);
    }
    if (ok) gt_sig_write(s);
    say("game tile %s: eboot.bin refreshed: %s", kGameSlots[s], ok ? "ok" : "FAILED (tried again later)");
    return ok;
}

/* ---------------------------------------------------------------- taking a tile down (a thread) */
static void gt_heal_game_sandboxes(int min_age);

static int gt_remove_pass(int s, char *reply, size_t reply_n) {
    char dir[128], body[80];
    snprintf(dir, sizeof(dir), HB "/%s", kGameSlots[s]);
    rm_tree(dir);
    snprintf(body, sizeof(body), "{\"title_id\":\"%s\"}", kGameSlots[s]);
    const int code = smp_post("/api/v1/games/uninstall", body, reply, reply_n);
    if (code == 409) gt_heal_game_sandboxes(2);
    return code;
}

static void gt_set_result(int s, int result, const char *why, double t_acc) {
    pthread_mutex_lock(&g_gt_mu);
    gt_cpy(g_gt[s].rwhy, sizeof(g_gt[s].rwhy), why);
    g_gt[s].t_acc = t_acc;
    GT_STORE(g_gt[s].result, result);
    GT_STORE(g_gt[s].worker, 0);
    pthread_mutex_unlock(&g_gt_mu);
}

static void *gt_thread(void *arg) {
    const GtJob job = *(GtJob *)arg;
    free(arg);
    const int s = job.slot;
    char why[40] = "";
    if (job.kind == GT_JOB_BUILD) {
        const int ok = gt_build(s, why, sizeof(why));
        char reply[300] = "";
        int code = 0;
        if (ok) {
            code = smp_post("/api/v1/scan", "{\"reset_attempts\":true}", reply, sizeof(reply));
            say("game tile %s: scan HTTP %d %.100s", kGameSlots[s], code, code > 0 ? reply : "");
        }
        GT_STORE(g_gt_build_busy, 0);
        gt_set_result(s, ok ? 1 : -1, why, 0);
    } else if (job.kind == GT_JOB_REFRESH) {
        const int ok = gt_refresh(s);
        GT_STORE(g_gt_build_busy, 0);
        gt_set_result(s, ok ? 1 : -1, "build", 0);
    } else {
        /* The shelf itself is an app that ShadowMount+ counts as an active game: while it runs the uninstall is refused (409 game_active). So nothing is asked
         * (and the tile's folder stays, the tile stays usable) until no app runs; then like remove_pass() for a disc tile: folder first, then the uninstall,
         * asked again every 0.4 s while it says 409 (2 s, 5 s, 20 s later on). */
        int tries = 0, code = 0, unknown = 0, done = 0;
        char reply[300] = "";
        double t_pass = mono_s();
        while (!g_stop) {
            if (kp_quiet()) break; /* resumed after the console works again (the main loop starts it afresh) */
            const int apps = app_processes();
            if (apps != 0 && !(apps < 0 && ++unknown >= 10)) { usleep(2000000); continue; }
            t_pass = mono_s();
            code = gt_remove_pass(s, reply, sizeof(reply));
            tries++;
            if (remove_final(code) || (code >= 500 && tries >= 40)) { done = 1; break; }
            if (tries <= 2 || tries % 25 == 0) say("game tile %s: uninstall HTTP %d %.150s (try %d)", kGameSlots[s], code, code > 0 ? reply : "", tries);
            usleep(tries < 50 ? 400000 : tries < 70 ? 2000000 : tries < 100 ? 5000000 : 20000000);
        }
        if (done) {
            say("game tile %s removed: uninstall HTTP %d after %d pass(es) (the last took %.1f s)", kGameSlots[s], code, tries, mono_s() - t_pass);
            gt_set_result(s, 2, "", mono_s());
        } else {
            gt_set_result(s, 0, "", 0); /* stopped (quiet): the main loop starts it again */
        }
    }
    return NULL;
}

static int gt_start(int s, int kind) {
    GtJob *job = (GtJob *)malloc(sizeof(*job));
    if (!job) return 0;
    job->slot = s;
    job->kind = kind;
    pthread_mutex_lock(&g_gt_mu);
    GT_STORE(g_gt[s].worker, 1);
    GT_STORE(g_gt[s].result, 0);
    pthread_mutex_unlock(&g_gt_mu);
    if (kind != GT_JOB_REMOVE) GT_STORE(g_gt_build_busy, 1);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, kind == GT_JOB_REMOVE ? (1 << 20) : (4 << 20)); /* the pictures are drawn on a build thread's stack */
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    const int rc = pthread_create(&th, &at, gt_thread, job);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        free(job);
        pthread_mutex_lock(&g_gt_mu);
        GT_STORE(g_gt[s].worker, 0);
        pthread_mutex_unlock(&g_gt_mu);
        if (kind != GT_JOB_REMOVE) GT_STORE(g_gt_build_busy, 0);
        say("game tile: no thread (errno %d)", rc);
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- the requests */
static int gt_split_tabs(char *line, char **f, int max) {
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++)
        if (*p == '\t') { *p = '\0'; f[n++] = p + 1; }
    return n;
}

static void gt_enqueue(int kind, const char *path, const char *serial, const char *title, const char *cover) {
    for (int i = g_gt_qn - 1; i >= 0; i--) /* asked already: the newest request for this game (or tile) is the same. If it is the other kind (add, remove, add) the press is a new one: the last press wins */
        if (strcmp(g_gt_q[i].path, path) == 0) { if (g_gt_q[i].kind == kind) return; break; }
    if (g_gt_qn >= GT_QMAX) { gt_fail_add(path, serial, title, "busy"); return; }
    GtReq *r = &g_gt_q[g_gt_qn++];
    r->kind = kind;
    gt_cpy(r->path, sizeof(r->path), path);
    gt_cpy(r->serial, sizeof(r->serial), serial);
    gt_cpy(r->title, sizeof(r->title), title);
    gt_cpy(r->cover, sizeof(r->cover), cover);
}

static void gt_parse_requests(char *txt, size_t n) {
    if (n > GT_REQ_MAX) { say("game tile: a request file of %zu bytes is too large: ignored", n); return; }
    char *save = txt;
    char *line = gt_next_line(&save);
    if (line) { const size_t l = strlen(line); if (l && line[l - 1] == '\r') line[l - 1] = '\0'; }
    if (!line || strcmp(line, "v1") != 0) { say("game tile: the request file does not start with v1: ignored"); return; }
    int count = 0;
    while ((line = gt_next_line(&save)) != NULL && count < 32) {
        const size_t l = strlen(line);
        if (l && line[l - 1] == '\r') line[l - 1] = '\0';
        if (!line[0]) continue;
        count++;
        char *f[6];
        const int nf = gt_split_tabs(line, f, 6);
        char path[GT_PATH] = "", serial[20] = "", title[128] = "", cover[300] = "";
        int pok = 0;
        if (nf >= 2) { /* normalized (// collapsed, no trailing /) before anything is compared; the length is judged on the whole field: 600 bytes are refused, not cut and accepted */
            gt_norm_path(f[1]);
            pok = gt_path_ok(f[1]);
            gt_cpy(path, sizeof(path), f[1]);
        }
        if (strcmp(f[0], "add") == 0 && nf >= 4) {
            gt_serial(serial, sizeof(serial), f[2]);
            gt_clean_text(title, sizeof(title), f[3]);
            if (title[0] == '\0') gt_title_from_path(title, sizeof(title), path);
            if (title[0] == '\0') gt_cpy(title, sizeof(title), serial[0] ? serial : "PS2 game");
            if (nf >= 5 && gt_cover_ok(f[4])) gt_cpy(cover, sizeof(cover), f[4]);
            if (!pok) { gt_fail_add(path[0] ? path : "?", serial, title, "badpath"); continue; }
            if (!gt_enabled()) { gt_fail_add(path, serial, title, "disabled"); continue; } /* the flag may have gone away, or gametile_off appeared, since the helper started */
            gt_fail_forget(path); /* a retry: the old failed line leaves state.txt now (a new failure is a new line) */
            gt_enqueue(1, path, serial, title, cover);
        } else if (strcmp(f[0], "remove") == 0 && nf >= 2) {
            if (gt_slot_by_id(f[1]) >= 0) gt_enqueue(3, f[1], "", "", ""); /* by the tile's ID: also for a game that is not there to be listed (a USB drive that is out) */
            else if (pok) { gt_fail_forget(path); gt_enqueue(2, path, "", "", ""); }
        } else {
            say("game tile: a request line is not understood: ignored");
        }
    }
}

static int gt_req_name(const char *n) {
    const size_t l = strlen(n);
    if (l < 13 || strcmp(n + l - 4, ".txt") != 0) return 0; /* "request-" + something + ".txt" */
    return strncmp(n, "request-", 8) == 0;
}

/* a request is a plain file. A folder, a pipe (opening one for reading waits for a writer, for ever: the whole main loop of the helper, the disc tiles with it) or a link
 * that is named like a request is left where it is: it is never taken, never opened, and it holds nothing up. */
static int gt_plain_file(const char *p) {
    struct stat s;
    return lstat(p, &s) == 0 && S_ISREG(s.st_mode);
}

/* once a second: ONE request file is taken (renamed to request.work, so that the shelf can add the next one at once) and read. The shelf writes each press as a file
 * of its own (request-<unix ms>-<n>.txt, made as .tmp and renamed in) and never opens one that is there, so no press can be lost or come back; they are taken in the
 * order of their names. A request.txt (the first form of the contract) is taken after them. */
static void gt_requests_read(void) {
    struct stat st;
    const int have_work = lstat(GT_REQ_WORK, &st) == 0;
    if (have_work && !S_ISREG(st.st_mode)) { /* something that is not a file sits where the request in work goes: it is taken away, or no request would be read ever again */
        rm_tree(GT_REQ_WORK);
        return;
    }
    if (!have_work) {
        if (g_gt_qn > GT_QMAX - 8) return; /* the queue is nearly full: the files wait */
        char first[96] = "";
        DIR *d = opendir(GT_DIR);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!gt_req_name(e->d_name) || strlen(e->d_name) >= sizeof(first) || (first[0] && strcmp(e->d_name, first) >= 0)) continue;
            char cand[160];
            snprintf(cand, sizeof(cand), GT_DIR "/%s", e->d_name);
            if (gt_plain_file(cand)) snprintf(first, sizeof(first), "%s", e->d_name);
        }
        closedir(d);
        char from[160];
        if (first[0]) snprintf(from, sizeof(from), GT_DIR "/%s", first);
        else if (gt_plain_file(GT_REQ)) snprintf(from, sizeof(from), "%s", GT_REQ);
        else return;
        if (rename(from, GT_REQ_WORK) != 0) return;
    }
    size_t n = 0;
    char *txt = NULL;
    if (stat(GT_REQ_WORK, &st) == 0 && st.st_size > GT_REQ_MAX) say("game tile: a request file of %lld bytes is too large: ignored", (long long)st.st_size); /* not even read */
    else txt = read_file(GT_REQ_WORK, &n);
    if (txt) {
        gt_parse_requests(txt, n);
        free(txt);
    }
    unlink(GT_REQ_WORK);
}

/* ---------------------------------------------------------------- slots */
static int gt_pick_slot(void) {
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < GT_N; k++) {
            const int s = (g_gt_next + k) % GT_N;
            if (g_gt[s].st != GT_FREE) continue;
            char dir[128];
            snprintf(dir, sizeof(dir), HB "/%s", kGameSlots[s]);
            if (exists(dir)) continue; /* somebody else's folder, or the leftover of a removal that is not over */
            const int clean = gt_slot_clean(s);
            if (!gt_ours(s) && (!clean || db_has_title(kGameSlots[s]) > 0)) continue; /* a title of somebody else is installed under this ID */
            if (pass == 0 && !clean) continue;
            return s;
        }
    }
    return -1;
}

/* rows of an earlier tile of this ID are removed before the ID is used again (SQLite: the main loop only) */
static void gt_purge_ghost(int s) {
    const char *tid = kGameSlots[s];
    char p[128];
    snprintf(p, sizeof(p), HB "/%s", tid);
    if (exists(p)) return;
    snprintf(p, sizeof(p), "/user/app/%s", tid);
    if (exists(p)) return;
    if (db_has_title(tid) > 0) {
        say("game tile slot %s is free but the shell's database still has rows of it: they are deleted", tid);
        purge_title_rows(tid);
    }
}

static void gt_slot_reset(int s) {
    memset(&g_gt[s], 0, sizeof(g_gt[s]));
    g_gt_state_dirty = 1;
}

static void gt_queue_pop_at(int i) {
    if (i < 0 || i >= g_gt_qn) return;
    memmove(&g_gt_q[i], &g_gt_q[i + 1], sizeof(g_gt_q[0]) * (size_t)(g_gt_qn - i - 1));
    g_gt_qn--;
}

/* One request of the queue is looked at, at most one step a second. 1 when it was dealt with (and taken out of the queue), 0 when it has to wait. */
static int gt_queue_try(int qi) {
    GtReq *r = &g_gt_q[qi];
    int s = r->kind == 3 ? gt_slot_by_id(r->path) : gt_find_path(r->path);
    if (r->kind == 1) {
        if (s >= 0 && g_gt[s].st == GT_REMOVING) return 0; /* the removal first */
        if (s >= 0) { say("game tile: %s has its tile already (%s)", r->path, kGameSlots[s]); gt_queue_pop_at(qi); return 1; }
        if (GT_LOAD(g_gt_build_busy) || gt_disc_busy()) return 0;
        if (!exists(r->path)) { gt_fail_add(r->path, r->serial, r->title, "nogame"); gt_queue_pop_at(qi); return 1; }
        s = gt_pick_slot();
        if (s < 0) { gt_fail_add(r->path, r->serial, r->title, "limit"); gt_queue_pop_at(qi); return 1; }
        gt_purge_ghost(s);
        gt_fail_forget(r->path);
        GtSlot *g = &g_gt[s];
        memset(g, 0, sizeof(*g));
        g->st = GT_BUILDING;
        gt_cpy(g->path, sizeof(g->path), r->path);
        gt_cpy(g->serial, sizeof(g->serial), r->serial);
        gt_cpy(g->title, sizeof(g->title), r->title);
        gt_cpy(g->cover, sizeof(g->cover), r->cover);
        g_gt_next = (s + 1) % GT_N;
        gt_next_save();
        g_gt_state_dirty = 1;
        say("game tile: \"%s\" (%s) from %s gets %s", g->title, g->serial[0] ? g->serial : "no serial", g->path, kGameSlots[s]);
        if (!gt_start(s, GT_JOB_BUILD)) {
            gt_fail_add(g->path, g->serial, g->title, "build");
            gt_slot_reset(s);
        }
        gt_queue_pop_at(qi);
        return 1;
    } else {
        if (s < 0 || g_gt[s].st == GT_FREE || g_gt[s].st == GT_FOREIGN) { say("game tile: nothing to remove for %s", r->path); gt_queue_pop_at(qi); return 1; }
        if (g_gt[s].st == GT_BUILDING) return 0; /* the build first */
        if (g_gt[s].st != GT_REMOVING) {
            g_gt[s].st = GT_REMOVING;
            GT_STORE(g_gt[s].result, 0);
            g_gt[s].purges = g_gt[s].rows_deleted = 0;
            g_gt_state_dirty = 1;
            gt_removing_save();
            say("game tile %s (%s): removal asked", kGameSlots[s], g_gt[s].path);
            if (!flag("gametile_nohide")) set_tile_row_visible(kGameSlots[s], 0); /* gone from the home screen at its next look, though the uninstall waits for the shelf to close */
        }
        gt_queue_pop_at(qi);
        return 1;
    }
}

static void gt_queue_run(void) {
    for (int i = 0; i < g_gt_qn; i++) {
        int later = 0; /* an earlier request for the same game (or tile) goes first: the order of the person's presses is kept for one game */
        for (int k = 0; k < i; k++) if (strcmp(g_gt_q[k].path, g_gt_q[i].path) == 0) later = 1;
        if (later) continue;
        if (gt_queue_try(i)) return;
    }
}

/* ---------------------------------------------------------------- following the slots (main loop) */
static void gt_follow(int s, time_t now) {
    GtSlot *g = &g_gt[s];
    const char *tid = kGameSlots[s];
    int res = 0, worker = 0;
    char rwhy[40] = "";
    double t_acc = 0;
    pthread_mutex_lock(&g_gt_mu);
    res = GT_LOAD(g->result);
    worker = GT_LOAD(g->worker);
    t_acc = g->t_acc;
    gt_cpy(rwhy, sizeof(rwhy), g->rwhy);
    pthread_mutex_unlock(&g_gt_mu);
    if (g->st == GT_BUILDING) {
        if (worker || res == 0) return;
        if (res == 1) {
            g->st = GT_INSTALLING;
            g->scan_at = now + 8;
            g->scan_tries = 0;
            say("game tile %s: built; waiting for ShadowMount+ and the home screen", tid);
        } else {
            gt_fail_add(g->path, g->serial, g->title, rwhy[0] ? rwhy : "build"); /* (it also says so in a notification) */
            { char dir[128]; snprintf(dir, sizeof(dir), HB "/%s", tid); rm_tree(dir); } /* a build that stopped when the console began to shut down touched nothing more and left its half folder: this slot is ours (gt_pick_slot took it), and the console works again */
            gt_slot_reset(s);
            return;
        }
        GT_STORE(g->result, 0);
        g_gt_state_dirty = 1;
    } else if (g->st == GT_REMOVING) {
        if (!worker && res != 2) { /* no thread works here and no removal has been accepted: (res is 1 or -1 when a refresh of the eboot ended after the removal was asked for) */
            if (kp_quiet()) return;
            if (now - g->look_at >= 3) { /* the shelf (or any app) counts as an active game for ShadowMount+: nothing is started while one runs */
                g->look_at = now;
                const int apps = app_processes();
                if (apps == 0) {
                    g->unk_looks = 0;
                    if (++g->idle_looks >= 2) { g->idle_looks = 0; gt_start(s, GT_JOB_REMOVE); return; }
                } else if (apps < 0) { /* the process table cannot be read: after ten looks (30 s) the removal is tried anyway, as the thread does */
                    g->idle_looks = 0;
                    if (++g->unk_looks >= 10) { g->unk_looks = 0; gt_start(s, GT_JOB_REMOVE); return; }
                } else {
                    g->idle_looks = g->unk_looks = 0;
                }
            }
            if (now - g->rows_at >= 2 && !flag("gametile_nohide")) { g->rows_at = now; set_tile_row_visible(tid, 0); }
            return;
        }
        if (res != 2) {
            if (now - g->rows_at >= 2 && !flag("gametile_nohide")) { g->rows_at = now; set_tile_row_visible(tid, 0); }
            return;
        }
        /* accepted: like gone_tick() - the rows the shell leaves behind are deleted 1.5 s, 10 s and 30 s after the uninstall went through */
        const double t_now = mono_s();
        int rows = db_has_title(tid); /* -1: the shell's database cannot be read just now. Nothing is decided then (asked again next second), but the slot is not held for ever: after 90 s it is
                                       * let go, as the table of timings says (the rows of the tile, if any are left, are deleted when the ID is used again) */
        if (rows > 0) {
            const double age_s = t_now - t_acc;
            const int due = (g->purges == 0 && age_s >= 1.5) || (g->purges == 1 && age_s >= 10.0) || (g->purges == 2 && age_s >= 30.0);
            char p[120];
            snprintf(p, sizeof(p), "/user/app/%s", tid);
            if (due && (!exists(p) || age_s >= 8.0)) {
                g->purges++;
                const int d = purge_title_rows(tid);
                if (d > 0) g->rows_deleted += d;
                rows = db_has_title(tid);
            }
        }
        if (rows == 0 || t_now - t_acc > 90.0) {
            say("game tile %s (%s): gone from the home screen's list %.1f s after the uninstall (%d leftover row(s) deleted)", tid, g->title, t_now - t_acc, g->rows_deleted);
            notify_tile("%s was taken off the home screen.", g->title);
            gt_slot_reset(s);
            gt_removing_save();
        }
    }
}

/* one tile at a time, every 2 s: is it on the home screen yet (a registered title: rows in the shell's database, and ShadowMount+ knows it)? A tile that is not,
 * and no app runs (the shelf counts as an active game for ShadowMount+: it takes nothing in while it runs), is asked for with a scan. An installed tile that has
 * no rows any more (deleted from the home screen's menu) is registered again by ShadowMount+, so it is looked at once a minute. */
static void gt_probe(time_t now) {
    static time_t last;
    static int rr;
    if (now - last < 2) return;
    for (int k = 1; k <= GT_N; k++) {
        const int s = (rr + k) % GT_N;
        GtSlot *g = &g_gt[s];
        const char *tid = kGameSlots[s];
        if (GT_LOAD(g->worker)) continue;
        if (g->st == GT_INSTALLING) {
            last = now;
            rr = s;
            const int rows = db_has_title(tid);
            if (rows > 0) {
                char ib[80], ir[300];
                snprintf(ib, sizeof(ib), "{\"title_id\":\"%s\"}", tid);
                if (smp_call("POST", "/api/v1/games/info", ib, ir, sizeof(ir)) == 200) {
                    g->st = GT_OK;
                    if (!flag("gametile_nohide")) set_tile_row_visible(tid, 1);
                    g->rows_at = now;
                    g->rows_zero = 0;
                    g_gt_state_dirty = 1;
                    say("game tile %s (%s) is on the home screen", tid, g->title);
                    notify_tile("%s is on the home screen.", g->title);
                }
            } else if (rows == 0 && now >= g->scan_at) {
                if (app_processes() != 0) { /* the shelf (or a game) runs: ShadowMount+ takes nothing in; the process table (a sysctl of all processes) is looked at again in 10 s, not every 2 s */
                    g->scan_at = now + 10;
                } else {
                    char reply[300] = "";
                    const int busy = smp_busy();
                    const int code = busy == 1 ? 409 : smp_post("/api/v1/scan", "{\"reset_attempts\":true}", reply, sizeof(reply));
                    g->scan_tries++;
                    g->scan_at = now + (g->scan_tries < 6 ? 8 : 30);
                    if (g->scan_tries <= 2 || g->scan_tries % 10 == 0) say("game tile %s: not on the home screen yet: scan HTTP %d (try %d)", tid, code, g->scan_tries);
                }
            }
            return;
        }
        if (g->st == GT_OK && now - g->rows_at >= 60) {
            last = now;
            rr = s;
            g->rows_at = now;
            const int rows = db_has_title(tid);
            if (rows == 0 && ++g->rows_zero >= 3) {
                g->st = GT_INSTALLING;
                g->scan_at = now;
                g->scan_tries = 0;
                g->rows_zero = 0;
                g_gt_state_dirty = 1;
                say("game tile %s: not in the home screen's list any more: registered again", tid);
            } else if (rows > 0) {
                g->rows_zero = 0;
            }
            return;
        }
    }
}

/* a newer eboot of PS5SX2, or a damaged folder: one tile at a time, only while no app runs */
static void gt_recheck(time_t now) {
    static time_t next;
    if (now < next) return;
    next = now + 60;
    if (GT_LOAD(g_gt_build_busy) || gt_disc_busy() || app_processes() != 0) return;
    for (int s = 0; s < GT_N; s++) {
        GtSlot *g = &g_gt[s];
        if ((g->st != GT_OK && g->st != GT_INSTALLING) || GT_LOAD(g->worker)) continue;
        if (!gt_folder_ok(s)) {
            say("game tile %s: its folder is damaged: taken down", kGameSlots[s]);
            gt_fail_add(g->path, g->serial, g->title, "damaged");
            g->st = GT_REMOVING;
            GT_STORE(g->result, 0);
            gt_removing_save();
            g_gt_state_dirty = 1;
            continue;
        }
        if (!flag("gametile_norefresh") && !gt_sig_current(s) && now >= g->refresh_at && gt_start(s, GT_JOB_REFRESH)) {
            g->refresh_at = now + 600; /* a refresh that fails is tried again after ten minutes, not every minute (37 MB each time) */
            next = now + 3;            /* the next tile follows as soon as this one is done */
            return;
        }
    }
}

/* ---------------------------------------------------------------- stale sandboxes of the game tiles' apps (the disc tiles' heal_tick() does not know these ids) */
static int gt_game_sandbox_name(const char *name) {
    if (strlen(name) < 11 || name[9] != '_') return 0;
    for (const char *p = name + 10; *p; p++)
        if (!isalnum((unsigned char)*p)) return 0;
    for (int s = 0; s < GT_N; s++)
        if (strncmp(name, kGameSlots[s], 9) == 0) return 1;
    return 0;
}

static int gt_list_sandboxes(Sbx *out, int max) {
    DIR *d = opendir(SANDBOX_DIR);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (n >= max || !gt_game_sandbox_name(e->d_name)) continue;
        char p[160];
        snprintf(p, sizeof(p), SANDBOX_DIR "/%s", e->d_name);
        struct stat st;
        time_t age_s = 0;
        if (stat(p, &st) == 0) {
            const time_t last = st.st_ctime > st.st_mtime ? st.st_ctime : st.st_mtime;
            age_s = time(NULL) - last;
            if (age_s < 0) age_s = 0;
        }
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        out[n].age = age_s;
        n++;
    }
    closedir(d);
    return n;
}

/* heal_one() for a game tile's sandbox (its own copy: heal_one() has a single call site, and a second caller would change how the compiler inlines it into the
 * disc tiles' heal_sandboxes()). The mounts are taken off, then the folders removed; nothing is walked into a mount. 1 when it is gone. */
static int gt_heal_one(const char *name) {
    char p[160];
    snprintf(p, sizeof(p), SANDBOX_DIR "/%s", name);
    log_sandbox("before the clean-up (game tile)", name);
    struct stat st;
    if (lstat(p, &st) != 0) return 1;
    struct statfs root;
    if (statfs(p, &root) != 0) { say("sandbox %s: statfs failed (errno %d): nothing removed", name, errno); return 0; }
    Mnt mm[16];
    const int nm = mounts_below(p, mm, 16);
    for (int i = 0; i < nm; i++) {
        int rc = unmount(mm[i].on, 0);
        if (rc != 0) rc = unmount(mm[i].on, MNT_FORCE);
        if (rc == 0) rmdir(mm[i].on);
    }
    int budget = 400;
    (void)rm_same_fs(p, st.st_dev, &root.f_fsid, 0, &budget);
    const int gone = !exists(p);
    say("sandbox %s (game tile): %d mount(s) taken off; folders removed: %s", name, nm, gone ? "yes" : "NO");
    return gone;
}

static void gt_heal_game_sandboxes(int min_age) {
    if (app_processes() != 0) return;
    Sbx sb[16];
    const int n = gt_list_sandboxes(sb, 16);
    pthread_mutex_lock(&g_heal_mu);
    for (int i = 0; i < n; i++)
        if (sb[i].age >= min_age) gt_heal_one(sb[i].name);
    pthread_mutex_unlock(&g_heal_mu);
}

static void gt_heal_tick(time_t now) {
    static time_t next, stale_since, last_try;
    static int fails;
    if (now < next) return;
    next = now + 3;
    if (flag("gametile_noheal") || flag("disctile_noheal")) return;
    Sbx sb[16];
    const int n = gt_list_sandboxes(sb, 16);
    if (n <= 0) { stale_since = 0; fails = 0; return; }
    if (app_processes() != 0) { stale_since = 0; return; }
    if (stale_since == 0) stale_since = now;
    if (now - stale_since < 6 || now - last_try < (fails < 4 ? 20 : 120)) return;
    last_try = now;
    int healed = 0;
    pthread_mutex_lock(&g_heal_mu);
    for (int i = 0; i < n; i++)
        if (sb[i].age >= GT_HEAL_MIN_AGE && gt_heal_one(sb[i].name)) healed++;
    pthread_mutex_unlock(&g_heal_mu);
    fails = healed ? 0 : fails + 1;
    if (healed) say("cleaned up %d stale app sandbox(es) of game tiles", healed);
}

/* ---------------------------------------------------------------- start and loop */
static void gt_adopt(int s, const char *pending) { /* pending: the text of removing.txt (NULL when there is none) */
    const char *tid = kGameSlots[s];
    char dir[128], p[160];
    snprintf(dir, sizeof(dir), HB "/%s", tid);
    GtSlot *g = &g_gt[s];
    memset(g, 0, sizeof(*g));
    if (!exists(dir)) return; /* free (or a removal that was under way: removing.txt) */
    snprintf(p, sizeof(p), "%s/" GT_MARKER, dir);
    if (!exists(p)) {
        snprintf(p, sizeof(p), "%s/sce_sys.new", dir);
        if (!exists(p)) {
            /* A folder of ours that was being taken apart when the helper stopped (a crash, a power cut) has lost its marker already. It is still known for ours when this slot's removal is
             * written down (removing.txt) AND a tile of this ID was made here (the ledger): the slot stays FREE here, gt_init makes it REMOVING and the removal is finished (folder,
             * uninstall, rows). Anything else without a marker is somebody else's. */
            if (pending && strstr(pending, tid) && gt_ours(s)) {
                say("game tile slot %s: a half taken-down folder of ours (its removal was under way): the removal is finished", tid);
                return;
            }
            g->st = GT_FOREIGN;
            say("game tile slot %s: a folder that is not ours is there: the slot is left alone", tid);
            return;
        }
        say("game tile slot %s: an unfinished folder of an earlier run is taken away", tid);
        rm_tree(dir);
        return;
    }
    char path[GT_PATH], serial[20], title[128];
    if (!gt_marker_read(s, path, sizeof(path), serial, sizeof(serial), title, sizeof(title)) || !gt_folder_ok(s)) {
        say("game tile slot %s: the folder is not whole: it is taken down", tid);
        rm_tree(dir);
        return;
    }
    g->st = GT_INSTALLING;
    gt_cpy(g->path, sizeof(g->path), path);
    gt_cpy(g->serial, sizeof(g->serial), serial);
    gt_cpy(g->title, sizeof(g->title), title);
    g->scan_at = time(NULL) + 5;
    whitelist_add(tid);
}

static void gt_drop_old_requests(void) {
    DIR *d = opendir(GT_DIR);
    if (!d) return;
    struct dirent *e;
    const time_t now = time(NULL);
    while ((e = readdir(d)) != NULL) {
        if (!gt_req_name(e->d_name) && strcmp(e->d_name, "request.txt") != 0 && strcmp(e->d_name, "request.work") != 0) continue;
        char p[160];
        struct stat st;
        snprintf(p, sizeof(p), GT_DIR "/%s", e->d_name);
        if (stat(p, &st) == 0 && now - st.st_mtime > 600) { say("game tile: the request file %s is %lld s old: dropped unread", e->d_name, (long long)(now - st.st_mtime)); unlink(p); }
    }
    closedir(d);
}

/* once, from the first gt_tick() at which the console is not shutting down or resting */
static void gt_init(void) {
    if (!gt_enabled()) { /* off (the default): no folder, no alive.txt, no state.txt, no request read, nothing polled; only the clean-up below, and only if game tiles were ever made here */
        for (int s = 0; s < GT_N; s++) if (gt_ours(s)) g_gt_heal_only = 1; /* game tiles were made once: their played apps' sandboxes would block ShadowMount+ for the disc tiles */
        say("GameTile 1: off (flag gametile_off)%s", g_gt_heal_only ? "; the sandboxes of earlier game tiles are still cleaned up" : "");
        return;
    }
    mkdirs(GT_DIR);
    chmod(GT_DIR, 0777);
    mkdirs(GT_PRIV);
    gt_drop_old_requests();
    char *ns = read_file(GT_PRIV "/next.txt", NULL);
    if (ns) { g_gt_next = atoi(ns) % GT_N; if (g_gt_next < 0) g_gt_next = 0; free(ns); }
    char *rm = read_file(GT_PRIV "/removing.txt", NULL); /* removals that were under way when the helper stopped */
    for (int s = 0; s < GT_N; s++) gt_adopt(s, rm);
    if (rm) {
        for (int s = 0; s < GT_N; s++) {
            if (!strstr(rm, kGameSlots[s]) || g_gt[s].st == GT_FOREIGN || !gt_ours(s)) continue;
            if (g_gt[s].st == GT_FREE) gt_cpy(g_gt[s].title, sizeof(g_gt[s].title), "?");
            g_gt[s].st = GT_REMOVING;
        }
        free(rm);
    }
    for (int s = 0; s < GT_N; s++) {
        char p[96];
        if (g_gt[s].st != GT_FREE) continue;
        if (!gt_ours(s)) continue; /* never used here: whatever is under this ID is not ours */
        snprintf(p, sizeof(p), "/user/app/%s", kGameSlots[s]);
        if (exists(p)) {
            g_gt[s].st = GT_REMOVING;
            gt_cpy(g_gt[s].title, sizeof(g_gt[s].title), "?");
            say("game tile slot %s: installed but its folder is gone: taken down", kGameSlots[s]);
        } else {
            gt_purge_ghost(s); /* rows of a tile that was removed earlier */
        }
    }
    gt_removing_save();
    int n = 0;
    for (int s = 0; s < GT_N; s++) if (g_gt[s].st == GT_INSTALLING) n++;
    g_gt_ready = 1;
    g_gt_state_dirty = 1;
    gt_state_flush();
    say("GameTile 1: %d game tile(s) found; requests are read from " GT_DIR "/request-*.txt (one file a second)", n);
    klog_printf("[kp] GameTile 1: %d game tile(s)\n", n);
}

/* alive.txt: "<unix time> <pid> gametile <rev>" every 20 s while the helper works (not while the console shuts down): the shelf shows that the helper that
 * reads its requests is there (a file older than a minute: it is not). Written whole (temporary file, then rename). */
static time_t g_gt_alive_next;
static void gt_alive(time_t now) {
    if (now < g_gt_alive_next) return;
    g_gt_alive_next = now + 20;
    char b[96];
    const int n = snprintf(b, sizeof(b), "%lld %d gametile " GT_REV "\n", (long long)time(NULL), (int)getpid());
    if (n <= 0 || write_file(GT_DIR "/alive.tmp", b, (size_t)n) != 0) { g_gt_alive_next = now + 2; return; } /* (a write that failed is tried again soon: the shelf calls the helper absent after a minute) */
    chmod(GT_DIR "/alive.tmp", 0666);
    if (rename(GT_DIR "/alive.tmp", GT_DIR "/alive.txt") != 0) { unlink(GT_DIR "/alive.tmp"); g_gt_alive_next = now + 2; }
}

/* The folder the shelf and the helper share is checked once a second: somebody who "cleans up" /data/PCSX2 (or a tester who deletes the folder to start afresh) must not leave
 * the helper deaf and mute until it is loaded again. It is made again with the mode the shelf needs (0777; the helper's umask may take bits away from mkdir's), and state.txt
 * and alive.txt are written at once. */
static void gt_dir_ensure(void) {
    struct stat st;
    if (stat(GT_DIR, &st) == 0 && S_ISDIR(st.st_mode)) return;
    if (lstat(GT_DIR, &st) == 0) unlink(GT_DIR); /* a file or a link where the folder goes */
    mkdirs(GT_DIR);
    chmod(GT_DIR, 0777);
    g_gt_state_dirty = 1;
    g_gt_alive_next = 0;
    say("game tile: the folder " GT_DIR " was gone: made again");
}

/* from the main loop, after the disc work of the pass; once a second. noinline: payload_run() then gets one call instruction and nothing else, so that the
 * assembly of every disc function can be compared with the baseline (scratch asmdiff: all identical but payload_run's call and its stack-slot numbering). */
__attribute__((noinline)) static void gt_tick(time_t now) {
    static time_t last;
    if (now == last) return;
    last = now;
    if (kp_quiet()) return;
    if (kp_game_quiet() && !kp_game_quiet_due(5)) return; /* a game plays from a drive: once per 3 s */
    if (!g_gt_init_done) { g_gt_init_done = 1; gt_init(); }
    if (g_gt_ready && flag("gametile_off")) { /* 179: gametile_off appeared after the helper started: go dormant without a reload */
        g_gt_ready = 0; g_gt_heal_only = 1;
        say("GameTile: flag gametile_off appeared: no more tiles are made (earlier ones' sandboxes are still cleaned up)");
    }
    if (!g_gt_ready) { if (g_gt_heal_only) gt_heal_tick(now); return; }
    gt_dir_ensure();
    gt_alive(now);
    gt_requests_read();
    gt_queue_run();
    for (int s = 0; s < GT_N; s++)
        if (g_gt[s].st == GT_BUILDING || g_gt[s].st == GT_REMOVING) gt_follow(s, now);
    gt_probe(now);
    gt_recheck(now);
    gt_heal_tick(now);
    gt_state_flush();
}
