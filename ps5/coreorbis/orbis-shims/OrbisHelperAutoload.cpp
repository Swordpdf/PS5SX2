// PS5SX2 (vk-285-179, AI-assisted): see OrbisHelperAutoload.h. The app sends the PS5SX2 Disc Helper ELF it ships to the
// console's ELF loader (127.0.0.1:9021) at start, so the user does not load it by hand. Every step goes to boot.log.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "OrbisHelperAutoload.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// From orbis_paths.cpp.
bool OrbisFlag(const char* name) __attribute__((weak));
std::string OrbisLogPath(const char* name);

namespace
{
constexpr int kPort = 9021;
constexpr long long kFreshSec = 15;  // a helper heartbeat older than this is "not running"
constexpr long long kBusySec = 20;   // a disc-dump-progress stamp newer than this means a copy is running

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	std::printf("[helper] %s\n", line);
	std::fflush(stdout);
}

std::string ReadSmall(const std::string& path)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return {};
	std::string s;
	char buf[1024];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < (64u << 10))
		s.append(buf, n);
	std::fclose(f);
	return s;
}

orbis_helper::Status ReadStatus()
{
	orbis_helper::Status st;
	const std::string text = ReadSmall(OrbisLogPath("ps5sx2-helper.txt"));
	if (text.empty())
		return st;
	long long when = 0;
	size_t at = 0;
	while (at < text.size())
	{
		size_t eol = text.find('\n', at);
		if (eol == std::string::npos)
			eol = text.size();
		const std::string line = text.substr(at, eol - at);
		at = eol + 1;
		if (line.rfind("pid=", 0) == 0)
			st.pid = std::atoi(line.c_str() + 4);
		else if (line.rfind("time=", 0) == 0)
			when = std::atoll(line.c_str() + 5);
		else if (line.rfind("rev=", 0) == 0)
			st.rev = line.substr(4);
	}
	const long long now = static_cast<long long>(std::time(nullptr));
	st.age_s = now - when;
	st.alive = st.pid > 0 && when > 0 && st.age_s >= -5 && st.age_s <= kFreshSec;

	// A copy in progress: a fresh disc-dump-progress.txt stamp.
	struct stat ds;
	if (stat(OrbisLogPath("disc-dump-progress.txt").c_str(), &ds) == 0)
		st.busy = now - static_cast<long long>(ds.st_mtime) <= kBusySec;
	return st;
}

// The helper ELF the app ships: in its own title folder first, then where a tester may have put it by hand.
// fix180: /app0 is the app's folder only BEFORE the jailbreak (main-boot.cpp reads disc-tile.txt there for that reason); after it (OrbisHelperAutoStart)
// /app0/... was never found: every one of the 51 vk-285-180 session reports says "auto-load: skip - no helper ELF shipped with the app", though the
// package has PPSA99203/PS5SXHelper.elf. So the app's folder is also looked at by its /data path (it is there after the jailbreak, for the main app and
// for a disc or game tile, which are copies of the app under other title IDs).
std::string FindElf()
{
	static const char* const kPaths[] = {
		"/app0/PS5SXHelper.elf",
		"/app0/PS5SXHelper-merged-nomount.elf",
		"/data/homebrew/PPSA99203/PS5SXHelper.elf",
		"/data/homebrew/PPSA99203/PS5SXHelper-merged-nomount.elf",
		"/data/PCSX2/PS5SXHelper-merged-nomount.elf",
		"/data/PCSX2/PS5SXHelper.elf",
	};
	for (const char* p : kPaths)
	{
		struct stat s;
		if (stat(p, &s) == 0 && S_ISREG(s.st_mode) && s.st_size > 4096)
			return p;
	}
	return {};
}

// Send the whole ELF to 127.0.0.1:9021 (the loader streams it and runs it as a system process). true when the loader took
// all of it. Modelled on orbis_try_jailbreak's loopback send.
bool SendToLoader(const std::string& path, std::string& detail)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
	{
		detail = "cannot open the ELF (errno " + std::to_string(errno) + ")";
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	const long sz = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<unsigned char> data(sz > 0 ? static_cast<size_t>(sz) : 0);
	const size_t got = sz > 0 ? std::fread(data.data(), 1, data.size(), f) : 0;
	std::fclose(f);
	if (got != data.size() || data.empty())
	{
		detail = "short read of the ELF";
		return false;
	}
	return OrbisLoaderSend(data, detail, nullptr);
}
} // namespace

// 180: the socket part of SendToLoader, shared with the PS5SX2 Installer's start (OrbisInstallerStart.cpp).
bool OrbisLoaderSend(const std::vector<unsigned char>& data, std::string& detail, bool* no_loader)
{
	if (no_loader)
		*no_loader = false;
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		detail = "socket errno " + std::to_string(errno);
		return false;
	}
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(kPort));
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
	{
		const int e = errno;
		if (no_loader)
			*no_loader = e == ECONNREFUSED;
		detail = e == ECONNREFUSED ? "nothing answered on 9021 (no ELF loader - is the jailbreak's loader up?)"
								   : ("connect errno " + std::to_string(e));
		close(fd);
		return false;
	}
	size_t sent = 0;
	while (sent < data.size())
	{
		const ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
		if (n <= 0)
		{
			detail = "send stopped at " + std::to_string(sent) + "/" + std::to_string(data.size()) + " (errno " + std::to_string(errno) + ")";
			shutdown(fd, SHUT_RDWR);
			close(fd);
			return false;
		}
		sent += static_cast<size_t>(n);
	}
	shutdown(fd, SHUT_WR); // tell the loader the file is complete
	close(fd);
	detail = "sent " + std::to_string(sent) + " bytes to 127.0.0.1:9021";
	return true;
}

// 180: called before the jailbreak when nobody answered its request (main-boot.cpp): the helper (which answers it) is not running, so
// send the shipped one at once. /data may not be visible yet, so no heartbeat is read: a helper that runs would have answered.
bool OrbisHelperStartBeforeJailbreak()
{
	if (OrbisFlag && OrbisFlag("helper_noautoload"))
	{
		Log("before the jailbreak: flag helper_noautoload: nothing sent");
		return false;
	}
	const std::string elf = FindElf();
	if (elf.empty())
	{
		Log("before the jailbreak: no helper ELF shipped with the app: nothing sent");
		return false;
	}
	std::string detail;
	const bool ok = SendToLoader(elf, detail);
	Log("before the jailbreak (no answer to the request): %s the helper %s: %s", ok ? "sent" : "could not send", elf.c_str(), detail.c_str());
	return ok;
}

// Called once at start, after the jailbreak (main-boot.cpp): decide and, if needed, send the helper.
void OrbisHelperAutoStart()
{
	const bool noautoload = OrbisFlag && OrbisFlag("helper_noautoload");
	const std::string elf = noautoload ? std::string() : FindElf();
	const orbis_helper::Status st = ReadStatus();

	std::string why;
	const orbis_helper::Decision d = orbis_helper::Decide(/*enabled*/ true, noautoload, !elf.empty(), st, orbis_helper::kShippedRev, why);
	Log("auto-load: %s - %s (shipped %s; %s)", orbis_helper::DecisionName(d), why.c_str(), orbis_helper::kShippedRev,
		st.alive ? (std::string("a helper runs: pid ") + std::to_string(st.pid) + ", rev " + (st.rev.empty() ? "?" : st.rev) +
					", heartbeat " + std::to_string(st.age_s) + " s ago" + (st.busy ? ", copying" : "")).c_str()
				 : "no helper heartbeat");

	if (d != orbis_helper::Decision::Load)
		return;

	std::string detail;
	const bool ok = SendToLoader(elf, detail);
	Log("auto-load: %s the helper %s: %s", ok ? "sent" : "could not send", elf.c_str(), detail.c_str());
}
