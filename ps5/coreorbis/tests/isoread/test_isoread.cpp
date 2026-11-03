// PS5SX2 (187, AI-assisted): PC test of what the image reader does when a read fails (audit io-2): the REAL pcsx2/CDVD/FlatFileReader.cpp
// and ThreadedFileReader.cpp on a real file, with failures injected (ld --wrap=fread,--wrap=pread, see test-isoread.sh; FileSystem's calls
// and the few other things the two files need from PCSX2 are stand-ins here). Checks:
//   - a failed read of data a caller waits for is retried on the read thread and comes; a dead descriptor is got over by opening the
//     file again; one that keeps failing gives up after about 1.5 s, and the same chunk then fails at once for 5 s (the CDVD layer's
//     256 retries do not each wait again);
//   - the read-ahead and ReadSync's own read (under the reader's lock) are never retried, and no retry runs on the caller's thread;
//   - Close stops the retries at once; the switch file iso_noretry gives the old fail-at-once back;
//   - a read past the end of the image fails at once, as before 187, with no [iso] line;
//   - each failure's line has the offset, size, errno, ferror/feof and what the read was for.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CDVD/FlatFileReader.h"
#include "Host.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SmallString.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// ---- what the two files need from PCSX2 and the app ----
static std::mutex g_log_lock;
static std::vector<std::string> g_log;
void Log::Writev(LOGLEVEL, ConsoleColors, const char* format, va_list ap)
{
	char line[1024];
	std::vsnprintf(line, sizeof(line), format, ap);
	std::printf("        log: %s\n", line);
	std::lock_guard<std::mutex> lock(g_log_lock);
	g_log.emplace_back(line);
}
Error::Error() = default;
Error::~Error() = default;
void Error::SetString(Error* errptr, std::string description)
{
	if (errptr)
	{
		errptr->m_type = Type::User;
		errptr->m_description = std::move(description);
	}
}
void Error::SetStringView(Error* errptr, std::string_view description)
{
	SetString(errptr, std::string(description));
}
// (Precache's: never called here)
SmallStringBase::SmallStringBase() = default;
SmallStringBase::~SmallStringBase() {}
void SmallStringBase::append(char) {}
std::string_view Host::TranslateToStringView(const std::string_view, const std::string_view msg) { return msg; }
std::string_view Path::GetFileName(const std::string_view path) { return path; }
u64 GetAvailablePhysicalMemory() { return 8ull << 30; }
void OrbisHelperThreadAdd(pthread_t) {}
void OrbisHelperThreadRemove(pthread_t) {}
void Threading::SetNameOfCurrentThread(const char*) {}
static std::mutex g_flags_lock;
static std::set<std::string> g_flags;
bool OrbisFlag(const char* name)
{
	std::lock_guard<std::mutex> lock(g_flags_lock);
	return g_flags.count(name) != 0;
}

// ---- the injected failures ----
struct Plan
{
	long long offset = -1;           // reads at this file offset (a chunk's start) fail ...
	int fread_fails = 0;             // ... this many more times through fread (-1: always)
	int pread_fails = 0;             // ... and through pread
	std::FILE* dead_file = nullptr;  // every read through this FILE or descriptor fails (a drive that went away) ...
	int dead_fd = -1;
	bool revive_on_reopen = false;   // ... until the file is opened again (the drive came back)
	const char* reopen_path = nullptr; // the file a reopen opens instead (another size), or null
	bool reopen_fails = false;       // a reopen does not open (ENOENT)
	int preads_at_offset = 0;        // counters
	int preads = 0;
	int opens = 0;
	bool pread_on_main = false;
};
static std::mutex g_plan_lock;
static Plan g_plan;
static std::thread::id g_main;
static std::FILE* g_current = nullptr; // the FILE the last open returned

extern "C" size_t __real_fread(void* ptr, size_t size, size_t n, std::FILE* fp);
extern "C" ssize_t __real_pread(int fd, void* buf, size_t n, off_t off);

extern "C" size_t __wrap_fread(void* ptr, size_t size, size_t n, std::FILE* fp)
{
	const long long pos = ftello(fp);
	{
		std::lock_guard<std::mutex> lock(g_plan_lock);
		if (fp == g_plan.dead_file || (pos == g_plan.offset && g_plan.fread_fails != 0))
		{
			if (fp != g_plan.dead_file && g_plan.fread_fails > 0)
				g_plan.fread_fails--;
			errno = EIO;
			return 0;
		}
	}
	return __real_fread(ptr, size, n, fp);
}

extern "C" ssize_t __wrap_pread(int fd, void* buf, size_t n, off_t off)
{
	{
		std::lock_guard<std::mutex> lock(g_plan_lock);
		g_plan.preads++;
		if (std::this_thread::get_id() == g_main)
			g_plan.pread_on_main = true;
		if (off == g_plan.offset)
			g_plan.preads_at_offset++;
		if (fd == g_plan.dead_fd || (off == g_plan.offset && g_plan.pread_fails != 0))
		{
			if (fd != g_plan.dead_fd && g_plan.pread_fails > 0)
				g_plan.pread_fails--;
			errno = EIO;
			return -1;
		}
	}
	return __real_pread(fd, buf, n, off);
}

std::FILE* FileSystem::OpenCFile(const char* filename, const char* mode, Error* error)
{
	std::lock_guard<std::mutex> lock(g_plan_lock);
	const bool reopen = g_plan.opens++ > 0;
	if (reopen && g_plan.reopen_fails)
	{
		errno = ENOENT;
		Error::SetString(error, "errno 2: No such file or directory");
		return nullptr;
	}
	std::FILE* fp = std::fopen(reopen && g_plan.reopen_path ? g_plan.reopen_path : filename, mode);
	if (reopen && g_plan.revive_on_reopen)
	{
		g_plan.dead_file = nullptr;
		g_plan.dead_fd = -1;
	}
	g_current = fp;
	return fp;
}
int FileSystem::FSeek64(std::FILE* fp, s64 offset, int whence) { return fseeko(fp, static_cast<off_t>(offset), whence); }
s64 FileSystem::FSize64(std::FILE* fp)
{
	const off_t pos = ftello(fp);
	if (pos < 0 || fseeko(fp, 0, SEEK_END) != 0)
		return -1;
	const off_t size = ftello(fp);
	return fseeko(fp, pos, SEEK_SET) == 0 ? static_cast<s64>(size) : -1;
}
size_t FileSystem::ReadFileWithProgress(std::FILE*, void*, size_t, ProgressCallback*, Error*, size_t) { return 0; }

// ---- the test ----
static int s_failed = 0;
static void Check(bool ok, const std::string& what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
	if (!ok)
		s_failed++;
}

static constexpr u32 kSector = 2048;
static constexpr u64 kChunk = 128 * 1024;
static constexpr u64 kFileSize = 8ull * 1024 * 1024 + 3 * kSector; // 4099 sectors, chunks 0-64: the last one is short
static constexpr u32 kSectors = static_cast<u32>(kFileSize / kSector);
static u8 Byte(u64 offset) { return static_cast<u8>((offset * 2654435761ull) >> 13); }
static bool SectorOk(const u8* p, u32 sector)
{
	for (u32 i = 0; i < kSector; i++)
		if (p[i] != Byte(static_cast<u64>(sector) * kSector + i))
			return false;
	return true;
}
static u32 SectorOfChunk(u64 chunk) { return static_cast<u32>(chunk * kChunk / kSector); }

static size_t LogSize()
{
	std::lock_guard<std::mutex> lock(g_log_lock);
	return g_log.size();
}
// a line after `from` that has all of `parts`
static bool Logged(size_t from, std::initializer_list<const char*> parts)
{
	std::lock_guard<std::mutex> lock(g_log_lock);
	for (size_t i = from; i < g_log.size(); i++)
	{
		bool all = true;
		for (const char* p : parts)
			all = all && g_log[i].find(p) != std::string::npos;
		if (all)
			return true;
	}
	return false;
}
static bool WaitLogged(size_t from, std::initializer_list<const char*> parts, int ms)
{
	for (int t = 0; t < ms; t += 5)
	{
		if (Logged(from, parts))
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return Logged(from, parts);
}
static long long MsSince(std::chrono::steady_clock::time_point t0)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}
static void SetPlan(const Plan& p)
{
	std::lock_guard<std::mutex> lock(g_plan_lock);
	const int opens = g_plan.opens;
	g_plan = p;
	g_plan.opens = opens;
}
static Plan GetPlan()
{
	std::lock_guard<std::mutex> lock(g_plan_lock);
	return g_plan;
}

static void WriteFile(const std::string& path, u64 size)
{
	std::FILE* f = std::fopen(path.c_str(), "wb");
	std::vector<u8> b(size);
	for (u64 i = 0; i < size; i++)
		b[i] = Byte(i);
	std::fwrite(b.data(), 1, b.size(), f);
	std::fclose(f);
}

// BeginRead + FinishRead of one sector, as InputIsoFile does it (the CDVD read path): the bytes FinishRead answered
static int ReadSector(FlatFileReader& r, u32 sector, u8* buf)
{
	r.BeginRead(buf, sector, 1);
	return r.FinishRead();
}

int main(int argc, char** argv)
{
	g_main = std::this_thread::get_id();
	const std::string dir = argc > 1 ? argv[1] : "/tmp";
	const std::string path = dir + "/image.iso", other = dir + "/other.iso";
	WriteFile(path, kFileSize);
	WriteFile(other, kFileSize - kChunk);
	std::vector<u8> buf(kSector * 4);
	Error error;

	FlatFileReader r;
	Check(r.Open(path, &error), "the image opens");
	r.SetBlockSize(kSector);

	// 1. reads that work: right bytes, no [iso] line
	{
		const size_t l0 = LogSize();
		const bool ok = r.ReadSync(buf.data(), 10, 1) == static_cast<int>(kSector) && SectorOk(buf.data(), 10) &&
						ReadSector(r, 100, buf.data()) == static_cast<int>(kSector) && SectorOk(buf.data(), 100);
		Check(ok && !Logged(l0, {"[iso]"}), "reads that work: right bytes and no [iso] line");
	}
	// 2. a request whose read fails, then works on the 3rd pread: the data comes, ~70 ms late (waits of 10, 20, 40 ms)
	{
		Plan p;
		p.offset = 15 * kChunk;
		p.fread_fails = -1;
		p.pread_fails = 2;
		SetPlan(p);
		const size_t l0 = LogSize();
		const auto t0 = std::chrono::steady_clock::now();
		const int got = ReadSector(r, SectorOfChunk(15) + 5, buf.data());
		const long long ms = MsSince(t0);
		Check(got == static_cast<int>(kSector) && SectorOk(buf.data(), SectorOfChunk(15) + 5), "transient failure: the requested sector comes, right bytes");
		Check(ms >= 60 && ms < 1000, "transient failure: it took the three short waits (" + std::to_string(ms) + " ms)");
		Check(Logged(l0, {"[iso] read failed at offset 1966080 (131072 bytes, a request): errno 5", "ferror", "feof", "0 bytes came"}),
			"transient failure: the failure's line has offset, size, what for, errno, ferror/feof, bytes");
		Check(Logged(l0, {"[iso] the read at offset 1966080 (131072 bytes) came on retry 3"}), "transient failure: the line says it came on retry 3");
	}
	// 3. the read-ahead fails: no retry (and the request it follows is not held up)
	{
		Plan p;
		p.offset = 21 * kChunk;
		p.fread_fails = -1;
		p.pread_fails = -1;
		SetPlan(p);
		const size_t l0 = LogSize();
		const auto t0 = std::chrono::steady_clock::now();
		const int got = ReadSector(r, SectorOfChunk(20), buf.data()); // the read-ahead goes on into chunk 21
		const long long ms = MsSince(t0);
		const bool seen = WaitLogged(l0, {"[iso] read failed at offset 2752512", "read-ahead"}, 2000);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		Check(got == static_cast<int>(kSector) && SectorOk(buf.data(), SectorOfChunk(20)) && ms < 500, "read-ahead failure: the request before it comes at once");
		Check(seen && GetPlan().preads_at_offset == 0 && !Logged(l0, {"retry"}), "read-ahead failure: said as read-ahead, never retried");
	}
	// 4. ReadSync's own read fails (it holds the reader's lock): not retried there; the read thread gets the request and retries it
	{
		Plan p;
		p.offset = 30 * kChunk;
		p.fread_fails = -1;
		p.pread_fails = 1;
		SetPlan(p);
		const size_t l0 = LogSize();
		const int got = r.ReadSync(buf.data(), SectorOfChunk(30) + 1, 1);
		const Plan after = GetPlan();
		Check(got == static_cast<int>(kSector) && SectorOk(buf.data(), SectorOfChunk(30) + 1), "ReadSync failure: the sector comes (through the read thread)");
		Check(Logged(l0, {"offset 3932160", "a direct read"}) && Logged(l0, {"offset 3932160", "a request"}),
			"ReadSync failure: a direct read failed first, then the read thread's request");
		Check(!after.pread_on_main && after.preads_at_offset == 2, "ReadSync failure: every retry ran on the read thread, none under the caller's lock");
	}
	// 5. a dead descriptor (the drive went away and came back): pread keeps failing, the file opened again works, and goes on working
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		Plan p;
		p.dead_file = g_current;
		p.dead_fd = fileno(g_current);
		p.revive_on_reopen = true;
		SetPlan(p);
		const size_t l0 = LogSize();
		const auto t0 = std::chrono::steady_clock::now();
		const int got = ReadSector(r, SectorOfChunk(25) + 2, buf.data());
		const long long ms = MsSince(t0);
		Check(got == static_cast<int>(kSector) && SectorOk(buf.data(), SectorOfChunk(25) + 2), "dead descriptor: the sector comes after the file is opened again");
		Check(ms >= 1000 && ms < 3000, "dead descriptor: after the pread retries, about 1.3 s (" + std::to_string(ms) + " ms)");
		Check(Logged(l0, {"came on retry 8", "with the file opened again"}), "dead descriptor: the line says the file was opened again (retry 8)");
		const size_t l1 = LogSize();
		const bool later = ReadSector(r, SectorOfChunk(27), buf.data()) == static_cast<int>(kSector) && SectorOk(buf.data(), SectorOfChunk(27));
		Check(later && !Logged(l1, {"[iso]"}), "dead descriptor: later reads work on the new file");
	}
	// 6. a read that keeps failing, and the file that opens again has another size: not used; gives up after ~1.3 s; the same chunk then
	//    fails at once for a while (the CDVD layer's own 256 retries), and comes again when the failure is gone
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		Plan p;
		p.offset = 40 * kChunk;
		p.fread_fails = -1;
		p.pread_fails = -1;
		p.reopen_path = other.c_str();
		SetPlan(p);
		const size_t l0 = LogSize();
		auto t0 = std::chrono::steady_clock::now();
		int got = ReadSector(r, SectorOfChunk(40), buf.data());
		long long ms = MsSince(t0);
		Check(got <= 0 && ms >= 1000 && ms < 3000, "gives up: no data after about 1.3 s (" + std::to_string(ms) + " ms)");
		Check(Logged(l0, {"still fails after 7 retries", "another size", "not used", "errno 5", "the game gets a read error"}),
			"gives up: the line says so, with the other size and errno");
		const int preads = GetPlan().preads_at_offset;
		t0 = std::chrono::steady_clock::now();
		bool fast = true;
		for (int i = 0; i < 20; i++)
			fast = ReadSector(r, SectorOfChunk(40), buf.data()) <= 0 && fast;
		ms = MsSince(t0);
		Check(fast && ms < 300 && GetPlan().preads_at_offset == preads, "gives up: 20 more tries of that chunk fail at once (" + std::to_string(ms) + " ms), no pread");
		Plan clear;
		SetPlan(clear);
		got = ReadSector(r, SectorOfChunk(40), buf.data());
		Check(got == static_cast<int>(kSector) && SectorOk(buf.data(), SectorOfChunk(40)), "gives up: the chunk comes again once it reads (the image was never closed)");
	}
	// 7. the file does not open again: gives up, says why
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		Plan p;
		p.offset = 45 * kChunk;
		p.fread_fails = -1;
		p.pread_fails = -1;
		p.reopen_fails = true;
		SetPlan(p);
		const size_t l0 = LogSize();
		const int got = ReadSector(r, SectorOfChunk(45), buf.data());
		Check(got <= 0 && Logged(l0, {"still fails after 7 retries", "did not open again", "No such file"}), "no reopen: gives up, the line says the file did not open");
	}
	// 8. Close while the retries wait: they stop at once (Close does not sit out the 1.3 s)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		Plan p;
		p.offset = 50 * kChunk;
		p.fread_fails = -1;
		p.pread_fails = -1;
		SetPlan(p);
		const size_t l0 = LogSize();
		r.BeginRead(buf.data(), SectorOfChunk(50), 1);
		std::this_thread::sleep_for(std::chrono::milliseconds(100)); // in the retries now
		const auto t0 = std::chrono::steady_clock::now();
		r.Close();
		const long long ms = MsSince(t0);
		Check(ms < 200, "Close during the retries: back in " + std::to_string(ms) + " ms");
		Check(Logged(l0, {"retries of the read at offset 6553600 stopped"}), "Close during the retries: the line says they stopped");
		SetPlan(Plan());
		Check(r.Open(path, &error), "the image opens again");
		r.SetBlockSize(kSector);
	}
	// 9. the switch file iso_noretry: the read fails at once, as before 187
	{
		{
			std::lock_guard<std::mutex> lock(g_flags_lock);
			g_flags.insert("iso_noretry");
		}
		Plan p;
		p.offset = 55 * kChunk;
		p.fread_fails = -1;
		p.pread_fails = -1;
		SetPlan(p);
		const size_t l0 = LogSize();
		const auto t0 = std::chrono::steady_clock::now();
		const int got = ReadSector(r, SectorOfChunk(55), buf.data());
		const long long ms = MsSince(t0);
		Check(got <= 0 && ms < 300 && GetPlan().preads_at_offset == 0 && Logged(l0, {"offset 7208960", "a request"}) && !Logged(l0, {"retry"}),
			"iso_noretry: fails at once (" + std::to_string(ms) + " ms), said, no retry");
		std::lock_guard<std::mutex> lock(g_flags_lock);
		g_flags.clear();
	}
	// 10. past the end of the image (a truncated ISO): fails at once, no [iso] line, as before 187; the last whole sector still reads
	{
		SetPlan(Plan());
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const size_t l0 = LogSize();
		const auto t0 = std::chrono::steady_clock::now();
		const int past = r.ReadSync(buf.data(), kSectors + 10, 1);
		const int across = r.ReadSync(buf.data(), kSectors - 1, 2); // the last sector and one after the end
		const int begin_past = ReadSector(r, kSectors + 100, buf.data());
		const long long ms = MsSince(t0);
		Check(past <= 0 && begin_past <= 0 && across != static_cast<int>(2 * kSector) && ms < 300 && !Logged(l0, {"[iso]"}) &&
				  GetPlan().preads == 0,
			"past the end: fails at once (" + std::to_string(ms) + " ms), no [iso] line, no retry");
		const bool last = r.ReadSync(buf.data(), kSectors - 1, 1) == static_cast<int>(kSector) && SectorOk(buf.data(), kSectors - 1);
		Check(last, "past the end: the last whole sector reads");
	}
	r.Close();
	std::printf("%s\n", s_failed == 0 ? "ALL PASSED" : "SOME FAILED");
	return s_failed == 0 ? 0 : 1;
}
