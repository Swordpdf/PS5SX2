// PS5SX2 (2.11, AI-assisted): the relaunch payload for "back to the menu" through the console's ELF loader (see ProsperoRelaunch.h and relaunch/relaunch.c).
//
// The same way as ProsperoImeRelay.cpp's keyboard relay: the embedded payload gets its request written over a marker, the whole file goes to the loader
// on 127.0.0.1:9021, which runs it as a system process. The payload writes "<state>.started" once it is running and can launch (its libraries are loaded and
// sceLncUtilLaunchApp is found); this code waits for that (the handshake) before the app commits to leaving, gives up at once if the payload ends with only a
// "<state>.done" (it cannot launch), and creates "<state>.cancel" when it must not launch after all. Every
// request has its own state prefix (logs/relaunch-<time>-<n>), so a payload that starts late can never act on a newer request's files.
//
// The loader is sometimes busy or being restarted by the HEN supervisor (it kills and re-sends its own loader through this port every 15..40 s on some
// consoles): a refused or timed-out connect is tried again every 250 ms inside the 3 s budget.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProsperoRelaunch.h"

#include "OrbisPaths.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef ORBIS_RELAUNCH_ELF
// The relaunch payload (relaunch/relaunch.c), built and named by Makefile.vk.
__asm__(".section .rodata.orbis_relaunch_elf,\"a\",@progbits\n"
		".balign 16\n"
		".global orbis_relaunch_elf\norbis_relaunch_elf:\n"
		".incbin \"" ORBIS_RELAUNCH_ELF "\"\n"
		".global orbis_relaunch_elf_end\norbis_relaunch_elf_end:\n"
		".byte 0\n"
		".previous\n");
extern "C" const uint8_t orbis_relaunch_elf[], orbis_relaunch_elf_end[];
#endif

#ifndef ORBIS_RELAUNCH_LOADER_PORT
#define ORBIS_RELAUNCH_LOADER_PORT 9021 // the console's ELF loader (the PC test picks another)
#endif

namespace
{
constexpr char kMarker[] = "PS5SX2-RELAUNCH-REQUEST-V1";
constexpr size_t kRoom = 1024; // g_request's size in relaunch.c
constexpr uint16_t kLoaderPort = ORBIS_RELAUNCH_LOADER_PORT;
constexpr int kBudgetMs = 3000;    // connect (with its retries) and the payload's '.started' together
constexpr int kMinHandshakeMs = 1500; // after a late connect the payload still gets this long to answer
constexpr int kMaxTotalMs = 4500;
constexpr int kStaleSeconds = 600;

using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
std::string g_state; // the current request's state prefix, "" when none
bool g_cancelled = false;
unsigned g_seq = 0;

int Ms(Clock::time_point from, Clock::time_point to)
{
	return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
}

bool Exists(const std::string& path)
{
	struct stat st = {};
	return stat(path.c_str(), &st) == 0;
}

void Touch(const std::string& path)
{
	if (FILE* f = std::fopen(path.c_str(), "wb"))
	{
		std::fputs("1\n", f);
		std::fclose(f);
	}
}

#ifdef ORBIS_RELAUNCH_ELF
// The word after "status " in the first line of a payload's .done file ("nolaunch", "cancelled", ...), "unknown" when it can't be read.
std::string DoneStatus(const std::string& path)
{
	char line[96] = {};
	if (FILE* f = std::fopen(path.c_str(), "rb"))
	{
		if (!std::fgets(line, sizeof(line), f))
			line[0] = '\0';
		std::fclose(f);
	}
	std::string text = line;
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
		text.pop_back();
	return text.compare(0, 7, "status ") == 0 && text.size() > 7 ? text.substr(7) : std::string("unknown");
}

bool EndsWith(const std::string& s, const char* tail)
{
	const size_t n = std::strlen(tail);
	return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// Notes of earlier requests (relaunch-*.started / .cancel / .done / .tmp) that are older than ten minutes: a restart leaves no payload waiting that long.
void RemoveStale(const std::string& dir)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return;
	const time_t now = std::time(nullptr);
	while (const dirent* e = readdir(d))
	{
		const std::string name = e->d_name;
		if (name.compare(0, 9, "relaunch-") != 0)
			continue;
		if (!EndsWith(name, ".started") && !EndsWith(name, ".cancel") && !EndsWith(name, ".done") && !EndsWith(name, ".tmp"))
			continue;
		struct stat st = {};
		const std::string path = dir + "/" + name;
		if (stat(path.c_str(), &st) == 0 && now - st.st_mtime > kStaleSeconds)
			unlink(path.c_str());
	}
	closedir(d);
}

std::string Dirname(const std::string& path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Titles go into a line of the request: letters, digits and '_' only.
bool CleanTitle(const char* in, std::string& out)
{
	out.clear();
	if (!in)
		return true;
	for (const char* p = in; *p; p++)
	{
		const char c = *p;
		const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
		if (!ok || out.size() >= 32)
			return false;
		out += c;
	}
	return true;
}

// Where the request goes in the embedded file: the marker, then zeros to the end of the array.
size_t RequestOffset()
{
	static const size_t offset = [] {
		const size_t size = static_cast<size_t>(orbis_relaunch_elf_end - orbis_relaunch_elf);
		const size_t mlen = sizeof(kMarker) - 1;
		for (size_t i = 0; i + kRoom <= size; i++)
		{
			if (std::memcmp(orbis_relaunch_elf + i, kMarker, mlen) != 0)
				continue;
			size_t j = i + mlen;
			while (j < i + kRoom && orbis_relaunch_elf[j] == 0)
				j++;
			if (j == i + kRoom)
				return i;
		}
		return SIZE_MAX;
	}();
	return offset;
}

// One connect to the loader. The descriptor, or -1 with `err` set (ETIMEDOUT when the loader did not answer in `wait_ms`).
int TryConnect(int wait_ms, int& err)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		err = errno;
		return -1;
	}
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kLoaderPort);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
	{
		err = errno;
		close(fd);
		return -1;
	}
	if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 && errno != EINPROGRESS)
	{
		err = errno; // ECONNREFUSED: no ELF loader (just now)
		close(fd);
		return -1;
	}
	struct pollfd p = {fd, POLLOUT, 0};
	if (poll(&p, 1, wait_ms) != 1)
	{
		err = ETIMEDOUT;
		close(fd);
		return -1;
	}
	int soerr = 0;
	socklen_t len = sizeof(soerr);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr != 0)
	{
		err = soerr ? soerr : errno;
		close(fd);
		return -1;
	}
	return fd; // left non-blocking: a loader that does not take the payload must not hang the way out (the send below waits on poll, inside the budget)
}
#endif
} // namespace

bool OrbisRelaunchBuilt()
{
#ifdef ORBIS_RELAUNCH_ELF
	return true;
#else
	return false;
#endif
}

std::string OrbisRelaunchConfirmCameBack()
{
	const std::string note = OrbisLogPath("menu-guard.txt");
	FILE* f = std::fopen(note.c_str(), "rb");
	if (!f)
		return {};
	char state[512] = {}, when[32] = {};
	const bool got = std::fgets(state, sizeof(state), f) && std::fgets(when, sizeof(when), f);
	std::fclose(f);
	unlink(note.c_str());
	std::string s = got ? state : "";
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
		s.pop_back();
	const long long age = got ? static_cast<long long>(std::time(nullptr)) - std::atoll(when) : -1;
	if (s.empty() || s[0] != '/' || age < 0 || age > 120)
		return "a back-to-the-menu guard note that is too old or unreadable was removed";
	Touch(s + ".alive");
	return "this start is the one \"back to the menu\" re-executed (" + std::to_string(age) + " s after it): its guard was told (" + s + ".alive), it launches nothing";
}

std::string OrbisRelaunchLastLog(size_t max_lines)
{
	FILE* f = std::fopen(OrbisLogPath("relaunch.log").c_str(), "rb");
	if (!f)
		return {};
	std::vector<std::string> lines;
	char buf[512];
	while (std::fgets(buf, sizeof(buf), f))
	{
		std::string l = buf;
		while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
			l.pop_back();
		if (l.find("request: pid") != std::string::npos)
			lines.clear(); // the last request's lines only
		lines.push_back(l);
	}
	std::fclose(f);
	std::string out;
	const size_t from = lines.size() > max_lines ? lines.size() - max_lines : 0;
	for (size_t i = from; i < lines.size(); i++)
		out += lines[i] + "\n";
	return out;
}

bool OrbisRelaunchStart(const char* closing_title, const char* launch_title, int pid, std::string& why, const OrbisRelaunchTiming& timing)
{
#ifndef ORBIS_RELAUNCH_ELF
	(void)closing_title;
	(void)launch_title;
	(void)pid;
	(void)timing;
	why = "this build has no relaunch payload";
	return false;
#else
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto begin = Clock::now();
	std::string closing, launch;
	if (!CleanTitle(closing_title, closing) || !CleanTitle(launch_title, launch) || launch.empty() || pid <= 1)
	{
		why = "bad title or pid";
		return false;
	}
	const size_t offset = RequestOffset();
	if (offset == SIZE_MAX)
	{
		why = "the embedded payload has no request marker";
		return false;
	}
	const std::string probe = OrbisLogPath("relaunch-probe");
	RemoveStale(Dirname(probe));
	// The earlier request is not ours any more: its payload (if any is still waiting) is told to stop.
	if (!g_state.empty() && !g_cancelled)
		Touch(g_state + ".cancel");
	g_cancelled = false;
	g_state = OrbisLogPath(("relaunch-" + std::to_string(static_cast<long long>(std::time(nullptr))) + "-" + std::to_string(++g_seq)).c_str());
	const std::string state = g_state;
	if (state.find('\n') != std::string::npos)
	{
		why = "the logs path has a line break in it";
		g_state.clear();
		return false;
	}
	std::string request = std::string(kMarker) + "\npid=" + std::to_string(pid) + "\nwait=" + closing + "\nlaunch=" + launch + "\nstate=" + state +
	                      "\nsettle_ms=" + std::to_string(timing.settle_ms) + "\nmin_exit_ms=" + std::to_string(timing.min_exit_ms) +
	                      "\nretry_ms=" + std::to_string(timing.retry_ms) + "\nlimit_s=45\n";
	if (timing.guard_ms > 0) // fix180: guard mode; the new process finds the state through logs/menu-guard.txt (written once the payload is up, below)
		request += "guard_ms=" + std::to_string(timing.guard_ms) + "\nhard_ms=" + std::to_string(timing.hard_ms) + "\n";
	unlink(OrbisLogPath("menu-guard.txt").c_str()); // fix181: never a note of an earlier leave, nor one for a payload that did not start
	if (request.size() >= kRoom)
	{
		why = "the request is too large";
		g_state.clear();
		return false;
	}
	std::vector<uint8_t> elf(orbis_relaunch_elf, orbis_relaunch_elf_end);
	std::memcpy(elf.data() + offset, request.data(), request.size());
	elf[offset + request.size()] = 0;

	// 1. the loader: tried again every 250 ms while it refuses or does not answer (inside the budget)
	int fd = -1;
	int last_err = 0;
	int tries = 0;
	for (;;)
	{
		const int left = kBudgetMs - Ms(begin, Clock::now());
		if (left <= 0)
			break;
		tries++;
		int err = 0;
		fd = TryConnect(left < 500 ? left : 500, err);
		if (fd >= 0)
			break;
		last_err = err;
		if (err != ECONNREFUSED && err != ETIMEDOUT)
			break; // no use trying again
		if (kBudgetMs - Ms(begin, Clock::now()) <= 250)
			break;
		usleep(250000);
	}
	if (fd < 0)
	{
		why = "connect: errno " + std::to_string(last_err) + (tries > 1 ? " (" + std::to_string(tries) + " tries)" : std::string());
		g_state.clear(); // nothing was sent: there is no payload to cancel
		std::printf("[relaunch] no relaunch payload: %s\n", why.c_str());
		std::fflush(stdout);
		return false;
	}
	const auto fail_after_send = [&](const std::string& reason) {
		why = reason;
		Touch(state + ".cancel"); // a payload that starts late launches nothing
		g_cancelled = true;
		close(fd);
		std::printf("[relaunch] no relaunch payload: %s\n", why.c_str());
		std::fflush(stdout);
		return false;
	};

	// 2. the payload goes over
	for (size_t sent = 0; sent < elf.size();)
	{
		const ssize_t n = send(fd, elf.data() + sent, elf.size() - sent, 0);
		if (n > 0)
		{
			sent += static_cast<size_t>(n);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		{
			const int left = kMaxTotalMs - Ms(begin, Clock::now());
			if (left <= 0)
				return fail_after_send("send: the loader did not take the payload in time");
			struct pollfd w = {fd, POLLOUT, 0};
			poll(&w, 1, left < 200 ? left : 200);
			continue;
		}
		return fail_after_send("send: errno " + std::to_string(errno));
	}
	shutdown(fd, SHUT_WR);
	const auto sent_at = Clock::now();
	// The connection is kept open by a thread until the loader closes it (or ten minutes pass). The payload does not need that (the fork's relaunch.elf
	// outlives the app that sent it), but a closed connection might end a payload on some loader, and this is harmless.
	std::thread([fd] {
		const auto deadline = Clock::now() + std::chrono::minutes(10);
		for (;;)
		{
			const int left = Ms(Clock::now(), deadline);
			struct pollfd q = {fd, POLLIN, 0};
			if (left <= 0 || poll(&q, 1, left) != 1)
				break;
			char buf[256];
			const ssize_t got = recv(fd, buf, sizeof(buf), 0);
			if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
				continue; // the descriptor is non-blocking
			if (got <= 0)
				break;
		}
		close(fd);
	}).detach();

	// 3. the handshake: the payload's '.started' (the rest of the budget, but at least kMinHandshakeMs after a late connect, and never past kMaxTotalMs)
	int wait_until = kBudgetMs;
	const int late = Ms(begin, sent_at) + kMinHandshakeMs;
	if (late > wait_until)
		wait_until = late;
	if (wait_until > kMaxTotalMs)
		wait_until = kMaxTotalMs;
	while (!Exists(state + ".started"))
	{
		if (Exists(state + ".done"))
		{
			// The payload ended without a handshake (it cannot launch: no sceLncUtilLaunchApp): waiting out the budget would only freeze the picture longer.
			g_cancelled = true; // nothing is left to call off
			why = "the payload ended without starting (" + DoneStatus(state + ".done") + "); cancelled";
			std::printf("[relaunch] no relaunch payload: %s\n", why.c_str());
			std::fflush(stdout);
			return false;
		}
		if (Ms(begin, Clock::now()) >= wait_until)
		{
			Touch(state + ".cancel");
			g_cancelled = true;
			why = "the payload did not answer in " + std::to_string(Ms(begin, Clock::now())) + " ms (no '.started'); cancelled";
			std::printf("[relaunch] no relaunch payload: %s\n", why.c_str());
			std::fflush(stdout);
			return false; // the thread above still holds the socket; it ends with the loader's close
		}
		usleep(50000);
	}
	std::printf("[relaunch] the payload is up after %d ms (%d connect%s), state %s\n", Ms(begin, Clock::now()), tries, tries == 1 ? "" : "s", state.c_str());
	std::fflush(stdout);
	// fix181: the guard's note only now (181: a console whose loader refused the connect, errno 13, had the note anyway, and its next start said "its guard
	// was told" about a guard that never ran)
	if (timing.guard_ms > 0)
		if (FILE* f = std::fopen(OrbisLogPath("menu-guard.txt").c_str(), "wb"))
		{
			std::fprintf(f, "%s\n%lld\n", state.c_str(), static_cast<long long>(std::time(nullptr)));
			std::fclose(f);
		}
	return true;
#endif
}

void OrbisRelaunchCancel()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_state.empty() || g_cancelled)
		return;
	Touch(g_state + ".cancel");
	g_cancelled = true;
	std::printf("[relaunch] the request was taken back (%s.cancel)\n", g_state.c_str());
	std::fflush(stdout);
}
