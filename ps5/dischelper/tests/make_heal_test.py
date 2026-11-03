#!/usr/bin/env python3
# Helper 171p (build 187), AI-assisted: the host test of the stale-sandbox clean-up AS IT SHIPS. Cuts rm_same_fs() and heal_one() .. heal_tick() out of
# disctile.c as they are, puts a made-up world around them (sandbox folders in a test folder, mounts that exist only in a table, the app count, ShadowMount+'s
# busy answer, a virtual clock) and checks, built once with DISC_SMP_OWNS_DATA=1 (the no-mount helper) and once with 0:
#   no-mount: no mount is ever taken off by the automatic clean-up; PS5SX2's own sandboxes and any with a mount below them stay (said once); empty leftovers
#             go; no further tries when only those are left; nothing while an app runs; the Reload tile's clean-up still takes mounts off.
#   other:    nothing while ShadowMount+ is free; a clean-up only after it said busy for 60 s in a row with no app running; a free answer starts the 60 s again.
# Usage: make_heal_test.py <disctile.c> <out.c>
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()


def cut(start, end):
    a = src.index(start)
    b = src.index(end, a)
    return src[a:b]


types = re.search(r"#ifndef DISC_SMP_OWNS_DATA\n#define DISC_SMP_OWNS_DATA 0\n#endif\ntypedef struct \{ char name\[32\]; time_t age; \} Sbx;[^\n]*\n"
                  r"typedef struct \{ char on\[160\]; char from\[96\]; char fs\[16\]; \} Mnt;\n", src).group(0)
rm = cut("static int rm_same_fs(", "/* one sandbox: mounts off, then removed.")
heal = cut("/* one sandbox: mounts off, then removed.", "/* From the main loop, twice a second, for every tile that is being taken down")
for name in ("heal_one", "heal_left_alone", "heal_sandboxes", "heal_tick"):
    assert re.search(r"static (int|void) %s\(" % name, heal), name
assert not re.search(r"\bval\b", heal), "the cut uses the word val (the test renames statfs's fsid field)"

head = r'''
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#define T_DIR "/tmp/heal171p"
#define SANDBOX_DIR T_DIR "/sbx"
#define MNT_FORCE 0x80000
typedef __fsid_t fsid_t_host;
#define fsid_t fsid_t_host
#define val __val

static int g_apps, g_busy, g_busy_calls, g_unmounts, g_says, g_notifies;
static volatile int g_scan_pending;
static volatile time_t g_scan_next;
static char g_said[96][400];
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (getenv("HT_VERBOSE")) printf("    say: %s\n", line);
    if (g_says < 96) snprintf(g_said[g_says], sizeof(g_said[0]), "%s", line);
    g_says++;
}
static void notify_tile(const char *fmt, ...) { (void)fmt; g_notifies++; }
static int flag(const char *name) { (void)name; return 0; }
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static void log_sandbox(const char *what, const char *name) { (void)what; (void)name; }
static int app_processes(void) { return g_apps; }
static int smp_busy(void) { g_busy_calls++; return g_busy; }
'''

world = r'''
/* the mounts exist only here: "<sandbox>/<sub>" entries that unmount() takes away */
typedef struct { char on[160]; int up; } FakeMnt;
static FakeMnt g_m[32];
static int g_nm;
static void add_mount(const char *sbx, const char *sub) {
    snprintf(g_m[g_nm].on, sizeof(g_m[0].on), SANDBOX_DIR "/%s/%s", sbx, sub);
    g_m[g_nm].up = 1;
    g_nm++;
}
static int mounts_below(const char *dir, Mnt *out, int max) {
    int k = 0;
    const size_t dl = strlen(dir);
    for (int i = 0; i < g_nm && k < max; i++) {
        const char *p = g_m[i].on;
        if (!g_m[i].up || strncmp(p, dir, dl) != 0 || (p[dl] != '\0' && p[dl] != '/')) continue;
        snprintf(out[k].on, sizeof(out[k].on), "%s", p);
        snprintf(out[k].from, sizeof(out[k].from), "/data");
        snprintf(out[k].fs, sizeof(out[k].fs), "nullfs");
        k++;
    }
    return k;
}
static int unmount(const char *on, int flags) {
    (void)flags;
    g_unmounts++;
    for (int i = 0; i < g_nm; i++)
        if (g_m[i].up && strcmp(g_m[i].on, on) == 0) { g_m[i].up = 0; return 0; }
    errno = EINVAL;
    return -1;
}
/* the sandbox folders as the test made them, all old enough to be touched */
static int list_sandboxes(Sbx *out, int max) {
    DIR *d = opendir(SANDBOX_DIR);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (n >= max || strncmp(e->d_name, "PPSA992", 7) != 0) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        out[n].age = 100;
        n++;
    }
    closedir(d);
    return n;
}
'''

tail = r'''
static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } else if (getenv("HT_VERBOSE")) printf("ok:   %s\n", msg); } while (0)
static void sh(const char *c) { if (system(c) != 0) { fprintf(stderr, "cmd failed: %s\n", c); exit(2); } }
static void mk(const char *name, int with_mount) {
    char c[400];
    snprintf(c, sizeof(c), "mkdir -p " SANDBOX_DIR "/%s/data " SANDBOX_DIR "/%s/tmp && echo x > " SANDBOX_DIR "/%s/tmp/f", name, name, name);
    sh(c);
    if (with_mount) add_mount(name, "data");
}
static int there(const char *name) { char p[200]; snprintf(p, sizeof(p), SANDBOX_DIR "/%s", name); return exists(p); }
static int said(const char *needle) { int n = 0; for (int i = 0; i < g_says && i < 96; i++) if (strstr(g_said[i], needle)) n++; return n; }
static time_t now = 1000000;
static void run(int secs) { for (int i = 0; i < secs; i++) { heal_tick(now); now++; } }
static void fresh(void) { /* an empty sandbox folder for a few seconds: heal_tick's state starts over */
    sh("rm -rf " T_DIR " && mkdir -p " SANDBOX_DIR);
    g_nm = 0; g_unmounts = 0; g_says = 0; g_busy_calls = 0; g_apps = 0; g_busy = 0;
    run(3);
}
static time_t until_gone(const char *name, int max_s) { for (int i = 0; i < max_s; i++) { heal_tick(now); now++; if (!there(name)) return now - 1; } return 0; }

int main(void) {
#if DISC_SMP_OWNS_DATA
    fresh();
    mk("PPSA99203_000", 1); mk("PPSA99231_000", 1); mk("PPSA99232_000", 0);
    run(200);
    CHECK(g_unmounts == 0, "no-mount helper: no mount taken off in 200 s");
    CHECK(there("PPSA99203_000"), "no-mount helper: PS5SX2's own sandbox (with ShadowMount+'s /data mount) stays");
    CHECK(there("PPSA99231_000"), "no-mount helper: a tile's sandbox with a mount below it stays");
    CHECK(!there("PPSA99232_000"), "no-mount helper: an empty leftover sandbox of a tile is removed");
    CHECK(said("left alone") == 1, "no-mount helper: the sandboxes left alone are said once");
    const int tries = said("stale app sandbox(es) with no app running");
    run(400);
    CHECK(said("stale app sandbox(es) with no app running") == tries, "no-mount helper: no more tries when only the ones left alone are there");
    CHECK(said("left alone") == 1, "no-mount helper: ... and they are not said again");
    CHECK(heal_left_alone("PPSA99203_000") && heal_left_alone("PPSA99231_000"), "heal_left_alone: PS5SX2's own and a mounted one");

    fresh();
    mk("PPSA99203_001", 0);
    run(200);
    CHECK(there("PPSA99203_001"), "no-mount helper: PS5SX2's own sandbox stays even with nothing mounted in it");

    fresh();
    mk("PPSA99233_000", 0);
    g_apps = 1;
    run(200);
    CHECK(there("PPSA99233_000"), "no-mount helper: nothing removed while an app runs");
    g_apps = 0;
    run(60);
    CHECK(!there("PPSA99233_000"), "no-mount helper: removed once no app has run for a while");

    fresh();
    mk("PPSA99203_000", 1); mk("PPSA99231_000", 1);
    const int healed = heal_sandboxes(2, "reload", 0);
    CHECK(healed == 2 && g_unmounts == 2 && !there("PPSA99203_000") && !there("PPSA99231_000"),
          "no-mount helper: the Reload tile's clean-up does what it always did (mounts off, folders removed)");
#else
    fresh();
    mk("PPSA99203_000", 1); mk("PPSA99231_000", 1);
    g_busy = 0;
    run(300);
    CHECK(g_unmounts == 0 && there("PPSA99203_000") && there("PPSA99231_000"), "other helper: nothing cleaned up while ShadowMount+ says free");
    g_busy = 1;
    const time_t t_busy = now;
    const time_t at = until_gone("PPSA99231_000", 200);
    CHECK(at && at - t_busy >= 60, "other helper: a clean-up only after ShadowMount+ said busy for 60 s");
    CHECK(at && at - t_busy <= 90, "other helper: ... and soon after that (within 90 s)");
    CHECK(!there("PPSA99203_000") && g_unmounts >= 2, "other helper: its mounts taken off, PS5SX2's leftover too (its own mount thread puts /data back)");

    fresh();
    mk("PPSA99231_001", 1);
    g_busy = 1;
    run(45);
    g_busy = 0;
    run(12);
    g_busy = 1;
    const time_t t2 = now;
    const time_t at2 = until_gone("PPSA99231_001", 200);
    CHECK(at2 && at2 - t2 >= 60, "other helper: a free answer in between starts the 60 s again");

    fresh();
    mk("PPSA99232_001", 1);
    g_busy = 1;
    g_apps = 1;
    run(200);
    CHECK(there("PPSA99232_001") && g_unmounts == 0, "other helper: nothing while an app runs, busy or not");
    CHECK(!heal_left_alone("PPSA99203_000") && !heal_left_alone("PPSA99232_001"), "other helper: heal_left_alone names nothing");
#endif
    sh("rm -rf " T_DIR);
    printf("heal test (DISC_SMP_OWNS_DATA=%d): %s (%d failure(s))\n", DISC_SMP_OWNS_DATA, fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
'''

open(sys.argv[2], "w", encoding="utf-8").write(head + types + world + rm + heal + tail)
