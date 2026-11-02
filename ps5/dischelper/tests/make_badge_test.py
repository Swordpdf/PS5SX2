#!/usr/bin/env python3
# Helper 171n (build 186), AI-assisted: the host test of the PS2 badge on PS5SX2's tiles AS IT SHIPS. Cuts the 171n functions out of disctile.c
# (set_title_names, badge_title_names, write_param_json's naming, the pass over app.db / appinfo.db / the param.json copies) with the shipped database
# helpers they use, puts a made-up console around them (a test folder for /data/homebrew, /user/app, appmeta; two SQLite files shaped like the PS5's)
# and checks: every name of PS5SX2's own title IDs gets the badge once, nobody else's does, TITLE_ID is left alone, a second pass changes nothing.
# Usage: make_badge_test.py <disctile.c> <out.c>
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()


def function(name):
    m = re.search(r"\nstatic [a-z_ *]+\b%s\([^)]*\) \{" % re.escape(name), src)
    assert m, name
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[m.start() + 1:j + 1] + "\n"
    raise AssertionError(name)


a = src.index("#define PS2_BADGE")
b = src.index("/* sce_sys/param.json for a slot: PS5SX2's own")
names = src[a:b]
c = src.index("/* ---------------------------------------------------------------- 171n: the PS2 badge on the names already there")
d = src.index("/* ---------------------------------------------------------------- 171n end */", c)
block = src[c:d]
block = block.replace('"/system_data/priv/mms/appinfo.db"', 'BT_DIR "/appinfo.db"')
block = block.replace('"/user/app/%s/sce_sys/param.json"', 'BT_DIR "/user_app/%s/sce_sys/param.json"')
block = block.replace('"/system_data/priv/appmeta/%s/param.json"', 'BT_DIR "/appmeta/%s/param.json"')
assert "/system_data" not in block and '"/user/app' not in block
helpers = "".join(function(n) for n in ("exec", "icon_tables", "db_int", "concept_table_for", "table_exists"))
writer = function("write_param_json")

head = r'''
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "sqlite3.h"

#define BT_DIR "/tmp/bt171n"
#define HB BT_DIR "/homebrew"
#define APP_DB BT_DIR "/app.db"
#define APP_SRC HB "/PPSA99203"
static const char kParamFallback[] = "{\"titleName\": \"PS5SX2\"}";
static int g_says;
static char g_last[600];

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static char *read_file(const char *path, size_t *n_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) b[n] = '\0';
    if (n_out) *n_out = (size_t)n;
    return b;
}
static int write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    const int ok = fwrite(data, 1, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}
static int flag(const char *name) { (void)name; return 0; }
static int kp_quiet(void) { return 0; }
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last, sizeof(g_last), fmt, ap);
    va_end(ap);
    g_says++;
    if (getenv("BT_VERBOSE")) printf("  say: %s\n", g_last);
}
static void json_escape(char *dst, size_t size, const char *src) {
    size_t k = 0;
    for (const char *p = src; *p && k + 8 < size; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { dst[k++] = '\\'; dst[k++] = (char)c; }
        else dst[k++] = (char)c;
    }
    dst[k] = '\0';
}
static char *replace_all(const char *s, const char *from, const char *to) {
    const size_t fl = strlen(from), tl = strlen(to);
    size_t count = 0;
    for (const char *p = s; (p = strstr(p, from)) != NULL; p += fl) count++;
    char *out = (char *)malloc(strlen(s) + count * (tl + 1) + 1), *o = out;
    if (!out) return NULL;
    for (const char *p = s;;) {
        const char *q = strstr(p, from);
        if (!q) { strcpy(o, p); break; }
        memcpy(o, p, (size_t)(q - p)); o += q - p;
        memcpy(o, to, tl); o += tl;
        p = q + fl;
    }
    return out;
}
'''

tail = r'''
static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s (line %d)\n", what, __LINE__); fails++; } else if (getenv("BT_VERBOSE")) printf("ok: %s\n", what); } while (0)
#define B PS2_BADGE

static void sql(const char *path, const char *s) {
    sqlite3 *db = NULL;
    sqlite3_open(path, &db);
    char *err = NULL;
    if (sqlite3_exec(db, s, NULL, NULL, &err) != SQLITE_OK) { printf("setup sql failed: %s\n", err ? err : "?"); fails++; }
    sqlite3_free(err);
    sqlite3_close(db);
}
static int count(const char *path, const char *q) {
    sqlite3 *db = NULL;
    sqlite3_open(path, &db);
    const int n = (int)db_int(db, q, -1);
    sqlite3_close(db);
    return n;
}
static void put(const char *path, const char *text) {
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "mkdir -p \"$(dirname '%s')\"", path);
    (void)!system(cmd);
    write_file(path, text, strlen(text));
}
static int has(const char *path, const char *text) {
    char *s = read_file(path, NULL);
    const int yes = s && strstr(s, text) != NULL;
    free(s);
    return yes;
}

int main(void) {
    /* 1. set_title_names: every titleName value replaced, whatever it was (2.20's "PS5SX2", 186's with the badge, escaped quotes) */
    {
        char *a = set_title_names("{\"x\": 1, \"localizedParameters\": {\"en-US\": {\"titleName\": \"PS5SX2\"}, \"fr-FR\": {\"titleName\": \"P\\\"S\"}}}", "NEW");
        CHECK(a && strcmp(a, "{\"x\": 1, \"localizedParameters\": {\"en-US\": {\"titleName\": \"NEW\"}, \"fr-FR\": {\"titleName\": \"NEW\"}}}") == 0, "both names replaced, an escaped quote skipped");
        free(a);
        CHECK(set_title_names("{\"title\": \"x\"}", "NEW") == NULL, "no titleName: NULL");
    }
    /* 2. badge_title_names: once, and nothing the second time */
    {
        char *a = badge_title_names("{\"titleName\": \"PS5SX2\", \"titleName\": \"" B " Already\"}");
        CHECK(a && strcmp(a, "{\"titleName\": \"" B " PS5SX2\", \"titleName\": \"" B " Already\"}") == 0, "the badge and a space in front of the name that had none");
        char *b2 = a ? badge_title_names(a) : NULL;
        CHECK(a && !b2, "a second pass changes nothing");
        free(a); free(b2);
    }
    /* 3. write_param_json: the tile's title with the badge, from PS5SX2's param.json with or without the badge */
    (void)!system("rm -rf " BT_DIR " && mkdir -p " BT_DIR "/out");
    put(APP_SRC "/sce_sys/param.json", "{\"titleId\": \"PPSA99203\", \"en-US\": {\"titleName\": \"" B " PS5SX2\"}, \"contentId\": \"UP9000-PPSA99203_00-PCSX2ORBIS00001\", \"conceptId\": \"99203\"}");
    CHECK(write_param_json(BT_DIR "/out", 0, "PPSA99251", "Ratchet & Clank") && has(BT_DIR "/out/param.json", "\"titleName\": \"" B " Ratchet & Clank\""),
          "a game tile's name: badge + title");
    CHECK(has(BT_DIR "/out/param.json", "\"titleId\": \"PPSA99251\"") && !has(BT_DIR "/out/param.json", "PS5SX2\""), "its own title ID, PS5SX2's name gone");
    CHECK(write_param_json(BT_DIR "/out", 0, "PPSA99231", B " Already") && has(BT_DIR "/out/param.json", "\"titleName\": \"" B " Already\"") &&
          !has(BT_DIR "/out/param.json", B " " B), "a title that has the badge does not get it twice");
    put(APP_SRC "/sce_sys/param.json", "{\"en-US\": {\"titleName\": \"PS5SX2\"}}");
    CHECK(write_param_json(BT_DIR "/out", 0, "PPSA99232", "Disc \"Two\"") && has(BT_DIR "/out/param.json", "\"titleName\": \"" B " Disc \\\"Two\\\"\""),
          "2.20's param.json (no badge) works too, the title escaped");

    /* 4. the pass over the databases and the param.json copies */
    (void)!system("rm -rf " BT_DIR " && mkdir -p " BT_DIR);
    sql(APP_DB, "CREATE TABLE tbl_contentinfo(titleId TEXT, titleName TEXT);"
                "CREATE TABLE tbl_iconinfo_0305952281(titleId TEXT, titleName TEXT, platform INTEGER);"
                "CREATE TABLE tbl_concepticoninfo_0305952281(primaryTitleId TEXT, primaryTitleName TEXT);"
                "INSERT INTO tbl_contentinfo VALUES('PPSA99203','PS5SX2'),('PPSA99251','Ratchet & Clank'),('CUSA00001','Other Game'),('PPSA99230','Reload PS2 Disc');"
                "INSERT INTO tbl_iconinfo_0305952281 VALUES('PPSA99203','PS5SX2',0),('PPSA99251','Ratchet & Clank',0),('CUSA00001','Other Game',1);"
                "INSERT INTO tbl_concepticoninfo_0305952281 VALUES('PPSA99203','PS5SX2'),('PPSA99251','Ratchet & Clank'),('CUSA00001','Other Game');");
    sql(BT_DIR "/appinfo.db", "CREATE TABLE tbl_appinfo(titleId TEXT, metaDataId TEXT, key TEXT, val TEXT);"
                              "INSERT INTO tbl_appinfo VALUES('PPSA99203','m','TITLE','PS5SX2'),('PPSA99203','m','TITLE_01','PS5SX2'),"
                              "('PPSA99203','m','TITLE_ID','PPSA99203'),('PPSA99251','m','TITLE','Ratchet & Clank'),('CUSA00001','m','TITLE','Other Game');");
    put(HB "/PPSA99251/sce_sys/param.json", "{\"titleName\": \"Ratchet & Clank\"}");
    put(BT_DIR "/user_app/PPSA99251/sce_sys/param.json", "{\"titleName\": \"Ratchet & Clank\"}");
    put(BT_DIR "/appmeta/PPSA99203/param.json", "{\"titleName\": \"PS5SX2\"}");
    put(HB "/PPSA99203/sce_sys/param.json", "{\"titleName\": \"PS5SX2\"}");
    badge_names_tick(1000);
    CHECK(count(APP_DB, "SELECT COUNT(*) FROM tbl_contentinfo WHERE titleName='" B " PS5SX2'") == 1, "PS5SX2's own name in tbl_contentinfo");
    CHECK(count(APP_DB, "SELECT COUNT(*) FROM tbl_iconinfo_0305952281 WHERE titleName LIKE '" B " %'") == 2, "PS5SX2 and the game tile in tbl_iconinfo");
    CHECK(count(APP_DB, "SELECT COUNT(*) FROM tbl_concepticoninfo_0305952281 WHERE primaryTitleName='" B " Ratchet & Clank'") == 1, "the concept row too");
    CHECK(count(APP_DB, "SELECT COUNT(*) FROM tbl_contentinfo WHERE titleName LIKE '%" B "%' AND titleId IN ('CUSA00001','PPSA99230')") == 0,
          "nobody else's name, not the Reload tile's");
    CHECK(count(BT_DIR "/appinfo.db", "SELECT COUNT(*) FROM tbl_appinfo WHERE val LIKE '" B " %'") == 3, "appinfo.db: TITLE and TITLE_01 of ours");
    CHECK(count(BT_DIR "/appinfo.db", "SELECT COUNT(*) FROM tbl_appinfo WHERE key='TITLE_ID' AND val='PPSA99203'") == 1, "TITLE_ID left alone");
    CHECK(has(HB "/PPSA99251/sce_sys/param.json", B " Ratchet") && has(BT_DIR "/user_app/PPSA99251/sce_sys/param.json", B " Ratchet") &&
          has(BT_DIR "/appmeta/PPSA99203/param.json", B " PS5SX2"), "the param.json copies");
    CHECK(has(HB "/PPSA99203/sce_sys/param.json", "\"PS5SX2\""), "PS5SX2's own folder is left to the package");
    const int says = g_says;
    badge_names_tick(1100);
    CHECK(g_says == says, "not again within 5 min");
    badge_names_tick(1400);
    CHECK(g_says == says && count(APP_DB, "SELECT COUNT(*) FROM tbl_contentinfo WHERE titleName LIKE '%" B " " B "%'") == 0,
          "5 min later: nothing missing, nothing written, never twice");
    if (getenv("BT_REAL_DIR")) {
        /* a copy of a console's app.db and appinfo.db (never the console's own files): what the pass would change there */
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "cp \"%s/app.db\" " APP_DB " && cp \"%s/appinfo.db\" " BT_DIR "/appinfo.db", getenv("BT_REAL_DIR"), getenv("BT_REAL_DIR"));
        (void)!system(cmd);
        int changed_app = 0, changed_info = 0;
        const int m1 = badge_db(APP_DB, &changed_app), m2 = badge_db(BT_DIR "/appinfo.db", &changed_info);
        printf("real copy: app.db %d missing, %d changed; appinfo.db %d missing, %d changed\n", m1, changed_app, m2, changed_info);
        CHECK(badge_db(APP_DB, &changed_app) == 0 && badge_db(BT_DIR "/appinfo.db", &changed_info) == 0, "real copy: nothing missing afterwards");
        CHECK(count(APP_DB, "PRAGMA integrity_check") != -1, "real copy: still a database");
    }
    (void)!system("rm -rf " BT_DIR);
    printf("PS2 badge test: %s (%d failure(s))\n", fails ? "FAILED" : "all passed", fails);
    return fails ? 1 : 0;
}
'''

open(sys.argv[2], "w", encoding="utf-8").write(head + names + writer + helpers + block + tail)
