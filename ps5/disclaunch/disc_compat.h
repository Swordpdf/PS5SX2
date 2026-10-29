/* disc_compat.h (PS5SX2 helper 171k, 2026-10-29, AI-assisted): PS5SX2's disc helper next to the PS5SX3 helper.
 *
 * Both helpers may be loaded on one console, in any order. The PS5SX3 helper (rpcs3/ps5/helper, versions 13-26) leaves every drive alone
 * while this helper's disc-auto-alive.txt is fresh (it finds a PS3 disc only by its files on the PS5's mount, and copies it through that
 * mount at ~5 MB/s), and since its version 21 it KILLS this helper (SIGKILL, it is "payload.elf") the moment it sees a PS3 disc, so that
 * the drive is its own. This header is the rule set that makes the kill unnecessary:
 *
 *   1. A PS2 disc is PS5SX2's, every other disc is not. A drive is classified once per disc, by the cheapest thing that tells:
 *      - the PS5's mount of the drive shows a PS3 game's files (PS3_DISC.SFB or PS3_GAME/PARAM.SFO): a PS3 disc, the drive is never opened
 *        at all (looked at before every open, so a PS3 disc already mounted is not read by us even once);
 *      - otherwise the disc's ISO 9660 root, read once after the PS2 serial was not found: PS3_DISC.SFB / PS3_GAME -> PS3, SYSTEM.CNF
 *        without BOOT2 -> PS1, SYSTEM.CNF with BOOT2 -> PS2; anything else stays "looking" until the serial tries end (5 s), then "other".
 *   2. A drive whose disc is not a PS2 disc is YIELDED: the tile thread, the disc watcher and the DiscSpeed8 pin leave it alone until the
 *      disc goes. A yield held by the mount ends when the mount's PS3 files go; one found by the sectors is checked by one cheap look
 *      (open + DIOCGMEDIASIZE, no read; TEST UNIT READY + READ CAPACITY for a USB drive) every DC_LOOK_S. After a yielded disc goes the
 *      drive is left alone DC_GRACE_S more (the PS5SX3 helper rests 5 s after any disc out; an eject is when two helpers touching the
 *      drive is least wanted: the TB13.2 kernel panic on 4.03).
 *   3. What each drive is, is published every second (and at each change) in /data/PCSX2/logs/ps5sx2-helper.txt, next to pid/time/rev:
 *        compat=1                      this helper yields non-PS2 discs: there is no need to stop it
 *        cd0=empty|looking|ps2|yield   (cd1..cd3 the same; "yield" also during the grace)
 *        usb=none|empty|looking|ps2|yield   the USB optical drive the watcher reads through its pass device
 *      The PS5SX3 helper (its side: Claude outputs/ps5sx3-helper-compat.patch) reads it: compat=1 -> never stopped; no "ps2" on any line
 *      and at least one "yield" -> the drives are its own to ask (its "PS5SX2's disc helper runs" mode off); otherwise as before.
 *   4. The system's "Unsupported Disc" tile: hidden here only while a PS2 disc is in (as before), put back at a stand-alone payload's stop
 *      only when a PS2 disc is in (it used to put it back for any disc, a PS3 one too, against the PS5SX3 helper's hide).
 *   5. Title IDs: PS5SX2 = PPSA99203, 99230 (Reload), 99231-38 (disc tiles), 99251-60 (game tiles), 99299 (ShadowMount+ busy probe);
 *      PS5SX3 = PPSA99303, 99331-34 (disc tiles), 99339 (its probe), 99340-89 (game tiles). A removed tile's leftover concept rows are
 *      deleted only when no title outside PS5SX2's own IDs uses that concept (dc_ps5sx2_tid; disc_compat_db.h).
 *
 * Everything here is pure (no I/O of its own: the file system and the sectors come through callbacks), so the host test
 * (tests/compat_test.c) runs it as it is.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DISC_COMPAT_H
#define DISC_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define DC_NCD 4        /* /dev/cd0 .. /dev/cd3: the tile thread's drives (cd0 = the PS5's own) */
#define DC_USB 4        /* the USB optical drive the watcher reads through its pass device */
#define DC_NDRIVES 5
#define DC_LOOK_S 5.0   /* a yielded drive without a PS3 mount to watch: one cheap look per 5 s */
#define DC_GRACE_S 5.0  /* a yielded disc gone: the drive left alone 5 s more */

/* what a disc is */
enum { DC_KIND_NONE = 0, DC_KIND_PS1 = 1, DC_KIND_PS2 = 2, DC_KIND_PS3 = 3, DC_KIND_UNKNOWN = 8, DC_KIND_OTHER = 9 };
/* what this helper does with a drive (DC_NONE: no such drive, the USB line only) */
enum { DC_EMPTY = 0, DC_LOOKING = 1, DC_PS2 = 2, DC_YIELD = 3, DC_NONE = 4 };

static inline const char *dc_claim_name(int c) {
    switch (c) {
    case DC_EMPTY: return "empty";
    case DC_LOOKING: return "looking";
    case DC_PS2: return "ps2";
    case DC_YIELD: return "yield";
    default: return "none";
    }
}

static inline const char *dc_kind_name(int k) {
    switch (k) {
    case DC_KIND_NONE: return "no disc";
    case DC_KIND_PS1: return "a PS1 disc";
    case DC_KIND_PS2: return "a PS2 disc";
    case DC_KIND_PS3: return "a PS3 disc";
    case DC_KIND_UNKNOWN: return "a disc not known yet";
    default: return "not a PlayStation 1/2/3 disc";
    }
}

/* PS5SX2's own title IDs: PPSA99203 (the app), 99230-99238 (the Reload and disc tiles), 99251-99260 (game tiles), 99299 (the busy probe). */
static inline int dc_ps5sx2_tid(const char *t) {
    if (!t || strncmp(t, "PPSA992", 7) != 0 || t[7] < '0' || t[7] > '9' || t[8] < '0' || t[8] > '9') return 0;
    if (t[9] != '\0' && t[9] != '_') return 0;
    const int n = (t[7] - '0') * 10 + (t[8] - '0');
    return n == 3 || (n >= 30 && n <= 38) || (n >= 51 && n <= 60) || n == 99;
}

/* PS5SX3's (for the log): PPSA99303, 99331-99334, 99339, 99340-99389. */
static inline int dc_ps5sx3_tid(const char *t) {
    if (!t || strncmp(t, "PPSA993", 7) != 0 || t[7] < '0' || t[7] > '9' || t[8] < '0' || t[8] > '9') return 0;
    if (t[9] != '\0' && t[9] != '_') return 0;
    const int n = (t[7] - '0') * 10 + (t[8] - '0');
    return n == 3 || (n >= 31 && n <= 34) || (n >= 39 && n <= 89);
}

/* ---------------------------------------------------------------- the disc's kind from its ISO 9660 root */
typedef int (*dc_reader)(void *ctx, uint32_t lba, uint8_t *out); /* one 2048-byte sector into out: 1, 0 when it does not read */

static inline uint32_t dc_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* an ISO 9660 name ("SYSTEM.CNF;1", "PS3_GAME") is `want`, case aside */
static inline int dc_name_is(const uint8_t *name, int nlen, const char *want) {
    const int wl = (int)strlen(want);
    return nlen >= wl && strncasecmp((const char *)name, want, (size_t)wl) == 0 && (nlen == wl || name[wl] == ';');
}

/* DC_KIND_PS2 / PS1 / PS3 by the root folder's names; DC_KIND_OTHER for a readable disc with neither (or no ISO 9660 at all: a PS4/PS5
 * BD, a video disc); DC_KIND_UNKNOWN when sector 16 does not read (a disc still spinning up: asked again later). Reads sector 16, at most
 * 64 sectors of the root folder and SYSTEM.CNF's first two. */
static inline int dc_iso_kind(dc_reader rd, void *ctx) {
    uint8_t pvd[2048];
    if (!rd(ctx, 16, pvd)) return DC_KIND_UNKNOWN;
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return DC_KIND_OTHER;
    const uint32_t root_lba = dc_le32(pvd + 156 + 2), root_size = dc_le32(pvd + 156 + 10);
    const uint32_t secs = (root_size + 2047) / 2048 < 64 ? (root_size + 2047) / 2048 : 64;
    int kind = DC_KIND_OTHER;
    uint8_t dir[2048];
    for (uint32_t ds = 0; ds < secs; ds++) {
        if (!rd(ctx, root_lba + ds, dir)) continue;
        for (int off = 0; off < 2048;) {
            const int rlen = dir[off];
            if (rlen == 0 || off + rlen > 2048 || rlen < 34) break;
            const int nlen = dir[off + 32];
            const uint8_t *name = dir + off + 33;
            if (33 + nlen > rlen) break;
            if (dc_name_is(name, nlen, "PS3_DISC.SFB") || dc_name_is(name, nlen, "PS3_GAME")) return DC_KIND_PS3;
            if (dc_name_is(name, nlen, "SYSTEM.CNF")) {
                const uint32_t clba = dc_le32(dir + off + 2);
                uint32_t csz = dc_le32(dir + off + 10);
                if (csz > 4096) csz = 4096;
                char cnf[4097];
                memset(cnf, 0, sizeof(cnf));
                uint8_t sec[2048];
                size_t got = 0;
                for (uint32_t k = 0; k < (csz + 2047) / 2048 && got < csz; k++) {
                    if (!rd(ctx, clba + k, sec)) break;
                    const size_t take = csz - got < 2048 ? csz - got : 2048;
                    memcpy(cnf + got, sec, take);
                    got += take;
                }
                if (strstr(cnf, "BOOT2")) return DC_KIND_PS2;
                if (strstr(cnf, "BOOT")) kind = DC_KIND_PS1; /* (a PS3 marker later in the folder still wins) */
            }
            off += rlen;
        }
    }
    return kind;
}

/* ---------------------------------------------------------------- the PS5's mount of a drive */
typedef struct {
    char from[96]; /* "/dev/cd0" */
    char on[160];  /* "/mnt/disc" */
} dc_mnt;

typedef struct {
    int (*exists)(void *u, const char *path);       /* stat() == 0 */
    int (*mounts)(void *u, dc_mnt *out, int max);   /* the mount table (getmntinfo), how many */
    void *u;
} dc_fs;

static inline int dc_root_ps3(const dc_fs *fs, const char *root) {
    char p[224];
    snprintf(p, sizeof(p), "%s/PS3_DISC.SFB", root);
    if (fs->exists(fs->u, p)) return 1;
    snprintf(p, sizeof(p), "%s/PS3_GAME/PARAM.SFO", root);
    return fs->exists(fs->u, p);
}

/* 1 when the PS5's mount of /dev/cd<drive> shows a PS3 game's files (`where` = the mount point). The PS5's own drive (0) is also looked
 * for at the fixed places the PS5SX3 helper knows (/mnt/disc first), unless the mount table says that place is another drive's. */
static inline int dc_mount_ps3(const dc_fs *fs, int drive, char *where, size_t max) {
    static const char *const fixed[] = {"/mnt/disc", "/mnt/cd0", "/mnt/bd", "/mnt/dvd", "/mnt/cdrom", "/disc"};
    dc_mnt m[48];
    const int n = fs->mounts ? fs->mounts(fs->u, m, 48) : 0;
    char dev[24];
    snprintf(dev, sizeof(dev), "/dev/cd%d", drive);
    if (where && max) where[0] = '\0';
    if (drive == 0) {
        for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
            int other = 0;
            for (int j = 0; j < n; j++)
                if (!strcmp(m[j].on, fixed[i]) && !strncmp(m[j].from, "/dev/cd", 7) && strcmp(m[j].from, dev) != 0) other = 1;
            if (other || !dc_root_ps3(fs, fixed[i])) continue;
            if (where && max) snprintf(where, max, "%s", fixed[i]);
            return 1;
        }
    }
    for (int j = 0; j < n; j++) {
        if (strcmp(m[j].from, dev) != 0 || !dc_root_ps3(fs, m[j].on)) continue;
        if (where && max) snprintf(where, max, "%s", m[j].on);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- a drive's claim */
typedef struct {
    int claim;          /* DC_EMPTY / DC_LOOKING / DC_PS2 / DC_YIELD */
    int kind;           /* the disc's kind when known (why the yield) */
    int by_mount;       /* the yield is held by the PS5's mount showing PS3 files */
    long long media;    /* the disc's size when yielded by its sectors (0: not known) */
    double next_look;   /* a yield by the sectors: the next cheap look */
    double grace_until; /* a yielded disc went: nothing opened until then */
} dc_drive;

/* What the other helper is told: "yield" also during the grace. */
static inline int dc_published(const dc_drive *d, double now) {
    if (d->claim == DC_YIELD || now < d->grace_until) return DC_YIELD;
    return d->claim;
}

/* Before the drive is opened: 1 when it may be opened now. `mount_ps3`: the PS5's mount of this drive shows PS3 files at this moment.
 * *changed is set to 1 when the claim changed (the caller logs and publishes it). A yield by the sectors lets one cheap look through
 * every DC_LOOK_S (the caller then calls dc_after_look with what it saw). */
static inline int dc_gate(dc_drive *d, int mount_ps3, double now, int *changed) {
    if (mount_ps3) {
        if (d->claim != DC_YIELD || !d->by_mount) {
            d->claim = DC_YIELD;
            d->kind = DC_KIND_PS3;
            d->by_mount = 1;
            if (changed) *changed = 1;
        }
        return 0;
    }
    if (d->claim == DC_YIELD) {
        if (d->by_mount) { /* the mount's PS3 files went: the disc is out (or unmounted: a look after the grace tells) */
            d->claim = DC_EMPTY;
            d->kind = DC_KIND_NONE;
            d->by_mount = 0;
            d->media = 0;
            d->grace_until = now + DC_GRACE_S;
            if (changed) *changed = 1;
            return 0;
        }
        if (now >= d->next_look || d->next_look - now > DC_LOOK_S) {
            d->next_look = now + DC_LOOK_S;
            return 1;
        }
        return 0;
    }
    if (now < d->grace_until && d->grace_until - now <= DC_GRACE_S) return 0;
    return 1;
}

/* After a cheap look at a drive yielded by its sectors: `media` is the size the drive gives now (0: no disc). 1 when the yield ended (the
 * disc went: the grace starts; another disc: no grace, the caller goes on with it as a new disc), 0 when it holds (the same disc). */
static inline int dc_after_look(dc_drive *d, long long media, double now) {
    if (d->claim != DC_YIELD || d->by_mount) return 0;
    if (media > 0 && (d->media <= 0 || media == d->media)) return 0;
    d->claim = DC_EMPTY;
    d->kind = DC_KIND_NONE;
    d->media = 0;
    if (media <= 0) d->grace_until = now + DC_GRACE_S;
    return 1;
}

/* What a look found: `kind` of the disc in (DC_KIND_NONE: no disc; DC_KIND_UNKNOWN: a disc, not classified yet). 1 when the claim changed. */
static inline int dc_note(dc_drive *d, int kind, long long media, double now) {
    const int was = d->claim;
    if (d->claim == DC_YIELD && d->by_mount) return 0; /* (the mount rules while it shows the PS3 files) */
    switch (kind) {
    case DC_KIND_NONE:
        if (d->claim != DC_YIELD) d->claim = DC_EMPTY;
        break;
    case DC_KIND_UNKNOWN:
        if (d->claim != DC_YIELD) d->claim = DC_LOOKING;
        break;
    case DC_KIND_PS2:
        d->claim = DC_PS2;
        d->kind = kind;
        break;
    default: /* PS1, PS3, other: not ours */
        if (d->claim != DC_YIELD) {
            d->claim = DC_YIELD;
            d->kind = kind;
            d->by_mount = 0;
            d->media = media;
            d->next_look = now + DC_LOOK_S;
        }
        break;
    }
    return d->claim != was;
}

/* ---------------------------------------------------------------- the status file's lines */
/* "compat=1\ncd0=...\ncd1=...\ncd2=...\ncd3=...\nusb=...\n" from the published claims (claims[DC_NDRIVES]); the length written. */
static inline int dc_status_lines(const int *claims, char *out, size_t max) {
    return snprintf(out, max, "compat=1\ncd0=%s\ncd1=%s\ncd2=%s\ncd3=%s\nusb=%s\n", dc_claim_name(claims[0]), dc_claim_name(claims[1]),
                    dc_claim_name(claims[2]), dc_claim_name(claims[3]), dc_claim_name(claims[DC_USB]));
}

/* What the PS5SX3 helper reads from the status file (the same rule its patch implements): 1 = the drives are free to ask (no drive holds a
 * PS2 disc and one is yielded), 0 = this helper holds a drive or yields none (its "PS5SX2's disc helper runs" mode), -1 = no compat line
 * (an older helper: 171j and before). */
static inline int dc_status_yields(const char *text) {
    if (!text || !strstr(text, "compat=")) return -1;
    int yield = 0, ps2 = 0;
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if ((n > 4 && (!strncmp(p, "cd", 2) || !strncmp(p, "usb=", 4)))) {
            const char *eq = memchr(p, '=', n);
            if (eq) {
                const size_t vl = n - (size_t)(eq + 1 - p);
                if (vl == 5 && !strncmp(eq + 1, "yield", 5)) yield = 1;
                if (vl == 3 && !strncmp(eq + 1, "ps2", 3)) ps2 = 1;
            }
        }
        p += n;
        if (*p == '\n') p++;
    }
    return !ps2 && yield;
}

#endif
