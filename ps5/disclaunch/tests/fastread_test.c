/* Host test of disc_fastread.h (helper 171h "disc speed", AI-assisted): a fake disc and fake drives.
 * cc -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -I<daemon dir> fastread_test.c -o fastread_test && ./fastread_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disc_fastread.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
    uint32_t ss, sectors;
    uint8_t *disc, *img;
    uint32_t big_max;              /* big reads above this many sectors are refused (0: none) */
    uint32_t bad_lo, bad_hi;       /* sectors that never read (bad_hi = 0: none) */
    uint32_t rawbad_lo, rawbad_hi; /* sectors the big reader cannot read but the piece reader can */
    uint32_t gone_at;              /* from this sector on nothing reads and gone() says 1 (0: never) */
    uint64_t fail_write_at;        /* a write at this output index fails (0: never) */
    uint32_t gone_calls, ticks, big_calls, piece_calls;
    uint64_t last_tick, max_written;
    int tick_order_ok;
} fake;

static uint8_t pattern(uint32_t lba, uint32_t i) { return (uint8_t)(lba * 31u + i * 7u + (lba >> 8)); }

static int in(uint32_t x, uint32_t lo, uint32_t hi) { return hi && x >= lo && x <= hi; }

static int read_common(fake *f, uint32_t lba, uint32_t n, uint8_t *buf, int big) {
    if ((uint64_t)lba + n > f->sectors) return 0;
    if (big && f->big_max && n > f->big_max) return 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t s = lba + k;
        if (f->gone_at && s >= f->gone_at) return 0;
        if (in(s, f->bad_lo, f->bad_hi)) return 0;
        if (big && in(s, f->rawbad_lo, f->rawbad_hi)) return 0;
    }
    memcpy(buf, f->disc + (size_t)lba * f->ss, (size_t)n * f->ss);
    return 1;
}
static int rd_big(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf) { fake *f = ctx; f->big_calls++; return read_common(f, lba, n, buf, 1); }
static int rd_piece(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf) { fake *f = ctx; f->piece_calls++; return read_common(f, lba, n, buf, 0); }
static int wr(void *ctx, uint64_t index, const uint8_t *buf, uint32_t n) {
    fake *f = ctx;
    if (f->fail_write_at && index <= f->fail_write_at && f->fail_write_at < index + n) return 0;
    if (index + n > f->sectors) return 0;
    memcpy(f->img + index * f->ss, buf, (size_t)n * f->ss);
    if (index + n > f->max_written) f->max_written = index + n;
    return 1;
}
static int gone(void *ctx) { fake *f = ctx; f->gone_calls++; return f->gone_at != 0; }
static void tick(void *ctx, uint64_t done) { fake *f = ctx; f->ticks++; if (done <= f->last_tick) f->tick_order_ok = 0; f->last_tick = done; }
static double fclock(void) { static double t; t += 0.001; return t; }

static fake *make(uint32_t ss, uint32_t sectors) {
    fake *f = calloc(1, sizeof(*f));
    f->ss = ss; f->sectors = sectors;
    f->disc = malloc((size_t)ss * sectors);
    f->img = calloc((size_t)ss, sectors);
    for (uint32_t s = 0; s < sectors; s++) for (uint32_t i = 0; i < ss; i++) f->disc[(size_t)s * ss + i] = pattern(s, i);
    f->tick_order_ok = 1;
    return f;
}
static void drop(fake *f) { free(f->disc); free(f->img); free(f); }

static uint8_t g_buf[512 * 2352];

static dfr_cfg cfg(fake *f, int with_big, uint32_t big, uint32_t piece) {
    dfr_cfg c;
    memset(&c, 0, sizeof(c));
    c.sector_size = f->ss;
    if (with_big) { c.big = rd_big; c.big_sectors = big; c.big_tries = 1; }
    c.piece = rd_piece; c.piece_sectors = piece; c.piece_tries = 3;
    c.one_tries = 2;
    c.max_bad = 20000;
    c.write = wr; c.gone = gone; c.tick = tick; c.clock = fclock;
    c.ctx = f;
    c.buf = g_buf; c.buf_bytes = sizeof(g_buf);
    return c;
}

/* the image is the disc except for the sectors in [zlo, zhi] (zeros there) */
static int image_ok(const fake *f, uint32_t upto, uint32_t zlo, uint32_t zhi) {
    static uint8_t zero[2352];
    for (uint32_t s = 0; s < upto; s++) {
        const uint8_t *a = f->img + (size_t)s * f->ss;
        if (in(s, zlo, zhi) ? memcmp(a, zero, f->ss) != 0 : memcmp(a, f->disc + (size_t)s * f->ss, f->ss) != 0) { printf("  sector %u differs\n", s); return 0; }
    }
    return 1;
}

int main(void) {
    /* 1. a USB DVD read from its /dev/cdN: 1 MiB reads only, no question to the drive */
    {
        fake *f = make(2048, 100003);
        dfr_cfg c = cfg(f, 1, 512, 32);
        dfr_stats st; memset(&st, 0, sizeof(st));
        const int rc = dfr_copy(&c, &st, 0, f->sectors);
        CHECK(rc == DFR_OK, "1: rc %d", rc);
        CHECK(image_ok(f, f->sectors, 0, 0), "1: image");
        CHECK(st.big_reads == (100003 + 511) / 512, "1: big reads %llu", (unsigned long long)st.big_reads);
        CHECK(st.piece_reads == 0 && st.one_reads == 0 && st.bad == 0, "1: pieces %llu", (unsigned long long)st.piece_reads);
        CHECK(st.largest == 512 * 2048, "1: largest %u", st.largest);
        CHECK(f->gone_calls == 0, "1: the drive was asked %u times during a clean copy", f->gone_calls);
        CHECK(st.done == f->sectors && f->last_tick == f->sectors && f->tick_order_ok, "1: done %llu ticks", (unsigned long long)st.done);
        CHECK(st.read_s > 0, "1: read time");
        drop(f);
    }
    /* 2. no /dev/cdN (or flag disc_oldreads): the old loop, 64 KiB pieces */
    {
        fake *f = make(2048, 70001);
        dfr_cfg c = cfg(f, 0, 0, 32);
        dfr_stats st; memset(&st, 0, sizeof(st));
        const int rc = dfr_copy(&c, &st, 0, f->sectors);
        CHECK(rc == DFR_OK && image_ok(f, f->sectors, 0, 0), "2: rc %d", rc);
        CHECK(st.piece_reads == (70001 + 31) / 32 && st.big_reads == 0 && f->big_calls == 0, "2: piece reads %llu", (unsigned long long)st.piece_reads);
        CHECK(st.largest == 32 * 2048, "2: largest %u", st.largest);
        drop(f);
    }
    /* 3. a PS2 CD, raw 2352-byte sectors, a drive that refuses 27 per READ CD: given up after two windows, the rest in 16s */
    {
        fake *f = make(2352, 20000);
        f->big_max = 16;
        dfr_cfg c = cfg(f, 1, 27, 16); /* (rd_big refuses more than big_max sectors, as the drive would the bigger READ CD) */
        dfr_stats st; memset(&st, 0, sizeof(st));
        const int rc = dfr_copy(&c, &st, 0, f->sectors);
        CHECK(rc == DFR_OK && image_ok(f, f->sectors, 0, 0), "3: rc %d", rc);
        CHECK(st.big_off == 1 && st.big_reads == 2 && st.big_refused == 2, "3: big off %d reads %llu refused %u", st.big_off, (unsigned long long)st.big_reads, st.big_refused);
        CHECK(st.bad == 0 && f->gone_calls == 0, "3: bad %u gone %u", st.bad, f->gone_calls);
        CHECK(st.largest == 16 * 2352, "3: largest %u", st.largest);
        drop(f);
    }
    /* 4. a PS2 CD the drive takes 27 at a time: 63,504-byte reads */
    {
        fake *f = make(2352, 3333);
        dfr_cfg c = cfg(f, 1, 27, 16);
        dfr_stats st; memset(&st, 0, sizeof(st));
        const int rc = dfr_copy(&c, &st, 0, f->sectors);
        CHECK(rc == DFR_OK && image_ok(f, f->sectors, 0, 0), "4: rc %d", rc);
        CHECK(st.big_reads == (3333 + 26) / 27 && st.piece_reads == 0 && st.largest == 63504, "4: big %llu largest %u", (unsigned long long)st.big_reads, st.largest);
        drop(f);
    }
    /* 5. unreadable sectors: zeros there and nowhere else, counted; the big read stays (a bad place is not a refused size) */
    {
        fake *f = make(2048, 50000);
        f->bad_lo = 5000; f->bad_hi = 5009;
        f->rawbad_lo = 30000; f->rawbad_hi = 30000; /* (one window the raw node cannot read but the pass device can) */
        dfr_cfg c = cfg(f, 1, 512, 32);
        dfr_stats st; memset(&st, 0, sizeof(st));
        const int rc = dfr_copy(&c, &st, 0, f->sectors);
        CHECK(rc == DFR_OK, "5: rc %d", rc);
        CHECK(image_ok(f, f->sectors, 5000, 5009), "5: image");
        CHECK(st.bad == 10, "5: bad %u", st.bad);
        CHECK(st.big_off == 0 && st.big_fails == 2, "5: big off %d fails %u", st.big_off, st.big_fails);
        CHECK(f->gone_calls == 1, "5: the drive asked %u times (once, after the first failed piece)", f->gone_calls);
        drop(f);
    }
    /* 6. the raw node fails twice in a row where the pass device reads: correct image; big given up (slower, never wrong) */
    {
        fake *f = make(2048, 10000);
        f->rawbad_lo = 600; f->rawbad_hi = 1100;
        dfr_cfg c = cfg(f, 1, 512, 32);
        dfr_stats st; memset(&st, 0, sizeof(st));
        const int rc = dfr_copy(&c, &st, 0, f->sectors);
        CHECK(rc == DFR_OK && image_ok(f, f->sectors, 0, 0) && st.bad == 0, "6: rc %d bad %u", rc, st.bad);
        CHECK(st.big_off == 1, "6: big off %d", st.big_off);
        drop(f);
    }
    /* 7. the disc goes: stopped, nothing written past where it went */
    {
        fake *f = make(2048, 40000);
        f->gone_at = 12345;
        dfr_cfg c = cfg(f, 1, 512, 32);
        dfr_stats st; memset(&st, 0, sizeof(st));
        const int rc = dfr_copy(&c, &st, 0, f->sectors);
        CHECK(rc == DFR_GONE, "7: rc %d", rc);
        CHECK(st.done <= 12345 && f->max_written <= 12345 && image_ok(f, (uint32_t)st.done, 0, 0), "7: done %llu written %llu", (unsigned long long)st.done,
              (unsigned long long)f->max_written);
        CHECK(st.bad == 0, "7: bad %u", st.bad);
        drop(f);
    }
    /* 8. too many unreadable sectors */
    {
        fake *f = make(2048, 4000);
        f->bad_lo = 100; f->bad_hi = 199;
        dfr_cfg c = cfg(f, 1, 512, 32);
        c.max_bad = 50;
        dfr_stats st; memset(&st, 0, sizeof(st));
        const int rc = dfr_copy(&c, &st, 0, f->sectors);
        CHECK(rc == DFR_TOO_BAD && st.bad > 50, "8: rc %d bad %u", rc, st.bad);
        drop(f);
    }
    /* 9. a write that fails */
    {
        fake *f = make(2048, 4000);
        f->fail_write_at = 2000;
        dfr_cfg c = cfg(f, 1, 512, 32);
        dfr_stats st; memset(&st, 0, sizeof(st));
        const int rc = dfr_copy(&c, &st, 0, f->sectors);
        CHECK(rc == DFR_WRITE && st.done <= 2000, "9: rc %d done %llu", rc, (unsigned long long)st.done);
        drop(f);
    }
    /* 10. tracks: several calls, the image goes on where the last one stopped (a .bin of every track) */
    {
        fake *f = make(2352, 9000);
        dfr_cfg c = cfg(f, 1, 27, 16);
        dfr_stats st; memset(&st, 0, sizeof(st));
        int rc = dfr_copy(&c, &st, 0, 4000);
        if (rc == DFR_OK) rc = dfr_copy(&c, &st, 4000, 150);
        if (rc == DFR_OK) rc = dfr_copy(&c, &st, 4150, 4850);
        CHECK(rc == DFR_OK && st.done == 9000 && image_ok(f, 9000, 0, 0), "10: rc %d done %llu", rc, (unsigned long long)st.done);
        drop(f);
    }
    /* 11. arguments: a buffer too small for the big read, no piece reader */
    {
        fake *f = make(2048, 100);
        dfr_cfg c = cfg(f, 1, 512, 32);
        c.buf_bytes = 64 * 1024;
        dfr_stats st; memset(&st, 0, sizeof(st));
        CHECK(dfr_copy(&c, &st, 0, 100) == DFR_ARGS, "11: small buffer");
        c = cfg(f, 1, 512, 32);
        c.piece = NULL;
        CHECK(dfr_copy(&c, &st, 0, 100) == DFR_ARGS, "11: no piece reader");
        CHECK(f->big_calls == 0 && f->piece_calls == 0, "11: nothing read");
        drop(f);
    }
    /* 12. which /dev/cdN is the USB drive's disc */
    {
        static uint8_t a[2048], b[2048];
        a[0] = 1; memcpy(a + 1, "CD001", 5); a[40] = 'X';
        memcpy(b, a, sizeof(b));
        CHECK(dfr_same_disc(2048LL * 1000, 1000, a, b) == 1, "12: same");
        CHECK(dfr_same_disc(2048LL * 1001, 1000, a, b) == 0, "12: other size");
        CHECK(dfr_same_disc(0, 0, a, b) == 0, "12: no media");
        b[40] = 'Y';
        CHECK(dfr_same_disc(2048LL * 1000, 1000, a, b) == 0, "12: other volume");
        memcpy(b, a, sizeof(b));
        a[1] = 'B'; b[1] = 'B';
        CHECK(dfr_same_disc(2048LL * 1000, 1000, a, b) == 0, "12: not an ISO 9660 descriptor");
    }
    /* 13. the CD read size fits the PS5 drive's 64 KiB */
    CHECK(27 * 2352 <= 65536 && 28 * 2352 > 65536, "13: 27 is the most raw CD sectors in 64 KiB");

    printf("%s: %d checks, %d failures\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
