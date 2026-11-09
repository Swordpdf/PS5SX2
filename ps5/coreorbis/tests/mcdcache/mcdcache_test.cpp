// Cached memory-card write recovery (AI-assisted).
// The production FlushCache body is included verbatim by test-mcdcache.sh. Only the surrounding class, timer and
// error reporting are stand-ins; reads/writes/seeks/flushes use real libc FILE streams with fault-injecting callbacks.
// All data is synthetic and in memory. No player files are opened.
// SPDX-License-Identifier: GPL-3.0+

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using u8 = uint8_t;
using u32 = uint32_t;
using s64 = int64_t;
using uint = unsigned;
#include "mcdcache_size.inc"

std::atomic<u32> g_orbis_mcd_flush_kib{0};
struct OrbisMcdTimer
{
	OrbisMcdTimer() {}
};
static int s_errors = 0, s_failures = 0, s_checks = 0;
namespace Host
{
void ReportErrorAsync(const char*, const char*) { ++s_errors; }
}
namespace FileSystem
{
int FSeek64(FILE* f, s64 offset, int whence) { return fseeko(f, offset, whence); }
}
class FileMemoryCard
{
public:
	FILE* m_file[8] = {};
	std::vector<u8> m_cache[8], m_dirty[8];
	bool m_dirty_any[8] = {};
	void FlushCache(uint slot, const char* why);
};
#include "mcdcache_actual.inc"

enum class Fault
{
	None,
	Seek,
	Write,
	ShortWrite,
	SecondWrite,
	Flush,
};
struct Disk
{
	std::vector<u8> data = std::vector<u8>(MC2_ERASE_SIZE * 3, 0x37);
	size_t pos = 0;
	Fault fault = Fault::None;
	unsigned writes = 0, seeks = 0;
	bool injected = false;
};

static long WriteBytes(Disk& disk, const char* bytes, size_t size)
{
	++disk.writes;
	if (disk.fault == Fault::Write || disk.fault == Fault::Flush ||
		(disk.fault == Fault::SecondWrite && disk.writes >= 2) ||
		(disk.fault == Fault::ShortWrite && disk.injected))
	{
		disk.injected = true;
		errno = ENOSPC;
		return -1;
	}
	if (disk.fault == Fault::ShortWrite)
	{
		disk.injected = true;
		size /= 2;
	}
	if (disk.pos + size > disk.data.size())
		disk.data.resize(disk.pos + size);
	std::memcpy(disk.data.data() + disk.pos, bytes, size);
	disk.pos += size;
	return static_cast<long>(size);
}

static int64_t SeekBytes(Disk& disk, int64_t offset, int whence)
{
	++disk.seeks;
	if (disk.fault == Fault::Seek)
	{
		disk.injected = true;
		errno = EIO;
		return -1;
	}
	const int64_t pos = offset + (whence == SEEK_CUR ? static_cast<int64_t>(disk.pos) :
		whence == SEEK_END ? static_cast<int64_t>(disk.data.size()) : 0);
	if (pos < 0)
	{
		errno = EINVAL;
		return -1;
	}
	disk.pos = static_cast<size_t>(pos);
	return pos;
}

static FILE* OpenDisk(Disk& disk)
{
#if defined(__linux__)
	cookie_io_functions_t io = {};
	io.write = [](void* cookie, const char* data, size_t size) -> ssize_t {
		return WriteBytes(*static_cast<Disk*>(cookie), data, size);
	};
	io.seek = [](void* cookie, off64_t* offset, int whence) {
		const int64_t pos = SeekBytes(*static_cast<Disk*>(cookie), *offset, whence);
		if (pos < 0)
			return -1;
		*offset = pos;
		return 0;
	};
	return fopencookie(&disk, "w", io);
#else
	return funopen(&disk, nullptr,
		[](void* cookie, const char* data, int size) { return static_cast<int>(WriteBytes(*static_cast<Disk*>(cookie), data, size)); },
		[](void* cookie, fpos_t offset, int whence) -> fpos_t { return SeekBytes(*static_cast<Disk*>(cookie), offset, whence); }, nullptr);
#endif
}

static void Check(bool ok, const char* scenario, const char* what)
{
	++s_checks;
	std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", scenario, what);
	s_failures += !ok;
}

static void Run(Fault fault, const char* name)
{
	Disk disk;
	FileMemoryCard card;
	FILE* file = OpenDisk(disk);
	Check(file != nullptr, name, "test stream opens");
	if (!file)
		return;
	char buffer[MC2_ERASE_SIZE * 4];
	// The flush case writes one contiguous range into a large buffer, so only fflush encounters the error.
	setvbuf(file, fault == Fault::Flush ? buffer : nullptr, fault == Fault::Flush ? _IOFBF : _IONBF, sizeof(buffer));
	card.m_file[0] = file;
	card.m_cache[0] = disk.data;
	std::fill(card.m_cache[0].begin(), card.m_cache[0].begin() + MC2_ERASE_SIZE, 0xa5);
	std::fill(card.m_cache[0].begin() + MC2_ERASE_SIZE * 2, card.m_cache[0].end(), 0x5a);
	card.m_dirty[0] = fault == Fault::Flush ? std::vector<u8>{1, 1, 1} : std::vector<u8>{1, 0, 1};
	const std::vector<u8> pending = card.m_dirty[0];
	card.m_dirty_any[0] = true;
	disk.fault = fault;
	const int errors_before = s_errors;
	card.FlushCache(0, name);
	if (fault != Fault::None)
	{
		Check(disk.injected, name, "I/O failure was injected");
		Check(s_errors == errors_before + 1, name, "failure is reported");
		Check(card.m_dirty_any[0] && card.m_dirty[0] == pending, name, "every original dirty range remains pending");
		if (fault == Fault::SecondWrite)
			Check(std::equal(disk.data.begin(), disk.data.begin() + MC2_ERASE_SIZE, card.m_cache[0].begin()), name,
				"first range persisted before second range failed");
		disk.fault = Fault::None;
		// No new game write and no test-side clearerr: production must recover its own stream state.
		card.FlushCache(0, "retry after storage recovered");
		Check(s_errors == errors_before + 1, name, "successful retry reports no further failure");
	}
	Check(disk.data == card.m_cache[0], name, "all cached changes persist after successful flush or retry");
	Check(std::all_of(disk.data.begin() + MC2_ERASE_SIZE, disk.data.begin() + MC2_ERASE_SIZE * 2,
		[](u8 value) { return value == 0x37; }), name, "unchanged block is preserved");
	Check(!card.m_dirty_any[0] && std::all_of(card.m_dirty[0].begin(), card.m_dirty[0].end(), [](u8 value) { return value == 0; }),
		name, "successful persistence clears dirty state");
	const unsigned writes = disk.writes, seeks = disk.seeks;
	card.FlushCache(0, "already clean");
	Check(disk.writes == writes && disk.seeks == seeks, name, "a clean cache performs no I/O");
	fclose(file);
}

int main()
{
	Run(Fault::None, "successful baseline");
	Run(Fault::Seek, "seek failure");
	Run(Fault::Write, "write failure");
	Run(Fault::ShortWrite, "short write");
	Run(Fault::SecondWrite, "second dirty range fails");
	Run(Fault::Flush, "buffered flush failure");
	std::printf("%d/%d checks passed; %d failed\n", s_checks - s_failures, s_checks, s_failures);
	return s_failures ? 1 : 0;
}
