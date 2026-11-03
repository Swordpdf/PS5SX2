// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "FlatFileReader.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Error.h"

#include <cerrno>
#include <chrono> // 187
#include <cstring>
#include <unistd.h> // 187: pread

static constexpr size_t CHUNK_SIZE = 128 * 1024;

// 187 (audit io-2): images on a USB drive. 6 of 174 such sessions of build 177 ended in 'CDVD READ ERROR' after 37 min to 9.4 h
// (none of 807 from /data), and the game froze: a failed read returned -1 or 0 with nothing said, and the CDVD layer's 256 retries
// (CDVD.cpp) came and failed within about 0.3 s. Now:
// - every failed read says what failed: offset, size, errno, ferror and feof, and what it was for ([iso] lines: an image's first 20,
//   then a count at most every 10 s);
// - a failed read of data the game waits for, on the read thread (ThreadedFileReader::ReadFor::Request; never the read-ahead, nor
//   ReadSync's own read under the reader's lock), is retried: pread on the descriptor after 10, 20, 40, ... ms for about 1.5 s, then
//   once more after opening the file again (a drive that went away and came back leaves the old descriptor dead). Close and Open
//   stop the waits within ~5 ms (RetryWait). A chunk given up on fails at once for the next 5 s, so the CDVD layer's own retries
//   do not each wait again. The switch file iso_noretry gives back the read that fails at once.
// This file's reader serves .iso/.bin/.img and every other image that is not CHD, CSO, ZSO, GZ or a block dump (InputIsoFile.cpp),
// on any drive, and on an NFS share too: OrbisNfs.cpp answers fileno, pread, rewind, fopen and fclose for its own files.
bool OrbisFlag(const char* name) __attribute__((weak)); // OrbisPaths.h (weak: absent where the app's paths are not linked)

namespace
{
using Clock = std::chrono::steady_clock;
constexpr auto kRetryFor = std::chrono::milliseconds(1500); // pread retries within this long (waits of 10, 20, ... 640 ms)
constexpr auto kHoldOff = std::chrono::seconds(5);          // a chunk given up on fails at once for this long
constexpr u32 kFreeLines = 20;                              // an image's failure lines before they are counted

// The open image's failures (there is one image at a time, CDVDisoReader.cpp's; Open2 starts afresh). One thread at a time uses
// it: ReadChunk runs on the read thread, or on ReadSync's caller while the read thread is idle (ThreadedFileReader.cpp).
struct IsoReadLog
{
	u32 lines = 0;                // failure lines printed
	u32 unsaid = 0;               // failures since the last line, not printed
	Clock::time_point said;       // the last failure line
	u32 outcomes = 0;             // retry outcome lines printed (at most 100)
	s64 gave_up_chunk = -1;       // the chunk the last retries gave up on ...
	Clock::time_point gave_up_at; // ... and when
};
IsoReadLog s_log;

// All `size` bytes at `offset` by pread on `fd`, a short read continued. The bytes that came; -1 when an error came first.
ssize_t PreadAll(int fd, void* dst, size_t size, u64 offset)
{
	size_t done = 0;
	while (done < size)
	{
		const ssize_t n = pread(fd, static_cast<char*>(dst) + done, size - done, static_cast<off_t>(offset + done));
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return done ? static_cast<ssize_t>(done) : -1;
		if (n == 0)
			break;
		done += static_cast<size_t>(n);
	}
	return static_cast<ssize_t>(done);
}

// One failed read, said: an image's first kFreeLines in full, then a count at most every 10 s (from the next failure on).
void LogFailure(const char* what, u64 offset, u32 size, size_t got, int err, bool ferr, bool eof, const char* for_what)
{
	const Clock::time_point now = Clock::now();
	if (s_log.lines < kFreeLines)
	{
		s_log.lines++;
		s_log.said = now;
		Console.Error("[iso] %s failed at offset %llu (%u bytes, %s): errno %d (%s), ferror %d, feof %d, %zu bytes came%s", what,
			static_cast<unsigned long long>(offset), size, for_what, err, err ? std::strerror(err) : "none", ferr ? 1 : 0, eof ? 1 : 0, got,
			s_log.lines == kFreeLines ? " (further failures are counted every 10 s)" : "");
		return;
	}
	s_log.unsaid++;
	if (now - s_log.said >= std::chrono::seconds(10))
	{
		Console.Error("[iso] %u more failed reads (the last at offset %llu, %s: errno %d)", s_log.unsaid,
			static_cast<unsigned long long>(offset), for_what, err);
		s_log.unsaid = 0;
		s_log.said = now;
	}
}
} // namespace

FlatFileReader::FlatFileReader() = default;

FlatFileReader::~FlatFileReader()
{
	pxAssert(!m_file);
}

bool FlatFileReader::Open2(std::string filename, Error* error)
{
	s_log = IsoReadLog(); // 187: a new image (see the top)
	m_filename = std::move(filename);
	if (!(m_file = FileSystem::OpenCFile(m_filename.c_str(), "rb", error)))
		return false;

	const s64 filesize = FileSystem::FSize64(m_file);
	if (filesize <= 0)
	{
		Error::SetStringView(error, "Failed to determine file size.");
		Close2();
		return false;
	}

	m_file_size = static_cast<u64>(filesize);
	return true;
}

bool FlatFileReader::Precache2(ProgressCallback* progress, Error* error)
{
	if (!m_file || !CheckAvailableMemoryForPrecaching(m_file_size, error))
		return false;

	m_file_cache = std::make_unique_for_overwrite<u8[]>(m_file_size);
	if (FileSystem::FSeek64(m_file, 0, SEEK_SET) != 0 ||
		FileSystem::ReadFileWithProgress(
			m_file, m_file_cache.get(), m_file_size, progress, error) != m_file_size)
	{
		m_file_cache.reset();
		return false;
	}

	std::fclose(m_file);
	m_file = nullptr;
	return true;
}

ThreadedFileReader::Chunk FlatFileReader::ChunkForOffset(u64 offset)
{
	ThreadedFileReader::Chunk chunk = {};
	if (offset >= m_file_size)
	{
		chunk.chunkID = -1;
	}
	else
	{
		chunk.chunkID = offset / CHUNK_SIZE;
		chunk.length = static_cast<u32>(std::min<u64>(m_file_size - offset, CHUNK_SIZE));
		chunk.offset = static_cast<u64>(chunk.chunkID) * CHUNK_SIZE;
	}

	return chunk;
}

int FlatFileReader::ReadChunk(void* dst, s64 blockID)
{
	if (blockID < 0)
		return -1;

	const u64 file_offset = static_cast<u64>(blockID) * CHUNK_SIZE;
	if (m_file_cache)
	{
		if (file_offset >= m_file_size)
			return -1;

		const u64 read_size = std::min<u64>(m_file_size - file_offset, CHUNK_SIZE);
		std::memcpy(dst, &m_file_cache[file_offset], read_size);
		return static_cast<int>(read_size);
	}

	const u32 read_size = static_cast<u32>(std::min<u64>(m_file_size - file_offset, CHUNK_SIZE));
	// 187: the same seek and read; errno cleared first so that a failure's line never shows an older one, and the bytes counted
	errno = 0;
	const bool seek_failed = FileSystem::FSeek64(m_file, file_offset, SEEK_SET) != 0;
	const size_t got = seek_failed ? 0 : std::fread(dst, 1, read_size, m_file);
	if (got == read_size)
		return static_cast<int>(read_size);

	// 187: what failed (see the top), then for data the game waits for, the retries
	const int err = errno;
	const char* const for_what = m_readFor == ReadFor::Request ? "a request" : m_readFor == ReadFor::Caller ? "a direct read" : "read-ahead";
	LogFailure(seek_failed ? "seek" : "read", file_offset, read_size, got, err, std::ferror(m_file) != 0, std::feof(m_file) != 0, for_what);
	const int failed = seek_failed ? -1 : 0; // what this returned before 187
	if (m_readFor != ReadFor::Request || file_offset >= m_file_size || (OrbisFlag && OrbisFlag("iso_noretry")))
		return failed;
	const Clock::time_point t0 = Clock::now();
	if (blockID == s_log.gave_up_chunk && t0 - s_log.gave_up_at < kHoldOff)
		return failed;

	// The FILE's error and end-of-file flags off: rewind (every read seeks first anyway), not clearerr, because OrbisNfs.cpp's FILEs
	// are its own and it stands in front of rewind but not of clearerr (OrbisNfs.wrap).
	std::rewind(m_file);
	int tries = 0, last_err = err;
	bool ok = false, stopped = false, reopened = false;
	for (u32 wait_ms = 10; !ok && Clock::now() - t0 + std::chrono::milliseconds(wait_ms) <= kRetryFor; wait_ms *= 2)
	{
		if (!RetryWait(wait_ms))
		{
			stopped = true;
			break;
		}
		tries++;
		errno = 0;
		ok = PreadAll(fileno(m_file), dst, read_size, file_offset) == static_cast<ssize_t>(read_size);
		if (!ok)
			last_err = errno;
	}
	char reopen[160] = "";
	if (!ok && !stopped && RetryWait(0))
	{
		// once more with the file opened again, when it opens with the size it had
		Error open_error;
		std::FILE* const fresh = FileSystem::OpenCFile(m_filename.c_str(), "rb", &open_error);
		const s64 fresh_size = fresh ? FileSystem::FSize64(fresh) : -1;
		if (fresh && fresh_size == static_cast<s64>(m_file_size))
		{
			std::fclose(m_file);
			m_file = fresh;
			reopened = true;
			tries++;
			errno = 0;
			ok = FileSystem::FSeek64(m_file, file_offset, SEEK_SET) == 0 && std::fread(dst, 1, read_size, m_file) == read_size;
			if (!ok)
				last_err = errno;
			std::snprintf(reopen, sizeof(reopen), "the file opened again");
		}
		else
		{
			if (fresh)
			{
				std::snprintf(reopen, sizeof(reopen), "the file opened again with another size (%lld bytes, was %llu): not used",
					static_cast<long long>(fresh_size), static_cast<unsigned long long>(m_file_size));
				std::fclose(fresh);
			}
			else
				std::snprintf(reopen, sizeof(reopen), "the file did not open again (%s)", open_error.GetDescription().c_str());
		}
	}
	else if (!ok)
		stopped = true;

	const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
	const bool say = s_log.outcomes < 100;
	s_log.outcomes += say ? 1 : 0;
	if (ok)
	{
		if (say)
			Console.Warning("[iso] the read at offset %llu (%u bytes) came on retry %d, after %lld ms%s", static_cast<unsigned long long>(file_offset),
				read_size, tries, ms, reopened ? ", with the file opened again" : "");
		return static_cast<int>(read_size);
	}
	if (stopped)
	{
		if (say)
			Console.Warning("[iso] retries of the read at offset %llu stopped after %d in %lld ms: the request was cancelled (the image closes or "
							"another one opens)", static_cast<unsigned long long>(file_offset), tries, ms);
		return failed;
	}
	s_log.gave_up_chunk = blockID;
	s_log.gave_up_at = Clock::now();
	if (say)
		Console.Error("[iso] the read at offset %llu (%u bytes) still fails after %d retries in %lld ms, %s (errno %d: %s): the game gets a "
					  "read error", static_cast<unsigned long long>(file_offset), read_size, tries, ms, reopen, last_err,
			last_err ? std::strerror(last_err) : "none");
	return failed;
}

void FlatFileReader::Close2()
{
	if (!m_file)
		return;

	std::fclose(m_file);
	m_file = nullptr;
	m_file_size = 0;
}

u32 FlatFileReader::GetBlockCount() const
{
	return static_cast<u32>(m_file_size / m_blocksize);
}
