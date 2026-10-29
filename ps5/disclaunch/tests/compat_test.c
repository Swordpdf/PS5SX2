/* Host test of disc_compat.h and disc_compat_db.h (helper 171k, next to the PS5SX3 helper; AI-assisted).
 * run-compat-test.sh builds it with ASAN+UBSAN and with -O2, against SQLite (the SDK's sqlite3.h, the system's libsqlite3). */
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disc_compat.h"
#include "disc_compat_db.h"
#include "iso_fake.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------- sectors */
typedef struct { iso_fake *m; uint32_t bad_from; int reads; } rdctx;
static int rd(void *ctx, uint32_t lba, uint8_t *out) {
    rdctx *c = (rdctx *)ctx;
    c->reads++;
    if (lba >= c->m->secs || (c->bad_from && lba >= c->bad_from)) return 0;
    memcpy(out, c->m->d + (size_t)lba * 2048, 2048);
    return 1;
}
static int kind_of(iso_fake *m, uint32_t bad_from, int *reads) {
    rdctx c = {m, bad_from, 0};
    const int k = dc_iso_kind(rd, &c);
    if (reads) *reads = c.reads;
    return k;
}

static void test_iso_kind(void) {
    printf("dc_iso_kind: the disc's kind from its root folder\n");
    iso_fake m;
    int reads = 0;
    iso_init(&m, 64, 1, 1);
    iso_add(&m, "SYSTEM.CNF;1", "BOOT2 = cdrom0:\\SLUS_213.51;1\r\nVER = 1.00\r\n");
    iso_add(&m, "SLUS_213.51;1", "x");
    CHECK(kind_of(&m, 0, &reads) == DC_KIND_PS2 && reads <= 4, "a PS2 disc (BOOT2): PS2 in %d reads", reads);
    iso_free(&m);

    iso_init(&m, 64, 1, 1);
    iso_add(&m, "SYSTEM.CNF;1", "BOOT = cdrom:\\SCUS_944.55;1\r\nTCB = 4\r\n");
    CHECK(kind_of(&m, 0, NULL) == DC_KIND_PS1, "a PS1 disc (BOOT, no BOOT2): PS1");
    iso_free(&m);

    iso_init(&m, 64, 1, 1);
    iso_add(&m, "PS3_DISC.SFB;1", "\x2e" "SFB");
    iso_add(&m, "PS3_GAME", NULL);
    iso_add(&m, "PS3_UPDATE", NULL);
    CHECK(kind_of(&m, 0, &reads) == DC_KIND_PS3 && reads == 2, "a PS3 disc (PS3_DISC.SFB first): PS3 in 2 reads (%d)", reads);
    iso_free(&m);

    iso_init(&m, 64, 1, 1);
    iso_add(&m, "PS3_GAME", NULL);
    CHECK(kind_of(&m, 0, NULL) == DC_KIND_PS3, "a PS3 disc with only the PS3_GAME folder: PS3");
    iso_free(&m);

    iso_init(&m, 64, 1, 1);
    iso_add(&m, "ps3_disc.sfb;1", "x");
    CHECK(kind_of(&m, 0, NULL) == DC_KIND_PS3, "lower case names: PS3 all the same");
    iso_free(&m);

    iso_init(&m, 64, 1, 1);
    iso_add(&m, "SYSTEM.CNF;1", "BOOT = cdrom:\\X;1\r\n");
    iso_add(&m, "PS3_GAME", NULL);
    CHECK(kind_of(&m, 0, NULL) == DC_KIND_PS3, "a BOOT-only SYSTEM.CNF and PS3_GAME: PS3 wins over PS1");
    iso_free(&m);

    iso_init(&m, 256, 1, 2);
    for (int i = 0; i < 70; i++) { char n[24]; snprintf(n, sizeof(n), "FILLER%02d.DAT;1", i); iso_add(&m, n, NULL); }
    iso_add(&m, "PS3_DISC.SFB;1", "x");
    CHECK(kind_of(&m, 0, NULL) == DC_KIND_PS3, "the PS3 marker in the root folder's second sector: PS3");
    iso_free(&m);

    iso_init(&m, 64, 1, 1);
    iso_add(&m, "PS3_GAMEX", NULL);
    iso_add(&m, "SYSTEM.CNFX;1", "BOOT2 = cdrom0:\\SLUS_000.01;1");
    iso_add(&m, "VIDEO_TS", NULL);
    CHECK(kind_of(&m, 0, NULL) == DC_KIND_OTHER, "longer names that start like the markers (PS3_GAMEX, SYSTEM.CNFX) and a video disc's folder: other");
    iso_free(&m);

    iso_init(&m, 64, 0, 1);
    CHECK(kind_of(&m, 0, NULL) == DC_KIND_OTHER, "no ISO 9660 volume descriptor (a PS4/PS5 BD): other");
    iso_free(&m);

    iso_init(&m, 64, 1, 1);
    iso_add(&m, "PS3_DISC.SFB;1", "x");
    CHECK(kind_of(&m, 16, NULL) == DC_KIND_UNKNOWN, "sector 16 does not read (the disc spins up): unknown (asked again)");
    CHECK(kind_of(&m, 20, NULL) == DC_KIND_OTHER, "the root folder does not read: other (no marker seen)");
    iso_free(&m);

    iso_init(&m, 64, 1, 1); /* a damaged folder record: a name that runs past the record */
    uint8_t *r = m.d + 20 * 2048;
    r[0] = 40;
    r[32] = 200;
    CHECK(kind_of(&m, 0, NULL) == DC_KIND_OTHER, "a name longer than its record: no read past it, other");
    iso_free(&m);
}

/* ---------------------------------------------------------------- the PS5's mount */
typedef struct {
    const char *files[16];
    dc_mnt m[8];
    int nm;
} fakefs;
static int ff_exists(void *u, const char *p) {
    fakefs *f = (fakefs *)u;
    for (int i = 0; i < 16 && f->files[i]; i++) if (!strcmp(f->files[i], p)) return 1;
    return 0;
}
static int ff_mounts(void *u, dc_mnt *out, int max) {
    fakefs *f = (fakefs *)u;
    const int n = f->nm < max ? f->nm : max;
    memcpy(out, f->m, (size_t)n * sizeof(dc_mnt));
    return n;
}
static void ff_mount(fakefs *f, const char *from, const char *on) {
    snprintf(f->m[f->nm].from, sizeof(f->m[f->nm].from), "%s", from);
    snprintf(f->m[f->nm].on, sizeof(f->m[f->nm].on), "%s", on);
    f->nm++;
}

static void test_mount(void) {
    printf("dc_mount_ps3: a PS3 disc's files on the PS5's mount\n");
    char where[160];
    {
        fakefs f = {{"/mnt/disc/PS3_GAME/PARAM.SFO"}, {{"", ""}}, 0};
        const dc_fs fs = {ff_exists, ff_mounts, &f};
        CHECK(dc_mount_ps3(&fs, 0, where, sizeof(where)) == 1 && !strcmp(where, "/mnt/disc"), "/mnt/disc with PS3_GAME/PARAM.SFO: cd0's PS3 disc (%s)", where);
        CHECK(dc_mount_ps3(&fs, 1, where, sizeof(where)) == 0 && !where[0], "but not cd1's");
    }
    {
        fakefs f = {{"/mnt/disc/PS3_DISC.SFB"}, {{"", ""}}, 0};
        const dc_fs fs = {ff_exists, ff_mounts, &f};
        CHECK(dc_mount_ps3(&fs, 0, where, sizeof(where)) == 1, "PS3_DISC.SFB alone: a PS3 disc");
    }
    {
        fakefs f = {{"/mnt/disc/SYSTEM.CNF"}, {{"", ""}}, 0};
        const dc_fs fs = {ff_exists, ff_mounts, &f};
        CHECK(dc_mount_ps3(&fs, 0, where, sizeof(where)) == 0, "a PS2 disc's SYSTEM.CNF on the mount: not a PS3 disc");
    }
    {
        fakefs f = {{"/mnt/disc/PS3_GAME/PARAM.SFO"}, {{"", ""}}, 0};
        ff_mount(&f, "/dev/cd1", "/mnt/disc");
        const dc_fs fs = {ff_exists, ff_mounts, &f};
        CHECK(dc_mount_ps3(&fs, 0, where, sizeof(where)) == 0, "/mnt/disc mounted from /dev/cd1: not cd0's");
        CHECK(dc_mount_ps3(&fs, 1, where, sizeof(where)) == 1 && !strcmp(where, "/mnt/disc"), "it is cd1's (%s)", where);
    }
    {
        fakefs f = {{"/mnt/usb_bd/PS3_DISC.SFB"}, {{"", ""}}, 0};
        ff_mount(&f, "/dev/cd2", "/mnt/usb_bd");
        ff_mount(&f, "/dev/da0s1", "/mnt/usb0");
        const dc_fs fs = {ff_exists, ff_mounts, &f};
        CHECK(dc_mount_ps3(&fs, 2, where, sizeof(where)) == 1 && !strcmp(where, "/mnt/usb_bd"), "a mount of /dev/cd2 elsewhere with a PS3 disc: cd2's");
        CHECK(dc_mount_ps3(&fs, 0, where, sizeof(where)) == 0, "and not cd0's");
    }
    {
        fakefs f = {{NULL}, {{"", ""}}, 0};
        const dc_fs fs = {ff_exists, NULL, &f};
        CHECK(dc_mount_ps3(&fs, 0, NULL, 0) == 0, "nothing mounted, no mount table: 0 (and no `where`)");
    }
}

/* ---------------------------------------------------------------- the claim of a drive, over time */
static void test_claim(void) {
    printf("dc_gate / dc_note / dc_after_look: a drive's claim over time\n");
    dc_drive d;
    memset(&d, 0, sizeof(d));
    int ch = 0, opens = 0;
    double t = 100.0;

    /* a PS3 disc already mounted when the helper starts: never opened */
    for (int i = 0; i < 40; i++, t += 0.25) opens += dc_gate(&d, 1, t, &ch);
    CHECK(opens == 0 && d.claim == DC_YIELD && d.by_mount && d.kind == DC_KIND_PS3 && ch, "a PS3 disc on the mount from the start: 0 opens in 10 s, yielded");
    CHECK(dc_published(&d, t) == DC_YIELD, "published: yield");
    CHECK(dc_note(&d, DC_KIND_PS2, 123, t) == 0 && d.claim == DC_YIELD, "a stray note while the mount shows the PS3 files changes nothing");
    /* the disc goes: the mount's files go, a rest of DC_GRACE_S */
    ch = 0;
    CHECK(dc_gate(&d, 0, t, &ch) == 0 && ch && d.claim == DC_EMPTY, "the mount's files gone: claim empty, not opened yet");
    CHECK(dc_published(&d, t) == DC_YIELD, "published as yield during the rest");
    opens = 0;
    const double out_at = t;
    double first = 0;
    for (int i = 0; i < 40; i++, t += 0.25) if (dc_gate(&d, 0, t, &ch)) { opens++; if (!first) first = t; }
    CHECK(first >= out_at + DC_GRACE_S - 0.001 && first <= out_at + DC_GRACE_S + 0.26, "opened again %.2f s after the disc went", first - out_at);
    CHECK(dc_published(&d, t) == DC_EMPTY, "published: empty after the rest");

    /* a PS2 disc */
    memset(&d, 0, sizeof(d));
    t = 200.0;
    CHECK(dc_note(&d, DC_KIND_UNKNOWN, 1000, t) == 1 && d.claim == DC_LOOKING, "a disc in, not known yet: looking");
    CHECK(dc_note(&d, DC_KIND_PS2, 1000, t) == 1 && d.claim == DC_PS2 && dc_published(&d, t) == DC_PS2, "its PS2 serial: ps2");
    opens = 0;
    for (int i = 0; i < 40; i++, t += 0.25) opens += dc_gate(&d, 0, t, &ch);
    CHECK(opens == 40, "a PS2 disc's drive is looked at every time (%d of 40)", opens);
    CHECK(dc_note(&d, DC_KIND_NONE, 0, t) == 1 && d.claim == DC_EMPTY && dc_gate(&d, 0, t, &ch) == 1, "out: empty, no rest (it was ours)");

    /* a PS3 disc found by its sectors, then mounted, then out */
    memset(&d, 0, sizeof(d));
    t = 300.0;
    dc_note(&d, DC_KIND_UNKNOWN, 5000, t);
    CHECK(dc_note(&d, DC_KIND_PS3, 5000, t) == 1 && d.claim == DC_YIELD && !d.by_mount && d.media == 5000, "a PS3 disc by its root folder: yielded by the sectors");
    opens = 0;
    int looks_same = 0;
    for (int i = 0; i < 80; i++, t += 0.25) if (dc_gate(&d, 0, t, &ch)) { opens++; looks_same += dc_after_look(&d, 5000, t) == 0; }
    CHECK(opens == 3 && looks_same == 3 && d.claim == DC_YIELD, "20 s (300-319.75): one cheap look per 5 s (%d), the same disc each time: still yielded", opens);
    ch = 0;
    CHECK(dc_gate(&d, 1, t, &ch) == 0 && ch && d.by_mount, "its mount shows up: held by the mount now");
    opens = 0;
    for (int i = 0; i < 80; i++, t += 0.25) opens += dc_gate(&d, 1, t, &ch);
    CHECK(opens == 0, "held by the mount: no look at all in 20 s");

    /* another kind of disc (a video DVD): yielded after the tries, cheap looks; swapped for a PS2 disc */
    memset(&d, 0, sizeof(d));
    t = 400.0;
    dc_note(&d, DC_KIND_UNKNOWN, 7000, t);
    dc_note(&d, DC_KIND_OTHER, 7000, t);
    CHECK(d.claim == DC_YIELD && d.kind == DC_KIND_OTHER, "a disc with no PS2 serial after the tries: yielded");
    t += DC_LOOK_S;
    CHECK(dc_gate(&d, 0, t, &ch) == 1, "its look comes after DC_LOOK_S");
    CHECK(dc_after_look(&d, 9000, t) == 1 && d.claim == DC_EMPTY && dc_gate(&d, 0, t, &ch) == 1, "another disc (another size): taken back at once, no rest");
    dc_note(&d, DC_KIND_OTHER, 9000, t);
    t += DC_LOOK_S;
    CHECK(dc_gate(&d, 0, t, &ch) == 1 && dc_after_look(&d, 0, t) == 1 && dc_gate(&d, 0, t + 1, &ch) == 0 && dc_gate(&d, 0, t + DC_GRACE_S, &ch) == 1,
          "no disc at the look: taken back after the rest");
    CHECK(dc_after_look(&d, 0, t) == 0, "dc_after_look on a drive that is not yielded: nothing");

    /* a yield by the sectors with an unknown size (0): any disc counts as the same, only "no disc" ends it */
    memset(&d, 0, sizeof(d));
    dc_note(&d, DC_KIND_PS1, 0, 500.0);
    CHECK(dc_after_look(&d, 4242, 505.0) == 0 && dc_after_look(&d, 0, 510.0) == 1, "a yield with no size known: ends only when no disc is in");

    /* the clock going back (a monotonic clock should not; the gate must not lock the drive away for ever) */
    memset(&d, 0, sizeof(d));
    dc_note(&d, DC_KIND_OTHER, 1, 1000.0);
    CHECK(dc_gate(&d, 0, 10.0, &ch) == 1, "a look time far ahead (the clock went back): the look is taken");
    d.claim = DC_EMPTY;
    d.grace_until = 1000.0;
    CHECK(dc_gate(&d, 0, 10.0, &ch) == 1, "a rest far ahead (the clock went back): not waited for");
}

/* ---------------------------------------------------------------- the status file and what the PS5SX3 helper reads from it */
static void test_status(void) {
    printf("the status file's lines (ps5sx2-helper.txt) and the PS5SX3 helper's rule\n");
    int c[DC_NDRIVES] = {DC_YIELD, DC_EMPTY, DC_EMPTY, DC_EMPTY, DC_NONE};
    char s[256], full[400];
    dc_status_lines(c, s, sizeof(s));
    CHECK(!strcmp(s, "compat=1\ncd0=yield\ncd1=empty\ncd2=empty\ncd3=empty\nusb=none\n"), "the lines: %s", s);
    snprintf(full, sizeof(full), "pid=77\ntime=1761700000\nrev=171k\n%s", s);
    CHECK(dc_status_yields(full) == 1, "cd0 yielded, nothing else: the drives are the PS5SX3 helper's to ask");
    c[0] = DC_PS2;
    dc_status_lines(c, s, sizeof(s));
    CHECK(dc_status_yields(s) == 0, "cd0 holds a PS2 disc: not");
    c[0] = DC_YIELD;
    c[DC_USB] = DC_PS2;
    dc_status_lines(c, s, sizeof(s));
    CHECK(dc_status_yields(s) == 0, "a PS2 disc in the USB drive, cd0 yielded: not (one rule for all drives)");
    c[DC_USB] = DC_YIELD;
    c[0] = DC_EMPTY;
    dc_status_lines(c, s, sizeof(s));
    CHECK(dc_status_yields(s) == 1, "the USB drive yielded, cd0 empty: yes");
    for (int i = 0; i < DC_NDRIVES; i++) c[i] = DC_EMPTY;
    dc_status_lines(c, s, sizeof(s));
    CHECK(dc_status_yields(s) == 0, "every drive empty: not (the PS5SX3 helper keeps its mount-only mode)");
    c[1] = DC_LOOKING;
    dc_status_lines(c, s, sizeof(s));
    CHECK(dc_status_yields(s) == 0, "a drive looking: not");
    CHECK(dc_status_yields("pid=77\ntime=1761700000\nrev=171j\n") == -1, "171j's file (no compat line): -1");
    CHECK(dc_status_yields(NULL) == -1, "no file: -1");
    CHECK(dc_status_yields("compat=1\ncd0=yieldx\ncd1=ps22\n") == 0, "values that only start like yield / ps2 do not count");
    CHECK(dc_status_yields("compat=1\r\ncd0=yield") == 1, "the last line without a newline counts");
    char tiny[24];
    const int n = dc_status_lines(c, tiny, sizeof(tiny));
    CHECK(n > (int)sizeof(tiny) && strlen(tiny) == sizeof(tiny) - 1, "a small buffer: cut, terminated");
}

/* ---------------------------------------------------------------- the title IDs */
static void test_ids(void) {
    printf("title IDs: PS5SX2's and PS5SX3's ranges\n");
    const char *ours[] = {"PPSA99203", "PPSA99230", "PPSA99231", "PPSA99238", "PPSA99251", "PPSA99260", "PPSA99299", "PPSA99231_000"};
    for (size_t i = 0; i < sizeof(ours) / sizeof(ours[0]); i++) CHECK(dc_ps5sx2_tid(ours[i]) && !dc_ps5sx3_tid(ours[i]), "%s is PS5SX2's", ours[i]);
    const char *theirs[] = {"PPSA99303", "PPSA99331", "PPSA99334", "PPSA99339", "PPSA99341", "PPSA99388", "PPSA99389"};
    for (size_t i = 0; i < sizeof(theirs) / sizeof(theirs[0]); i++) CHECK(dc_ps5sx3_tid(theirs[i]) && !dc_ps5sx2_tid(theirs[i]), "%s is PS5SX3's", theirs[i]);
    const char *none[] = {"PPSA99239", "PPSA99250", "PPSA99261", "PPSA99290", "PPSA99304", "PPSA99330", "PPSA99335", "PPSA99390", "CUSA00001", "PPSA9923",
                          "PPSA99231X", "", NULL};
    for (size_t i = 0; i < sizeof(none) / sizeof(none[0]); i++)
        CHECK(!dc_ps5sx2_tid(none[i]) && !dc_ps5sx3_tid(none[i]), "%s is neither's", none[i] ? none[i] : "(null)");
    int both = 0;
    for (int n = 99000; n < 100000; n++) {
        char t[16];
        snprintf(t, sizeof(t), "PPSA%05d", n);
        both += dc_ps5sx2_tid(t) && dc_ps5sx3_tid(t);
    }
    CHECK(both == 0, "no ID of PPSA99000-99999 is both helpers'");
}

/* ---------------------------------------------------------------- the concept rows a removed tile leaves */
static void sql(sqlite3 *db, const char *s) {
    char *err = NULL;
    if (sqlite3_exec(db, s, NULL, NULL, &err) != SQLITE_OK) { printf("SQL failed: %s: %s\n", s, err ? err : "?"); g_fail++; }
    sqlite3_free(err);
}
static void test_concepts(void) {
    printf("dc_db_keep_shared_concepts: a concept another title uses stays\n");
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    sql(db, "CREATE TABLE tbl_iconinfo_1 (titleId TEXT, localConceptId TEXT, visible INTEGER)");
    sql(db, "CREATE TABLE tbl_concepticoninfo_1 (localConceptId TEXT, primaryTitleId TEXT)");
    sql(db, "CREATE TABLE tbl_iconinfo_2 (titleId TEXT, localConceptId TEXT, visible INTEGER)");
    sql(db, "INSERT INTO tbl_iconinfo_1 VALUES ('PPSA99233','cid:a',1), ('PPSA99233','cid:b',1), ('PPSA99230','cid:c',1)");
    sql(db, "INSERT INTO tbl_iconinfo_1 VALUES ('PPSA99331','cid:b',1)");      /* a PS5SX3 disc tile with the same concept */
    sql(db, "INSERT INTO tbl_concepticoninfo_1 VALUES ('cid:a','PPSA99233'), ('cid:c','PPSA99230'), ('cid:d','CUSA12345'), ('cid:e','')");
    sql(db, "INSERT INTO tbl_iconinfo_2 VALUES ('PPSA99233','cid:d',1)");
    char tabs[3][64] = {"tbl_iconinfo_1", "tbl_iconinfo_2"};
    char ctabs[3][80] = {"tbl_concepticoninfo_1", ""};
    char cids[256] = "'cid:a','cid:b','cid:c','cid:d','cid:e'", kept[200];
    const int n = dc_db_keep_shared_concepts(db, tabs, ctabs, 2, cids, sizeof(cids), kept, sizeof(kept));
    CHECK(n == 2 && !strcmp(cids, "'cid:a','cid:c','cid:e'") && !strcmp(kept, "cid:b cid:d"),
          "cid:b (a PS5SX3 tile's too) and cid:d (a game's concept row) stay; ours and an empty primary go: %d [%s] kept [%s]", n, cids, kept);
    char none[256] = "";
    CHECK(dc_db_keep_shared_concepts(db, tabs, ctabs, 2, none, sizeof(none), kept, sizeof(kept)) == 0 && !none[0] && !kept[0], "an empty list: nothing");
    char one[256] = "'cid:zz'";
    CHECK(dc_db_keep_shared_concepts(db, tabs, ctabs, 2, one, sizeof(one), NULL, 0) == 0 && !strcmp(one, "'cid:zz'"), "an unknown concept: kept in the list");
    char bad[256] = "'cid:a";
    CHECK(dc_db_keep_shared_concepts(db, tabs, ctabs, 2, bad, sizeof(bad), kept, sizeof(kept)) == 0 && !bad[0], "an unterminated list: emptied (nothing deleted)");
    sqlite3_close(db);
}

int main(void) {
    test_iso_kind();
    test_mount();
    test_claim();
    test_status();
    test_ids();
    test_concepts();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
