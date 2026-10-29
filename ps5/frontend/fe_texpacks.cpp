// PS5 port frontend: HD texture packs from archive.org (see fe_texpacks.h). 2026-10-05, AI-assisted; needs proper testing
// on the console.
//
// Copyright (C) 2026 swordpdf
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe_texpacks.h"
#include "fe_sevenzip.h" // b176: the .7z reader (LZMA SDK), streamed
#include "fe_texwrite.h" // 2026-10-29: the files written by a process of their own (the title's write allowance)
#include "fe_settings.h"

#include "OrbisTexturePak.h" // pcsx2/: the one-file pack's format, shared with GSTextureReplacements.cpp

#include "archive.h"
#include "archive_entry.h"
#include "rapidjson/document.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace fe
{
namespace
{
using State = TexturePackStatus::State;

// ---- MD5 (RFC 1321), to check a download against archive.org's list.
class Md5
{
public:
	void Update(const void* data, size_t n)
	{
		const uint8_t* p = static_cast<const uint8_t*>(data);
		m_length += n;
		while (n > 0)
		{
			const size_t take = std::min(n, sizeof(m_buffer) - m_used);
			std::memcpy(m_buffer + m_used, p, take);
			m_used += take;
			p += take;
			n -= take;
			if (m_used == sizeof(m_buffer))
			{
				Block(m_buffer);
				m_used = 0;
			}
		}
	}
	std::string Hex() const
	{
		Md5 copy = *this;
		const uint64_t bits = copy.m_length * 8;
		const uint8_t pad = 0x80;
		copy.Update(&pad, 1);
		const uint8_t zero = 0;
		while (copy.m_used != 56)
			copy.Update(&zero, 1);
		uint8_t len[8];
		for (int i = 0; i < 8; i++)
			len[i] = static_cast<uint8_t>(bits >> (8 * i));
		copy.Update(len, 8);
		const uint32_t words[4] = {copy.m_a, copy.m_b, copy.m_c, copy.m_d};
		static const char* const hex = "0123456789abcdef";
		std::string out;
		for (uint32_t w : words)
			for (int i = 0; i < 4; i++)
			{
				const uint8_t b = static_cast<uint8_t>(w >> (8 * i));
				out += hex[b >> 4];
				out += hex[b & 15];
			}
		return out;
	}

private:
	static uint32_t Rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
	void Block(const uint8_t* p)
	{
		static const uint32_t K[64] = {0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
			0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
			0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
			0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9,
			0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
			0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0,
			0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
		static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14,
			20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
			6, 10, 15, 21};
		uint32_t m[16];
		for (int i = 0; i < 16; i++)
			m[i] = static_cast<uint32_t>(p[i * 4]) | (static_cast<uint32_t>(p[i * 4 + 1]) << 8) | (static_cast<uint32_t>(p[i * 4 + 2]) << 16) |
			       (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
		uint32_t a = m_a, b = m_b, c = m_c, d = m_d;
		for (int i = 0; i < 64; i++)
		{
			uint32_t f;
			int g;
			if (i < 16)
				f = (b & c) | (~b & d), g = i;
			else if (i < 32)
				f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
			else if (i < 48)
				f = b ^ c ^ d, g = (3 * i + 5) % 16;
			else
				f = c ^ (b | ~d), g = (7 * i) % 16;
			f += a + K[i] + m[g];
			a = d;
			d = c;
			c = b;
			b += Rol(f, S[i]);
		}
		m_a += a;
		m_b += b;
		m_c += c;
		m_d += d;
	}

	uint32_t m_a = 0x67452301, m_b = 0xefcdab89, m_c = 0x98badcfe, m_d = 0x10325476;
	uint64_t m_length = 0;
	uint8_t m_buffer[64] = {};
	size_t m_used = 0;
};

// ---- files

bool IsDir(const std::string& p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool Exists(const std::string& p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

uint64_t FileSize(const std::string& p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? static_cast<uint64_t>(st.st_size) : 0;
}

bool MakeDirs(const std::string& path)
{
	for (size_t i = 1; i <= path.size(); i++)
		if (i == path.size() || path[i] == '/')
		{
			const std::string sub = path.substr(0, i);
			if (mkdir(sub.c_str(), 0777) != 0 && errno != EEXIST)
				return false;
		}
	return IsDir(path);
}

// Removes a file or a folder and everything in it (links are removed, never followed).
bool RemoveTree(const std::string& path)
{
	struct stat st;
	if (lstat(path.c_str(), &st) != 0)
		return errno == ENOENT;
	if (!S_ISDIR(st.st_mode))
		return unlink(path.c_str()) == 0;
	bool ok = true;
	if (DIR* d = opendir(path.c_str()))
	{
		std::vector<std::string> names;
		while (dirent* e = readdir(d))
			if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0)
				names.emplace_back(e->d_name);
		closedir(d);
		for (const std::string& n : names)
			ok = RemoveTree(path + "/" + n) && ok;
	}
	return rmdir(path.c_str()) == 0 && ok;
}

// Moves `from` to `to`; into a folder that is there already, file by file (a file of the same name is replaced).
bool MoveTree(const std::string& from, const std::string& to)
{
	if (!Exists(to) || !IsDir(from) || !IsDir(to))
		return rename(from.c_str(), to.c_str()) == 0;
	bool ok = true;
	if (DIR* d = opendir(from.c_str()))
	{
		std::vector<std::string> names;
		while (dirent* e = readdir(d))
			if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0)
				names.emplace_back(e->d_name);
		closedir(d);
		for (const std::string& n : names)
			ok = MoveTree(from + "/" + n, to + "/" + n) && ok;
	}
	rmdir(from.c_str());
	return ok;
}

// One whole file. `no_space` when the disk is full.
bool WriteWhole(const std::string& path, const std::vector<uint8_t>& data, bool& no_space)
{
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
	{
		no_space = errno == ENOSPC;
		return false;
	}
	size_t at = 0;
	while (at < data.size())
	{
		const ssize_t w = write(fd, data.data() + at, data.size() - at);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
		{
			no_space = w < 0 && errno == ENOSPC;
			close(fd);
			return false;
		}
		at += static_cast<size_t>(w);
	}
	if (close(fd) != 0)
	{
		no_space = errno == ENOSPC;
		return false;
	}
	return true;
}

// 2026-10-05 (AI-assisted): the unpacked files' writers. On the console each new file costs tens of milliseconds of
// file-system work: Ratchet & Clank's 13,200 files took more than 15 minutes written one at a time (pr9h). A few threads
// write them while the unpacking thread decompresses the next, with at most kMaxBytes waiting.
class FileWriters
{
public:
	FileWriters(int threads, const std::function<void(const char*)>& thread_start)
	{
		for (int i = 0; i < threads; i++)
			m_threads.emplace_back([this, thread_start] {
				if (thread_start)
					thread_start("writer");
				Run();
			});
	}
	~FileWriters() { Finish(); }
	FileWriters(const FileWriters&) = delete;
	FileWriters& operator=(const FileWriters&) = delete;

	// Queues one file, waiting while the queue is full. False once a write has failed.
	bool Put(std::string path, std::vector<uint8_t> data)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_room.wait(lock, [&] { return m_failed || m_items.empty() || (m_bytes + data.size() <= kMaxBytes && m_items.size() < kMaxItems); });
		if (m_failed)
			return false;
		m_bytes += data.size();
		m_items.push_back(Item{std::move(path), std::move(data)});
		m_work.notify_one();
		return true;
	}

	// Waits until every queued file is written (or dropped after a failure). True when all were written.
	bool Finish()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_done = true;
		}
		m_work.notify_all();
		for (std::thread& t : m_threads)
			if (t.joinable())
				t.join();
		std::lock_guard<std::mutex> lock(m_mutex);
		return !m_failed;
	}

	uint32_t Written()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_written;
	}
	bool NoSpace()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_no_space;
	}
	std::string FailedPath()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_failed_path;
	}
	// Seconds the writers have spent writing, all of them together.
	double WriteSeconds() const { return m_write_us.load() / 1e6; }

private:
	static constexpr size_t kMaxBytes = 64u << 20;
	static constexpr size_t kMaxItems = 2048;
	struct Item
	{
		std::string path;
		std::vector<uint8_t> data;
	};

	void Run()
	{
		for (;;)
		{
			Item item;
			bool skip = false;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_work.wait(lock, [&] { return m_done || !m_items.empty(); });
				if (m_items.empty())
					return;
				item = std::move(m_items.front());
				m_items.pop_front();
				skip = m_failed; // after a failure the rest is only dropped
			}
			bool no_space = false;
			const auto t0 = std::chrono::steady_clock::now();
			const bool ok = skip || WriteWhole(item.path, item.data, no_space);
			m_write_us += static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_bytes -= item.data.size();
				if (ok && !skip)
					m_written++;
				else if (!ok && !m_failed)
				{
					m_failed = true;
					m_no_space = no_space;
					m_failed_path = item.path;
				}
			}
			m_room.notify_all();
		}
	}

	std::vector<std::thread> m_threads;
	std::mutex m_mutex;
	std::condition_variable m_work, m_room;
	std::deque<Item> m_items;
	size_t m_bytes = 0;
	bool m_done = false, m_failed = false, m_no_space = false;
	uint32_t m_written = 0;
	std::string m_failed_path;
	std::atomic<uint64_t> m_write_us{0};
};

// 2026-10-05 (AI-assisted): a pack's replacements written into one file, <serial>/replacements.pak (pcsx2/OrbisTexturePak.h).
// The writers above still took Ratchet & Clank 2 most of an hour (pr9k: 3,385 files in 11 s, then 5 to 10 files a second
// with all four busy), while a test payload wrote 256 MB into one file in a second. Entries are appended through big
// buffers; the index and then the header go at the end, so a pack cut short is never read.
//
// 2.11 (swordpdf: "speed up texture pack extraction ... make sure to not slow the menu down"): the entries are STREAMED in (Begin, Append for each block the
// unpacker gives, End) instead of being collected whole first (an entry of hundreds of MB was a vector of that size, copied again into the buffer: on a console
// with ~195 MB to spare that is memory the system takes back from somewhere), and a thread of its own writes the full buffers while the unpacking thread
// fills the next (three buffers of 8 MiB): the unpacking is no longer held up by a write, nor the write by the unpacking. Flags, for a console that asks for
// them: texpack_sync (fdatasync after each buffer) and texpack_direct (O_DIRECT).
//
// 2026-10-29: the file goes through a TexOutFile: written by the texture pack writer's process when the manager has one (Options::remote; the
// console holds a title's own writes to ~2 MiB/s after its first ~1.3 GiB, fe_texwrite.h), by this process otherwise or when that one stops.
class PakWriter
{
public:
	struct Options
	{
		bool sync_each = false;
		bool direct = false;
		TexRemoteWriter* remote = nullptr;                 // 2026-10-29: the writer process, or null
		std::function<void(const std::string&)> log;        // 2026-10-29: its fallbacks are said here
		std::function<void(const char*)> thread_start;
		// b176: asked (every 50 ms) while the unpacking thread waits for a free buffer, so a stop is heard when the disk is slow (an 8 MiB buffer takes 4 s at the
		// 2 MB/s the console writes at times); true: Hand gives up. Unset: waits as before.
		std::function<bool()> stopping;
	};

	explicit PakWriter(Options options)
		: m_opt(std::move(options))
	{
	}
	~PakWriter()
	{
		StopWriter(); // m_out closes the file
	}
	PakWriter(const PakWriter&) = delete;
	PakWriter& operator=(const PakWriter&) = delete;

	bool Open(const std::string& path)
	{
		m_path = path;
		if (!m_out.Open(path, true, m_opt.remote, m_opt.direct, m_opt.log))
			return Fail(m_out.Errno());
		m_write_at = 0;
		for (std::vector<uint8_t>& b : m_bufs)
			b.assign(kBufferBytes, 0);
		m_lens.assign(m_bufs.size(), 0);
		m_fill = 0;
		m_fill_len = OrbisTexturePak::kHeaderSize; // the header's place, written last (the buffer is zeros there)
		m_offset = OrbisTexturePak::kHeaderSize;
		for (size_t i = 1; i < m_bufs.size(); i++)
			m_free.push_back(static_cast<int>(i));
		m_writer = std::thread([this] { WriterMain(); });
		return true;
	}

	// One entry, `name` its path under replacements/: Begin, any number of Append / AppendZeros, End.
	bool Begin(const std::string& name)
	{
		if (!m_out.IsOpen() || m_failed)
			return false;
		m_entry = OrbisTexturePak::Entry();
		m_entry.offset = m_offset;
		m_entry.name = name;
		return true;
	}
	bool Append(const void* data, size_t n)
	{
		const uint8_t* p = static_cast<const uint8_t*>(data);
		m_offset += n;
		while (n > 0)
		{
			const size_t take = std::min(n, kBufferBytes - m_fill_len);
			std::memcpy(m_bufs[static_cast<size_t>(m_fill)].data() + m_fill_len, p, take);
			m_fill_len += take;
			p += take;
			n -= take;
			if (m_fill_len == kBufferBytes && !Hand())
				return false;
		}
		return true;
	}
	bool AppendZeros(uint64_t n) // a gap in a sparse entry
	{
		static const std::vector<uint8_t> zeros(1u << 20, 0);
		while (n > 0)
		{
			const size_t take = static_cast<size_t>(std::min<uint64_t>(n, zeros.size()));
			if (!Append(zeros.data(), take))
				return false;
			n -= take;
		}
		return true;
	}
	bool End()
	{
		m_entry.size = m_offset - m_entry.offset;
		m_index.push_back(std::move(m_entry));
		return !m_failed;
	}

	// The rest of the data, the index, then the header. False (Error, NoSpace) when a write failed.
	bool Finish()
	{
		if (!m_out.IsOpen())
			return false;
		if (m_fill_len > 0 && !Hand(false))
		{
			StopWriter();
			return false;
		}
		StopWriter();
		if (m_failed)
			return false;
		std::vector<uint8_t> index;
		for (const OrbisTexturePak::Entry& e : m_index)
			OrbisTexturePak::AppendIndexEntry(index, e);
		if (!WriteAll(index.data(), index.size()))
			return false;
		const std::vector<uint8_t> header = OrbisTexturePak::MakeHeader(static_cast<uint32_t>(m_index.size()), m_offset, index.size());
		if (!m_out.PWrite(header.data(), header.size(), 0))
			return Fail(m_out.Errno());
		if (!m_out.Close())
			return Fail(m_out.Errno());
		return true;
	}

	uint32_t Count() const { return static_cast<uint32_t>(m_index.size()); }
	uint64_t Bytes() const { return m_offset; }
	bool NoSpace() const { return m_errno == ENOSPC; }
	bool Failed() const { return m_failed.load(); }
	// b176: a stop or a failure elsewhere: what is queued is dropped, not written (the file goes with the staging folder), and the writer thread ends at once.
	void Abort()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_failed = true;
		}
		m_room.notify_all();
		m_work.notify_all();
	}
	const std::string& Path() const { return m_path; }
	std::string Error() const { return std::strerror(m_errno ? m_errno : EIO); }
	// Seconds the writer thread spent in write() (and fdatasync), and seconds the unpacking thread waited for a free buffer (the writes were behind).
	double WriteSeconds() const { return m_write_us.load() / 1e6; }
	double WaitSeconds() const { return m_wait_us / 1e6; }
	// 2026-10-29: the bytes the writer process wrote, and the bytes this process wrote itself (after Finish, or the writer thread's end).
	uint64_t RemoteBytes() const { return m_out.RemoteBytes(); }
	uint64_t LocalBytes() const { return m_out.LocalBytes(); }

private:
	static constexpr size_t kBufferBytes = 8u << 20;

	bool Fail(int err)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_errno)
			m_errno = err ? err : EIO;
		m_failed = true;
		m_room.notify_all();
		return false;
	}

	// The full buffer goes to the writer; with `next`, a free one becomes the one being filled (waiting for the writer if none is).
	bool Hand(bool next = true)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		if (m_failed)
			return false;
		m_lens[static_cast<size_t>(m_fill)] = m_fill_len;
		m_full.push_back(m_fill);
		m_work.notify_one();
		m_fill_len = 0;
		if (!next)
			return true;
		const auto t0 = std::chrono::steady_clock::now();
		bool stopped = false;
		if (m_opt.stopping)
		{
			while (!m_failed && m_free.empty() && !stopped)
			{
				stopped = m_opt.stopping();
				if (!stopped)
					m_room.wait_for(lock, std::chrono::milliseconds(50));
			}
		}
		else
			m_room.wait(lock, [&] { return m_failed || !m_free.empty(); });
		m_wait_us += static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
		if (m_failed || (stopped && m_free.empty()))
			return false;
		m_fill = m_free.back();
		m_free.pop_back();
		return true;
	}

	void WriterMain()
	{
		if (m_opt.thread_start)
			m_opt.thread_start("pakwriter");
		for (;;)
		{
			int b;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_work.wait(lock, [&] { return m_quit || !m_full.empty(); });
				if (m_full.empty())
					return;
				b = m_full.front();
				m_full.pop_front();
			}
			const auto t0 = std::chrono::steady_clock::now();
			bool ok = !m_failed.load() && WriteAll(m_bufs[static_cast<size_t>(b)].data(), m_lens[static_cast<size_t>(b)]);
			if (ok && m_opt.sync_each && !m_out.Sync())
				ok = Fail(m_out.Errno());
			m_write_us += static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_free.push_back(b);
			}
			m_room.notify_all();
			(void)ok;
		}
	}

	void StopWriter()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_quit = true;
		}
		m_work.notify_all();
		if (m_writer.joinable())
			m_writer.join();
	}

	// The bytes at the file's end (the buffers, then the index): one call, which the TexOutFile sends to the writer process or writes itself.
	bool WriteAll(const uint8_t* p, size_t n)
	{
		if (!m_out.PWrite(p, n, m_write_at))
			return Fail(m_out.Errno());
		m_write_at += n;
		return true;
	}

	Options m_opt;
	std::string m_path;
	TexOutFile m_out;       // 2026-10-29 (was a descriptor of this process's)
	uint64_t m_write_at = 0; // where the next buffer goes in the file
	std::thread m_writer;
	std::mutex m_mutex;
	std::condition_variable m_work, m_room;
	std::array<std::vector<uint8_t>, 3> m_bufs;
	std::vector<size_t> m_lens;
	std::deque<int> m_full; // handed to the writer, in order
	std::vector<int> m_free;
	int m_fill = 0; // the buffer the unpacking thread is filling
	size_t m_fill_len = 0;
	bool m_quit = false;
	std::atomic<bool> m_failed{false};
	int m_errno = 0;
	uint64_t m_offset = 0;
	OrbisTexturePak::Entry m_entry;
	std::vector<OrbisTexturePak::Entry> m_index;
	std::atomic<uint64_t> m_write_us{0};
	uint64_t m_wait_us = 0;
};

// "<serial>/replacements/<name>" (any case of "replacements"): the serial and the name under replacements/, for the pack.
bool SplitReplacement(const std::string& rel, std::string& serial, std::string& name)
{
	const size_t slash = rel.find('/');
	if (slash == std::string::npos)
		return false;
	const size_t second = rel.find('/', slash + 1);
	if (second == std::string::npos || second + 1 >= rel.size())
		return false;
	std::string folder = rel.substr(slash + 1, second - slash - 1);
	for (char& c : folder)
		c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	if (folder != "replacements")
		return false;
	serial = rel.substr(0, slash);
	name = rel.substr(second + 1);
	return OrbisTexturePak::NameIsSane(name);
}

// "<serial>/replacements.pak" (any case): the pack file's own place, which nothing in the archive may take.
bool IsPakPlace(const std::string& rel)
{
	const size_t slash = rel.find('/');
	if (slash == std::string::npos)
		return false;
	const size_t second = rel.find('/', slash + 1);
	std::string part = rel.substr(slash + 1, second == std::string::npos ? std::string::npos : second - slash - 1);
	for (char& c : part)
		c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	return part == OrbisTexturePak::kFileName;
}

bool HasEntries(const std::string& dir)
{
	DIR* d = opendir(dir.c_str());
	if (!d)
		return false;
	bool any = false;
	while (dirent* e = readdir(d))
		if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0)
		{
			any = true;
			break;
		}
	closedir(d);
	return any;
}

std::string Lower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
	return s;
}

std::string Trim(const std::string& s)
{
	size_t b = 0, e = s.size();
	while (b < e && (s[b] == ' ' || s[b] == '\t'))
		b++;
	while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t'))
		e--;
	return s.substr(b, e - b);
}

// "key=value" lines.
std::map<std::string, std::string> ReadKeyValues(const std::string& path)
{
	std::map<std::string, std::string> out;
	std::string text;
	if (!settings::ReadFile(path, text))
		return out;
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = text.substr(at, end - at);
		const size_t eq = line.find('=');
		if (eq != std::string::npos)
			out[line.substr(0, eq)] = line.substr(eq + 1);
		at = end + 1;
	}
	return out;
}

std::string OneLine(std::string s)
{
	for (char& c : s)
		if (c == '\n' || c == '\r')
			c = ' ';
	return s;
}

const char* StateName(State s)
{
	switch (s)
	{
		case State::Loading: return "loading";
		case State::Unavailable: return "unavailable";
		case State::None: return "none";
		case State::Available: return "available";
		case State::Queued: return "queued";
		case State::Checking: return "checking";
		case State::Downloading: return "downloading";
		case State::Verifying: return "verifying";
		case State::Unpacking: return "unpacking";
		case State::Installing: return "installing";
		case State::Installed: return "installed";
		case State::Removing: return "removing";
		case State::Failed: return "failed";
		case State::NeedSpace: return "needs space";
	}
	return "?";
}

// ---- b176 (swordpdf: "we forgot to add the new texture packs links"): the .7z packs, read by fe_sevenzip.c ---------------------------------------------

// The extension of a pack's file name, for the names of its files in the work folder (<key>.rar, <key>.7z): ".rar", ".zip", ".7z"; ".rar" when the name
// has none that is letters and digits. (The old rule took the last four characters, which for "Zapper.7z" is "r.7z".)
std::string ExtOf(const std::string& name)
{
	const size_t dot = name.rfind('.');
	if (dot == std::string::npos || dot == 0 || name.size() - dot < 2 || name.size() - dot > 6)
		return ".rar";
	std::string ext = Lower(name.substr(dot));
	for (size_t i = 1; i < ext.size(); i++)
		if (!std::isalnum(static_cast<unsigned char>(ext[i])))
			return ".rar";
	return ext;
}

// Does the file start with a 7z signature? (A download is taken by what it is, not by its name.)
bool IsSevenZipFile(const std::string& path)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	unsigned char sig[6] = {};
	const size_t n = std::fread(sig, 1, sizeof(sig), f);
	std::fclose(f);
	static const unsigned char k7z[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
	return n == sizeof(sig) && std::memcmp(sig, k7z, sizeof(sig)) == 0;
}

// What the 7z unpack will not take on, or takes only whole: the numbers behind "Too big for the console".
constexpr uint64_t kSevenZipMargin = 48ull << 20;   // flexible memory left alone: the shelf and the system need it (no exceptions in this build: a failed `new` ends the app)
constexpr uint64_t kSevenZipRoom = 256ull << 20;    // free space wanted beyond the unpacked files
constexpr uint64_t kSevenZipHeaderFloor = 16ull << 20; // what the header phase may take before its real size is known (typical peaks 2 to 27 MB)
constexpr uint64_t kLooseEntryMax = 8ull << 20;     // a file of the pack that is not a texture (not under replacements/) is kept whole in memory: that big at most
constexpr uint64_t kLooseTotalMax = 64ull << 20;    // and that much of them together (the file writers' queue is that big)
constexpr int kLimitsRevision = 1;                  // raised when Sz7LimitsDefault's numbers, the coders fe_sevenzip.c reads or the rules for which files are kept (TexturePackTarget ...) change: a refusal kept in <key>.<serial>.refused counts only under the limits it was made with
constexpr int kMaxUnpackTries = 2;                  // 7z unpacks begun and never ended, after which a pack isn't carried on with by itself

struct Sz7Deleter
{
	void operator()(Sz7Archive* z) const { Sz7Free(z); }
};

// How a 7z problem is told to the player, and what becomes of the download.
struct SevenZipVerdict
{
	std::string message;
	bool drop = false; // the pack can never be used here: its download and job go, the refusal is kept (<key>.<serial>.refused)
	bool damaged = false; // the archive is at fault: a download with no checksum of its own is deleted so the next try fetches it afresh
};

SevenZipVerdict JudgeSevenZip(Sz7Code code, const std::string& text, bool header_phase)
{
	SevenZipVerdict v;
	switch (code)
	{
		case SZ7_NOT_7Z:
			v.message = "Can't open the pack: it is not a 7z file";
			v.damaged = true;
			break;
		case SZ7_HEADER_BAD:
		case SZ7_EOF:
		case SZ7_CRC:
		case SZ7_DATA:
			v.message = "The pack is damaged: " + text;
			v.damaged = true;
			break;
		case SZ7_IO: v.message = "Couldn't read the pack: " + text; break;
		case SZ7_NO_MEMORY:
			if (header_phase) // the header asks for more than the allowance: a property of the file, not of the moment
			{
				v.message = "Too big for the console's memory: " + text;
				v.drop = true;
			}
			else
				v.message = "Not enough memory to unpack this pack right now";
			break;
		case SZ7_DICT_TOO_BIG:
		case SZ7_HEADER_TOO_BIG:
		case SZ7_TOO_MANY_FILES:
		case SZ7_ENTRY_TOO_BIG:
		case SZ7_TOO_BIG:
			v.message = "Too big for the console: " + text;
			v.drop = true;
			break;
		case SZ7_UNSUPPORTED:
		case SZ7_ENCRYPTED:
		case SZ7_NO_CRC:
			v.message = "Can't use this pack: " + text;
			v.drop = true;
			break;
		case SZ7_SINK:
		case SZ7_CANCELLED:
		case SZ7_OK: break;
	}
	return v;
}

// One 7z unpack: which entries are wanted (the same rules as the libarchive path, so a pack gives the same files whichever reader opens it), and the consumer
// that fe_sevenzip.c streams them into (the same PakWriter and FileWriters).
struct SevenZipRun
{
	using Clock = std::chrono::steady_clock;

	// in
	std::string serial, staging;
	std::function<bool()> stopping;
	std::function<void(uint64_t)> progress;
	std::function<void(const std::string&)> log;
	std::function<void(const char*)> thread_start;
	std::function<std::string()> memory_text; // "flexible memory free 150 MiB"
	bool sync_each = false, direct = false;
	TexRemoteWriter* remote = nullptr; // 2026-10-29
	uint64_t packed_total = 0;

	// the wanted entries (the first pass counts them, the second runs the same decisions again)
	struct Pick
	{
		bool keep = false, pak = false;
		std::string rel, pak_serial, pak_name;
	};
	std::set<std::string> pak_serials; // the serial folder of every texture (an entry under replacements/): which games the pack holds textures for
	uint64_t kept_total = 0, loose_bytes = 0;
	uint32_t loose_files = 0, loose_left_out = 0, not_wanted = 0;

	void Reset()
	{
		pak_serials.clear();
		kept_total = loose_bytes = 0;
		loose_files = loose_left_out = not_wanted = 0;
	}

	Pick Decide(const char* utf8, uint64_t size)
	{
		Pick p;
		const std::string rel = TexturePackTarget(utf8 ? utf8 : "", serial);
		if (rel.empty())
		{
			not_wanted++;
			return p;
		}
		std::string ps, pn;
		const bool pak = SplitReplacement(rel, ps, pn);
		if (!pak && IsPakPlace(rel))
		{
			not_wanted++;
			return p;
		}
		if (!pak)
		{
			if (size > kLooseEntryMax || loose_bytes + size > kLooseTotalMax)
			{
				loose_left_out++;
				return p;
			}
			loose_bytes += size;
			loose_files++;
		}
		p.keep = true;
		p.pak = pak;
		p.rel = rel;
		if (pak)
		{
			p.pak_serial = ps;
			p.pak_name = pn;
			pak_serials.insert(ps);
		}
		kept_total += size;
		return p;
	}

	// the second pass
	std::map<std::string, std::unique_ptr<PakWriter>> paks;
	std::unique_ptr<FileWriters> loose;
	std::set<std::string> made; // folders made, so each is made once
	Pick cur;
	PakWriter* cur_pak = nullptr;
	std::string cur_out;
	std::vector<uint8_t> cur_data;
	bool failed = false, no_space = false, stopped = false; // stopped: the pack writer gave up waiting because a stop was asked for (not a failure)
	std::string error;
	uint32_t packed = 0, queued = 0;

	// An unpack that ends before it is done (a stop, a cancel, a failure) drops what the pack writers still hold, instead of writing it to a file that is deleted next.
	~SevenZipRun()
	{
		for (auto& kv : paks)
			if (kv.second)
				kv.second->Abort();
	}
	uint64_t kept_so_far = 0, packed_done = 0; // the wanted files' bytes given so far, and the packed bytes read
	double t_sink = 0; // seconds inside the consumer's calls (making folders, the pack writer, queueing loose files): the rest of the time is the decoder's
	Clock::time_point t_start = Clock::now(), last_report = Clock::now();

	static double Seconds(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

	int Bad(const std::string& what, bool space = false)
	{
		failed = true;
		no_space = space;
		error = what;
		return 1;
	}

	int Begin(const char* utf8, uint64_t size)
	{
		const auto t0 = Clock::now();
		cur = Decide(utf8, size);
		cur_pak = nullptr;
		int r = 0;
		if (!cur.keep)
			r = 1;
		else if (cur.pak)
		{
			const std::string dir = staging + "/" + cur.pak_serial;
			if (!made.count(dir))
			{
				if (!MakeDirs(dir))
					r = Bad("Couldn't make " + dir, errno == ENOSPC);
				else
					made.insert(dir);
			}
			if (r == 0)
			{
				std::unique_ptr<PakWriter>& pak = paks[cur.pak_serial];
				bool good = true;
				if (!pak)
				{
					PakWriter::Options options;
					options.sync_each = sync_each;
					options.direct = direct;
					options.thread_start = thread_start;
					options.stopping = stopping;
					options.remote = remote;
					options.log = log;
					pak = std::make_unique<PakWriter>(std::move(options));
					good = pak->Open(dir + "/" + OrbisTexturePak::kFileName);
				}
				good = good && pak->Begin(cur.pak_name);
				if (!good)
					r = Bad(pak->NoSpace() ? std::string("The disk is full")
					                       : "Couldn't write " + cur.pak_serial + "/" + OrbisTexturePak::kFileName + " (" + pak->Error() + ")",
						pak->NoSpace());
				else
					cur_pak = pak.get();
			}
		}
		else
		{
			cur_out = staging + "/" + cur.rel;
			const std::string dir = cur_out.substr(0, cur_out.rfind('/'));
			if (!made.count(dir))
			{
				if (!MakeDirs(dir))
					r = Bad("Couldn't make " + dir, errno == ENOSPC);
				else
					made.insert(dir);
			}
			cur_data.clear();
			cur_data.reserve(static_cast<size_t>(size));
		}
		t_sink += Seconds(t0, Clock::now());
		if (r > 0 && failed)
			return -1; // the consumer failed (not "leave it out"): the reader stops, `error` says why
		return r;
	}

	int Data(const uint8_t* p, size_t n)
	{
		const auto t0 = Clock::now();
		int r = 0;
		if (cur_pak)
		{
			if (!cur_pak->Append(p, n))
			{
				if (!cur_pak->Failed()) // it gave up waiting for the disk because a stop was asked for
				{
					stopped = true;
					r = 1;
				}
				else
					r = Bad(cur_pak->NoSpace() ? std::string("The disk is full")
					                           : "Couldn't write " + cur.pak_serial + "/" + OrbisTexturePak::kFileName + " (" + cur_pak->Error() + ")",
						cur_pak->NoSpace());
			}
		}
		else
			cur_data.insert(cur_data.end(), p, p + n);
		t_sink += Seconds(t0, Clock::now());
		return r;
	}

	int End()
	{
		const auto t0 = Clock::now();
		int r = 0;
		if (cur_pak)
		{
			cur_pak->End();
			packed++;
			cur_pak = nullptr;
		}
		else
		{
			if (!loose)
				loose = std::make_unique<FileWriters>(2, thread_start);
			if (!loose->Put(cur_out, std::move(cur_data)))
				r = Bad("", false); // a write failed: FileWriters::Finish says which
			else
				queued++;
			cur_data = std::vector<uint8_t>();
		}
		t_sink += Seconds(t0, Clock::now());
		return r;
	}

	void Report(const char* what)
	{
		double pak_wait = 0, pak_write = 0;
		for (const auto& kv : paks)
			if (kv.second)
			{
				pak_wait += kv.second->WaitSeconds();
				pak_write += kv.second->WriteSeconds();
			}
		const double total = Seconds(t_start, Clock::now());
		char line[420];
		std::snprintf(line, sizeof(line),
			"7z %s: %u files (%.0f MiB) in %.0f s; packed %.0f of %.0f MiB; decoding %.0f s, in the writers' calls %.1f s (waiting for the pack's writer %.1f s; its write() calls %.1f s); "
			"%u other files; %s",
			what, packed + queued, static_cast<double>(kept_so_far) / 1048576.0, total, static_cast<double>(packed_done) / 1048576.0,
			static_cast<double>(packed_total) / 1048576.0, std::max(0.0, total - t_sink), t_sink, pak_wait, pak_write, queued, memory_text ? memory_text().c_str() : "");
		log(line);
	}

	int Cancelled(uint64_t done)
	{
		packed_done = done;
		progress(done);
		if (Seconds(last_report, Clock::now()) >= 10.0)
		{
			last_report = Clock::now();
			Report("unpacking");
		}
		return stopping() ? 1 : 0;
	}

	static int SBegin(void* c, const char* n, uint64_t s) { return static_cast<SevenZipRun*>(c)->Begin(n, s); }
	static int SData(void* c, const uint8_t* p, size_t n)
	{
		SevenZipRun* run = static_cast<SevenZipRun*>(c);
		run->kept_so_far += n;
		return run->Data(p, n);
	}
	static int SEnd(void* c) { return static_cast<SevenZipRun*>(c)->End(); }
	static int SCancelled(void* c, uint64_t done) { return static_cast<SevenZipRun*>(c)->Cancelled(done); }
	static int SWant(void* c, const char* n, uint64_t s) { return static_cast<SevenZipRun*>(c)->Decide(n, s).keep ? 1 : 0; }
};
} // namespace

// ---- names and the list

std::string NormalSerial(const std::string& text)
{
	if (text.size() != 10 || text[4] != '-')
		return {};
	std::string out = text;
	for (int i = 0; i < 4; i++)
	{
		char c = out[static_cast<size_t>(i)];
		if (c >= 'a' && c <= 'z')
			c = static_cast<char>(c - 'a' + 'A');
		if (c < 'A' || c > 'Z')
			return {};
		out[static_cast<size_t>(i)] = c;
	}
	for (int i = 5; i < 10; i++)
		if (out[static_cast<size_t>(i)] < '0' || out[static_cast<size_t>(i)] > '9')
			return {};
	return out;
}

std::string PercentEncode(const std::string& text)
{
	static const char* const hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : text)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
			out += static_cast<char>(c);
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

std::string PercentDecode(const std::string& text)
{
	auto value = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
	std::string out;
	for (size_t i = 0; i < text.size(); i++)
	{
		if (text[i] == '%' && i + 2 < text.size() && value(text[i + 1]) >= 0 && value(text[i + 2]) >= 0)
		{
			out += static_cast<char>(value(text[i + 1]) * 16 + value(text[i + 2]));
			i += 2;
		}
		else
			out += text[i];
	}
	return out;
}

std::string TexturePackMd5(const void* data, size_t size)
{
	Md5 m;
	m.Update(data, size);
	return m.Hex();
}

std::string TexturePackMetadataUrl()
{
	return std::string("https://archive.org/metadata/") + kTexturePackItem;
}

std::string TexturePackUrl(const TexturePack& pack)
{
	if (pack.OnDrive())
		return TexturePackDriveUrl(pack.drive_id);
	return std::string("https://archive.org/download/") + kTexturePackItem + "/" + PercentEncode(pack.name);
}

std::string TexturePackDriveUrl(const std::string& drive_id)
{
	// usercontent, with confirm=t: Drive's own address (drive.google.com/uc) answers a big file with a virus-scan page to click through.
	return "https://drive.usercontent.google.com/download?id=" + PercentEncode(drive_id) + "&export=download&confirm=t";
}

std::string FormatBytes(uint64_t bytes)
{
	char buf[32];
	if (bytes >= 1000ull * 1000 * 1000)
		std::snprintf(buf, sizeof(buf), "%.1f GB", static_cast<double>(bytes) / 1e9);
	else if (bytes >= 1000ull * 1000)
		std::snprintf(buf, sizeof(buf), "%.0f MB", static_cast<double>(bytes) / 1e6);
	else
		std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1e3);
	return buf;
}

bool ParseTexturePackCatalog(const std::string& json, std::vector<TexturePack>& out, std::string& error)
{
	out.clear();
	rapidjson::Document doc;
	doc.Parse(json.c_str(), json.size());
	if (doc.HasParseError() || !doc.IsObject())
	{
		error = "archive.org's list isn't JSON";
		return false;
	}
	const auto files = doc.FindMember("files");
	if (files == doc.MemberEnd() || !files->value.IsArray())
	{
		error = doc.HasMember("error") ? "archive.org doesn't have the item" : "archive.org's list has no files";
		return false;
	}
	auto text = [](const rapidjson::Value& o, const char* key) -> std::string {
		const auto m = o.FindMember(key);
		if (m == o.MemberEnd())
			return {};
		if (m->value.IsString())
			return std::string(m->value.GetString(), m->value.GetStringLength());
		if (m->value.IsUint64())
			return std::to_string(m->value.GetUint64());
		return {};
	};
	for (const rapidjson::Value& f : files->value.GetArray())
	{
		if (!f.IsObject())
			continue;
		TexturePack p;
		p.name = text(f, "name");
		const std::string source = text(f, "source");
		const std::string lower = Lower(p.name);
		const bool archive = lower.size() > 4 && (lower.compare(lower.size() - 4, 4, ".rar") == 0 || lower.compare(lower.size() - 4, 4, ".zip") == 0);
		if (p.name.empty() || (!source.empty() && source != "original") || !archive || p.name.find('/') != std::string::npos)
			continue;
		// The serials: every "[XXXX-00000]"; the title: the name without them and the extension; the label: what follows
		// the last one ("HD Remaster", "HD Remaster Definitive Edition").
		std::string title;
		size_t last_close = std::string::npos;
		const std::string stem = p.name.substr(0, p.name.size() - 4);
		for (size_t i = 0; i < stem.size(); i++)
		{
			if (stem[i] == '[' && i + 11 < stem.size() && stem[i + 11] == ']')
			{
				const std::string serial = NormalSerial(stem.substr(i + 1, 10));
				if (!serial.empty())
				{
					if (std::find(p.serials.begin(), p.serials.end(), serial) == p.serials.end())
						p.serials.push_back(serial);
					last_close = i + 11;
					i += 11;
					continue;
				}
			}
			title += stem[i];
		}
		if (p.serials.empty())
			continue;
		std::string collapsed;
		for (char c : title)
			if (!(c == ' ' && (collapsed.empty() || collapsed.back() == ' ')))
				collapsed += c;
		p.title = Trim(collapsed);
		p.label = last_close != std::string::npos ? Trim(stem.substr(last_close + 1)) : std::string();
		if (p.label.empty())
			p.label = p.title;
		p.bytes = std::strtoull(text(f, "size").c_str(), nullptr, 10);
		p.md5 = Lower(text(f, "md5"));
		p.files = static_cast<uint32_t>(std::strtoul(text(f, "filecount").c_str(), nullptr, 10));
		if (p.bytes == 0 || p.md5.size() != 32)
			continue;
		out.push_back(std::move(p));
	}
	if (out.empty())
	{
		error = "archive.org's list has no texture packs";
		return false;
	}
	return true;
}

bool ParseExtraTexturePacks(const std::string& json, std::vector<TexturePack>& out, std::string& error, int* unsupported)
{
	out.clear();
	if (unsupported)
		*unsupported = 0;
	rapidjson::Document doc;
	doc.Parse(json.c_str(), json.size());
	if (doc.HasParseError() || !doc.IsObject())
	{
		error = "the Drive pack list isn't JSON";
		return false;
	}
	const auto packs = doc.FindMember("packs");
	if (packs == doc.MemberEnd() || !packs->value.IsArray())
	{
		error = "the Drive pack list has no \"packs\"";
		return false;
	}
	auto text = [](const rapidjson::Value& o, const char* key) -> std::string {
		const auto m = o.FindMember(key);
		return m != o.MemberEnd() && m->value.IsString() ? std::string(m->value.GetString(), m->value.GetStringLength()) : std::string();
	};
	for (const rapidjson::Value& f : packs->value.GetArray())
	{
		if (!f.IsObject())
			continue;
		TexturePack p;
		p.host = "gdrive";
		p.drive_id = text(f, "id");
		p.name = text(f, "name");
		p.title = Trim(text(f, "title"));
		p.label = Trim(text(f, "label"));
		p.maker = Trim(text(f, "maker"));
		// The id goes into an address: letters, digits, '-' and '_' only.
		bool id_ok = p.drive_id.size() >= 10 && p.drive_id.size() <= 80;
		for (const char c : p.drive_id)
			id_ok = id_ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_');
		if (!id_ok || p.name.empty() || p.name.find('/') != std::string::npos)
			continue;
		const std::string lower = Lower(p.name);
		const bool seven = lower.size() > 3 && lower.compare(lower.size() - 3, 3, ".7z") == 0;
		const bool readable = seven || (lower.size() > 4 && (lower.compare(lower.size() - 4, 4, ".rar") == 0 || lower.compare(lower.size() - 4, 4, ".zip") == 0));
		if (!readable)
		{
			if (unsupported)
				++*unsupported; // a format no reader here knows
			continue;
		}
		// b176: optional hints for the shelf's text (tools/audit_7z_headers.py writes them): never used as the file's size
		const auto number = [&f](const char* key) -> uint64_t {
			const auto m = f.FindMember(key);
			uint64_t v = 0;
			if (m != f.MemberEnd() && m->value.IsUint64())
				v = m->value.GetUint64();
			else if (m != f.MemberEnd() && m->value.IsString())
				v = std::strtoull(m->value.GetString(), nullptr, 10);
			return v < (1ull << 40) ? v : 0;
		};
		p.hint_bytes = number("size");
		p.hint_unpacked = number("unpacked");
		const auto serials = f.FindMember("serials");
		if (serials != f.MemberEnd() && serials->value.IsArray())
			for (const rapidjson::Value& s : serials->value.GetArray())
				if (s.IsString())
				{
					const std::string serial = NormalSerial(std::string(s.GetString(), s.GetStringLength()));
					if (!serial.empty() && std::find(p.serials.begin(), p.serials.end(), serial) == p.serials.end())
						p.serials.push_back(serial);
				}
		if (p.serials.empty())
			continue;
		if (p.title.empty())
			p.title = p.name.substr(0, p.name.size() - (seven ? 3 : 4));
		if (p.label.empty())
			p.label = p.maker.empty() ? p.title : p.maker;
		out.push_back(std::move(p));
	}
	return true;
}

std::string TexturePackTarget(const std::string& entry, const std::string& serial)
{
	if (entry.empty() || entry[0] == '/' || entry.size() > 900)
		return {};
	std::vector<std::string> parts;
	size_t at = 0;
	while (at <= entry.size())
	{
		size_t end = entry.find('/', at);
		if (end == std::string::npos)
			end = entry.size();
		const std::string part = entry.substr(at, end - at);
		if (part == "..")
			return {};
		for (unsigned char c : part)
			if (c < 0x20 || c == 0x7f || c == '\\')
				return {};
		if (!part.empty() && part != ".")
			parts.push_back(part);
		at = end + 1;
	}
	auto join = [&](size_t from) {
		std::string out;
		for (size_t i = from; i < parts.size(); i++)
			out += (out.empty() ? "" : "/") + parts[i];
		return out;
	};
	// The pack's own layout: <serial>/replacements/... (also a second disc's serial); its dumps/ are the maker's, not needed.
	for (size_t i = 0; i < parts.size(); i++)
	{
		const std::string s = NormalSerial(parts[i]);
		if (s.empty())
			continue;
		if (i + 1 >= parts.size() || Lower(parts[i + 1]) == "dumps")
			return {};
		return s + "/" + join(i + 1);
	}
	// Else a replacements/ folder: under this game's serial.
	const std::string own = NormalSerial(serial);
	for (size_t i = 0; i < parts.size(); i++)
		if (Lower(parts[i]) == "replacements" && i + 1 < parts.size() && !own.empty())
			return own + "/replacements/" + join(i + 1);
	return {};
}

// ---- the manager

TexturePackManager::TexturePackManager(TexturePackPlatform platform, std::string textures_dir, std::string work_dir, std::string catalog_cache)
	: m_platform(std::move(platform))
	, m_textures(std::move(textures_dir))
	, m_work(std::move(work_dir))
	, m_cache(std::move(catalog_cache))
	, m_finished(std::make_shared<bool>(false))
{
}

TexturePackManager::~TexturePackManager()
{
	if (m_thread.joinable())
		Stop(60000);
}

double TexturePackManager::Now() const
{
	if (m_platform.now)
		return m_platform.now();
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void TexturePackManager::Log(const std::string& line) const
{
	if (m_platform.log)
		m_platform.log("[texpacks] " + line);
}

// 2.11 (AI-assisted; swordpdf: "look at the structure of this [Google Drive] folder and figure out how to integrate the links into the hd
// pack downloader"): the packs of the community's Drive folders (assets/hd-pack-sources.json), beside archive.org's. The list built into the
// app first, then the player's own file, which may add more (the same Drive file twice stays once).
std::vector<TexturePack> TexturePackManager::MergeExtra(const std::string& fetched_json, int& unsupported)
{
	std::vector<TexturePack> merged;
	unsupported = 0;
	// b176: the .7z packs are offered (fe_sevenzip.c reads them) unless the flag file texpack_no7z asks for build 175's behaviour: left out, counted and logged
	const bool no7z = m_platform.flag && m_platform.flag("texpack_no7z");
	auto add = [&](const std::string& text, const std::string& what) {
		std::vector<TexturePack> found;
		std::string error;
		int left_out = 0, seven_left_out = 0, seven = 0;
		if (!ParseExtraTexturePacks(text, found, error, &left_out))
		{
			Log(what + ": " + error);
			return;
		}
		if (no7z)
			found.erase(std::remove_if(found.begin(), found.end(),
							[&](const TexturePack& p) {
								const bool is7z = ExtOf(p.name) == ".7z";
								seven_left_out += is7z;
								return is7z;
							}),
				found.end());
		else
			for (const TexturePack& p : found)
				seven += ExtOf(p.name) == ".7z";
		unsupported += left_out + seven_left_out;
		for (TexturePack& p : found)
		{
			bool twice = false;
			for (const TexturePack& q : merged)
				twice = twice || q.drive_id == p.drive_id;
			if (!twice)
				merged.push_back(std::move(p));
		}
		Log(what + ": " + std::to_string(found.size()) + " Drive packs offered" + (seven ? " (" + std::to_string(seven) + " of them 7z)" : std::string()) +
			(seven_left_out ? ", " + std::to_string(seven_left_out) + " 7z left out (flag texpack_no7z)" : std::string()) +
			(left_out ? ", " + std::to_string(left_out) + " in a format nothing here reads, left out" : std::string()));
	};
	if (!fetched_json.empty())
		add(fetched_json, "the Drive pack list fetched last");
	if (!m_built_in_json.empty())
		add(m_built_in_json, "the Drive pack list built in");
	if (!m_extra_file.empty())
	{
		std::string text;
		if (settings::ReadFile(m_extra_file, text))
			add(text, m_extra_file);
	}
	return merged;
}

void TexturePackManager::SetExtraSources(const std::string& built_in_json, const std::string& extra_file, const std::string& fetched_file,
	const std::string& fetch_url)
{
	m_built_in_json = built_in_json;
	m_extra_file = extra_file;
	m_fetched_file = fetched_file;
	m_fetch_url = fetch_url;
	std::string fetched;
	if (!m_fetched_file.empty())
		settings::ReadFile(m_fetched_file, fetched);
	int unsupported = 0;
	std::vector<TexturePack> merged = MergeExtra(fetched, unsupported);
	{
		int seven = 0;
		for (const TexturePack& p : merged)
			seven += ExtOf(p.name) == ".7z";
		Log("Drive packs in all: " + std::to_string(merged.size()) + " offered (" + std::to_string(merged.size() - static_cast<size_t>(seven)) + " rar/zip, " +
			std::to_string(seven) + " 7z), " + std::to_string(unsupported) + " left out");
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	m_extra = std::move(merged);
	m_extra_unsupported = unsupported;
}

void TexturePackManager::RefreshExtraSources()
{
	if (m_fetch_url.empty() || !(m_platform.get_text_quick || m_platform.get_text) || (m_platform.flag && m_platform.flag("nohdsources")))
		return;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stop)
			return;
	}
	struct stat st;
	// not again within 6 hours of a copy, or within an hour of a try that failed (a console that can't reach the host asks once an hour, not at every start)
	const struct
	{
		const std::string path;
		double limit;
	} fresh[2] = {{m_fetched_file, 6 * 3600.0}, {m_fetched_file + ".tried", 3600.0}};
	for (const auto& f : fresh)
		if (!f.path.empty() && stat(f.path.c_str(), &st) == 0)
		{
			const double age = std::difftime(std::time(nullptr), st.st_mtime);
			if (age >= 0 && age < f.limit)
			{
				Log("the Drive pack list was looked for " + std::to_string(static_cast<int>(age / 60)) + " min ago: not fetched again yet");
				return;
			}
		}
	std::string body;
	const double t0 = Now();
	const int status = m_platform.get_text_quick ? m_platform.get_text_quick(m_fetch_url, body) : m_platform.get_text(m_fetch_url, body);
	Log("the Drive pack list from the project: status " + std::to_string(status) + ", " + std::to_string(body.size()) + " bytes in " +
		std::to_string(static_cast<int>((Now() - t0) * 1000)) + " ms");
	std::vector<TexturePack> check;
	std::string error;
	if (status != 200 || !ParseExtraTexturePacks(body, check, error) || check.empty())
	{
		if (status == 200)
			Log("the Drive pack list from the project is not a list (" + error + "): not used");
		if (!m_fetched_file.empty() && status != -4) // (-4: stopped)
		{
			MakeDirs(m_fetched_file.substr(0, m_fetched_file.rfind('/')));
			settings::WriteFileAtomic(m_fetched_file + ".tried", "x\n");
		}
		return;
	}
	if (!m_fetched_file.empty())
	{
		MakeDirs(m_fetched_file.substr(0, m_fetched_file.rfind('/')));
		if (!settings::WriteFileAtomic(m_fetched_file, body))
			Log("couldn't keep the Drive pack list in " + m_fetched_file);
		unlink((m_fetched_file + ".tried").c_str());
	}
	int unsupported = 0;
	std::vector<TexturePack> merged = MergeExtra(body, unsupported);
	const size_t count = merged.size();
	std::lock_guard<std::mutex> lock(m_mutex);
	m_extra = std::move(merged);
	m_extra_unsupported = unsupported;
	Log("Drive packs now: " + std::to_string(count));
}

void TexturePackManager::Start()
{
	if (m_started)
		return;
	m_started = true;
	auto finished = m_finished;
	m_thread = std::thread([this, finished] {
		if (m_platform.thread_start)
			m_platform.thread_start("worker");
		Run();
		std::lock_guard<std::mutex> lock(m_mutex);
		*finished = true;
	});
}

bool TexturePackManager::Stop(int wait_ms)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stop = true;
	}
	m_cv.notify_all();
	if (m_platform.abort)
		m_platform.abort();
	{
		std::lock_guard<std::mutex> lock(m_writer_mutex); // 2026-10-29: a write the writer process is doing is not waited for
		if (m_writer)
			m_writer->Abort();
	}
	if (!m_thread.joinable())
		return true;
	const double until = Now() + wait_ms / 1000.0;
	for (;;)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (*m_finished)
				break;
		}
		if (Now() >= until)
		{
			Log("still busy at the shelf's end: left to finish on its own");
			m_thread.detach();
			return false;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	m_thread.join();
	return true;
}

// pr9h (2026-10-05) took the folder above the textures folder as the textures folder when the textures folder wasn't there
// (main-boot's OrbisDir fallback): its downloads, its unpacking and its installed packs went into /data/PCSX2 itself.
// This moves what it left where it belongs: the downloads folder (so a download carries on), the unpacking folder (the
// next unpack clears it) and serial folders that carry this manager's marker. Renames only, nothing else is touched.
void TexturePackManager::MoveFromParent()
{
	const size_t slash = m_textures.rfind('/');
	if (slash == std::string::npos || slash == 0)
		return;
	const std::string parent = m_textures.substr(0, slash);
	const auto move = [&](const std::string& from, const std::string& to) {
		if (!Exists(from) || Exists(to))
			return;
		MakeDirs(to.substr(0, to.rfind('/')));
		const bool ok = rename(from.c_str(), to.c_str()) == 0;
		Log(std::string(ok ? "moved " : "couldn't move ") + from + " to " + to);
	};
	if (m_work.compare(0, m_textures.size() + 1, m_textures + "/") == 0)
		move(parent + m_work.substr(m_textures.size()), m_work);
	move(parent + "/.ps5sx2-unpack", m_textures + "/.ps5sx2-unpack");
	if (DIR* d = opendir(parent.c_str()))
	{
		std::vector<std::string> names;
		while (dirent* e = readdir(d))
			if (NormalSerial(e->d_name) == e->d_name)
				names.emplace_back(e->d_name);
		closedir(d);
		for (const std::string& n : names)
			if (Exists(parent + "/" + n + "/" + kMarker))
			{
				move(parent + "/" + n, m_textures + "/" + n);
				std::lock_guard<std::mutex> lock(m_mutex);
				m_installed.erase(n);
			}
	}
}

// `path` renamed to <textures>/.ps5sx2-old-<time>-<n>, for EmptySetAside. False when it isn't there or can't be renamed
// (then the caller deletes it itself).
bool TexturePackManager::SetAside(const std::string& path)
{
	if (!Exists(path))
		return false;
	static std::atomic<unsigned> counter{0};
	const std::string aside = m_textures + "/.ps5sx2-old-" + std::to_string(static_cast<long long>(std::time(nullptr))) + "-" +
		std::to_string(counter++);
	const bool ok = rename(path.c_str(), aside.c_str()) == 0;
	Log(ok ? "set aside " + path + " as " + aside + ", to delete later" : "couldn't set aside " + path);
	return ok;
}

// Deletes what SetAside renamed, a file at a time, and gives way (within 64 files) as soon as the worker has anything else
// to do or is asked to stop; the next idle moment carries on.
void TexturePackManager::EmptySetAside()
{
	std::vector<std::string> old;
	if (DIR* d = opendir(m_textures.c_str()))
	{
		while (dirent* e = readdir(d))
			if (std::strncmp(e->d_name, ".ps5sx2-old-", 12) == 0)
				old.emplace_back(m_textures + "/" + e->d_name);
		closedir(d);
	}
	if (old.empty())
		return;
	const auto busy = [this] {
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_stop || m_retry || !m_queue.empty() || !m_removals.empty();
	};
	const double t0 = Now();
	uint32_t removed = 0;
	bool interrupted = false;
	std::function<bool(const std::string&)> remove = [&](const std::string& p) -> bool {
		struct stat st;
		if (lstat(p.c_str(), &st) != 0)
			return true;
		if (!S_ISDIR(st.st_mode))
		{
			if ((removed & 63) == 63 && busy())
			{
				interrupted = true;
				return false;
			}
			if (unlink(p.c_str()) == 0)
				removed++;
			return true;
		}
		std::vector<std::string> names;
		if (DIR* d = opendir(p.c_str()))
		{
			while (dirent* e = readdir(d))
				if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0)
					names.emplace_back(e->d_name);
			closedir(d);
		}
		for (const std::string& n : names)
			if (!remove(p + "/" + n))
				return false;
		rmdir(p.c_str());
		return true;
	};
	for (const std::string& p : old)
		if (!remove(p))
			break;
	Log("deleted " + std::to_string(removed) + " files set aside, in " + std::to_string(static_cast<int>(Now() - t0)) + " s" +
		(interrupted ? " (the rest later)" : ""));
}

void TexturePackManager::Run()
{
	MoveFromParent();
	LoadCatalog();
	ResumeJobs();
	for (;;)
	{
		EmptySetAside(); // at once when nothing was set aside or there's other work
		if (!m_sources_refreshed) // 2.11: the project's Drive list, once, when there is nothing to do (a blocked host never keeps a pack from starting)
		{
			bool idle;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				idle = !m_stop && !m_retry && m_queue.empty() && m_removals.empty();
			}
			if (idle)
			{
				m_sources_refreshed = true;
				RefreshExtraSources();
				continue;
			}
		}
		Job job;
		std::string removal;
		bool retry = false;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [&] { return m_stop || m_retry || !m_queue.empty() || !m_removals.empty(); });
			if (m_stop)
				return;
			if (m_retry)
			{
				m_retry = false;
				retry = true;
			}
			else if (!m_removals.empty())
			{
				removal = m_removals.front();
				m_removals.pop_front();
			}
			else
			{
				job = m_queue.front();
				m_queue.pop_front();
				m_current = job.serial;
				m_cancel_current = false;
			}
		}
		if (retry)
		{
			LoadCatalog();
			continue;
		}
		if (!removal.empty())
		{
			// pr9l: renamed out of the way, so the sheet shows it gone at once; EmptySetAside deletes its files.
			const std::string dir = m_textures + "/" + removal;
			bool ok = Exists(dir + "/" + kMarker);
			if (ok && !SetAside(dir))
				ok = RemoveTree(dir);
			Log("removed " + dir + (ok ? "" : " (not all of it)"));
			std::lock_guard<std::mutex> lock(m_mutex);
			m_progress.erase(removal);
			m_installed.erase(removal);
			continue;
		}
		Process(job);
		EndWriter(); // 2026-10-29: the writer process ends with the job (a new one is started for the next)
		std::lock_guard<std::mutex> lock(m_mutex);
		m_current.clear();
		m_installed.erase(job.serial);
	}
}

void TexturePackManager::LoadCatalog()
{
	std::string json, error;
	std::vector<TexturePack> packs;
	struct stat st;
	const bool cached = stat(m_cache.c_str(), &st) == 0 && settings::ReadFile(m_cache, json);
	const double age = cached ? std::difftime(std::time(nullptr), st.st_mtime) : 1e9;
	bool ok = false;
	if (cached && age >= 0 && age < 24 * 3600 && ParseTexturePackCatalog(json, packs, error))
	{
		ok = true;
		Log("list from the cache (" + std::to_string(static_cast<int>(age / 60)) + " min old): " + std::to_string(packs.size()) + " packs");
	}
	else
	{
		std::string body;
		const double t0 = Now();
		const int status = m_platform.get_text ? m_platform.get_text(TexturePackMetadataUrl(), body) : -1;
		Log("list from archive.org: status " + std::to_string(status) + ", " + std::to_string(body.size()) + " bytes in " +
			std::to_string(static_cast<int>((Now() - t0) * 1000)) + " ms");
		if (status == 200 && ParseTexturePackCatalog(body, packs, error))
		{
			ok = true;
			std::string dir = m_cache.substr(0, m_cache.rfind('/'));
			MakeDirs(dir);
			if (!settings::WriteFileAtomic(m_cache, body))
				Log("couldn't keep the list in " + m_cache);
			Log(std::to_string(packs.size()) + " packs on archive.org");
		}
		else if (cached && ParseTexturePackCatalog(json, packs, error))
		{
			ok = true;
			Log("archive.org didn't answer; using the list from " + std::to_string(static_cast<int>(age / 3600)) + " h ago");
		}
		else if (error.empty())
			error = status < 0 ? "Can't reach archive.org" : "archive.org answered " + std::to_string(status);
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	if (ok)
	{
		m_catalog = std::move(packs);
		m_catalog_state = Catalog::Ready;
	}
	else
	{
		m_catalog_state = Catalog::Failed;
		m_catalog_error = error;
		Log("no list: " + error);
	}
}

void TexturePackManager::ResumeJobs()
{
	DIR* d = opendir(m_work.c_str());
	if (!d)
		return;
	std::vector<std::string> names;
	while (dirent* e = readdir(d))
	{
		const std::string n = e->d_name;
		if (n.size() > 4 && n.compare(n.size() - 4, 4, ".job") == 0)
			names.push_back(n);
	}
	closedir(d);
	std::sort(names.begin(), names.end());
	for (const std::string& n : names)
	{
		auto kv = ReadKeyValues(m_work + "/" + n);
		Job j;
		j.serial = NormalSerial(kv["serial"]);
		j.name = kv["name"];
		j.md5 = Lower(kv["md5"]);
		j.bytes = std::strtoull(kv["bytes"].c_str(), nullptr, 10);
		j.title = kv["title"];
		j.settings = kv["settings"];
		j.header = kv["header"];
		j.host = kv["host"];
		j.drive_id = kv["drive"];
		j.unpack_tries = static_cast<int>(std::min<unsigned long>(std::strtoul(kv["unpack_tries"].c_str(), nullptr, 10), 1000));
		// 2.11: a Drive pack has no checksum and, before its first start, no size; it is known by its Drive id
		const bool drive_job = j.host == "gdrive" && !j.drive_id.empty();
		if (j.serial.empty() || j.name.empty() || (!drive_job && (j.md5.size() != 32 || j.bytes == 0)))
		{
			Log("left out a job file that doesn't read: " + n);
			continue;
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		Progress& p = m_progress[j.serial];
		p = Progress();
		p.name = j.name;
		p.title = j.title;
		// b176: a 7z unpack that began (the job file says so before it starts) and never ended took the app with it, kMaxUnpackTries times: not carried on with
		// by itself again, or it would end the app at every start. Cross asks for it again (that resets the count to what the next try makes of it).
		if (j.unpack_tries >= kMaxUnpackTries)
		{
			p.state = State::Failed;
			p.message = "The last two tries at unpacking this pack were cut short (the app closed or ended while they ran)";
			Log("not carrying on with " + j.name + " for " + j.serial + ": " + std::to_string(j.unpack_tries) + " unpacks of it began and never ended");
			continue;
		}
		p.state = State::Queued;
		m_queue.push_back(j);
		Log("carrying on with " + j.name + " for " + j.serial);
	}
}

bool TexturePackManager::WriteJob(const Job& j)
{
	MakeDirs(m_work);
	const std::string text = "serial=" + j.serial + "\nname=" + OneLine(j.name) + "\nmd5=" + j.md5 + "\nbytes=" + std::to_string(j.bytes) +
		"\ntitle=" + OneLine(j.title) + "\nsettings=" + OneLine(j.settings) + "\nheader=" + OneLine(j.header) +
		(j.host.empty() ? std::string() : "\nhost=" + OneLine(j.host) + "\ndrive=" + OneLine(j.drive_id)) +
		(j.unpack_tries > 0 ? "\nunpack_tries=" + std::to_string(j.unpack_tries) : std::string()) + "\n";
	return settings::WriteFileAtomic(m_work + "/" + j.serial + ".job", text);
}

bool TexturePackManager::ReadJobFile(const std::string& serial, Job& out) const
{
	if (!Exists(m_work + "/" + serial + ".job"))
		return false;
	auto kv = ReadKeyValues(m_work + "/" + serial + ".job");
	if (kv["name"].empty())
		return false;
	out = Job();
	out.serial = serial;
	out.name = kv["name"];
	out.md5 = Lower(kv["md5"]);
	out.host = kv["host"];
	out.drive_id = kv["drive"];
	return true;
}

void TexturePackManager::DeleteJobFiles(const Job& j)
{
	const std::string ext = ExtOf(j.name);
	unlink((m_work + "/" + j.Key() + ".part").c_str());
	unlink((m_work + "/" + j.Key() + ext).c_str());
	unlink((m_work + "/" + j.serial + ".job").c_str());
}

void TexturePackManager::SetState(const std::string& serial, State state, uint64_t done, uint64_t total, const std::string& message)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	Progress& p = m_progress[serial];
	if (p.state != state || !message.empty())
		Log(serial + ": " + StateName(state) + (message.empty() ? std::string() : " (" + message + ")"));
	p.state = state;
	p.done = done;
	p.total = total;
	p.message = message;
	if (state != State::Downloading)
		p.rate = 0;
}

void TexturePackManager::SetProgress(const std::string& serial, uint64_t done)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	Progress& p = m_progress[serial];
	p.done = done;
	if (p.state == State::Downloading)
	{
		const double now = Now();
		if (now - m_rate_time >= 1.0)
		{
			const double rate = m_rate_time > 0 && done >= m_rate_bytes ? (done - m_rate_bytes) / (now - m_rate_time) : 0;
			p.rate = p.rate > 0 ? p.rate * 0.6 + rate * 0.4 : rate;
			m_rate_bytes = done;
			m_rate_time = now;
		}
	}
}

bool TexturePackManager::Stopping(const std::string& serial) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_stop || (m_current == serial && m_cancel_current);
}

void TexturePackManager::Process(const Job& job_in)
{
	Job job = job_in; // 2.11: a Drive pack's size is learned here
	const std::string ext = ExtOf(job.name);
	const std::string part = m_work + "/" + job.Key() + ".part", full = m_work + "/" + job.Key() + ext;
	auto cancelled = [&] {
		bool c;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			c = m_current == job.serial && m_cancel_current && !m_stop;
		}
		if (c)
		{
			DeleteJobFiles(job);
			RemoveTree(m_textures + "/.ps5sx2-unpack");
			std::lock_guard<std::mutex> lock(m_mutex);
			m_progress.erase(job.serial);
			Log("cancelled " + job.name);
		}
		return c;
	};
	// b176: a .7z. The flag texpack_no7z switches them off (a job resumed from before it was set is failed, not downloaded).
	if (ext == ".7z" && m_platform.flag && m_platform.flag("texpack_no7z"))
	{
		SetState(job.serial, State::Failed, 0, 0, "7z packs are switched off here (flag texpack_no7z)");
		return;
	}
	if (job.host == "gdrive" && job.bytes == 0)
	{
		// Drive's lists have no sizes: a one-byte range says it (Content-Range's total), and whether it gives the file at all.
		SetState(job.serial, State::Checking, 0, 0);
		const std::string url = TexturePackDriveUrl(job.drive_id);
		uint64_t total = 0;
		std::string type;
		const int status = m_platform.probe ? m_platform.probe(url, total, type) : -1;
		Log("Drive: " + job.name + ": status " + std::to_string(status) + ", " + std::to_string(total) + " bytes, " + (type.empty() ? "no type" : type));
		if (cancelled() || Stopping(job.serial))
			return;
		if ((status != 206 && status != 200) || total == 0 || type.compare(0, 9, "text/html") == 0)
		{
			SetState(job.serial, State::Failed, 0, 0,
				status < 0 ? "Can't reach Google Drive"
				: status == 404 ? "Google Drive no longer has this file"
				: "Google Drive isn't giving this file now (too many downloads of it today?): try again later");
			return;
		}
		job.bytes = total;
		WriteJob(job); // a later start carries on with the size known
	}
	// b176: a .7z the reader refused before is refused again without a download, as long as it was refused under the limits that hold now (kLimitsRevision) and for a file
	// of this size (Drive keeps a file's id when its maker uploads a new version, and the new one may be the fixed one), and the flag texpack_7z_bigdict (which lifts the
	// dictionary limit) is not set. The one-byte probe above is all this costs.
	if (ext == ".7z" && !(m_platform.flag && m_platform.flag("texpack_7z_bigdict")))
	{
		const auto kv = ReadKeyValues(RefusedFile(job));
		const auto rev = kv.find("limits"), msg = kv.find("message"), size = kv.find("bytes");
		if (rev != kv.end() && msg != kv.end() && size != kv.end() && std::strtol(rev->second.c_str(), nullptr, 10) == kLimitsRevision && !msg->second.empty() &&
			std::strtoull(size->second.c_str(), nullptr, 10) == job.bytes)
		{
			Log(job.name + " was refused before (" + msg->second + "): not downloaded again");
			DeleteJobFiles(job);
			SetState(job.serial, State::Failed, 0, 0, msg->second);
			return;
		}
	}
	Log("starting " + job.name + " for " + job.serial + " (" + FormatBytes(job.bytes) + (job.host == "gdrive" ? ", from Google Drive" : "") + ")");
	StartWriter();
	if (FileSize(full) != job.bytes && !Download(job, part, full))
	{
		cancelled();
		return;
	}
	if (cancelled() || Stopping(job.serial))
		return;
	// b176: a download is taken by what it is: a 7z (by its signature) is read by fe_sevenzip.c, which knows the exact size of what it will write once the
	// header is read (Unpack7z checks the room then); everything else goes through libarchive as before.
	const bool sevenz = IsSevenZipFile(full);
	// Room to unpack: DDS packs come to ~2.7 times their archive (Beyond Good & Evil: 46 MB -> 122 MB).
	const uint64_t need = job.bytes * 3 + (512ull << 20);
	const uint64_t room = m_platform.free_bytes ? m_platform.free_bytes(m_textures) : UINT64_MAX;
	if (!sevenz && room < need)
	{
		SetState(job.serial, State::NeedSpace, 0, 0, "Needs " + FormatBytes(need) + " free to unpack (" + FormatBytes(room) + " now)");
		Log("not enough room to unpack: " + FormatBytes(room) + " free, " + FormatBytes(need) + " wanted");
		return;
	}
	const std::string staging = m_textures + "/.ps5sx2-unpack";
	// pr9l: an unpack cut short by an older build left thousands of files here: renamed now, deleted when the worker is idle.
	if (!SetAside(staging))
		RemoveTree(staging);
	if (!MakeDirs(staging))
	{
		SetState(job.serial, State::Failed, 0, 0, "Couldn't make " + staging);
		return;
	}
	uint32_t files = 0;
	bool no_space = false, drop = false;
	std::string error;
	const double t0 = Now();
	bool ok;
	if (sevenz)
	{
		// b176, the crash guard: the job file says an unpack begins BEFORE it does, and that it ended when it did. A pack whose unpack ends the app (a
		// failed allocation aborts it: no exceptions in this build) is then not resumed by itself at every start (ResumeJobs, kMaxUnpackTries).
		job.unpack_tries++;
		WriteJob(job);
		ok = Unpack7z(job, full, staging, files, no_space, error, drop);
		job.unpack_tries = 0;
		if (Exists(m_work + "/" + job.serial + ".job"))
			WriteJob(job);
	}
	else
		ok = Unpack(job, full, staging, files, no_space, error);
	if (cancelled())
		return;
	if (Stopping(job.serial))
	{
		RemoveTree(staging); // the next start unpacks it again
		return;
	}
	if (!ok || files == 0)
	{
		RemoveTree(staging);
		// 2.11: no checksum vouched for this download: a try again fetches it afresh. Only when the archive is at fault (damaged, unreadable, nothing in it): a
		// local write failure (not the disk full) keeps the multi-gigabyte download for the next try.
		const bool archive_fault = ok || drop || error.rfind("The pack is damaged", 0) == 0 || error.rfind("Can't open the pack", 0) == 0;
		if (!no_space && job.md5.size() != 32 && archive_fault)
			unlink(full.c_str());
		if (drop) // b176: a pack this build can never unpack: no download, no job file to carry it on at the next start
			DeleteJobFiles(job);
		else if (sevenz && archive_fault && !no_space && job.md5.size() != 32)
		{
			// b176 (review): a 7z found damaged (its checksums, its data) is damaged the same way the next time: the job file goes too, or the next start would fetch the
			// same gigabytes again to fail again, at every start. Cross asks for it afresh.
			unlink((m_work + "/" + job.serial + ".job").c_str());
		}
		if (no_space)
			SetState(job.serial, State::NeedSpace, 0, 0, error.rfind("Needs ", 0) == 0 ? error : "The disk filled up while unpacking: free some space");
		else
			SetState(job.serial, State::Failed, 0, 0, ok ? "Nothing in it is a texture for " + job.serial : error);
		Log("unpacking failed: " + (ok ? std::string("no textures for ") + job.serial : error));
		return;
	}
	Log("unpacked " + std::to_string(files) + " files in " + std::to_string(static_cast<int>(Now() - t0)) + " s");
	if (m_writer)
		Log("the writer process wrote " + FormatBytes(m_writer->BytesWritten()) + " for this pack (download and unpack), its calls " +
			std::to_string(static_cast<int>(m_writer->WriteSeconds())) + " s" + (m_writer->Usable() ? "" : "; it stopped on the way and the app wrote the rest"));
	SetState(job.serial, State::Installing);
	// <serial>/ (and a second disc's) into the textures folder, beside or into what is there.
	bool moved = true;
	if (DIR* d = opendir(staging.c_str()))
	{
		std::vector<std::string> names;
		while (dirent* e = readdir(d))
			if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0)
				names.emplace_back(e->d_name);
		closedir(d);
		for (const std::string& n : names)
		{
			// pr9l: this app's earlier install of the pack as a replacements/ folder makes way for the one-file pack (loose
			// files would win over the pack's, and keep the per-file cost).
			const std::string target = m_textures + "/" + n;
			if (Exists(staging + "/" + n + "/" + OrbisTexturePak::kFileName) && Exists(target + "/" + kMarker) &&
				IsDir(target + "/replacements") && !SetAside(target + "/replacements"))
				RemoveTree(target + "/replacements");
			moved = MoveTree(staging + "/" + n, target) && moved;
		}
	}
	RemoveTree(staging);
	if (!moved)
	{
		SetState(job.serial, State::Failed, 0, 0, "Couldn't move the pack into " + m_textures);
		return;
	}
	Finish(job, files);
}

// 2026-10-29: Google Drive answers a range of a file whose download quota is used up with its "Google Drive - Quota exceeded" page (HTTP 200,
// text/html, ~2 KB) instead of the bytes, while a one-byte range (the size probe) still works. Build 177 took that page for the file's first
// bytes: it went into the .part file (2,040 bytes), every later range asked from there was refused, and the download never got further. A pack
// file never begins with an HTML page: one is recognised by its start.
static bool LooksLikeHtmlPage(const void* data, size_t n)
{
	const char* p = static_cast<const char*>(data);
	size_t i = 0;
	while (i < n && i < 16 && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n'))
		i++;
	const std::string head = Lower(std::string(p + i, std::min<size_t>(n - i, 15)));
	return head.rfind("<!doctype html", 0) == 0 || head.rfind("<html", 0) == 0;
}

// The message for Drive's page in place of a file.
static std::string DrivePageMessage(const std::string& page)
{
	if (page.find("Quota exceeded") != std::string::npos || page.find("quota") != std::string::npos)
		return "Google Drive's download limit for this file is used up for now (\"Quota exceeded\"): try again tomorrow, or pick the archive.org pack if there is one";
	return "Google Drive sent a page instead of the file: try again later";
}

// 2026-10-29: the texture pack writer process for this job (fe_texwrite.h): a pack is written twice, and a title's writes are held to ~2 MiB/s after its
// first ~1.3 GiB. Not with the flag texpack_titlewrites, nor on a platform without launch_writer (the PC), nor for 10 min after one that did not come.
void TexturePackManager::StartWriter()
{
	if (m_writer || !m_platform.launch_writer || (m_platform.flag && m_platform.flag("texpack_titlewrites")))
		return;
	if (m_writer_failed_at >= 0 && Now() - m_writer_failed_at < 600)
		return;
	MakeDirs(m_work); // the root must be there for the writer (the downloads folder is in it)
	std::string why;
	const double t0 = Now();
	std::unique_ptr<TexRemoteWriter> w = TexRemoteWriter::Start(m_textures, m_platform.writer_log, m_platform.launch_writer, m_platform.log, 5000, why);
	if (!w)
	{
		m_writer_failed_at = Now();
		Log("no writer process (" + why + "): the app writes the pack itself (the console slows a title's writes after its first ~1.3 GB)");
		return;
	}
	Log("writer process " + std::to_string(w->Pid()) + " ready in " + std::to_string(static_cast<int>((Now() - t0) * 1000)) + " ms: it writes the download and the unpacked pack");
	std::lock_guard<std::mutex> lock(m_writer_mutex);
	m_writer = std::move(w);
}

void TexturePackManager::EndWriter()
{
	std::unique_ptr<TexRemoteWriter> w;
	{
		std::lock_guard<std::mutex> lock(m_writer_mutex);
		w = std::move(m_writer);
	}
	if (w && !w->Usable())
		m_writer_failed_at = Now();
}

bool TexturePackManager::Download(const Job& job, const std::string& part, const std::string& full)
{
	MakeDirs(m_work);
	uint64_t have = FileSize(part);
	if (have > job.bytes)
	{
		unlink(part.c_str());
		have = 0;
	}
	// Room for the rest of the download and for unpacking it. b176: a 7z packs 1.4 to 3.7 times and only part of it is wanted, so what it takes is
	// not guessed from its size: the lenient rule here (the rest of the download, a copy's worth, 512 MB), and the exact one when its header is read.
	const bool sevenz_name = ExtOf(job.name) == ".7z";
	const uint64_t need = sevenz_name ? (job.bytes - have) + job.bytes + (512ull << 20) : (job.bytes - have) + job.bytes * 3 + (512ull << 20);
	const uint64_t room = m_platform.free_bytes ? m_platform.free_bytes(m_textures) : UINT64_MAX;
	if (room < need)
	{
		SetState(job.serial, State::NeedSpace, 0, 0, "Needs " + FormatBytes(need) + " free (" + FormatBytes(room) + " now)");
		Log("not enough room: " + FormatBytes(room) + " free, " + FormatBytes(need) + " wanted");
		return false;
	}
	// What came before goes through the checksum first (2.11: a Drive pack has none to be checked against: its part is taken as it is).
	Md5 md5;
	const bool checksummed = job.md5.size() == 32;
	if (have > 0 && job.host == "gdrive")
	{
		char head[64] = {};
		size_t n = 0;
		if (FILE* f = std::fopen(part.c_str(), "rb"))
		{
			n = std::fread(head, 1, sizeof(head), f);
			std::fclose(f);
		}
		if (LooksLikeHtmlPage(head, n))
		{
			Log("the part of " + job.name + " begins with Google Drive's page, not the file (an older build kept it): started again");
			unlink(part.c_str());
			have = 0;
		}
	}
	if (have > 0 && !checksummed)
		Log("picking up " + job.name + " at " + FormatBytes(have) + " (no checksum for this download: the archive's own checks come when it is unpacked)");
	if (have > 0 && checksummed)
	{
		SetState(job.serial, State::Checking, 0, have);
		FILE* f = std::fopen(part.c_str(), "rb");
		std::vector<uint8_t> buf(4 << 20);
		uint64_t read = 0;
		while (f && read < have)
		{
			const size_t n = std::fread(buf.data(), 1, static_cast<size_t>(std::min<uint64_t>(buf.size(), have - read)), f);
			if (n == 0)
				break;
			md5.Update(buf.data(), n);
			read += n;
			SetProgress(job.serial, read);
			if (Stopping(job.serial))
			{
				std::fclose(f);
				return false;
			}
		}
		if (f)
			std::fclose(f);
		if (read != have)
		{
			unlink(part.c_str());
			have = 0;
			md5 = Md5();
		}
		Log("picking up " + job.name + " at " + FormatBytes(have));
	}
	// 2026-10-29: the download's bytes go to the file kDownloadBuffer at a time (they come from the network in TLS records of 16 KiB, and each write
	// costs the console ~3 ms whatever its size: 16 KiB writes alone held a download to ~5 MB/s), through the writer process when there is one.
	TexOutFile out;
	if (!out.Open(part, have == 0, m_writer.get(), false, [this](const std::string& line) { Log(line.substr(line.compare(0, 11, "[texpacks] ") == 0 ? 11 : 0)); }))
	{
		SetState(job.serial, State::Failed, 0, 0, "Couldn't write to " + m_work);
		return false;
	}
	std::vector<uint8_t> pending;
	pending.reserve(kDownloadBuffer);
	uint64_t in_file = have;
	const auto flush = [&] {
		if (pending.empty())
			return true;
		if (!out.PWrite(pending.data(), pending.size(), in_file))
			return false;
		in_file += pending.size();
		pending.clear();
		return true;
	};
	SetState(job.serial, State::Downloading, have, job.bytes);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_rate_bytes = have;
		m_rate_time = Now();
	}
	TexturePack url_pack;
	url_pack.name = job.name;
	url_pack.host = job.host;
	url_pack.drive_id = job.drive_id;
	const std::string url = TexturePackUrl(url_pack);
	const double t0 = Now();
	const uint64_t at_start = have;
	int failures = 0;
	bool write_failed = false;
	while (have < job.bytes)
	{
		if (Stopping(job.serial))
			break;
		const uint64_t want = std::min<uint64_t>(kChunk, job.bytes - have);
		uint64_t got = 0;
		std::string page; // 2026-10-29: Drive's page in place of the file (never written)
		const auto sink = [&](const void* data, size_t n) {
			if (got == 0 && job.host == "gdrive" && LooksLikeHtmlPage(data, n))
			{
				page.assign(static_cast<const char*>(data), std::min<size_t>(n, 4096));
				return false;
			}
			if (got + n > want)
				return false;
			const uint8_t* b = static_cast<const uint8_t*>(data);
			pending.insert(pending.end(), b, b + n);
			if (pending.size() >= kDownloadBuffer && !flush())
			{
				write_failed = true;
				return false;
			}
			md5.Update(data, n);
			got += n;
			SetProgress(job.serial, have + got);
			return !Stopping(job.serial);
		};
		const int status = m_platform.get_range ? m_platform.get_range(url, have, want, sink) : -1;
		if (!write_failed && !flush()) // what is in the file always ends where a range ended: a stop keeps every byte counted
			write_failed = true;
		have += got;
		if (write_failed)
		{
			out.Close();
			SetState(job.serial, State::NeedSpace, 0, 0, "Couldn't write the download (is the disk full?)");
			Log("writing the download failed at " + FormatBytes(have));
			return false;
		}
		if (got == want && (status == 206 || status == 200))
		{
			failures = 0;
			continue;
		}
		if (!page.empty())
		{
			// Not tried again and again: the quota is Google's, per file, for a day.
			const size_t t = page.find("<title>"), te = page.find("</title>");
			out.Close();
			Log("range at " + std::to_string(have) + ": Google Drive answered " + std::to_string(status) + " with a page (" +
				(t != std::string::npos && te != std::string::npos && te > t ? page.substr(t + 7, te - t - 7) : std::string("no title")) + "), not the file; " +
				FormatBytes(have) + " of it kept for later");
			SetState(job.serial, State::Failed, 0, 0, DrivePageMessage(page));
			return false;
		}
		if (Stopping(job.serial))
			break;
		failures++;
		Log("range at " + std::to_string(have - got) + ": status " + std::to_string(status) + ", " + std::to_string(got) + " of " +
			std::to_string(want) + " bytes (try " + std::to_string(failures) + ")");
		if (failures >= 6)
		{
			out.Close();
			SetState(job.serial, State::Failed, 0, 0,
				status > 0 ? job.Source() + " answered " + std::to_string(status) +
					                  (job.host == "gdrive" && got < want ? " (Google Drive may be limiting downloads of this file)" : std::string())
					            : "The download keeps failing: check the connection");
			return false;
		}
		// 2, 4, 8, 16 and 30 s between tries, cut short by a stop.
		const double until = Now() + std::min(30.0, 2.0 * (1 << (failures - 1)));
		while (Now() < until && !Stopping(job.serial))
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (!out.Close())
	{
		SetState(job.serial, State::NeedSpace, 0, 0, "Couldn't write the download (is the disk full?)");
		Log("closing the download failed (errno " + std::to_string(out.Errno()) + ")");
		return false;
	}
	const double secs = Now() - t0;
	Log("downloaded " + FormatBytes(have - at_start) + " in " + std::to_string(static_cast<int>(secs)) + " s" +
		(secs > 0 ? " (" + FormatBytes(static_cast<uint64_t>((have - at_start) / secs)) + "/s)" : std::string()) +
		(out.RemoteBytes() ? ", " + FormatBytes(out.RemoteBytes()) + " written by the writer process" : std::string(", written by the app")) +
		(out.LocalBytes() && out.RemoteBytes() ? " and " + FormatBytes(out.LocalBytes()) + " by the app" : std::string()));
	if (have < job.bytes)
		return false; // stopped: the part stays for next time
	SetState(job.serial, State::Verifying, job.bytes, job.bytes);
	const std::string sum = md5.Hex();
	if (checksummed && sum != job.md5)
	{
		unlink(part.c_str());
		SetState(job.serial, State::Failed, 0, 0, "The download didn't match archive.org's checksum: try again");
		Log("MD5 " + sum + ", archive.org says " + job.md5 + ": the download is gone");
		return false;
	}
	if (rename(part.c_str(), full.c_str()) != 0)
	{
		SetState(job.serial, State::Failed, 0, 0, "Couldn't keep the download");
		return false;
	}
	Log(checksummed ? std::string("MD5 matches archive.org's") : "MD5 " + sum + " (nothing to compare it with: " + job.Source() + " gives none)");
	return true;
}

bool TexturePackManager::Unpack(const Job& job, const std::string& full, const std::string& staging, uint32_t& files, bool& no_space,
	std::string& error)
{
	SetState(job.serial, State::Unpacking, 0, job.bytes);
	archive* a = archive_read_new();
	archive_read_support_format_rar(a);
	archive_read_support_format_rar5(a);
	archive_read_support_format_zip(a);
	if (archive_read_open_filename(a, full.c_str(), 1 << 20) != ARCHIVE_OK)
	{
		error = std::string("Can't open the pack: ") + (archive_error_string(a) ? archive_error_string(a) : "?");
		archive_read_free(a);
		return false;
	}
	uint32_t skipped = 0, loose_over = 0;
	uint64_t loose_total = 0;
	bool ok = true;
	archive_entry* e = nullptr;
	int r;
	std::vector<std::string> made; // folders made, so each is made once
	// pr9l: <serial>/replacements/... goes into <serial>/replacements.pak (PakWriter); anything else in the pack is written
	// as it is, by FileWriters (made when the first such file comes).
	std::map<std::string, std::unique_ptr<PakWriter>> paks;
	std::unique_ptr<FileWriters> loose;
	// Where the time goes, logged every 10 s (the console's first unpack took over 30 minutes for 13,200 files, pr9h).
	using Clock = std::chrono::steady_clock;
	const auto Seconds = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); };
	double t_read = 0, t_pack = 0, t_queue = 0, t_dirs = 0;
	uint64_t out_bytes = 0;
	uint32_t packed = 0, queued = 0;
	auto last_report = Clock::now();
	const auto unpack_start = last_report;
	const auto pak_wait = [&] {
		double w = 0;
		for (const auto& kv : paks)
			w += kv.second ? kv.second->WaitSeconds() : 0.0;
		return w;
	};
	const auto pak_write = [&] {
		double w = 0;
		for (const auto& kv : paks)
			w += kv.second ? kv.second->WriteSeconds() : 0.0;
		return w;
	};
	const auto Report = [&](const char* what) {
		char line[320];
		std::snprintf(line, sizeof(line),
			"%s: %u files (%.0f MB) in %.0f s; reading %.0f s, writing the pack %.1f s (waiting for the pack's writer %.1f s; its write() calls %.1f s), folders %.1f s; %u other files (waiting %.1f s, "
			"their writers %.1f s)",
			what, packed + queued, out_bytes / 1048576.0, Seconds(unpack_start, Clock::now()), t_read, t_pack, pak_wait(), pak_write(), t_dirs, queued, t_queue,
			loose ? loose->WriteSeconds() : 0.0);
		Log(line);
	};
	while (true)
	{
		const auto th = Clock::now();
		r = archive_read_next_header(a, &e);
		t_read += Seconds(th, Clock::now());
		if (r != ARCHIVE_OK)
			break;
		if (Seconds(last_report, Clock::now()) >= 10.0)
		{
			last_report = Clock::now();
			Report("unpacking");
		}
		if (Stopping(job.serial))
		{
			ok = false;
			break;
		}
		SetProgress(job.serial, static_cast<uint64_t>(std::max<la_int64_t>(0, archive_filter_bytes(a, -1))));
		if (archive_entry_filetype(e) != AE_IFREG)
			continue;
		const char* utf8 = archive_entry_pathname_utf8(e);
		const char* raw = archive_entry_pathname(e);
		const std::string rel = TexturePackTarget(utf8 ? utf8 : raw ? raw : "", job.serial);
		if (rel.empty())
		{
			skipped++;
			archive_read_data_skip(a);
			continue;
		}
		std::string pak_serial, pak_name;
		const bool to_pak = SplitReplacement(rel, pak_serial, pak_name);
		if (!to_pak && IsPakPlace(rel))
		{
			skipped++;
			archive_read_data_skip(a);
			continue;
		}
		const std::string out = staging + "/" + rel;
		const std::string dir = to_pak ? staging + "/" + pak_serial : out.substr(0, out.rfind('/'));
		if (std::find(made.begin(), made.end(), dir) == made.end())
		{
			const auto td = Clock::now();
			const bool dir_ok = MakeDirs(dir);
			t_dirs += Seconds(td, Clock::now());
			if (!dir_ok)
			{
				error = "Couldn't make " + dir;
				no_space = errno == ENOSPC;
				ok = false;
				break;
			}
			made.push_back(dir);
		}
		// 2.11: an entry of the pack is streamed into its file block by block (the loose files below still come whole).
		if (to_pak)
		{
			std::unique_ptr<PakWriter>& pak = paks[pak_serial];
			bool good = true;
			const auto tp0 = Clock::now();
			if (!pak)
			{
				PakWriter::Options options;
				options.sync_each = m_platform.flag && m_platform.flag("texpack_sync");
				options.direct = m_platform.flag && m_platform.flag("texpack_direct");
				options.thread_start = m_platform.thread_start;
				options.remote = m_writer.get();
				options.log = [this](const std::string& line) { Log(line.compare(0, 11, "[texpacks] ") == 0 ? line.substr(11) : line); };
				pak = std::make_unique<PakWriter>(std::move(options));
				good = pak->Open(dir + "/" + OrbisTexturePak::kFileName);
			}
			good = good && pak->Begin(pak_name);
			t_pack += Seconds(tp0, Clock::now());
			uint64_t at = 0;
			int d = ARCHIVE_OK;
			const void* sbuf = nullptr;
			size_t ssize = 0;
			la_int64_t soffset = 0;
			unsigned blocks = 0;
			while (good)
			{
				const auto t1 = Clock::now();
				d = archive_read_data_block(a, &sbuf, &ssize, &soffset);
				t_read += Seconds(t1, Clock::now());
				if (d != ARCHIVE_OK)
					break;
				if (soffset < 0 || static_cast<uint64_t>(soffset) < at || static_cast<uint64_t>(soffset) + ssize > (1ull << 30))
				{
					d = ARCHIVE_FATAL;
					break;
				}
				const auto t2 = Clock::now();
				good = (static_cast<uint64_t>(soffset) <= at || pak->AppendZeros(static_cast<uint64_t>(soffset) - at)) && pak->Append(sbuf, ssize);
				t_pack += Seconds(t2, Clock::now());
				at = static_cast<uint64_t>(soffset) + ssize;
				if ((++blocks & 63) == 0 && Stopping(job.serial)) // a cancel is heard inside a very big entry too
				{
					ok = false;
					break;
				}
			}
			if (!ok)
				break;
			if (!good)
			{
				no_space = pak->NoSpace();
				error = no_space ? std::string("The disk is full") : "Couldn't write " + pak_serial + "/" + OrbisTexturePak::kFileName + " (" + pak->Error() + ")";
				ok = false;
				break;
			}
			if (d != ARCHIVE_EOF)
			{
				error = std::string("The pack is damaged: ") + (archive_error_string(a) ? archive_error_string(a) : "an entry is too big");
				ok = false;
				break;
			}
			pak->End();
			out_bytes += at;
			packed++;
			continue;
		}
		// The entry, whole (a gap in a sparse entry is zeros). b176: a file of the pack that is no texture (not under replacements/) is held whole in memory until a writer
		// has it (it used to be up to 1 GiB in one vector): kLooseEntryMax each and kLooseTotalMax together, the same caps as the 7z path; a bigger one is left out, counted.
		if (archive_entry_size_is_set(e) && archive_entry_size(e) > 0 &&
			(static_cast<uint64_t>(archive_entry_size(e)) > kLooseEntryMax || loose_total + static_cast<uint64_t>(archive_entry_size(e)) > kLooseTotalMax))
		{
			loose_over++;
			archive_read_data_skip(a);
			continue;
		}
		std::vector<uint8_t> data;
		if (archive_entry_size_is_set(e) && archive_entry_size(e) > 0 && archive_entry_size(e) < (1ll << 30))
			data.reserve(static_cast<size_t>(archive_entry_size(e)));
		const void* buf = nullptr;
		size_t size = 0;
		la_int64_t offset = 0;
		int d;
		bool too_big = false; // its size wasn't said first and it turned out over the caps
		const auto tr = Clock::now();
		while ((d = archive_read_data_block(a, &buf, &size, &offset)) == ARCHIVE_OK)
		{
			if (offset < 0 || static_cast<uint64_t>(offset) < data.size() || static_cast<uint64_t>(offset) + size > (1ull << 30))
			{
				d = ARCHIVE_FATAL;
				break;
			}
			if (static_cast<uint64_t>(offset) + size > kLooseEntryMax || loose_total + static_cast<uint64_t>(offset) + size > kLooseTotalMax)
			{
				too_big = true;
				break;
			}
			data.resize(static_cast<size_t>(offset)); // zeros for a gap
			const uint8_t* p = static_cast<const uint8_t*>(buf);
			data.insert(data.end(), p, p + size);
		}
		t_read += Seconds(tr, Clock::now());
		if (too_big)
		{
			loose_over++;
			archive_read_data_skip(a);
			continue;
		}
		if (d != ARCHIVE_EOF)
		{
			error = std::string("The pack is damaged: ") + (archive_error_string(a) ? archive_error_string(a) : "an entry is too big");
			ok = false;
			break;
		}
		out_bytes += data.size();
		loose_total += data.size();
		if (!loose)
			loose = std::make_unique<FileWriters>(2, m_platform.thread_start);
		const auto tq = Clock::now();
		const bool put = loose->Put(out, std::move(data));
		t_queue += Seconds(tq, Clock::now());
		if (!put)
			break; // a write failed: Finish says which
		queued++;
	}
	// The packs' indexes and headers.
	for (auto& [serial, pak] : paks)
	{
		if (!ok)
			break; // the staging folder goes
		const auto tp = Clock::now();
		const bool done = pak->Finish();
		t_pack += Seconds(tp, Clock::now());
		if (!done)
		{
			no_space = pak->NoSpace();
			error = no_space ? std::string("The disk is full") : "Couldn't write " + serial + "/" + OrbisTexturePak::kFileName + " (" + pak->Error() + ")";
			ok = false;
		}
		else
			Log(serial + "/" + OrbisTexturePak::kFileName + ": " + std::to_string(pak->Count()) + " files, " + FormatBytes(pak->Bytes()));
	}
	bool all_written = true;
	if (loose)
	{
		const auto tf = Clock::now();
		all_written = loose->Finish();
		t_queue += Seconds(tf, Clock::now());
	}
	Report("unpacked");
	if (!all_written)
	{
		no_space = loose->NoSpace();
		const std::string failed = loose->FailedPath();
		error = no_space ? std::string("The disk is full") :
						   "Couldn't write " + (failed.compare(0, staging.size() + 1, staging + "/") == 0 ? failed.substr(staging.size() + 1) : failed);
		ok = false;
	}
	files = packed + (loose ? loose->Written() : 0);
	if (ok && r != ARCHIVE_EOF)
	{
		error = std::string("The pack is damaged: ") + (archive_error_string(a) ? archive_error_string(a) : "?");
		ok = false;
	}
	Log(std::string("read ") + archive_format_name(a) + ": " + std::to_string(files) + " files kept (" + std::to_string(packed) +
		" in the pack), " + std::to_string(skipped) + " left out (dumps or other folders)" +
		(loose_over ? ", " + std::to_string(loose_over) + " other files left out for their size" : std::string()));
	archive_read_free(a);
	return ok;
}

std::string TexturePackManager::RefusedFile(const Job& job) const
{
	return m_work + "/" + job.Key() + "." + job.serial + ".refused";
}

// b176 (swordpdf: "we forgot to add the new texture packs links"): the .7z packs of the community's Drive folder (192 of the 228), read by fe_sevenzip.c with
// the LZMA SDK vendored in 3rdparty/lzma. The reader decodes one solid block at a time as a stream (never the whole block in memory): what it needs is
// the block's dictionary (min(declared, unpacked size), 64 MiB at most), a 1.25 MiB chunk, the file table the header gives, and the pack writer's three 8 MiB
// buffers a serial; the free-memory gate below checks that against the system's free flexible memory with a 48 MiB margin before anything is taken. The files
// go through the same TexturePackTarget / SplitReplacement / PakWriter / FileWriters as the libarchive path (same rules, same files, same threads at the same
// priority), the reader checks every file's CRC and the end of every block, and a pack that needs more than the limits give (or that this reader doesn't
// read: filters, a second coder, encryption, no CRCs) is refused with a message before a byte is written, once and for all (<key>.<serial>.refused).
bool TexturePackManager::Unpack7z(const Job& job, const std::string& full, const std::string& staging, uint32_t& files, bool& no_space, std::string& error,
	bool& drop)
{
	drop = false;
	files = 0;
	SetState(job.serial, State::Unpacking, 0, job.bytes);
	const auto flag = [this](const char* name) { return m_platform.flag && m_platform.flag(name); };
	const bool gate = m_platform.free_memory && !flag("texpack_nomemgate");
	const auto free_now = [this]() -> uint64_t { return m_platform.free_memory ? m_platform.free_memory() : UINT64_MAX; };
	const auto memory = [&free_now]() -> std::string {
		const uint64_t f = free_now();
		return f == UINT64_MAX ? std::string("flexible memory free: unknown") : "flexible memory free " + std::to_string(f >> 20) + " MiB";
	};
	const auto megabytes = [](uint64_t b) { return std::to_string((b + 1048575) >> 20) + " MiB"; };
	// A refusal that can never change is kept (with the limits it was made under, and the size of the file it was made for: Drive keeps a file's id when its maker uploads
	// a new version of it, and that one may be the fixed one), so that a pack the app can't use here is not downloaded again to be refused again.
	const auto remember = [&](const std::string& message) {
		MakeDirs(m_work);
		if (!settings::WriteFileAtomic(RefusedFile(job), "limits=" + std::to_string(kLimitsRevision) + "\nbytes=" + std::to_string(job.bytes) + "\nmessage=" + OneLine(message) + "\n"))
			Log("couldn't keep the refusal in " + RefusedFile(job));
	};
	const auto refuse = [&](Sz7Code c, const char* text, bool header_phase) {
		const SevenZipVerdict v = JudgeSevenZip(c, text, header_phase);
		error = v.message;
		drop = v.drop;
		if (v.drop)
			remember(v.message);
		Log(std::string("7z: ") + Sz7CodeName(c) + ": " + text);
		return false;
	};

	std::unique_ptr<Sz7Archive, Sz7Deleter> z(Sz7New());
	if (!z)
	{
		error = "Not enough memory to open the pack";
		return false;
	}
	Sz7Limits lim;
	Sz7LimitsDefault(&lim);
	if (flag("texpack_7z_bigdict"))
		lim.max_dict = 256ull << 20; // still behind the free-memory gate
	const uint64_t archive_bytes = FileSize(full);
	Log("7z: " + job.name + ": " + FormatBytes(archive_bytes) + " packed; " + memory());
	if (gate)
	{
		// the header phase may take (and holds a file table after it) whatever the header needs, up to its allowance, before its size is known
		const uint64_t free = free_now();
		if (free != UINT64_MAX && free < kSevenZipMargin + kSevenZipHeaderFloor)
		{
			error = "Not enough memory to unpack this pack right now (" + megabytes(free) + " free, " + megabytes(kSevenZipMargin + kSevenZipHeaderFloor) + " wanted)";
			Log("7z: " + error);
			return false;
		}
	}
	const Sz7Code open_code = Sz7Open(z.get(), full.c_str(), &lim);
	const Sz7Info* info = Sz7GetInfo(z.get());
	if (open_code != SZ7_OK)
		return refuse(open_code, Sz7Error(z.get()), true);
	Log("7z: " + std::to_string(info->files) + " entries in " + std::to_string(info->folders) + " block(s), " + FormatBytes(info->unpack_total) + " unpacked, " + info->method +
		", dictionary buffer " + FormatBytes(info->dict_need) + " (declared " + FormatBytes(info->dict_declared) + "), file table " + FormatBytes(info->header_live) +
		" (header peak " + FormatBytes(info->header_peak) + "); " + memory());

	SevenZipRun run;
	run.serial = job.serial;
	run.staging = staging;
	run.stopping = [this, &job] { return Stopping(job.serial); };
	run.progress = [this, &job](uint64_t done) { SetProgress(job.serial, done); };
	run.log = [this](const std::string& line) { Log(line.compare(0, 11, "[texpacks] ") == 0 ? line.substr(11) : line); };
	run.thread_start = m_platform.thread_start;
	run.memory_text = memory;
	run.sync_each = flag("texpack_sync");
	run.direct = flag("texpack_direct");
	run.remote = m_writer.get();
	run.packed_total = info->pack_total;
	Sz7Scan scan;
	const Sz7Code scan_code = Sz7ScanFiles(z.get(), SevenZipRun::SWant, &run, &scan);
	if (scan_code != SZ7_OK)
		return refuse(scan_code, Sz7Error(z.get()), false);
	const uint64_t kept_total = run.kept_total, loose_bytes = run.loose_bytes;
	const size_t pak_count = run.pak_serials.size();
	Log("7z: wanted " + std::to_string(scan.kept_files) + " files (" + FormatBytes(kept_total) + ") for " + std::to_string(pak_count) + " pack file(s), of them " +
		std::to_string(run.loose_files) + " loose (" + FormatBytes(loose_bytes) + "); left out: " + std::to_string(run.not_wanted) + " (dumps, other folders), " +
		std::to_string(run.loose_left_out) + " loose over " + FormatBytes(kLooseEntryMax) + " or " + FormatBytes(kLooseTotalMax) + " together");
	// No texture at all (nothing under a <serial>/replacements/ folder: a readme, a dumps/ folder, a layout nothing here reads): not a pack for this emulator, and not an
	// install whose marker says "installed" with nothing in it. The file decides this, so it is said once and for all (like the refusals below): a 2 GB download is not
	// repeated to learn it again.
	if (run.pak_serials.empty())
	{
		error = "Nothing in it is a texture for " + job.serial;
		drop = true;
		remember(error);
		Log("7z: " + error + " (" + std::to_string(scan.kept_files) + " files wanted, none of them under a replacements/ folder)");
		return false;
	}
	// The textures must be for THIS game (or a disc of it). The Drive list's serials come from a sheet with typos (three games share SLUS-21722 there); a pack whose
	// textures are all in other serials' folders would be installed under them while the marker (and the shelf's "installed") says this game: refused instead, once and
	// for all. (A folder of this game that holds only a readme or the like doesn't count: the game would get no texture from it.)
	if (!run.pak_serials.count(job.serial))
	{
		std::string held;
		for (const std::string& d : run.pak_serials)
			held += (held.empty() ? "" : ", ") + d;
		error = "Not for this game: the pack holds textures for " + held + ", not for " + job.serial + " (the pack list may have a wrong serial for it)";
		drop = true;
		remember(error);
		Log("7z: " + error);
		return false;
	}
	// The room: the .7z is on the disk already; what is added is what is wanted (and some)
	{
		const uint64_t want = kept_total + kSevenZipRoom;
		const uint64_t room = m_platform.free_bytes ? m_platform.free_bytes(m_textures) : UINT64_MAX;
		if (room < want)
		{
			no_space = true;
			error = "Needs " + FormatBytes(want) + " free to unpack (" + FormatBytes(room) + " now)";
			Log("7z: not enough room to unpack: " + FormatBytes(room) + " free, " + FormatBytes(want) + " wanted (" + FormatBytes(kept_total) + " of files)");
			return false;
		}
	}
	// The memory: the dictionary, the chunks (1 MiB out, 256 KiB in, the reader's 64 KiB look-ahead), the file table, three 8 MiB buffers a pack file, the packs'
	// indexes, the loose files in flight, and the margin. Against what the system still has free.
	{
		const uint64_t need = std::max<uint64_t>(info->dict_need, 4096) + (1ull << 20) + (256ull << 10) + (96ull << 10) + info->header_live +
			static_cast<uint64_t>(pak_count) * (24ull << 20) + scan.kept_files * 256 + loose_bytes + kSevenZipMargin;
		const uint64_t free = free_now();
		Log("7z: needs about " + megabytes(need) + " of flexible memory with a " + megabytes(kSevenZipMargin) + " margin; " + memory() + (gate ? "" : " (not checked)"));
		if (gate && free != UINT64_MAX && free < need)
		{
			error = "Not enough memory to unpack this pack right now (needs about " + megabytes(need) + " free, " + megabytes(free) + " now)";
			Log("7z: " + error);
			return false;
		}
	}
	run.Reset();
	run.t_start = run.last_report = SevenZipRun::Clock::now();
	SetState(job.serial, State::Unpacking, 0, info->pack_total);
	Sz7Sink sink;
	sink.begin = SevenZipRun::SBegin;
	sink.data = SevenZipRun::SData;
	sink.end = SevenZipRun::SEnd;
	sink.cancelled = SevenZipRun::SCancelled;
	sink.ctx = &run;
	Sz7Result result;
	const Sz7Code code = Sz7Extract(z.get(), &sink, &result);
	run.packed_done = result.packed_done;
	if (code == SZ7_CANCELLED || (code == SZ7_SINK && run.stopped))
	{
		error = "cancelled"; // (Process sees the stop or the cancel itself)
		Log(std::string("7z: stopped while unpacking, ") + (run.stopped ? "while waiting for the disk" : "between two chunks") + ", at " + FormatBytes(result.packed_done) + " of " +
			FormatBytes(info->pack_total) + " packed");
		return false;
	}
	if (code == SZ7_SINK)
	{
		if (run.error.empty() && run.loose) // a loose file's write failed: its writers say which
		{
			run.loose->Finish();
			no_space = run.loose->NoSpace();
			const std::string failed = run.loose->FailedPath();
			error = no_space ? std::string("The disk is full") :
							   "Couldn't write " + (failed.compare(0, staging.size() + 1, staging + "/") == 0 ? failed.substr(staging.size() + 1) : failed);
		}
		else
		{
			error = run.error.empty() ? std::string("Couldn't write the pack") : run.error;
			no_space = run.no_space;
		}
		Log("7z: unpacking stopped: " + error);
		return false;
	}
	if (code != SZ7_OK)
		return refuse(code, Sz7Error(z.get()), false);
	// The packs' indexes and headers, then the loose files.
	bool ok = true;
	for (auto& [serial, pak] : run.paks)
	{
		if (!pak->Finish())
		{
			no_space = pak->NoSpace();
			error = no_space ? std::string("The disk is full") : "Couldn't write " + serial + "/" + OrbisTexturePak::kFileName + " (" + pak->Error() + ")";
			ok = false;
			break;
		}
		Log(serial + "/" + OrbisTexturePak::kFileName + ": " + std::to_string(pak->Count()) + " files, " + FormatBytes(pak->Bytes()));
	}
	if (run.loose && !run.loose->Finish() && ok)
	{
		no_space = run.loose->NoSpace();
		const std::string failed = run.loose->FailedPath();
		error = no_space ? std::string("The disk is full") :
						   "Couldn't write " + (failed.compare(0, staging.size() + 1, staging + "/") == 0 ? failed.substr(staging.size() + 1) : failed);
		ok = false;
	}
	files = run.packed + (run.loose ? run.loose->Written() : 0);
	run.Report("unpacked");
	Log("7z unpacked " + job.name + ": archive " + std::to_string(archive_bytes) + " bytes, " + std::to_string(files) + " files kept of " + std::to_string(info->files) +
		" entries, dictionary buffer " + std::to_string(info->dict_need) + " bytes (declared " + std::to_string(info->dict_declared) + "), file table " +
		std::to_string(info->header_live) + " bytes, " + std::to_string(static_cast<int>(run.Seconds(run.t_start, SevenZipRun::Clock::now()))) + " s; " + memory());
	return ok;
}

void TexturePackManager::Finish(const Job& job, uint32_t files)
{
	char when[32];
	const std::time_t now = std::time(nullptr);
	std::tm tm{};
	gmtime_r(&now, &tm);
	std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", &tm);
	const bool drive = job.host == "gdrive";
	const std::string marker = std::string("# Installed by PS5SX2 from ") +
		(drive ? "the community's Google Drive HD texture packs" : "archive.org's PCSX2 HD Texture Packs") +
		". Removing it from the sheet\n# (Square, HD texture pack, Triangle twice) deletes this folder.\nname=" + OneLine(job.name) + "\nsource=" +
		(drive ? "https://drive.google.com/file/d/" + OneLine(job.drive_id) + "/view" : std::string("https://archive.org/details/") + kTexturePackItem) +
		(job.md5.empty() ? std::string() : "\nmd5=" + job.md5) + "\nfiles=" + std::to_string(files) + "\ninstalled=" + when + "\n";
	if (!settings::WriteFileAtomic(m_textures + "/" + job.serial + "/" + kMarker, marker))
		Log("couldn't write the marker in " + m_textures + "/" + job.serial);
	DeleteJobFiles(job);
	// Texture replacements on for this game.
	std::string what, error;
	if (!job.settings.empty())
	{
		const std::vector<settings::Change> changes = {{settings::Change::Set, "LoadTextureReplacements", "true"}};
		if (settings::EditSettingsFile(job.settings, job.header, changes, what, error))
			Log("settings: " + job.settings + (what.empty() ? " already had Texture replacements on" : ": " + what));
		else
			Log("couldn't switch Texture replacements on: " + error);
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_progress.erase(job.serial);
		m_installed.erase(job.serial);
	}
	Log("installed " + job.name + " in " + m_textures + "/" + job.serial);
	if (m_platform.notify)
		m_platform.notify(job.title, true, FormatBytes(job.bytes));
}

// What the textures folder has for a serial: file-system work, so Status calls it without the lock (and not while a pack
// is being unpacked, installed or removed).
TexturePackManager::Installed TexturePackManager::LookAtFolder(const std::string& serial) const
{
	Installed in;
	in.checked = Now();
	const std::string dir = m_textures + "/" + serial;
	const auto kv = ReadKeyValues(dir + "/" + kMarker);
	const auto name = kv.find("name");
	if (name != kv.end())
	{
		in.present = in.ours = true;
		in.name = name->second;
		in.where = dir;
		return in;
	}
	if (HasEntries(dir + "/replacements"))
	{
		in.present = true;
		in.where = dir;
		return in;
	}
	if (m_platform.existing_pack)
	{
		const std::string other = m_platform.existing_pack(serial);
		if (!other.empty() && other.compare(0, m_textures.size(), m_textures) != 0)
		{
			in.present = true;
			in.where = other;
		}
	}
	return in;
}

// 2.11: a pack by its file name, from archive.org's list or Drive's (the caller holds the lock).
const TexturePack* TexturePackManager::FindPack(const std::string& name) const
{
	for (const TexturePack& p : m_catalog)
		if (p.name == name)
			return &p;
	for (const TexturePack& p : m_extra)
		if (p.name == name)
			return &p;
	return nullptr;
}

std::vector<TexturePack> TexturePackManager::PacksFor(const std::string& serial) const
{
	std::vector<TexturePack> out;
	for (const TexturePack& p : m_catalog)
		if (std::find(p.serials.begin(), p.serials.end(), serial) != p.serials.end())
			out.push_back(p);
	// 2.11: the Drive packs after archive.org's (whose checksums vouch for them), by maker. b176: and the .rar and .zip ones before the .7z ones (the first is what Cross
	// takes unless Left/Right picked another): for the 8 games that have both a CCKrizalid .rar and a Bl4ckH4nd .7z the pack that was the default in build 175 stays
	// it, and the one that is smaller, quicker to unpack and needs less memory comes first.
	const size_t archive_count = out.size();
	for (const TexturePack& p : m_extra)
		if (std::find(p.serials.begin(), p.serials.end(), serial) != p.serials.end())
			out.push_back(p);
	std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(archive_count), out.end(), [](const TexturePack& a, const TexturePack& b) {
		const bool a7 = ExtOf(a.name) == ".7z", b7 = ExtOf(b.name) == ".7z";
		return a7 != b7 ? b7 : a.maker < b.maker;
	});
	// Two with the same edition (two titles under one serial): their titles tell them apart.
	for (size_t i = 0; i < out.size(); i++)
		for (size_t j = 0; j < out.size(); j++)
			if (i != j && out[i].label == out[j].label)
				out[i].label = out[i].title;
	return out;
}

TexturePackStatus TexturePackManager::Status(const std::string& raw_serial) const
{
	TexturePackStatus s;
	const std::string serial = NormalSerial(raw_serial);
	std::unique_lock<std::mutex> lock(m_mutex);
	if (serial.empty())
	{
		s.state = State::None;
		return s;
	}
	s.packs = PacksFor(serial);
	const auto it = m_progress.find(serial);
	if (it != m_progress.end())
	{
		const Progress& p = it->second;
		s.state = p.state;
		s.done = p.done;
		s.total = p.total;
		s.rate = p.rate;
		s.message = p.message;
		for (size_t i = 0; i < s.packs.size(); i++)
			if (s.packs[i].name == p.name)
				s.job_pack = static_cast<int>(i);
		return s;
	}
	// The folder, from the cache; looked at again when that is a few seconds old, without the lock (the worker's progress
	// waits on it) and not while a pack is being unpacked, installed or removed: the disk is busy then, and this is the
	// menu's thread (a look there waited up to 0.9 s on pr9h's unpack).
	Installed in;
	const auto cached = m_installed.find(serial);
	const bool fresh = cached != m_installed.end() && cached->second.checked >= 0 && Now() - cached->second.checked < 3.0;
	bool busy = false;
	for (const auto& kv : m_progress)
		busy = busy || kv.second.state == State::Unpacking || kv.second.state == State::Installing || kv.second.state == State::Removing;
	if (fresh || busy)
		in = cached != m_installed.end() ? cached->second : Installed();
	else
	{
		lock.unlock();
		in = LookAtFolder(serial);
		lock.lock();
		m_installed[serial] = in;
	}
	if (in.present)
	{
		s.state = State::Installed;
		s.installed = in.name;
		s.where = in.where;
		s.ours = in.ours;
		return s;
	}
	if (m_catalog_state == Catalog::Loading)
		s.state = State::Loading;
	else if (m_catalog_state == Catalog::Failed)
	{
		s.state = s.packs.empty() ? State::Unavailable : State::Available; // 2.11: Drive's packs list though archive.org's list failed
		s.message = m_catalog_error;
	}
	else
		s.state = s.packs.empty() ? State::None : State::Available;
	return s;
}

bool TexturePackManager::Begin(const std::string& raw_serial, int pack, const std::string& settings_path, const std::string& settings_header,
	const std::string& title)
{
	const std::string serial = NormalSerial(raw_serial);
	Job j;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (serial.empty() || m_stop)
			return false;
		const auto it = m_progress.find(serial);
		const bool redo = it != m_progress.end() && (it->second.state == State::Failed || it->second.state == State::NeedSpace);
		if (it != m_progress.end() && !redo)
			return false; // already on its way
		const std::vector<TexturePack> packs = PacksFor(serial);
		if (pack < 0 || pack >= static_cast<int>(packs.size()))
			return false;
		// 2.11: archive.org's packs wait for its list; a Drive pack does not need it
		if (m_catalog_state != Catalog::Ready && !packs[static_cast<size_t>(pack)].OnDrive())
			return false;
		// A failed job of another pack for this game: its files go.
		if (redo && it->second.name != packs[static_cast<size_t>(pack)].name)
		{
			// 2.11 (review): by the job's own file (the list may have changed since), else by the pack's name
			Job old;
			if (ReadJobFile(serial, old))
				DeleteJobFiles(old);
			else if (const TexturePack* was = FindPack(it->second.name))
			{
				old.serial = serial;
				old.name = was->name;
				old.md5 = was->md5;
				old.drive_id = was->drive_id;
				DeleteJobFiles(old);
			}
		}
		const TexturePack& p = packs[static_cast<size_t>(pack)];
		j.serial = serial;
		j.name = p.name;
		j.md5 = p.md5;
		j.host = p.host;
		j.drive_id = p.drive_id;
		j.bytes = p.bytes;
		j.title = title;
		j.settings = settings_path;
		j.header = settings_header;
	}
	if (!WriteJob(j))
		Log("couldn't write the job file in " + m_work + " (it won't carry on after a game)");
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		Progress& p = m_progress[serial];
		p = Progress();
		p.state = State::Queued;
		p.name = j.name;
		p.title = j.title;
		m_queue.push_back(j);
	}
	Log("asked for " + j.name + " (" + (j.bytes ? FormatBytes(j.bytes) : "size asked of " + j.Source() + " when it starts") + ") for " + serial);
	m_cv.notify_all();
	return true;
}

bool TexturePackManager::Cancel(const std::string& raw_serial)
{
	const std::string serial = NormalSerial(raw_serial);
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto it = m_progress.find(serial);
	if (it == m_progress.end())
		return false;
	if (m_current == serial)
	{
		m_cancel_current = true;
		if (m_platform.abort)
			m_platform.abort();
		return true;
	}
	// Queued, failed or short of space: its files go now.
	for (auto q = m_queue.begin(); q != m_queue.end();)
		q = q->serial == serial ? m_queue.erase(q) : q + 1;
	{
		// 2.11 (review): by the job's own file (the list may have changed since), else by the pack's name
		Job old;
		if (ReadJobFile(serial, old))
			DeleteJobFiles(old);
		else if (const TexturePack* was = FindPack(it->second.name))
		{
			old.serial = serial;
			old.name = was->name;
			old.md5 = was->md5;
			old.drive_id = was->drive_id;
			DeleteJobFiles(old);
		}
	}
	unlink((m_work + "/" + serial + ".job").c_str());
	m_progress.erase(it);
	Log("forgot the download for " + serial);
	return true;
}

bool TexturePackManager::Remove(const std::string& raw_serial)
{
	const std::string serial = NormalSerial(raw_serial);
	std::lock_guard<std::mutex> lock(m_mutex);
	if (serial.empty() || m_progress.count(serial) || !Exists(m_textures + "/" + serial + "/" + kMarker))
		return false; // only a pack this app installed, and not while it's busy with it
	Progress& p = m_progress[serial];
	p = Progress();
	p.state = State::Removing;
	m_removals.push_back(serial);
	m_cv.notify_all();
	return true;
}

void TexturePackManager::Retry()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_catalog_state != Catalog::Failed)
		return;
	m_catalog_state = Catalog::Loading;
	m_retry = true;
	m_cv.notify_all();
}

TexturePackActivity TexturePackManager::Activity() const
{
	TexturePackActivity a;
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_current.empty())
		return a;
	const auto it = m_progress.find(m_current);
	if (it == m_progress.end())
		return a;
	const Progress& p = it->second;
	if (p.state == State::Failed || p.state == State::NeedSpace)
		return a;
	a.active = true;
	a.title = p.title;
	a.state = p.state;
	a.fraction = p.total > 0 ? std::min(1.0, static_cast<double>(p.done) / static_cast<double>(p.total)) : 0;
	a.waiting = static_cast<int>(m_queue.size());
	return a;
}

TexturePackService TexturePackManager::Service()
{
	TexturePackService s;
	s.status = [this](const std::string& serial) { return Status(serial); };
	s.begin = [this](const std::string& serial, int pack, const std::string& path, const std::string& header, const std::string& title) {
		return Begin(serial, pack, path, header, title);
	};
	s.cancel = [this](const std::string& serial) { return Cancel(serial); };
	s.remove = [this](const std::string& serial) { return Remove(serial); };
	s.retry = [this] { Retry(); };
	s.activity = [this] { return Activity(); };
	return s;
}
} // namespace fe
