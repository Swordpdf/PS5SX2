#!/usr/bin/env python3
# Helper 171l (build 184), AI-assisted: the host test of "the Reload tile removed by the person stays removed" AS IT SHIPS. Cuts the 171l block of
# (171m, build 185: and the flag switched while running from PS5SX2's settings: reload_tile_flag_tick takes the tile away / makes it again, once, and
# switching it back on is not taken for a removal by the person)
# disctile.c (reload_tile_mark_shown .. reload_tile_watch) out as it is, puts a made-up world around it (files in a test folder, the home screen's rows,
# a virtual clock) and checks: the tile seen then deleted -> the flag is made once; our own rebuild / a scan in progress / an unreadable database /
# a tile never seen -> no flag. Usage: make_reload_test.py <disctile.c> <out.c>
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()
# 171n (build 186): the tile is off unless the flag disctile_reload asks for it; the shipped reload_tile_off is put in the test as it is, and the cases
# of 171l/171m run with that flag there (as someone who wants the tile has it)
off = re.search(r"static int reload_tile_off\(void\) \{[^\n]*\}\n", src).group(0)
a = src.index("/* ---------------------------------------------------------------- 171l: the Reload tile removed by the person")
b = src.index("/* ---------------------------------------------------------------- 171l end */", a)
block = src[a:b]
for name in ("reload_tile_mark_shown", "reload_tile_user_removed", "reload_tile_removed_by_user", "reload_tile_watch", "reload_tile_flag_tick"):
    assert ("static void %s(" % name) in block or ("static int %s(" % name) in block, name

head = r'''
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RT_DIR "/tmp/rt171l"
#define WORK RT_DIR "/work"
#define FLAGS_DIR RT_DIR "/flags"
#define RELOAD_TID "PPSA99230"
#define RELOAD_SHOWN WORK "/reload-tile-shown"
/* the shipped code names /user/app/PPSA99230: the test's own folder stands for it */
#define USER_APP RT_DIR "/user_app_" RELOAD_TID

static volatile int g_reload_running;
static volatile time_t g_reload_scan_at;
static volatile time_t g_reload_ours_at;
static int g_rows;          /* the home screen's rows of the tile; -1 = cannot be read */
static int g_ensures, g_says;
static char g_last[600];

static int exists(const char *p) {
    struct stat st;
    if (strcmp(p, "/user/app/" RELOAD_TID) == 0) p = USER_APP;
    return stat(p, &st) == 0;
}
static int write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int ok = fwrite(data, 1, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}
static int flag(const char *name) {
    char p[200];
    snprintf(p, sizeof(p), FLAGS_DIR "/%s", name);
    return exists(p);
}
static int db_has_title(const char *tid) { (void)tid; return g_rows; }
@OFF@
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last, sizeof(g_last), fmt, ap);
    va_end(ap);
    g_says++;
    if (getenv("RT_VERBOSE")) printf("  say: %s\n", g_last);
}
/* the shipped reload_tile_ensure: with the flag the folder is taken away and (171m) the seen mark forgotten; without it the tile is made again.
 * Counted here (g_ensures with the flag, g_makes without). */
static int g_makes;
static void reload_tile_ensure(void) {
    if (reload_tile_off()) { g_ensures++; unlink(RELOAD_SHOWN); }
    else g_makes++;
}
'''

tail = r'''
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s (line %d)\n", what, __LINE__); fails++; } else if (getenv("RT_VERBOSE")) printf("ok: %s\n", what); } while (0)

static void reset(void) {
    (void)!system("rm -rf " RT_DIR " && mkdir -p " WORK " " FLAGS_DIR " && touch " FLAGS_DIR "/disctile_reload");
    g_reload_running = 0; g_reload_scan_at = 0; g_reload_ours_at = 0; g_rows = 0; g_ensures = 0; g_says = 0; g_makes = 0; g_noreload_seen = -1;
}
static void installed(int yes) {
    g_rows = yes ? 2 : 0;
    if (yes) { FILE *f = fopen(USER_APP, "w"); if (f) fclose(f); } else unlink(USER_APP);
}
static time_t run(time_t from, int seconds) {
    for (time_t t = from; t < from + seconds; t++) { reload_tile_watch(t); reload_tile_flag_tick(t); }
    return from + seconds;
}
static void set_flag(int on) {
    if (on) { (void)!system("mkdir -p " FLAGS_DIR); FILE *f = fopen(FLAGS_DIR "/disctile_noreload", "w"); if (f) fclose(f); }
    else unlink(FLAGS_DIR "/disctile_noreload");
}

int main(void) {
    time_t now = 100000;
    /* 0. 171n: off unless disctile_reload is there; disctile_noreload turns it off even then */
    reset();
    unlink(FLAGS_DIR "/disctile_reload");
    CHECK(reload_tile_off(), "171n: no disctile_reload -> the tile is off");
    set_flag(1);
    CHECK(reload_tile_off(), "171n: disctile_noreload alone -> off");
    (void)!system("touch " FLAGS_DIR "/disctile_reload");
    CHECK(reload_tile_off(), "171n: both flags -> off");
    set_flag(0);
    CHECK(!reload_tile_off(), "171n: disctile_reload alone -> on");

    /* 1. seen, then deleted by the person: the flag after two looks, the folder taken away once */
    reset();
    installed(1);
    now = run(now, 12);
    CHECK(exists(RELOAD_SHOWN), "a tile on the home screen is marked as seen");
    CHECK(!flag("disctile_noreload"), "no flag while the tile is there");
    installed(0);
    now = run(now, 4);
    CHECK(!flag("disctile_noreload"), "one look is not enough");
    now = run(now, 12);
    CHECK(flag("disctile_noreload"), "deleted by the person -> flag disctile_noreload made");
    CHECK(g_ensures == 1, "the flag's way (reload_tile_ensure) ran once");
    CHECK(strstr(g_last, "removed by the person") != NULL, "logged");
    CHECK(!exists(RELOAD_SHOWN), "the seen mark is taken away");
    now = run(now, 60);
    CHECK(g_ensures == 1, "nothing more once the flag is there");

    /* 2. never seen (ShadowMount+ never installed it): no flag */
    reset();
    installed(0);
    now = run(now, 120);
    CHECK(!flag("disctile_noreload"), "a tile never seen on the home screen is not taken for removed");

    /* 3. our own rebuild of the folder (g_reload_ours_at) and a scan we wait for: no flag */
    reset();
    installed(1);
    now = run(now, 10);
    installed(0);
    g_reload_ours_at = now;
    now = run(now, 100);
    CHECK(!flag("disctile_noreload"), "no flag within 120 s of our own rebuild");
    g_reload_ours_at = 0;
    g_reload_scan_at = now + 1000;
    now = run(now, 60);
    CHECK(!flag("disctile_noreload"), "no flag while we wait for the tile to appear");
    g_reload_scan_at = 0;
    g_reload_running = 1;
    now = run(now, 60);
    CHECK(!flag("disctile_noreload"), "no flag while a reload runs");
    g_reload_running = 0;

    /* 4. the database cannot be read (-1): not "no row" */
    reset();
    installed(1);
    now = run(now, 10);
    unlink(USER_APP);
    g_rows = -1;
    now = run(now, 60);
    CHECK(!flag("disctile_noreload"), "an unreadable database is not a removal");

    /* 5. rows gone but /user/app still there (the shell still removing it): no flag */
    reset();
    installed(1);
    now = run(now, 10);
    g_rows = 0;
    now = run(now, 60);
    CHECK(!flag("disctile_noreload"), "the /user/app folder still there: not removed yet");

    /* 6. a blip: gone for one look only, then back */
    reset();
    installed(1);
    now = run(now, 10);
    installed(0);
    reload_tile_watch(now); now += 5;
    installed(1);
    now = run(now, 30);
    CHECK(!flag("disctile_noreload"), "gone for a single look and back: no flag");

    /* 7. the start-up check (reload_tile_user_removed) */
    reset();
    installed(1);
    now = run(now, 10);
    installed(0);
    CHECK(reload_tile_user_removed() == 1, "at the start: seen before and gone now -> removed");
    unlink(RELOAD_SHOWN);
    CHECK(reload_tile_user_removed() == 0, "at the start: never seen -> not removed");

    /* 8. 171m: the switch in PS5SX2's settings while the payload runs */
    reset();
    installed(1);
    now = run(now, 12);
    CHECK(exists(RELOAD_SHOWN) && g_ensures == 0 && g_makes == 0, "171m: tile there and seen; the first look of the flag does nothing");
    set_flag(1);                              /* Off */
    now = run(now, 4);
    CHECK(g_ensures == 1, "171m: flag made in the settings -> the tile taken away within 3 s");
    CHECK(strstr(g_last, "switched off while running") != NULL, "171m: logged");
    CHECK(!exists(RELOAD_SHOWN), "171m: the seen mark forgotten (taken away by the flag, not by the person)");
    installed(0);                             /* ShadowMount+ uninstalled it */
    now = run(now, 30);
    CHECK(g_ensures == 1 && g_makes == 0, "171m: nothing more while the flag stays");
    set_flag(0);                              /* On again */
    now = run(now, 4);
    CHECK(g_makes == 1, "171m: flag deleted -> the tile made again within 3 s");
    CHECK(strstr(g_last, "switched on while running") != NULL, "171m: logged");
    now = run(now, 60);                       /* the tile not on the home screen yet: not a removal by the person */
    CHECK(!flag("disctile_noreload"), "171m: switching it back on is not taken for a removal (no flag made again)");
    installed(1);
    now = run(now, 10);
    CHECK(exists(RELOAD_SHOWN) && g_makes == 1, "171m: once it is back on the home screen it counts as seen again");
    /* the person deletes it on the home screen: 171l's flag, and the tick does not take that for a switch */
    installed(0);
    now = run(now, 20);
    CHECK(flag("disctile_noreload") && g_ensures == 2, "171m: deleted on the home screen -> 171l's flag and its way, once");
    now = run(now, 20);
    CHECK(g_ensures == 2 && g_makes == 1, "171m: the flag the payload made itself is not taken for a switch");
    /* a reload in progress: the switch waits for it */
    set_flag(0);
    g_reload_running = 1;
    now = run(now, 10);
    CHECK(g_makes == 1, "171m: not while a reload runs");
    g_reload_running = 0;
    now = run(now, 4);
    CHECK(g_makes == 2, "171m: ... and done once it is over");

    (void)!system("rm -rf " RT_DIR);
    printf("reload-tile removal test: %s (%d failure(s))\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
'''

open(sys.argv[2], "w", encoding="utf-8").write(head.replace("@OFF@", off) + block + tail)
