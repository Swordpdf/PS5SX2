#!/usr/bin/env python3
# Helper 171k (next to the PS5SX3 helper), AI-assisted: the host test of the drive polling AS IT SHIPS. Cuts out of disctile.c, as they are:
# the Drive type, read_serial (+ le32, read_lba), the 171k block with poll_drive_look / poll_drive, and write_helper_status; puts a made-up
# world around them (a disc image file standing for /dev/cdN, a made-up mount table with the PS5's mount, a virtual clock, the tile calls
# recorded) and runs insertions and ejects of PS3, PS2 and other discs. Usage: make_poll_test.py <disctile.c> <out.c>
import sys

src = open(sys.argv[1], encoding="utf-8").read()

def cut(start, end):
    a = src.index(start)
    b = src.index(end, a)
    return src[a:b]

drive_t = cut("typedef struct {\n    int present;", "static Drive g_drv[MAX_DRIVES];")
serial = cut("static uint32_t le32(const uint8_t *p)", "/* The game's name from GameIndex.yaml")
block = cut("/* ---------------------------------------------------------------- 171k: next to the PS5SX3 helper", "static void __attribute__((unused)) on_term")
status = cut("static void write_helper_status(void) {", "#ifdef DISC_HOST_THREAD\n/* The host helper")

# the device and the status file live in the test's folder
assert block.count('"/dev/cd%d"') == 1
block = block.replace('"/dev/cd%d"', 'PT_DEV "%d"')
assert status.count('"/data/PCSX2/logs/ps5sx2-helper.txt"') == 1
status = status.replace('"/data/PCSX2/logs/ps5sx2-helper.txt"', 'PT_STATUS')

head = r'''
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sqlite3.h>
#include "disc_compat.h"
#include "disc_compat_db.h"

#define PT_DIR "/tmp/pt171k" /* (short: the payload keeps the device name in 16 bytes) */
#define PT_DEV PT_DIR "/cd"
#define PT_STATUS PT_DIR "/ps5sx2-helper.txt"
#define PAYLOAD_REV "171k"
#define MAX_DRIVES 4
#define NSLOTS 8
#define POLL_US 250000
#define MNT_NOWAIT 2
#define DIOCGMEDIASIZE 0x40086481

/* the made-up world */
static double g_now = 1000.0;
static double mono_s(void) { return g_now; }
static int g_opens[MAX_DRIVES], g_reads, g_started, g_flag_nowait;
static char g_started_serial[32], g_log[1 << 16];
static size_t g_log_n;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (getenv("PT_VERBOSE")) printf("    [%.2f] %s\n", g_now, b);
    if (g_log_n + strlen(b) + 2 < sizeof(g_log)) g_log_n += (size_t)snprintf(g_log + g_log_n, sizeof(g_log) - g_log_n, "%s\n", b);
}
static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int flag(const char *name) { return !strcmp(name, "disctile_nowait") ? g_flag_nowait : 0; }
static int kp_quiet(void) { return 0; }
static int fake_open(const char *p, int fl) {
    const size_t n = strlen(PT_DEV);
    if (!strncmp(p, PT_DEV, n) && p[n] >= '0' && p[n] <= '3') g_opens[p[n] - '0']++;
    return open(p, fl);
}
#define open fake_open
static int fake_ioctl(int fd, unsigned long req, off_t *out) {
    struct stat st;
    if (req != DIOCGMEDIASIZE || fstat(fd, &st) != 0) return -1;
    *out = st.st_size;
    return 0;
}
#define ioctl fake_ioctl
static ssize_t fake_pread(int fd, void *b, size_t n, off_t off) { g_reads++; return pread(fd, b, n, off); }
#define pread fake_pread
struct fake_statfs { char f_mntfromname[96]; char f_mntonname[160]; };
static struct fake_statfs g_mnt[8];
static int g_nmnt;
static int fake_getmntinfo(struct fake_statfs **m, int how) { (void)how; *m = g_mnt; return g_nmnt; }
#define statfs fake_statfs
#define getmntinfo fake_getmntinfo
static int g_slot_busy[NSLOTS];
static double g_remove_t0;
static void remove_tile(int slot, const char *why) { say("(test) remove_tile %d: %s", slot, why); }
static int hide_system_tile(void) { return 0; }
static int purge_free_slots(const char *why) { (void)why; return 0; }
static int reload_row_front(const char *why) { (void)why; return 0; }
'''

drive_glue = r'''
static Drive g_drv[MAX_DRIVES];
static void start_tile(int drive, const char *serial) { g_started++; snprintf(g_started_serial, sizeof(g_started_serial), "%s", serial); (void)drive; }
'''

tail = r'''
#undef pread
#undef open
#undef ioctl
/* ---------------------------------------------------------------- the test */
#include "iso_fake.h"
static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
                              else if (getenv("PT_VERBOSE")) { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int log_has(const char *s) { return strstr(g_log, s) != NULL; }
static void log_clear(void) { g_log_n = 0; g_log[0] = 0; }

static void put_image(int drive, iso_fake *m) {
    char p[64];
    snprintf(p, sizeof(p), PT_DEV "%d", drive);
    FILE *f = fopen(p, "wb");
    fwrite(m->d, 2048, m->secs, f);
    fclose(f);
}
/* the disc out: the drive's node stays, with no media (an empty file); no_node: no node at all (cd1-cd3 of a PS5 without USB drives) */
static void take_image(int drive) { char p[64]; snprintf(p, sizeof(p), PT_DEV "%d", drive); FILE *f = fopen(p, "wb"); if (f) fclose(f); }
static void no_node(int drive) { char p[64]; snprintf(p, sizeof(p), PT_DEV "%d", drive); unlink(p); }
static void mount_ps3(int drive) {
    char d[96];
    snprintf(d, sizeof(d), PT_DIR "/mnt%d", drive);
    mkdir(d, 0777);
    char f[160];
    snprintf(f, sizeof(f), "%s/PS3_DISC.SFB", d);
    FILE *x = fopen(f, "w");
    if (x) fclose(x);
    snprintf(g_mnt[g_nmnt].f_mntfromname, sizeof(g_mnt[0].f_mntfromname), "/dev/cd%d", drive);
    snprintf(g_mnt[g_nmnt].f_mntonname, sizeof(g_mnt[0].f_mntonname), "%s", d);
    g_nmnt++;
}
static void unmount_all(void) {
    for (int i = 0; i < g_nmnt; i++) { char f[200]; snprintf(f, sizeof(f), "%s/PS3_DISC.SFB", g_mnt[i].f_mntonname); unlink(f); rmdir(g_mnt[i].f_mntonname); }
    g_nmnt = 0;
}
/* the main loop's turn: every drive polled, the status written when a claim changed or a second went by (as payload_run does) */
static void turn(void) {
    for (int i = 0; i < MAX_DRIVES; i++) poll_drive(i);
    static double last;
    if (g_now - last >= 1.0 || g_dc_dirty) { last = g_now; g_dc_dirty = 0; write_helper_status(); }
    g_now += 0.25;
}
static void run_for(double secs) { const double end = g_now + secs; while (g_now < end - 1e-9) turn(); }
static void zero_counts(void) { memset(g_opens, 0, sizeof(g_opens)); g_reads = 0; }
static int status_yields(void) {
    FILE *f = fopen(PT_STATUS, "r");
    if (!f) return -2;
    char b[512] = "";
    const size_t n = fread(b, 1, sizeof(b) - 1, f);
    b[n] = 0;
    fclose(f);
    return dc_status_yields(b);
}
static int status_has(const char *s) {
    FILE *f = fopen(PT_STATUS, "r");
    if (!f) return 0;
    char b[512] = "";
    const size_t n = fread(b, 1, sizeof(b) - 1, f);
    b[n] = 0;
    fclose(f);
    return strstr(b, s) != NULL;
}
static void reset_world(void) {
    for (int i = 0; i < MAX_DRIVES; i++) { if (i) no_node(i); else take_image(i); memset(&g_drv[i], 0, sizeof(Drive)); g_drv[i].slot = -1; }
    unmount_all();
    memset(g_dc, 0, sizeof(g_dc));
    for (int i = 0; i < DC_NCD; i++) g_dc_pub[i] = DC_EMPTY;
    for (int i = 0; i < DC_NDRIVES; i++) g_dc_watch_ps2[i] = 0;
    g_dc_pub[DC_USB] = DC_NONE;
    g_dc_off = 0;
    g_started = 0;
    g_now += 100;
    zero_counts();
    log_clear();
}

static void ps3_image(iso_fake *m) { iso_init(m, 400, 1, 1); iso_add(m, "PS3_DISC.SFB;1", "SFB"); iso_add(m, "PS3_GAME", NULL); iso_add(m, "PS3_UPDATE", NULL); }
static void ps2_image(iso_fake *m, uint32_t secs) { iso_init(m, secs, 1, 1); iso_add(m, "SYSTEM.CNF;1", "BOOT2 = cdrom0:\\SLES_534.15;1\r\nVER = 1.00\r\n"); iso_add(m, "SLES_534.15;1", "x"); }
static void video_image(iso_fake *m) { iso_init(m, 300, 1, 1); iso_add(m, "VIDEO_TS", NULL); iso_add(m, "AUDIO_TS", NULL); }

int main(void) {
    mkdir(PT_DIR, 0777);
    unlink(PT_STATUS);
    reset_world();
    iso_fake m;

    printf("1. a PS3 disc already on the PS5's mount when the helper starts\n");
    ps3_image(&m); put_image(0, &m); iso_free(&m);
    mount_ps3(0);
    run_for(10);
    CHECK(g_opens[0] == 0 && g_reads == 0, "/dev/cd0 never opened, nothing read in 10 s (opens %d, reads %d)", g_opens[0], g_reads);
    CHECK(dc_claim_of(0) == DC_YIELD && status_has("compat=1\ncd0=yield\n") && status_yields() == 1,
          "cd0 published as yield; the status file says compat=1, cd0=yield: the PS5SX3 helper's rule says the drives are its own");
    CHECK(log_has("a PS3 disc (its files on the PS5's mount " PT_DIR "/mnt0)") && g_started == 0, "logged with the mount point; no tile");

    printf("2. that disc taken out: the mount goes, a 5 s rest, then the drive is PS5SX2's again\n");
    take_image(0); unmount_all(); zero_counts();
    const double out_at = g_now;
    double first = 0;
    while (g_now < out_at + 8) { const int before = g_opens[0]; turn(); if (!first && g_opens[0] > before) first = g_now - 0.25; }
    CHECK(first >= out_at + DC_GRACE_S - 0.01 && first <= out_at + DC_GRACE_S + 0.3, "cd0 opened again %.2f s after the mount went", first - out_at);
    CHECK(status_has("cd0=empty") && status_yields() == 0, "then published empty: the PS5SX3 helper is back to its mount-only mode");

    printf("3. a PS3 disc put in, mounted 2 s later (the sectors tell it first)\n");
    reset_world();
    ps3_image(&m); put_image(0, &m); iso_free(&m);
    turn();
    const int reads_first = g_reads;
    CHECK(dc_claim_of(0) == DC_YIELD && log_has("a PS3 disc (its root folder), not a PS2 disc") && reads_first <= 6,
          "at the first look: no PS2 serial, its root folder says PS3: yielded (%d sector reads in all)", reads_first);
    zero_counts();
    run_for(2);
    CHECK(g_opens[0] <= 1 && g_reads == 0, "the next 2 s: at most one look at the size, no read (opens %d, reads %d)", g_opens[0], g_reads);
    mount_ps3(0);
    zero_counts();
    run_for(20);
    CHECK(g_opens[0] == 0 && g_reads == 0 && status_yields() == 1, "mounted: never opened in 20 s; published yield");
    CHECK(g_started == 0 && g_drv[0].tries > 20, "no tile, and the serial is not tried again");
    take_image(0); unmount_all(); log_clear();
    run_for(8);
    CHECK(log_has("the PS3 disc's files are gone from the PS5's mount") && log_has(PT_DEV "0: the disc is out") && !g_drv[0].present,
          "taken out: the mount's files gone, after the rest the disc is out");

    printf("4. a PS2 disc: PS5SX2's\n");
    reset_world();
    ps2_image(&m, 300); put_image(0, &m); iso_free(&m);
    run_for(3);
    CHECK(g_started == 1 && !strcmp(g_started_serial, "SLES-53415") && dc_claim_of(0) == DC_PS2, "its tile started (%s), cd0 = ps2", g_started_serial);
    CHECK(g_opens[0] == 12 && status_has("cd0=ps2") && status_yields() == 0, "looked at every turn (%d of 12); the status says ps2: the PS5SX3 helper leaves the drives",
          g_opens[0]);
    take_image(0); log_clear();
    const double t0 = g_now;
    while (g_drv[0].present && g_now < t0 + 3) turn();
    CHECK(!g_drv[0].present && g_now - t0 <= 1.0 && dc_claim_of(0) == DC_EMPTY, "out within %.2f s, no rest (it was a PS2 disc), cd0 empty", g_now - t0);

    printf("5. a video DVD: looked at for the serial's 5 s, then left alone; swapped for a PS2 disc\n");
    reset_world();
    video_image(&m); put_image(0, &m); iso_free(&m);
    run_for(4);
    CHECK(dc_claim_of(0) == DC_LOOKING, "the first 4 s: looking");
    run_for(2);
    CHECK(dc_claim_of(0) == DC_YIELD && log_has("no PS2 serial on this disc") && status_yields() == 1, "after the tries: yielded, published");
    zero_counts();
    run_for(20);
    CHECK(g_opens[0] >= 3 && g_opens[0] <= 5 && g_reads == 0, "20 s: one look at its size per 5 s (%d), nothing read (%d)", g_opens[0], g_reads);
    take_image(0);
    ps2_image(&m, 350); put_image(0, &m); iso_free(&m);
    log_clear();
    run_for(7);
    CHECK(g_started == 1 && dc_claim_of(0) == DC_PS2 && log_has("was replaced by another"), "the swap seen at the next look: a PS2 disc, its tile (%d)", g_started);

    printf("6. flag disc_noyield: 171j's way\n");
    reset_world();
    g_dc_off = 1;
    unlink(PT_STATUS);
    ps3_image(&m); put_image(0, &m); iso_free(&m);
    mount_ps3(0);
    run_for(5);
    CHECK(g_opens[0] == 20 && dc_claim_of(0) == DC_EMPTY && status_yields() == -1, "every turn opened (%d of 20), nothing published, no compat line", g_opens[0]);

    printf("7. the PS2 CD the watcher reads (cd0 shows no media): published as ps2\n");
    reset_world();
    dc_watch_ps2(0, 1);
    run_for(2);
    CHECK(dc_claim_of(0) == DC_PS2 && status_has("cd0=ps2") && status_yields() == 0, "cd0=ps2 from the watcher's word");
    dc_watch_ps2(0, 0);
    dc_publish(DC_USB, DC_YIELD);
    run_for(2);
    CHECK(status_has("usb=yield") && status_yields() == 1, "the watcher's USB drive yielded: usb=yield, the drives free for the PS5SX3 helper");

    reset_world();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
'''

out = head + drive_t + drive_glue + serial + block + status + tail
open(sys.argv[2], "w", encoding="utf-8").write(out)
