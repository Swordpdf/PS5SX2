/* PS5SX2 disc-auto daemon (live-12, AI-assisted): CRC-32 (IEEE 802.3, as zip and Redump), MD5 (RFC 1321) and SHA-1
 * (FIPS 180-4) in one pass over a dump. Checked against Python's zlib/hashlib on the host (tests/hash_test.c).
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "disc_hash.h"

#include <stdio.h>
#include <string.h>

static uint32_t g_crc_table[256];

static void crc_table(void) {
    if (g_crc_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
}

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

/* MD5 ------------------------------------------------------------------ */
static const uint32_t K_MD5[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static const int R_MD5[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9, 14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(uint32_t s[4], const uint8_t *p) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)p[i * 4] | (uint32_t)p[i * 4 + 1] << 8 | (uint32_t)p[i * 4 + 2] << 16 | (uint32_t)p[i * 4 + 3] << 24;
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        const uint32_t t = d;
        d = c;
        c = b;
        b = b + rol(a + f + K_MD5[i] + m[g], R_MD5[i]);
        a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

/* SHA-1 ---------------------------------------------------------------- */
static void sha1_block(uint32_t s[5], const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e;
}

void disc_hash_init(disc_hash_ctx *c) {
    crc_table();
    memset(c, 0, sizeof(*c));
    c->crc = 0xFFFFFFFFu;
    c->md5[0] = 0x67452301; c->md5[1] = 0xefcdab89; c->md5[2] = 0x98badcfe; c->md5[3] = 0x10325476;
    c->sha1[0] = 0x67452301; c->sha1[1] = 0xEFCDAB89; c->sha1[2] = 0x98BADCFE; c->sha1[3] = 0x10325476; c->sha1[4] = 0xC3D2E1F0;
}

void disc_hash_update(disc_hash_ctx *c, const void *data, size_t n) {
    const uint8_t *p = data;
    uint32_t crc = c->crc;
    for (size_t i = 0; i < n; i++) crc = g_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    c->crc = crc;
    c->bytes += n;
    /* MD5 and SHA-1 share the 64-byte block size, so both buffers fill alike. */
    size_t i = 0;
    if (c->md5_n) {
        while (i < n && c->md5_n < 64) { c->md5_buf[c->md5_n] = c->sha1_buf[c->sha1_n] = p[i++]; c->md5_n++; c->sha1_n++; }
        if (c->md5_n == 64) { md5_block(c->md5, c->md5_buf); sha1_block(c->sha1, c->sha1_buf); c->md5_n = c->sha1_n = 0; }
    }
    for (; i + 64 <= n; i += 64) { md5_block(c->md5, p + i); sha1_block(c->sha1, p + i); }
    while (i < n) { c->md5_buf[c->md5_n++] = c->sha1_buf[c->sha1_n++] = p[i++]; }
}

void disc_hash_final(disc_hash_ctx *c, char crc_hex[9], char md5_hex[33], char sha1_hex[41]) {
    const uint64_t bits = c->bytes * 8;
    uint8_t pad[72];
    size_t padn = (c->md5_n < 56) ? 56 - c->md5_n : 120 - c->md5_n;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    /* MD5: little-endian length; SHA-1: big-endian. Feed the padding to each by hand (the CRC must not see it). */
    uint8_t md5_tail[8], sha_tail[8];
    for (int i = 0; i < 8; i++) { md5_tail[i] = (uint8_t)(bits >> (8 * i)); sha_tail[i] = (uint8_t)(bits >> (56 - 8 * i)); }
    for (int pass = 0; pass < 2; pass++) {
        uint8_t *buf = pass ? c->sha1_buf : c->md5_buf;
        unsigned *nn = pass ? &c->sha1_n : &c->md5_n;
        const uint8_t *tail = pass ? sha_tail : md5_tail;
        for (size_t k = 0; k < padn + 8; k++) {
            buf[(*nn)++] = k < padn ? pad[k] : tail[k - padn];
            if (*nn == 64) {
                if (pass) sha1_block(c->sha1, buf);
                else md5_block(c->md5, buf);
                *nn = 0;
            }
        }
    }
    snprintf(crc_hex, 9, "%08x", c->crc ^ 0xFFFFFFFFu);
    for (int i = 0; i < 16; i++) snprintf(md5_hex + 2 * i, 3, "%02x", (c->md5[i / 4] >> (8 * (i % 4))) & 0xFF);
    for (int i = 0; i < 20; i++) snprintf(sha1_hex + 2 * i, 3, "%02x", (c->sha1[i / 4] >> (24 - 8 * (i % 4))) & 0xFF);
}
