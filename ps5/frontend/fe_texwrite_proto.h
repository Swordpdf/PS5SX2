/* PS5 port frontend: the texture pack writer's protocol (2026-10-29, AI-assisted), shared by the app (fe_texwrite.cpp) and the payload that does the
 * writing (ps5/coreorbis/tex-writer/tex_writer.c). C, so the payload can include it.
 *
 * Why: the console lets a title write about 1.3 GiB at full speed and then about 2 MiB a second (the allowance refills only while the title does
 * not write); another process's writes are not held back (PS5SX3's helper writes its package installs for that reason). An HD pack is written
 * twice by the title (the download, then replacements.pak, 1.4 to 3.7 times the download for the 7z packs), so every pack of a gigabyte or more
 * ended at the flat 2 MB/s the logs showed. The app starts this payload through the console's ELF loader (127.0.0.1:9021) and hands it the
 * bytes over the loopback; the payload, a process of its own, writes them.
 *
 * The connection: the APP listens (127.0.0.1, a port the system picks) and the payload connects to it, so no fixed port is taken and only the
 * process the app started is served. The request written into the payload names the port, a random 16-byte token and the root folder; the
 * payload's first message is a hello with that token, and the app closes any connection whose hello is wrong.
 *
 * Messages (little-endian, both ends x86-64): a request is a tw_request and `len` bytes after it; every request gets one tw_reply and `len`
 * bytes after that. One request at a time.
 *   TW_OPEN   data = a path relative to the root; flags TW_CREATE | TW_TRUNC -> handle. A regular file only, opened for writing; one it creates
 *             gets mode 0666. Every folder on the way must exist already (the app makes them) and is opened with O_NOFOLLOW.
 *   TW_WRITE  handle, offset, data (1 .. TW_MAX_DATA bytes) -> value = the bytes written (all of them, or err set).
 *   TW_CLOSE  handle -> err = close()'s error; the handle is gone either way.
 *   TW_FSYNC  handle.
 *   TW_MARK   data = a path relative to the root, then '\0', then up to 64 bytes: the payload writes those bytes into that new file (O_EXCL) and
 *             closes it. The app reads the file through its own view to learn that both see the same folder, then deletes it.
 *   TW_BYE    the payload closes the connection and ends.
 * err is 0 or the errno of the failure (a path refused: EPERM; an unknown handle: EBADF; a bad op: EINVAL).
 * Paths: at most TW_MAX_PATH bytes, no NUL, no '/' at either end, no empty, "." or ".." part.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FE_TEXWRITE_PROTO_H
#define FE_TEXWRITE_PROTO_H

#include <stdint.h>

#define TW_REQUEST_MARKER "PS5SX2-TEXWRITE-REQUEST-V1"
#define TW_REQUEST_ROOM   1024 /* g_request's size in tex_writer.c */

#define TW_HELLO_MAGIC 0x31485754u /* "TWH1" */
#define TW_REQ_MAGIC   0x31515754u /* "TWQ1" */
#define TW_REP_MAGIC   0x31525754u /* "TWR1" */
#define TW_VERSION     1u
#define TW_MAX_DATA    (8u << 20)
#define TW_MAX_PATH    1024u
#define TW_MAX_FILES   16
#define TW_IDLE_MS     (5u * 60u * 1000u) /* the payload ends when the app has sent nothing for this long */

enum
{
	TW_OPEN = 1,
	TW_WRITE = 2,
	TW_CLOSE = 3,
	TW_FSYNC = 4,
	TW_MARK = 5,
	TW_BYE = 6,
};

enum
{
	TW_CREATE = 1,
	TW_TRUNC = 2,
};

typedef struct tw_hello
{
	uint32_t magic;    /* TW_HELLO_MAGIC */
	uint32_t version;  /* TW_VERSION */
	uint8_t token[16]; /* the request's token */
	uint32_t pid;      /* the payload's process, for the app's log */
	uint32_t max_data; /* TW_MAX_DATA */
} tw_hello;            /* 32 bytes */

typedef struct tw_request
{
	uint32_t magic; /* TW_REQ_MAGIC */
	uint16_t op;
	uint16_t flags;
	uint32_t handle;
	uint32_t len; /* bytes after the header */
	uint64_t offset;
} tw_request; /* 24 bytes */

typedef struct tw_reply
{
	uint32_t magic; /* TW_REP_MAGIC */
	int32_t err;    /* 0, or an errno */
	uint32_t handle;
	uint32_t len; /* bytes after the header (none so far) */
	uint64_t value;
} tw_reply; /* 24 bytes */

#endif
