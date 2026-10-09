/* PS5SX2 disc-auto daemon (live-12, AI-assisted): CRC-32, MD5 and SHA-1 of a dump, the three checksums Redump lists
 * for each image, so a copy can be checked against redump.org's entry for its disc.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PS5SX2_DISC_HASH_H
#define PS5SX2_DISC_HASH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t crc;
    uint64_t bytes;
    uint32_t md5[4];
    uint32_t sha1[5];
    uint8_t md5_buf[64], sha1_buf[64];
    unsigned md5_n, sha1_n;
} disc_hash_ctx;

void disc_hash_init(disc_hash_ctx *c);
void disc_hash_update(disc_hash_ctx *c, const void *data, size_t n);
/* Lower-case hex: crc 8 characters, md5 32, sha1 40 (each buffer one longer for the NUL). */
void disc_hash_final(disc_hash_ctx *c, char crc_hex[9], char md5_hex[33], char sha1_hex[41]);

#endif
