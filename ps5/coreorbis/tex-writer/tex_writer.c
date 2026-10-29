// PS5SX2 (2026-10-29, AI-assisted): the texture pack writer, a payload that writes the HD texture packs' files for the app.
//
// The console lets a title write ~1.3 GiB at full speed and then ~2 MiB a second; another process's writes are not held back. The app's
// texture packs (a download, then its replacements.pak) are written twice, so a pack of a gigabyte or more crawled at the flat 2 MB/s the
// session reports showed. ProsperoTexWriter.cpp embeds this program, writes a request over g_request's marker and sends the whole file to the
// console's ELF loader on 127.0.0.1:9021, which runs it as a process of its own; this process connects back to the app (the port in the
// request), says the request's token, and then opens and writes the files the app names, under the request's root folder only. The protocol is
// ps5/frontend/fe_texwrite_proto.h. It ends when the app closes the connection, says TW_BYE, or sends nothing for TW_IDLE_MS.
//
// Request (the app writes it over the marker, NUL-terminated):
//   PS5SX2-TEXWRITE-REQUEST-V1\n port=<the app's port>\n token=<32 hex digits>\n root=<absolute folder>\n log=<file, or empty>\n
//
// Built with the ps5-payload-dev SDK by Makefile.vk and embedded with .incbin; the host test (test-tex-writer.sh) builds it for the PC.
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_texwrite_proto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// The app finds the marker and writes the request over it; the array lies in .data with its whole size in the file.
__attribute__((used)) volatile char g_request[TW_REQUEST_ROOM] = TW_REQUEST_MARKER;

static char g_log_path[600];
static int g_root = -1;
static int g_files[TW_MAX_FILES];

static void note(const char* fmt, ...)
{
	if (!g_log_path[0])
		return;
	FILE* f = fopen(g_log_path, "a");
	if (!f)
		return;
	char line[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fprintf(f, "%ld %s\n", (long)time(NULL), line);
	fclose(f);
}

// "key=value" from the request, into `out` (empty when missing).
static void field(const char* req, const char* key, char* out, size_t room)
{
	out[0] = '\0';
	const size_t klen = strlen(key);
	const char* p = req;
	while (p && *p)
	{
		if (strncmp(p, key, klen) == 0 && p[klen] == '=')
		{
			p += klen + 1;
			size_t n = 0;
			while (p[n] && p[n] != '\n' && n + 1 < room)
				n++;
			memcpy(out, p, n);
			out[n] = '\0';
			return;
		}
		p = strchr(p, '\n');
		if (p)
			p++;
	}
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

// Exactly n bytes, waiting at most `idle_ms` for each piece (0: no limit). 1 done, 0 the app closed, -1 an error or the wait ran out.
static int read_all(int fd, void* buf, size_t n, unsigned idle_ms)
{
	uint8_t* p = (uint8_t*)buf;
	while (n > 0)
	{
		if (idle_ms)
		{
			struct pollfd q = {fd, POLLIN, 0};
			const int r = poll(&q, 1, (int)idle_ms);
			if (r < 0 && errno == EINTR)
				continue;
			if (r != 1)
				return -1;
		}
		const ssize_t got = recv(fd, p, n, 0);
		if (got < 0 && errno == EINTR)
			continue;
		if (got == 0)
			return 0;
		if (got < 0)
			return -1;
		p += got;
		n -= (size_t)got;
	}
	return 1;
}

static int send_all(int fd, const void* buf, size_t n)
{
	const uint8_t* p = (const uint8_t*)buf;
	while (n > 0)
	{
		const ssize_t put = send(fd, p, n, 0);
		if (put < 0 && errno == EINTR)
			continue;
		if (put <= 0)
			return -1;
		p += put;
		n -= (size_t)put;
	}
	return 0;
}

// A path the protocol allows: relative, at most TW_MAX_PATH bytes, no empty, "." or ".." part.
static int path_ok(const char* path, size_t len)
{
	if (len == 0 || len > TW_MAX_PATH || memchr(path, '\0', len) || path[0] == '/' || path[len - 1] == '/')
		return 0;
	size_t start = 0;
	for (size_t i = 0; i <= len; i++)
	{
		if (i == len || path[i] == '/')
		{
			const size_t part = i - start;
			if (part == 0 || (part == 1 && path[start] == '.') || (part == 2 && path[start] == '.' && path[start + 1] == '.'))
				return 0;
			start = i + 1;
		}
	}
	return 1;
}

// Opens `rel` under the root: each folder on the way with O_NOFOLLOW from the one before (a symbolic link anywhere is refused, never followed
// out of the root), then the file. -1 with errno set.
static int open_under_root(const char* rel, int flags)
{
	char path[TW_MAX_PATH + 1];
	strcpy(path, rel);
	int dir = g_root;
	char* part = path;
	for (;;)
	{
		char* slash = strchr(part, '/');
		if (!slash)
			break;
		*slash = '\0';
		const int next = openat(dir, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		const int e = errno;
		if (dir != g_root)
			close(dir);
		if (next < 0)
		{
			errno = e;
			return -1;
		}
		dir = next;
		part = slash + 1;
	}
	const int fd = openat(dir, part, flags | O_NOFOLLOW, 0666);
	const int e = errno;
	if (dir != g_root)
		close(dir);
	if (fd < 0)
	{
		errno = e;
		return -1;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
	{
		close(fd);
		errno = EINVAL;
		return -1;
	}
	return fd;
}

static int connect_back(int port)
{
	for (int attempt = 0; attempt < 30; attempt++)
	{
		const int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
			return -1;
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons((uint16_t)port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
			return fd;
		close(fd);
		usleep(100000);
	}
	return -1;
}

int main(void)
{
	char req[TW_REQUEST_ROOM];
	for (size_t i = 0; i < sizeof(req); i++)
		req[i] = g_request[i];
	req[sizeof(req) - 1] = '\0';
	if (strncmp(req, TW_REQUEST_MARKER "\n", sizeof(TW_REQUEST_MARKER)) != 0)
		return 1; // sent without a request: nothing to do
	char port_s[16], token_s[64], root[TW_MAX_PATH + 1];
	field(req, "port", port_s, sizeof(port_s));
	field(req, "token", token_s, sizeof(token_s));
	field(req, "root", root, sizeof(root));
	field(req, "log", g_log_path, sizeof(g_log_path));
	const int port = atoi(port_s);
	uint8_t token[16];
	int good = port > 0 && port < 65536 && strlen(token_s) == 32 && root[0] == '/';
	for (int i = 0; good && i < 16; i++)
	{
		const int hi = hexval(token_s[2 * i]), lo = hexval(token_s[2 * i + 1]);
		good = hi >= 0 && lo >= 0;
		token[i] = (uint8_t)(hi * 16 + lo);
	}
	if (!good)
	{
		note("a request without a port, token or root");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	g_root = open(root, O_RDONLY | O_DIRECTORY);
	if (g_root < 0)
	{
		note("can't open %s: errno %d", root, errno);
		return 3;
	}
	const int s = connect_back(port);
	if (s < 0)
	{
		note("can't reach the app on 127.0.0.1:%d: errno %d", port, errno);
		return 4;
	}
	const int one = 1;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	const int sockbuf = 1 << 20;
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, &sockbuf, sizeof(sockbuf));
	tw_hello hello;
	memset(&hello, 0, sizeof(hello));
	hello.magic = TW_HELLO_MAGIC;
	hello.version = TW_VERSION;
	memcpy(hello.token, token, sizeof(token));
	hello.pid = (uint32_t)getpid();
	hello.max_data = TW_MAX_DATA;
	if (send_all(s, &hello, sizeof(hello)) != 0)
		return 5;
	uint8_t* data = (uint8_t*)malloc(TW_MAX_DATA + 1);
	if (!data)
	{
		note("no memory for the %u-byte buffer", TW_MAX_DATA);
		return 6;
	}
	for (int i = 0; i < TW_MAX_FILES; i++)
		g_files[i] = -1;
	note("serving the app on 127.0.0.1:%d, root %s", port, root);
	uint64_t written = 0, writes = 0, opened = 0;
	const time_t t0 = time(NULL);
	const char* why = "the app closed the connection";
	for (;;)
	{
		tw_request q;
		const int r = read_all(s, &q, sizeof(q), TW_IDLE_MS);
		if (r <= 0)
		{
			if (r < 0)
				why = "nothing from the app for a while (or the connection broke)";
			break;
		}
		const uint32_t limit = q.op == TW_WRITE ? TW_MAX_DATA : TW_MAX_PATH + 1 + 64;
		if (q.magic != TW_REQ_MAGIC || q.len > limit)
		{
			why = "a request this payload does not know";
			break;
		}
		if (q.len && read_all(s, data, q.len, 30000) != 1)
		{
			why = "a request's data did not come";
			break;
		}
		data[q.len] = '\0';
		tw_reply a;
		memset(&a, 0, sizeof(a));
		a.magic = TW_REP_MAGIC;
		a.handle = q.handle;
		const int slot = (int)q.handle - 1;
		const int have = slot >= 0 && slot < TW_MAX_FILES && g_files[slot] >= 0;
		int bye = 0;
		switch (q.op)
		{
		case TW_OPEN:
		{
			int free_slot = -1;
			for (int i = 0; i < TW_MAX_FILES && free_slot < 0; i++)
				if (g_files[i] < 0)
					free_slot = i;
			if (!path_ok((const char*)data, q.len))
				a.err = EPERM;
			else if (free_slot < 0)
				a.err = EMFILE;
			else
			{
				const int fd = open_under_root((const char*)data, O_WRONLY | ((q.flags & TW_CREATE) ? O_CREAT : 0) | ((q.flags & TW_TRUNC) ? O_TRUNC : 0));
				if (fd < 0)
					a.err = errno ? errno : EIO;
				else
				{
					fchmod(fd, 0666); // the app may run as another user
					g_files[free_slot] = fd;
					a.handle = (uint32_t)free_slot + 1;
					opened++;
				}
			}
			break;
		}
		case TW_WRITE:
			if (!have)
				a.err = EBADF;
			else if (q.len == 0)
				a.err = EINVAL;
			else
			{
				size_t at = 0;
				while (at < q.len)
				{
					const ssize_t w = pwrite(g_files[slot], data + at, q.len - at, (off_t)(q.offset + at));
					if (w < 0 && errno == EINTR)
						continue;
					if (w <= 0)
					{
						a.err = w < 0 ? errno : EIO;
						break;
					}
					at += (size_t)w;
				}
				a.value = at;
				written += at;
				writes++;
			}
			break;
		case TW_CLOSE:
			if (!have)
				a.err = EBADF;
			else
			{
				if (close(g_files[slot]) != 0)
					a.err = errno;
				g_files[slot] = -1;
			}
			break;
		case TW_FSYNC:
			if (!have)
				a.err = EBADF;
			else if (fsync(g_files[slot]) != 0)
				a.err = errno;
			break;
		case TW_MARK:
		{
			const size_t plen = strnlen((const char*)data, q.len);
			const size_t blen = plen < q.len ? q.len - plen - 1 : 0;
			if (plen >= q.len || blen > 64 || !path_ok((const char*)data, plen))
				a.err = EPERM;
			else
			{
				const int fd = open_under_root((const char*)data, O_WRONLY | O_CREAT | O_EXCL);
				if (fd < 0)
					a.err = errno ? errno : EIO;
				else
				{
					fchmod(fd, 0666);
					if (blen && write(fd, data + plen + 1, blen) != (ssize_t)blen)
						a.err = errno ? errno : EIO;
					close(fd);
				}
			}
			break;
		}
		case TW_BYE:
			bye = 1;
			why = "the app said bye";
			break;
		default:
			a.err = EINVAL;
		}
		if (send_all(s, &a, sizeof(a)) != 0)
		{
			why = "the reply could not be sent";
			break;
		}
		if (bye)
			break;
	}
	for (int i = 0; i < TW_MAX_FILES; i++)
		if (g_files[i] >= 0)
			close(g_files[i]);
	close(s);
	note("done (%s): %llu MiB in %llu writes, %llu files opened, %ld s", why, (unsigned long long)(written >> 20), (unsigned long long)writes,
		(unsigned long long)opened, (long)(time(NULL) - t0));
	free(data);
	return 0;
}
