// PS5SX2 (2026-10-29, AI-assisted): the HD texture packs' writer process through the console's ELF loader (see ProsperoTexWriter.h).
//
// The same way as ProsperoImeRelay.cpp: the embedded payload (tex-writer/tex_writer.c, built by Makefile.vk) gets its request written over its
// marker and the whole file goes to the loader on 127.0.0.1:9021, which runs it as a process of its own. The payload stays up for a whole pack
// (download and unpack), so a thread holds the loader's connection open until the loader closes it (a closed connection might end the payload).
// Needs testing on the console: that the loader takes it while a title runs, that the payload's writes are not held back like the title's, and
// that it sees the textures folder at the same path (fe::TexRemoteWriter::Start checks that before anything is written).
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProsperoTexWriter.h"

#include "ps5/frontend/fe_texwrite.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef ORBIS_TEX_WRITER_ELF
// The payload (tex-writer/tex_writer.c), built and named by Makefile.vk.
__asm__(".section .rodata.orbis_tex_writer_elf,\"a\",@progbits\n"
		".balign 16\n"
		".global orbis_tex_writer_elf\norbis_tex_writer_elf:\n"
		".incbin \"" ORBIS_TEX_WRITER_ELF "\"\n"
		".global orbis_tex_writer_elf_end\norbis_tex_writer_elf_end:\n"
		".byte 0\n"
		".previous\n");
extern "C" const uint8_t orbis_tex_writer_elf[], orbis_tex_writer_elf_end[];
#endif

namespace
{
constexpr uint16_t kLoaderPort = 9021;
}

bool OrbisTexWriterBuilt()
{
#ifdef ORBIS_TEX_WRITER_ELF
	return true;
#else
	return false;
#endif
}

bool OrbisTexWriterLaunch(const std::string& request, std::string& why)
{
#ifndef ORBIS_TEX_WRITER_ELF
	(void)request;
	why = "this build has no writer payload";
	return false;
#else
	const std::vector<uint8_t> elf =
		fe::TexWriterPatched(orbis_tex_writer_elf, static_cast<size_t>(orbis_tex_writer_elf_end - orbis_tex_writer_elf), request);
	if (elf.empty())
	{
		why = "the embedded writer has no request marker (or the request is too long)";
		return false;
	}
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		why = "socket: errno " + std::to_string(errno);
		return false;
	}
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kLoaderPort);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const auto fail = [&](const char* step) {
		why = std::string(step) + ": errno " + std::to_string(errno);
		close(fd);
		return false;
	};
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
		return fail("fcntl");
	if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 && errno != EINPROGRESS)
		return fail("the ELF loader (127.0.0.1:9021)");
	struct pollfd p = {fd, POLLOUT, 0};
	if (poll(&p, 1, 500) != 1)
	{
		errno = ETIMEDOUT;
		return fail("the ELF loader (127.0.0.1:9021)");
	}
	int err = 0;
	socklen_t len = sizeof(err);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0)
	{
		errno = err;
		return fail("the ELF loader (127.0.0.1:9021)");
	}
	fcntl(fd, F_SETFL, flags);
	for (size_t sent = 0; sent < elf.size();)
	{
		const ssize_t n = send(fd, elf.data() + sent, elf.size() - sent, 0);
		if (n <= 0)
			return fail("send");
		sent += static_cast<size_t>(n);
	}
	shutdown(fd, SHUT_WR);
	// The payload stays for the whole pack: the loader's connection stays open until the loader closes it (its payload ended), or 3 hours.
	std::thread([fd] {
		for (int i = 0; i < 3 * 3600; i++)
		{
			struct pollfd q = {fd, POLLIN, 0};
			const int r = poll(&q, 1, 1000);
			if (r < 0 && errno != EINTR)
				break;
			if (r == 1)
			{
				char buf[256];
				if (recv(fd, buf, sizeof(buf), 0) <= 0)
					break;
			}
		}
		close(fd);
	}).detach();
	std::printf("[texpacks] writer payload sent to the ELF loader (127.0.0.1:%u, %zu bytes)\n", static_cast<unsigned>(kLoaderPort), elf.size());
	std::fflush(stdout);
	return true;
#endif
}
