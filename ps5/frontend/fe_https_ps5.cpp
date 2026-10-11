// PS5 port frontend: the console's parts for fe_https (DNS, a TCP socket, random numbers). 2026-10-05, AI-assisted;
// needs proper testing on the console.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_https.h"

#include <atomic>
#include <cerrno>
#include <cpuid.h>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" {
int sceNetResolverCreate(const char* name, int memid, int flags);
int sceNetResolverStartNtoa(int rid, const char* hostname, struct in_addr* addr, int timeout, int retry, int flags);
int sceNetResolverDestroy(int rid);
}

namespace fe
{
	namespace
	{
		std::string Hex(int v)
		{
			char buf[16];
			std::snprintf(buf, sizeof(buf), "%#x", static_cast<unsigned>(v));
			return buf;
		}

		bool Resolve(int pool, const std::string& host, in_addr& addr, std::string& error)
		{
			if (inet_pton(AF_INET, host.c_str(), &addr) == 1)
				return true;
			const int rid = sceNetResolverCreate("ps5sx2-https", pool, 0);
			if (rid < 0)
			{
				error = "no DNS resolver (" + Hex(rid) + ")";
				return false;
			}
			const int rc = sceNetResolverStartNtoa(rid, host.c_str(), &addr, 0, 0, 0); // the system's timeout and retries
			sceNetResolverDestroy(rid);
			if (rc < 0)
			{
				error = "can't find " + host + " (DNS " + Hex(rc) + ")";
				return false;
			}
			return true;
		}

		int ConnectTcp(int pool, const std::string& host, uint16_t port, int timeout_ms, int io_ms, std::string& error)
		{
			in_addr addr{};
			if (!Resolve(pool, host, addr, error))
				return -1;
			const int fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0)
			{
				error = "no socket (errno " + std::to_string(errno) + ")";
				return -1;
			}
			const int one = 1;
			setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)); // a closed connection is an error, not a SIGPIPE
			const int rcvbuf = 1024 * 1024;
			setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)); // a hint, for the downloads' speed
			sockaddr_in sa{};
			sa.sin_family = AF_INET;
			sa.sin_port = htons(port);
			sa.sin_addr = addr;
			// Connect with a time limit: non-blocking, then wait for it.
			const int flags = fcntl(fd, F_GETFL, 0);
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);
			int rc = connect(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
			if (rc != 0 && errno == EINPROGRESS)
			{
				pollfd p{fd, POLLOUT, 0};
				const int ready = poll(&p, 1, timeout_ms);
				if (ready == 1)
				{
					int so_error = 0;
					socklen_t len = sizeof(so_error);
					getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
					rc = so_error == 0 ? 0 : -1;
					errno = so_error;
				}
				else
				{
					if (ready == 0)
						errno = ETIMEDOUT;
					rc = -1;
				}
			}
			if (rc != 0)
			{
				const int e = errno;
				close(fd);
				char ip[INET_ADDRSTRLEN] = {};
				inet_ntop(AF_INET, &addr, ip, sizeof(ip));
				error = "no connection to " + host + " (" + ip + ":" + std::to_string(port) + ", errno " + std::to_string(e) + ")";
				return -1;
			}
			fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
			timeval tv{io_ms / 1000, (io_ms % 1000) * 1000};
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
			return fd;
		}

		// The kernel's arc4random (kern.arandom), up to 256 bytes a call.
		bool SysctlRandom(unsigned char* p, size_t len)
		{
			while (len > 0)
			{
				int mib[2] = {CTL_KERN, KERN_ARND};
				size_t got = len > 256 ? 256 : len;
				if (sysctl(mib, 2, p, &got, nullptr, 0) != 0 || got == 0)
					return false;
				p += got;
				len -= got;
			}
			return true;
		}

		bool UrandomRandom(unsigned char* p, size_t len)
		{
			const int fd = open("/dev/urandom", O_RDONLY);
			if (fd < 0)
				return false;
			while (len > 0)
			{
				const ssize_t n = read(fd, p, len);
				if (n <= 0)
				{
					close(fd);
					return false;
				}
				p += n;
				len -= static_cast<size_t>(n);
			}
			close(fd);
			return true;
		}

		// The CPU's RDRAND, with its carry flag checked and a stuck generator refused.
		bool RdrandRandom(unsigned char* p, size_t len)
		{
			unsigned a = 0, b = 0, c = 0, d = 0;
			if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 30)))
				return false;
			uint64_t last = 0;
			bool have_last = false;
			while (len > 0)
			{
				uint64_t v = 0;
				unsigned char ok = 0;
				for (int tries = 0; tries < 16 && !ok; tries++)
					__asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
				if (!ok || v == 0 || v == ~0ull || (have_last && v == last))
					return false;
				last = v;
				have_last = true;
				const size_t take = len < sizeof(v) ? len : sizeof(v);
				std::memcpy(p, &v, take);
				p += take;
				len -= take;
			}
			return true;
		}
	} // namespace

	HttpsPlatform MakeConsoleHttpsPlatform(int net_pool, std::function<void(const std::string&)> log)
	{
		HttpsPlatform platform;
		platform.connect = [net_pool](const std::string& host, uint16_t port, int timeout_ms, int io_ms, std::string& error) {
			return ConnectTcp(net_pool, host, port, timeout_ms, io_ms, error);
		};
		platform.random = [log](void* buf, size_t len) {
			// The first source that answers; the log says which, whenever that changes.
			static std::atomic<int> said{0};
			unsigned char* p = static_cast<unsigned char*>(buf);
			const int used = SysctlRandom(p, len) ? 1 : UrandomRandom(p, len) ? 2 : RdrandRandom(p, len) ? 3 : 0;
			if (used != 0 && said.exchange(used) != used && log)
			{
				static const char* const kNames[] = {"", "the kernel's (kern.arandom)", "/dev/urandom (kern.arandom refused)",
					"the CPU's RDRAND (kern.arandom and /dev/urandom refused)"};
				log(std::string("[https] random numbers: ") + kNames[used]);
			}
			if (used == 0 && log)
				log("[https] no random numbers from the system");
			return used != 0;
		};
		platform.log = std::move(log);
		return platform;
	}
} // namespace fe
