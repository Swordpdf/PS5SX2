/* PS5SX2 disc helper: a disc copied in sector order with big reads (helper 171h "disc speed", AI-assisted).
 *
 * What the PS5SX3 helper learned about the drives (its versions 9 to 14), used here for the copies the PS2 disc helper makes itself (a PS2 CD
 * through the PS5 drive's pass device, a disc in a USB drive):
 *   - one pass over the disc in sector order, as big a read as the device takes: /dev/cdN takes 1 MiB preads, while a READ command through
 *     the pass device is kept at 64 KiB (the PS5 drive refuses READ(12) above that);
 *   - a big read that fails is read again in small pieces, and a piece that still fails sector by sector (zeros for what stays unreadable),
 *     exactly as the old 64 KiB loop did;
 *   - nothing asks the drive whether it is ready while the copy reads: only a read that failed asks (`gone`);
 *   - a "big" size the drive refuses outright (the big read fails but every piece of it reads at once, twice in a row) is given up for the
 *     rest of the copy, so a drive that takes less never costs more than the old loop.
 * The file is pure (no system call): disc_launch.c gives it the readers and the writer, the host test (tests/fastread_test.c) fake ones.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DISC_FASTREAD_H
#define DISC_FASTREAD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* 1 when all `count` sectors from `lba` are in `buf` (count * sector_size bytes). */
typedef int (*dfr_read_fn)(void *ctx, uint32_t lba, uint32_t count, uint8_t *buf);
/* 1 when the `count` sectors were written at output sector `index` (sectors from the start of the image). */
typedef int (*dfr_write_fn)(void *ctx, uint64_t index, const uint8_t *buf, uint32_t count);
typedef int (*dfr_gone_fn)(void *ctx);                /* 1 when the disc is out (asked only after a read failed) */
typedef void (*dfr_tick_fn)(void *ctx, uint64_t done); /* after each write: sectors written so far */
typedef double (*dfr_clock_fn)(void);                 /* seconds, monotonic (for the time spent reading) */

typedef struct {
    uint32_t sector_size;                                     /* 2048, or 2352 for a CD read raw */
    dfr_read_fn big; uint32_t big_sectors; int big_tries;     /* the big read (NULL: none, pieces only = the old loop) */
    dfr_read_fn piece; uint32_t piece_sectors; int piece_tries;
    int one_tries;                                            /* a failed piece, sector by sector, with `piece` */
    uint32_t max_bad;                                         /* more unreadable sectors than this: the copy stops */
    dfr_write_fn write;
    dfr_gone_fn gone;                                         /* may be NULL */
    dfr_tick_fn tick;                                         /* may be NULL */
    dfr_clock_fn clock;                                       /* may be NULL */
    void *ctx;
    uint8_t *buf; size_t buf_bytes;                           /* at least max(big, piece) sectors */
} dfr_cfg;

typedef struct {
    uint64_t done;                      /* sectors written (over every dfr_copy call with these stats) */
    uint32_t bad;                       /* sectors written as zeros */
    uint64_t big_reads, piece_reads, one_reads;
    uint32_t big_fails;                 /* big reads that failed (read again in pieces) */
    uint32_t big_refused;               /* of those, the ones whose pieces all read at once */
    int big_off;                        /* the big read was given up (the drive refuses that size) */
    uint32_t largest;                   /* bytes of the largest read that worked */
    double read_s;                      /* seconds spent in reads */
} dfr_stats;

enum { DFR_OK = 0, DFR_GONE = 1, DFR_WRITE = 2, DFR_TOO_BAD = 3, DFR_ARGS = 4, DFR_STOPPED = 5 };
enum { DFR_REFUSED_OFF = 2 }; /* "big refused" this many times in a row: the big read is given up */

static inline const char *dfr_result_name(int rc) {
    switch (rc) {
    case DFR_OK: return "ok";
    case DFR_GONE: return "the disc went";
    case DFR_WRITE: return "a write failed";
    case DFR_TOO_BAD: return "too many unreadable sectors";
    case DFR_STOPPED: return "stopped";
    default: return "bad arguments";
    }
}

static inline int dfr_timed(const dfr_cfg *c, dfr_stats *st, dfr_read_fn fn, uint32_t lba, uint32_t n, uint8_t *buf) {
    const double t0 = c->clock ? c->clock() : 0.0;
    const int ok = fn(c->ctx, lba, n, buf);
    if (c->clock) st->read_s += c->clock() - t0;
    if (ok && n * c->sector_size > st->largest) st->largest = n * c->sector_size;
    return ok;
}

static inline int dfr_put(const dfr_cfg *c, dfr_stats *st, const uint8_t *buf, uint32_t n) {
    if (!c->write(c->ctx, st->done, buf, n)) return 0;
    st->done += n;
    if (c->tick) c->tick(c->ctx, st->done);
    return 1;
}

/* Copies sectors lba .. lba + count - 1 (written after what `st` already counts). DFR_OK when all of them were written. */
static inline int dfr_copy(const dfr_cfg *c, dfr_stats *st, uint32_t lba, uint32_t count) {
    if (!c || !st || !c->piece || !c->write || !c->buf || !c->sector_size || !c->piece_sectors || c->piece_tries < 1 || c->one_tries < 1) return DFR_ARGS;
    if ((size_t)c->piece_sectors * c->sector_size > c->buf_bytes) return DFR_ARGS;
    if (c->big && (!c->big_sectors || c->big_tries < 1 || (size_t)c->big_sectors * c->sector_size > c->buf_bytes)) return DFR_ARGS;
    const uint64_t end = (uint64_t)lba + count;
    int refused_run = 0;
    while (lba < end) {
        uint32_t window;
        if (c->big && !st->big_off) {
            const uint32_t n = end - lba < c->big_sectors ? (uint32_t)(end - lba) : c->big_sectors;
            int ok = 0;
            for (int t = 0; t < c->big_tries && !ok; t++) { ok = dfr_timed(c, st, c->big, lba, n, c->buf); st->big_reads++; }
            if (ok) {
                refused_run = 0;
                if (!dfr_put(c, st, c->buf, n)) return DFR_WRITE;
                lba += n;
                continue;
            }
            st->big_fails++;
            window = n; /* read again in pieces below */
        } else {
            window = end - lba < c->piece_sectors ? (uint32_t)(end - lba) : c->piece_sectors;
        }
        int all_first = 1; /* every piece of this window read at its first try */
        for (uint32_t k = 0; k < window;) {
            const uint32_t n = window - k < c->piece_sectors ? window - k : c->piece_sectors;
            const uint32_t at = lba + k;
            int ok = 0, t = 0;
            for (; t < c->piece_tries && !ok; t++) { ok = dfr_timed(c, st, c->piece, at, n, c->buf); st->piece_reads++; }
            if (!ok || t > 1) all_first = 0;
            if (!ok) {
                if (c->gone && c->gone(c->ctx)) return DFR_GONE;
                for (uint32_t i = 0; i < n; i++) {
                    uint8_t *s = c->buf + (size_t)i * c->sector_size;
                    int one = 0;
                    for (int u = 0; u < c->one_tries && !one; u++) { one = dfr_timed(c, st, c->piece, at + i, 1, s); st->one_reads++; }
                    if (!one) { memset(s, 0, c->sector_size); st->bad++; }
                }
                if (st->bad > c->max_bad) return DFR_TOO_BAD;
            }
            if (!dfr_put(c, st, c->buf, n)) return DFR_WRITE;
            k += n;
        }
        if (c->big && !st->big_off && window > c->piece_sectors) {
            /* the big read failed: a size refused (its pieces all read at once) or a bad place on the disc (they did not) */
            if (all_first) {
                st->big_refused++;
                if (++refused_run >= DFR_REFUSED_OFF) st->big_off = 1;
            } else {
                refused_run = 0;
            }
        }
        lba += window;
    }
    return DFR_OK;
}

/* Is a /dev/cdN this disc? Its size is the drive's capacity and its sector 16 (the volume descriptor) the one the pass device read. */
static inline int dfr_same_disc(int64_t node_bytes, uint32_t sectors, const uint8_t *s16_node, const uint8_t *s16_pass) {
    if (node_bytes <= 0 || !sectors || (uint64_t)node_bytes != (uint64_t)sectors * 2048u) return 0;
    if (!s16_node || !s16_pass) return 0;
    return memcmp(s16_node, s16_pass, 2048) == 0 && s16_node[0] == 1 && memcmp(s16_node + 1, "CD001", 5) == 0;
}

#endif
