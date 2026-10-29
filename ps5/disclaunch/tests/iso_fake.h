/* Made-up ISO 9660 images for the host tests of disc_compat.h (helper 171k, AI-assisted): sector 16 the primary volume descriptor, the root
 * folder at sector 20 (one or two sectors), the files' data from sector 24. Enough for dc_iso_kind and the payload's read_serial. */
#ifndef ISO_FAKE_H
#define ISO_FAKE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *d;
    uint32_t secs;
    uint32_t dir_lba, dir_secs, next_data;
    int dir_off;
} iso_fake;

static void iso_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* an image of `secs` sectors; iso = 0: no volume descriptor at all (a PS4/PS5 BD, a raw disc) */
static void iso_init(iso_fake *m, uint32_t secs, int iso, uint32_t dir_secs) {
    m->secs = secs;
    m->d = (uint8_t *)calloc(secs, 2048);
    m->dir_lba = 20;
    m->dir_secs = dir_secs;
    m->next_data = 24;
    m->dir_off = 0;
    if (!iso) return;
    uint8_t *pvd = m->d + 16 * 2048;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[156] = 34;
    iso_put32(pvd + 156 + 2, m->dir_lba);
    iso_put32(pvd + 156 + 10, dir_secs * 2048);
}

/* a root folder entry; a file's content (NULL for a folder) goes to the next free data sector */
static void iso_add(iso_fake *m, const char *name, const char *content) {
    const int nlen = (int)strlen(name);
    int rlen = 33 + nlen;
    if (rlen & 1) rlen++;
    if (m->dir_off % 2048 + rlen > 2048) m->dir_off = (m->dir_off / 2048 + 1) * 2048; /* a record never crosses a sector */
    uint8_t *r = m->d + (size_t)m->dir_lba * 2048 + m->dir_off;
    r[0] = (uint8_t)rlen;
    r[32] = (uint8_t)nlen;
    memcpy(r + 33, name, (size_t)nlen);
    if (content) {
        const uint32_t len = (uint32_t)strlen(content);
        if (m->next_data + (len + 2047) / 2048 + 1 > m->secs) abort(); /* (the image is too small for the test) */
        iso_put32(r + 2, m->next_data);
        iso_put32(r + 10, len);
        memcpy(m->d + (size_t)m->next_data * 2048, content, len);
        m->next_data += (len + 2047) / 2048 + 1;
    } else {
        if (m->next_data + 1 > m->secs) abort();
        iso_put32(r + 2, m->next_data++);
        iso_put32(r + 10, 2048);
        r[25] = 2;
    }
    m->dir_off += rlen;
}

static void iso_free(iso_fake *m) { free(m->d); m->d = NULL; }

#endif
